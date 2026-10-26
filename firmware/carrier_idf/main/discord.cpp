#include "discord.h"

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_random.h>
#include <esp_websocket_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <nvs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "board_time.h"
#include "commands.h"
#include "config.h"
#include "display.h"
#include "log.h"
#include "net.h"
#include "optos.h"
#include "pc_state.h"
#include "screens.h"
#include "secrets_or_example.h"
#include "sys.h"

#define API "https://discord.com/api/v10"
#define WS_BUF_SIZE (64 * 1024)

static esp_websocket_client_handle_t ws;
static volatile bool ready;
static volatile bool requestReconnect;
static uint32_t heartbeatMs, lastBeat;
static int32_t seq = -1;
static bool gotAck = true;

static char *wsBuf;
static int wsBufLen;
static bool wsMsgDropped;

// ---- hand-over to the main loop: one command at a time ----
static char reqLine[64];
static char reqReply[1500];
static volatile bool reqPending;
static SemaphoreHandle_t reqDone;
static SemaphoreHandle_t restMutex[2];

static bool runOnMainLoop(const char *line, uint32_t timeoutMs = 2500) {
  snprintf(reqLine, sizeof reqLine, "%s", line);
  reqReply[0] = 0;
  xSemaphoreTake(reqDone, 0);
  reqPending = true;
  return xSemaphoreTake(reqDone, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void discordPoll() {
  if (!reqPending) return;
  logCapture(reqReply, sizeof reqReply);
  commandRun(reqLine);
  logCapture(nullptr, 0);
  reqPending = false;
  xSemaphoreGive(reqDone);
}

static void runAndGetFirstLine(const char *cmd, char *out, size_t outSize) {
  if (!runOnMainLoop(cmd)) {
    snprintf(out, outSize, "%s: no answer from the board", cmd);
    return;
  }
  if (!reqReply[0]) {
    snprintf(out, outSize, "(done)");
    return;
  }
  const char *nl = strchr(reqReply, '\n');
  size_t len = nl ? (size_t)(nl - reqReply) : strlen(reqReply);
  if (len >= outSize) len = outSize - 1;
  memcpy(out, reqReply, len);
  out[len] = 0;
  if (len > 0 && out[len - 1] == '\r') out[len - 1] = 0;
  if (!out[0]) snprintf(out, outSize, "(done)");
}

// ---- action notifications (FreeRTOS queue from main loop) ----
#define NOTIFY_QUEUE_DEPTH 8
#define NOTIFY_MSG_LEN 128

struct NotifyMsg {
  uint8_t ch;            // DiscordChannel
  bool plain;            // from discordNotify: title picked by the bot task
  uint32_t color;
  char text[NOTIFY_MSG_LEN];
  char desc[320];
};

static QueueHandle_t notifyQueue;

void discordNotify(const char *what) {
  if (!DISCORD_TOKEN[0] || !notifyQueue || !what) return;
  NotifyMsg msg = {};
  msg.ch = DC_PC;
  msg.plain = true;
  snprintf(msg.text, sizeof msg.text, "%s", what);
  xQueueSend(notifyQueue, &msg, 0);
}

void discordPost(DiscordChannel ch, const char *title, const char *desc, uint32_t color) {
  if (!DISCORD_TOKEN[0] || !notifyQueue || !title) return;
  NotifyMsg msg = {};
  msg.ch = ch;
  msg.color = color;
  snprintf(msg.text, sizeof msg.text, "%s", title);
  snprintf(msg.desc, sizeof msg.desc, "%s", desc ? desc : "");
  xQueueSend(notifyQueue, &msg, 0);
}

static const char *channelId(DiscordChannel ch) {
  const char *id = ch == DC_WEATHER ? DISCORD_CH_WEATHER : DISCORD_CH_PC;
  return id[0] ? id : DISCORD_CHANNEL_ID;
}

// ---- #logging: every log line, timestamped, posted in batches ----
#define LOG_RING_SIZE (8 * 1024)
#define LOG_POST_MAX 1900       // a message holds 2000 characters
static char *logRing;           // PSRAM
static size_t logLen;
static uint32_t logDropped;
static portMUX_TYPE logMux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t botTaskHandle;
static volatile bool postingLog;   // the bot task's own lines while posting the log are not logged

void discordLog(const char *line, size_t len) {
  if (!logRing || !len) return;
  if (postingLog && xTaskGetCurrentTaskHandle() == botTaskHandle) return;
  char ts[12] = "";
  struct tm t;
  if (boardLocalTime(&t)) snprintf(ts, sizeof ts, "%02d:%02d:%02d ", t.tm_hour, t.tm_min, t.tm_sec);
  if (len > 240) len = 240;
  size_t tl = strlen(ts);
  taskENTER_CRITICAL(&logMux);
  if (logLen + tl + len + 1 < LOG_RING_SIZE) {
    memcpy(logRing + logLen, ts, tl);
    memcpy(logRing + logLen + tl, line, len);
    logLen += tl + len;
    logRing[logLen++] = '\n';
  } else {
    logDropped++;
  }
  taskEXIT_CRITICAL(&logMux);
}

// ---- REST ----
struct HttpRespBuf {
  char *text;
  int cap;
  int len;
};

static esp_err_t httpEventHandler(esp_http_client_event_t *evt) {
  if (evt->event_id == HTTP_EVENT_ON_DATA) {
    HttpRespBuf *rb = (HttpRespBuf *)evt->user_data;
    if (rb && rb->text && rb->len < rb->cap - 1) {
      int copyLen = evt->data_len;
      if (rb->len + copyLen > rb->cap - 1) {
        copyLen = rb->cap - 1 - rb->len;   // rb->text is a pointer now: never sizeof it
      }
      memcpy(rb->text + rb->len, evt->data, copyLen);
      rb->len += copyLen;
      rb->text[rb->len] = '\0';
    }
  }
  return ESP_OK;
}

// out (optional) gets the response body
// Two connections: the bot task's (posts, log, panel) and everyone else's
// (button presses, which Discord wants answered within 3 s)
static int rest(const char *method, const char *path, const char *body, char *out = nullptr, size_t outCap = 0) {
  int slot = xTaskGetCurrentTaskHandle() == botTaskHandle ? 0 : 1;
  SemaphoreHandle_t mtx = restMutex[slot];
  if (mtx) xSemaphoreTake(mtx, portMAX_DELAY);
  static esp_http_client_handle_t clients[2];
  static HttpRespBuf respBufs[2];
  static char respFallback[2][128];
  esp_http_client_handle_t &client = clients[slot];
  HttpRespBuf &respBuf = respBufs[slot];
  if (!respBuf.text) {
    respBuf.text = (char *)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    respBuf.cap = respBuf.text ? 8192 : sizeof respFallback[slot];
    if (!respBuf.text) respBuf.text = respFallback[slot];
  }
  char url[720];
  snprintf(url, sizeof url, "%s%s", API, path);

  respBuf.len = 0;
  respBuf.text[0] = 0;
  if (out && outCap) out[0] = 0;

  if (!client) {
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.keep_alive_enable = true;
    cfg.keep_alive_idle = 5;
    cfg.keep_alive_interval = 5;
    cfg.keep_alive_count = 3;
    cfg.event_handler = httpEventHandler;
    cfg.user_data = &respBuf;
    cfg.timeout_ms = 10000;
    cfg.buffer_size = 2048;      // Discord's response headers
    cfg.buffer_size_tx = 1536;   // request line with an interaction token, plus headers
    client = esp_http_client_init(&cfg);
    if (!client) {
      if (mtx) xSemaphoreGive(mtx);
      return -1;
    }
  } else {
    esp_http_client_set_url(client, url);
  }

  esp_http_client_method_t m = HTTP_METHOD_GET;
  if (!strcmp(method, "POST")) m = HTTP_METHOD_POST;
  else if (!strcmp(method, "PUT")) m = HTTP_METHOD_PUT;
  else if (!strcmp(method, "DELETE")) m = HTTP_METHOD_DELETE;
  else if (!strcmp(method, "PATCH")) m = HTTP_METHOD_PATCH;
  esp_http_client_set_method(client, m);

  char auth[128];
  snprintf(auth, sizeof auth, "Bot %s", DISCORD_TOKEN);
  esp_http_client_set_header(client, "Authorization", auth);
  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_header(client, "User-Agent", "DiscordBot (https://github.com/lnminh1411/ESP32-PC-Control-System, 1.0)");

  if (body && body[0]) {
    esp_http_client_set_post_field(client, body, strlen(body));
  } else {
    esp_http_client_set_post_field(client, nullptr, 0);
  }

  // Discord drops idle keep-alive connections after a few minutes; writing to
  // one fails with "connection reset" (3 error lines in #logging) before the
  // retry below. Start fresh when the last request is older than 60 s.
  static uint32_t lastUseMs[2];
  if (lastUseMs[slot] && nowMs() - lastUseMs[slot] > 60000) esp_http_client_close(client);
  esp_err_t err = esp_http_client_perform(client);
  lastUseMs[slot] = nowMs();
  if (err != ESP_OK) {
    esp_http_client_close(client);
    respBuf.len = 0;
    respBuf.text[0] = 0;
    err = esp_http_client_perform(client);
  }

  int code = esp_http_client_get_status_code(client);
  // the path without interaction tokens (long, and not for #logging)
  char shown[64];
  snprintf(shown, sizeof shown, "%s", path);
  if (!strncmp(path, "/interactions/", 14) || !strncmp(path, "/webhooks/", 10)) {
    char *tok = strchr(shown + 1, '/');
    tok = tok ? strchr(tok + 1, '/') : nullptr;
    if (tok) snprintf(tok, shown + sizeof shown - tok, "/...");
  }
  if (err != ESP_OK) {
    logMsg("discord: %s %s -> err %s", method, shown, esp_err_to_name(err));
    if (mtx) xSemaphoreGive(mtx);
    return -1;
  }
  if (code < 200 || code >= 300) {
    logMsg("discord: %s %s -> %d %.160s", method, shown, code, respBuf.text);
  }
  if (respBuf.len >= respBuf.cap - 1) logMsg("discord: %s %s response cut at %d bytes", method, shown, respBuf.len);
  if (out && outCap) snprintf(out, outCap, "%s", respBuf.text);
  if (mtx) xSemaphoreGive(mtx);
  return code;
}

static void postChannelEmbed(DiscordChannel ch, const char *title, const char *desc, uint32_t color) {
  const char *chId = channelId(ch);
  if (!chId[0]) return;
  cJSON *root = cJSON_CreateObject();
  cJSON *embeds = cJSON_AddArrayToObject(root, "embeds");
  cJSON *embed = cJSON_CreateObject();
  cJSON_AddStringToObject(embed, "title", title);
  if (desc && desc[0]) cJSON_AddStringToObject(embed, "description", desc);
  cJSON_AddNumberToObject(embed, "color", color);
  cJSON_AddItemToArray(embeds, embed);
  char *body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);

  char path[128];
  snprintf(path, sizeof path, "/channels/%s/messages", chId);
  rest("POST", path, body);
  free(body);
}

// One batch of the log ring as a code block in #logging
static void postLogBatch() {
  static char *chunk;   // PSRAM: internal RAM is kept for TLS and task stacks
  if (!chunk) chunk = (char *)heap_caps_malloc(LOG_POST_MAX + 64, MALLOC_CAP_SPIRAM);
  if (!chunk) return;
  size_t n = 0;
  uint32_t dropped;
  taskENTER_CRITICAL(&logMux);
  n = logLen < LOG_POST_MAX ? logLen : LOG_POST_MAX;
  if (n < logLen) {   // whole lines only
    while (n > 0 && logRing[n - 1] != '\n') n--;
    if (!n) n = LOG_POST_MAX;
  }
  memcpy(chunk, logRing, n);
  memmove(logRing, logRing + n, logLen - n);
  logLen -= n;
  dropped = logDropped;
  logDropped = 0;
  taskEXIT_CRITICAL(&logMux);
  if (!n) return;
  while (n && chunk[n - 1] == '\n') n--;
  chunk[n] = 0;
  for (size_t i = 0; i < n; i++)
    if (chunk[i] == '`') chunk[i] = '\'';   // keep the code block closed

  char *content = (char *)malloc(n + 64);
  if (!content) return;
  memcpy(content, "```\n", 4);
  memcpy(content + 4, chunk, n);
  strcpy(content + 4 + n, dropped ? "\n```(some lines dropped)" : "\n```");
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "content", content);
  cJSON *am = cJSON_AddObjectToObject(root, "allowed_mentions");
  cJSON_AddArrayToObject(am, "parse");
  char *body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  free(content);

  char path[128];
  snprintf(path, sizeof path, "/channels/%s/messages", DISCORD_CH_LOG);
  postingLog = true;
  rest("POST", path, body);
  postingLog = false;
  free(body);
}

// ---- actions panel tracking ----
static char panelToken[128];
static uint32_t panelTouchedMs, panelOpenedMs;
#define PANEL_TOKEN_LIFE_MS (14 * 60 * 1000UL)   // interaction tokens die at 15 min
static bool panelActive;
static char lastPanelResult[128];

static void deleteActivePanel() {
  if (!panelActive || !panelToken[0] || !DISCORD_APP_ID[0]) return;
  char delPath[640];
  snprintf(delPath, sizeof delPath, "/webhooks/%s/%s/messages/@original", DISCORD_APP_ID, panelToken);
  rest("DELETE", delPath, "");
  panelActive = false;
  panelToken[0] = 0;
}

// ---- #control: the standing panel message ----
static char ctlMsgId[24];   // its message id, kept in NVS with the channel it is in

// ---- slash command registration ----
static void registerCommands() {
  cJSON *root = cJSON_CreateArray();
  cJSON *c = cJSON_CreateObject();
  cJSON_AddStringToObject(c, "name", "actions");
  cJSON_AddStringToObject(c, "description", "Open the ESP32 PC Control panel (only you see it)");
  cJSON_AddItemToArray(root, c);
  char *body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);

  char path[128];
  snprintf(path, sizeof path, "/applications/%s/guilds/%s/commands", DISCORD_APP_ID, DISCORD_GUILD_ID);
  int code = rest("PUT", path, body);
  free(body);
  if (code >= 200 && code < 300) logMsg("discord: slash commands registered");
}

// ---- UI builders ----
enum PanelView {
  PANEL_NORMAL,
  PANEL_CONFIRM_RESET,
  PANEL_CONFIRM_SLEEP,
  PANEL_CONFIRM_RESTART,
  PANEL_CONFIRM_SHUTDOWN,
  PANEL_CONFIRM_HIBERNATE,
  PANEL_CONFIRM_REBOOT
};

// Panel tabs; the #control panel and the /actions panel each remember theirs
enum PanelCat : uint8_t { CAT_DOOR, CAT_PC, CAT_BOARD, CAT_DISPLAY, CAT_COUNT };
static const char *const CAT_LABEL[CAT_COUNT] = {"Door", "PC Control", "Board", "Display"};
static const char *const CAT_ID[CAT_COUNT] = {"tab_door", "tab_pc", "tab_board", "tab_display"};
static uint8_t catStanding = CAT_PC, catEphemeral = CAT_PC;
static uint8_t &panelCat(bool standing) { return standing ? catStanding : catEphemeral; }

// Faces the Display tab offers per panel, as the `face` command names them
struct FaceOpt { const char *name, *label; Face f; };
static const FaceOpt FACE_OPTS[] = {
  {"auto", "Auto (follows the PC)", FACE_NONE}, {"pc", "PC stats", FACE_PC_STATS}, {"gif", "GIF", FACE_GIF},
  {"media", "Media", FACE_MEDIA}, {"weather", "Weather", FACE_WEATHER}, {"ai", "AI usage", FACE_AI_USAGE},
  {"localai", "Local AI", FACE_LOCAL_AI}, {"clock", "Clock", FACE_CLOCK}};

static const char *faceLabel(Face f) {
  for (const FaceOpt &o : FACE_OPTS)
    if (o.f == f) return o.label;
  return "other";
}

static void addButton(cJSON *row, const char *label, const char *id, int style) {
  cJSON *b = cJSON_CreateObject();
  cJSON_AddNumberToObject(b, "type", 2);
  cJSON_AddStringToObject(b, "label", label);
  cJSON_AddStringToObject(b, "custom_id", id);
  cJSON_AddNumberToObject(b, "style", style); // 1 blurple, 2 grey, 3 green, 4 red
  cJSON_AddItemToArray(row, b);
}

static cJSON *addRow(cJSON *comps) {
  cJSON *r = cJSON_CreateObject();
  cJSON_AddNumberToObject(r, "type", 1);
  cJSON *rowComps = cJSON_AddArrayToObject(r, "components");
  cJSON_AddItemToArray(comps, r);
  return rowComps;
}

// Screen picker for one panel: a select menu with the override marked
static void addFaceSelect(cJSON *comps, int panel) {
  cJSON *row = addRow(comps);
  cJSON *sel = cJSON_CreateObject();
  cJSON_AddNumberToObject(sel, "type", 3);
  char id[12];
  snprintf(id, sizeof id, "face_%d", panel + 1);
  cJSON_AddStringToObject(sel, "custom_id", id);
  cJSON *opts = cJSON_AddArrayToObject(sel, "options");
  for (const FaceOpt &o : FACE_OPTS) {
    cJSON *op = cJSON_CreateObject();
    char label[48];
    snprintf(label, sizeof label, "Panel %d: %s", panel + 1, o.label);
    cJSON_AddStringToObject(op, "label", label);
    cJSON_AddStringToObject(op, "value", o.name);
    if (screensForce[panel] == o.f) cJSON_AddBoolToObject(op, "default", true);
    cJSON_AddItemToArray(opts, op);
  }
  cJSON_AddItemToArray(row, sel);
}

static void addField(cJSON *fields, const char *name, const char *value) {
  cJSON *f = cJSON_CreateObject();
  cJSON_AddStringToObject(f, "name", name);
  cJSON_AddStringToObject(f, "value", value);
  cJSON_AddBoolToObject(f, "inline", true);
  cJSON_AddItemToArray(fields, f);
}

static void buildEmbed(cJSON *embed, const char *footerText, bool standing) {
  uint8_t cat = panelCat(standing);
  char title[48];
  snprintf(title, sizeof title, "ESP32 PC Control | %s", CAT_LABEL[cat]);
  cJSON_AddStringToObject(embed, "title", title);
  cJSON_AddNumberToObject(embed, "color", 0x5865F2);
  cJSON *fields = cJSON_AddArrayToObject(embed, "fields");
  char v[64];

  addField(fields, "PC", (pcState() == PC_ON) ? "on" : (pcState() == PC_OFF) ? "asleep" : "no host");
  if (cat == CAT_DOOR) {
    int dp = doorPressed();
    if (dp > 0) snprintf(v, sizeof v, "B%d pressed", dp);
    else snprintf(v, sizeof v, "idle");
    addField(fields, "Door remote", v);
  } else if (cat == CAT_PC) {
    if (netUp() && netIp()[0]) snprintf(v, sizeof v, "%s (%d dBm)", netIp(), netRssi());
    else snprintf(v, sizeof v, "down");
    addField(fields, "Board Wi-Fi", v);
  } else if (cat == CAT_BOARD) {
    uint32_t s = nowMs() / 1000;
    snprintf(v, sizeof v, "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60), (unsigned long)(s % 60));
    addField(fields, "Uptime", v);
    if (netUp() && netIp()[0]) snprintf(v, sizeof v, "%s (%d dBm)", netIp(), netRssi());
    else snprintf(v, sizeof v, "down");
    addField(fields, "Wi-Fi", v);
    snprintf(v, sizeof v, "%u KB free, largest %u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
    addField(fields, "Internal RAM", v);
    snprintf(v, sizeof v, "%u KB free", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    addField(fields, "PSRAM", v);
    addField(fields, "Last restart", resetReasonText());
  } else {
    for (int i = 0; i < N_PANELS; i++) {
      char name[12];
      snprintf(name, sizeof name, "Panel %d", i + 1);
      snprintf(v, sizeof v, "%s, %u %%", screensForce[i] == FACE_NONE ? "auto" : faceLabel(screensForce[i]), backlightPct(i));
      addField(fields, name, v);
    }
    addField(fields, "Displays", backlightForcedOff() ? "off" : backlightIsIdle() ? "dark (PC asleep)" : "on");
  }

  const char *ft = (footerText && footerText[0]) ? footerText :
                   lastPanelResult[0] ? lastPanelResult :
                   standing ? "Owner only" : "Panel closes by itself after 2 minutes";
  cJSON *footer = cJSON_AddObjectToObject(embed, "footer");
  cJSON_AddStringToObject(footer, "text", ft);
}

static void confirmRow(cJSON *comps, const char *label, const char *id, int style) {
  cJSON *r1 = addRow(comps);
  addButton(r1, label, id, style);
  addButton(r1, "Cancel", "refresh", 2);
}

static void buildComponents(cJSON *data, PanelView view, bool standing) {
  cJSON *comps = cJSON_AddArrayToObject(data, "components");
  switch (view) {
    case PANEL_CONFIRM_RESET: confirmRow(comps, "Yes, reset", "reset_confirm", 4); return;
    case PANEL_CONFIRM_SLEEP: confirmRow(comps, "Yes, sleep", "sleep_confirm", 2); return;
    case PANEL_CONFIRM_RESTART: confirmRow(comps, "Yes, restart", "restart_confirm", 4); return;
    case PANEL_CONFIRM_SHUTDOWN: confirmRow(comps, "Yes, shut down", "shutdown_confirm", 4); return;
    case PANEL_CONFIRM_HIBERNATE: confirmRow(comps, "Yes, hibernate", "hibernate_confirm", 2); return;
    case PANEL_CONFIRM_REBOOT: confirmRow(comps, "Yes, restart the board", "reboot_confirm", 4); return;
    case PANEL_NORMAL: break;
  }

  // row 1: the tabs, the open one highlighted, and Refresh
  uint8_t cat = panelCat(standing);
  cJSON *tabs = addRow(comps);
  for (int i = 0; i < CAT_COUNT; i++) addButton(tabs, CAT_LABEL[i], CAT_ID[i], i == cat ? 1 : 2);
  addButton(tabs, "Refresh", "refresh", 2);

  // Close (the /actions panel only) goes on the tab's last row
  if (cat == CAT_DOOR) {
    cJSON *r = addRow(comps);
    addButton(r, "Door 1", "door_1", 1);
    addButton(r, "Door 2", "door_2", 1);
    addButton(r, "Door 3", "door_3", 1);
    addButton(r, "Door 4", "door_4", 1);
    if (!standing) addButton(r, "Close", "close", 2);
  } else if (cat == CAT_PC) {
    cJSON *r = addRow(comps);
    addButton(r, "Wake PC", "wake", 3);
    addButton(r, "Lock", "lock", 1);
    addButton(r, "Sleep", "sleep", 2);
    addButton(r, "Hibernate", "hibernate", 2);
    r = addRow(comps);
    addButton(r, "Restart", "restart", 4);
    addButton(r, "Shut down", "shutdown", 4);
    addButton(r, "PC reset", "reset", 4);
    if (!standing) addButton(r, "Close", "close", 2);
  } else if (cat == CAT_BOARD) {
    cJSON *r = addRow(comps);
    addButton(r, "Restart board", "reboot", 4);
    addButton(r, "Refresh weather", "wx_refresh", 2);
    addButton(r, "Reconnect Discord", "dc_reconnect", 2);
    if (!standing) addButton(r, "Close", "close", 2);
  } else {
    for (int i = 0; i < N_PANELS; i++) addFaceSelect(comps, i);
    cJSON *r = addRow(comps);
    addButton(r, "Dimmer", "bl_down", 2);
    addButton(r, "Brighter", "bl_up", 2);
    if (backlightForcedOff()) addButton(r, "Displays on", "disp_on", 3);
    else addButton(r, "Displays off", "disp_off", 4);
    addButton(r, "All auto", "face_auto", 2);
    if (!standing) addButton(r, "Close", "close", 2);
  }
}

// 4 = reply with a new (ephemeral) panel; 7 = update the panel the button is
// on, sent as an edit after ackButton() so slow actions don't time out
static void sendPanelResponse(const char *id, const char *token, int callbackType, bool standing, PanelView view, const char *footerText) {
  bool edit = callbackType == 7 && DISCORD_APP_ID[0];
  cJSON *root = cJSON_CreateObject();
  cJSON *data = root;
  if (!edit) {
    cJSON_AddNumberToObject(root, "type", callbackType);
    data = cJSON_AddObjectToObject(root, "data");
  }
  if (callbackType == 4) {
    cJSON_AddNumberToObject(data, "flags", 64); // ephemeral
  }
  cJSON *embeds = cJSON_AddArrayToObject(data, "embeds");
  cJSON *embed = cJSON_CreateObject();
  buildEmbed(embed, footerText, standing);
  cJSON_AddItemToArray(embeds, embed);
  buildComponents(data, view, standing);

  char *body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);

  char path[640];
  if (edit) snprintf(path, sizeof path, "/webhooks/%s/%s/messages/@original", DISCORD_APP_ID, token);
  else snprintf(path, sizeof path, "/interactions/%s/%s/callback", id, token);
  rest(edit ? "PATCH" : "POST", path, body);
  free(body);
}

// "Got it, the message will change": Discord's 3 s deadline met before any work
static void ackButton(const char *id, const char *token) {
  // the interaction id is a snowflake: when Discord made it, ms since 2015
  int64_t madeMs = (int64_t)(strtoull(id, nullptr, 10) >> 22) + 1420070400000LL;
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  int64_t lateMs = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000 - madeMs;
  uint32_t t0 = nowMs();
  char path[640];
  snprintf(path, sizeof path, "/interactions/%s/%s/callback", id, token);
  int code = rest("POST", path, "{\"type\":6}");
  logMsg("discord: button reached the board %lld ms after the click, answered in %lu ms (%d)",
         (long long)lateMs, (unsigned long)(nowMs() - t0), code);
}

// Post the standing panel in #control, or bring the one there up to date
static void controlPanelSync() {
  if (!DISCORD_CH_CONTROL[0]) return;
  char footer[48] = "Owner only";
  struct tm t;
  if (boardLocalTime(&t)) snprintf(footer, sizeof footer, "Owner only | updated %02d:%02d", t.tm_hour, t.tm_min);
  cJSON *root = cJSON_CreateObject();
  cJSON *embeds = cJSON_AddArrayToObject(root, "embeds");
  cJSON *embed = cJSON_CreateObject();
  buildEmbed(embed, footer, true);
  cJSON_AddItemToArray(embeds, embed);
  buildComponents(root, PANEL_NORMAL, true);
  char *body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!body) return;

  char path[128];
  if (ctlMsgId[0]) {
    snprintf(path, sizeof path, "/channels/%s/messages/%s", DISCORD_CH_CONTROL, ctlMsgId);
    int code = rest("PATCH", path, body);
    if (code == 404) ctlMsgId[0] = 0;   // deleted: post a new one
  }
  if (!ctlMsgId[0]) {
    const size_t respCap = 8192;
    char *resp = (char *)heap_caps_malloc(respCap, MALLOC_CAP_SPIRAM);
    if (!resp) { free(body); return; }
    snprintf(path, sizeof path, "/channels/%s/messages", DISCORD_CH_CONTROL);
    int code = rest("POST", path, body, resp, respCap);
    cJSON *r = code >= 200 && code < 300 ? cJSON_Parse(resp) : nullptr;
    cJSON *jid = r ? cJSON_GetObjectItem(r, "id") : nullptr;
    if (jid && cJSON_IsString(jid)) {
      snprintf(ctlMsgId, sizeof ctlMsgId, "%s", jid->valuestring);
      nvs_handle_t h;
      if (nvs_open("discord", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "ctl_ch", DISCORD_CH_CONTROL);
        nvs_set_str(h, "ctl_msg", ctlMsgId);
        nvs_commit(h);
        nvs_close(h);
      }
      logMsg("discord: control panel posted");
    }
    cJSON_Delete(r);
    free(resp);
  }
  free(body);
}

static void loadControlPanelId() {
  nvs_handle_t h;
  if (nvs_open("discord", NVS_READONLY, &h) != ESP_OK) return;
  char ch[24] = "";
  size_t len = sizeof ch;
  if (nvs_get_str(h, "ctl_ch", ch, &len) == ESP_OK && !strcmp(ch, DISCORD_CH_CONTROL)) {
    len = sizeof ctlMsgId;
    if (nvs_get_str(h, "ctl_msg", ctlMsgId, &len) != ESP_OK) ctlMsgId[0] = 0;
  }
  nvs_close(h);
}

static void replyEphemeralText(const char *id, const char *token, const char *text) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "type", 4);
  cJSON *data = cJSON_AddObjectToObject(root, "data");
  cJSON_AddStringToObject(data, "content", text);
  cJSON_AddNumberToObject(data, "flags", 64);
  char *body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);

  char path[640];
  snprintf(path, sizeof path, "/interactions/%s/%s/callback", id, token);
  rest("POST", path, body);
  free(body);
}

static void onInteraction(cJSON *d) {
  cJSON *jId = cJSON_GetObjectItem(d, "id");
  cJSON *jToken = cJSON_GetObjectItem(d, "token");
  const char *id = (jId && jId->valuestring) ? jId->valuestring : "";
  const char *token = (jToken && jToken->valuestring) ? jToken->valuestring : "";

  const char *user = "";
  cJSON *member = cJSON_GetObjectItem(d, "member");
  if (member) {
    cJSON *mUser = cJSON_GetObjectItem(member, "user");
    if (mUser) {
      cJSON *uId = cJSON_GetObjectItem(mUser, "id");
      if (uId && uId->valuestring) user = uId->valuestring;
    }
  }
  if (!user[0]) {
    cJSON *u = cJSON_GetObjectItem(d, "user");
    if (u) {
      cJSON *uId = cJSON_GetObjectItem(u, "id");
      if (uId && uId->valuestring) user = uId->valuestring;
    }
  }

  if (strcmp(user, DISCORD_OWNER_ID) != 0) {
    replyEphemeralText(id, token, "Not allowed.");
    return;
  }

  cJSON *jType = cJSON_GetObjectItem(d, "type");
  int type = jType ? jType->valueint : 0;
  cJSON *data = cJSON_GetObjectItem(d, "data");

  // a button on the standing #control panel, not on an /actions panel
  cJSON *msg = cJSON_GetObjectItem(d, "message");
  cJSON *jMid = msg ? cJSON_GetObjectItem(msg, "id") : nullptr;
  bool standing = ctlMsgId[0] && jMid && cJSON_IsString(jMid) && !strcmp(jMid->valuestring, ctlMsgId);

  if (type == 2) { // slash command
    cJSON *jName = data ? cJSON_GetObjectItem(data, "name") : nullptr;
    const char *name = (jName && jName->valuestring) ? jName->valuestring : "";
    if (!strcmp(name, "actions")) {
      char oldToken[sizeof panelToken];
      bool hadOld = panelActive;
      snprintf(oldToken, sizeof oldToken, "%s", panelToken);
      snprintf(panelToken, sizeof panelToken, "%s", token);
      panelTouchedMs = panelOpenedMs = nowMs();
      panelActive = true;
      lastPanelResult[0] = 0;
      sendPanelResponse(id, token, 4, false, PANEL_NORMAL, nullptr);
      if (hadOld && oldToken[0] && DISCORD_APP_ID[0]) {
        char delPath[640];
        snprintf(delPath, sizeof delPath, "/webhooks/%s/%s/messages/@original", DISCORD_APP_ID, oldToken);
        rest("DELETE", delPath, "");
      }
    } else {
      replyEphemeralText(id, token, "Unknown command.");
    }
  } else if (type == 3) { // button
    cJSON *jCid = data ? cJSON_GetObjectItem(data, "custom_id") : nullptr;
    const char *cid = (jCid && jCid->valuestring) ? jCid->valuestring : "";
    if (!strcmp(cid, "close") && !standing) {
      cJSON *r = cJSON_CreateObject();
      cJSON_AddNumberToObject(r, "type", 6); // DEFERRED_UPDATE_MESSAGE
      char *body = cJSON_PrintUnformatted(r);
      cJSON_Delete(r);

      char path[640];
      snprintf(path, sizeof path, "/interactions/%s/%s/callback", id, token);
      rest("POST", path, body);
      free(body);

      const char *delToken = token[0] ? token : panelToken;
      if (DISCORD_APP_ID[0] && delToken[0]) {
        char delPath[640];
        snprintf(delPath, sizeof delPath, "/webhooks/%s/%s/messages/@original", DISCORD_APP_ID, delToken);
        rest("DELETE", delPath, "");
      }
      panelActive = false;
      panelToken[0] = 0;
      lastPanelResult[0] = 0;
      return;
    }

    if (!standing) panelTouchedMs = nowMs();
    if (DISCORD_APP_ID[0]) ackButton(id, token);

    cJSON *jVals = data ? cJSON_GetObjectItem(data, "values") : nullptr;
    cJSON *jVal = (jVals && cJSON_IsArray(jVals)) ? cJSON_GetArrayItem(jVals, 0) : nullptr;
    if (jVal && cJSON_IsString(jVal) && !strncmp(cid, "face_", 5)) {
      // Display tab: a screen picked for one panel (not saved, `face` command)
      char cmd[32];
      snprintf(cmd, sizeof cmd, "face %s %s", cid + 5, jVal->valuestring);
      runAndGetFirstLine(cmd, lastPanelResult, sizeof lastPanelResult);
      sendPanelResponse(id, token, 7, standing, PANEL_NORMAL, lastPanelResult);
      return;
    }
    for (int i = 0; i < CAT_COUNT; i++) {
      if (!strcmp(cid, CAT_ID[i])) {
        panelCat(standing) = (uint8_t)i;
        lastPanelResult[0] = 0;
        sendPanelResponse(id, token, 7, standing, PANEL_NORMAL, nullptr);
        return;
      }
    }

    if (!strcmp(cid, "refresh")) {
      snprintf(lastPanelResult, sizeof lastPanelResult, "Refreshed");
      sendPanelResponse(id, token, 7, standing, PANEL_NORMAL, "Refreshed");
    } else if (!strcmp(cid, "lock")) {
      runAndGetFirstLine("power lock", lastPanelResult, sizeof lastPanelResult);
      if (!lastPanelResult[0]) snprintf(lastPanelResult, sizeof lastPanelResult, "Lock sent to the PC");
      sendPanelResponse(id, token, 7, standing, PANEL_NORMAL, lastPanelResult);
    } else if (!strcmp(cid, "reboot")) {
      sendPanelResponse(id, token, 7, standing, PANEL_CONFIRM_REBOOT, "Restart the board? (the PC is not touched)");
    } else if (!strcmp(cid, "reboot_confirm")) {
      snprintf(lastPanelResult, sizeof lastPanelResult, "Board restarting, back in ~30 s");
      sendPanelResponse(id, token, 7, standing, PANEL_NORMAL, lastPanelResult);
      logMsg("discord: board restart requested");
      runOnMainLoop("reboot");
    } else if (!strcmp(cid, "wx_refresh")) {
      runAndGetFirstLine("weather refresh", lastPanelResult, sizeof lastPanelResult);
      sendPanelResponse(id, token, 7, standing, PANEL_NORMAL, lastPanelResult);
    } else if (!strcmp(cid, "dc_reconnect")) {
      snprintf(lastPanelResult, sizeof lastPanelResult, "Reconnecting to Discord");
      sendPanelResponse(id, token, 7, standing, PANEL_NORMAL, lastPanelResult);
      requestReconnect = true;
    } else if (!strcmp(cid, "bl_down") || !strcmp(cid, "bl_up")) {
      // 20 % steps on every panel, kept between 10 and 100 (saved, as `bl` does)
      bool up = cid[3] == 'u';
      for (int i = 0; i < N_PANELS; i++) {
        int v = backlightPct(i) + (up ? 20 : -20);
        v = v < 10 ? 10 : v > 100 ? 100 : v;
        char cmd[24], ignore[64];
        snprintf(cmd, sizeof cmd, "bl %d %d", i + 1, v);
        runAndGetFirstLine(cmd, ignore, sizeof ignore);
      }
      snprintf(lastPanelResult, sizeof lastPanelResult, "Backlights %u / %u / %u %%", backlightPct(0), backlightPct(1),
               backlightPct(2));
      sendPanelResponse(id, token, 7, standing, PANEL_NORMAL, lastPanelResult);
    } else if (!strcmp(cid, "disp_on") || !strcmp(cid, "disp_off")) {
      runAndGetFirstLine(cid[5] == 'n' ? "display on" : "display off", lastPanelResult, sizeof lastPanelResult);
      sendPanelResponse(id, token, 7, standing, PANEL_NORMAL, lastPanelResult);
    } else if (!strcmp(cid, "face_auto")) {
      char ignore[64];
      for (int i = 0; i < N_PANELS; i++) {
        char cmd[16];
        snprintf(cmd, sizeof cmd, "face %d auto", i + 1);
        runAndGetFirstLine(cmd, ignore, sizeof ignore);
      }
      snprintf(lastPanelResult, sizeof lastPanelResult, "All panels back to auto");
      sendPanelResponse(id, token, 7, standing, PANEL_NORMAL, lastPanelResult);
    } else if (!strcmp(cid, "reset")) {
      sendPanelResponse(id, token, 7, standing, PANEL_CONFIRM_RESET, "Reset the PC?");
    } else if (!strcmp(cid, "reset_confirm")) {
      runAndGetFirstLine("reset", lastPanelResult, sizeof lastPanelResult);
      sendPanelResponse(id, token, 7, standing, PANEL_NORMAL, lastPanelResult);
    } else if (!strcmp(cid, "wake")) {
      runAndGetFirstLine("wake", lastPanelResult, sizeof lastPanelResult);
      sendPanelResponse(id, token, 7, standing, PANEL_NORMAL, lastPanelResult);
    } else if (!strcmp(cid, "sleep")) {
      sendPanelResponse(id, token, 7, standing, PANEL_CONFIRM_SLEEP, "Put PC to sleep?");
    } else if (!strcmp(cid, "sleep_confirm")) {
      runAndGetFirstLine("power sleep", lastPanelResult, sizeof lastPanelResult);
      sendPanelResponse(id, token, 7, standing, PANEL_NORMAL, lastPanelResult);
    } else if (!strcmp(cid, "restart")) {
      sendPanelResponse(id, token, 7, standing, PANEL_CONFIRM_RESTART, "Restart the PC?");
    } else if (!strcmp(cid, "restart_confirm")) {
      runAndGetFirstLine("power restart", lastPanelResult, sizeof lastPanelResult);
      sendPanelResponse(id, token, 7, standing, PANEL_NORMAL, lastPanelResult);
    } else if (!strcmp(cid, "shutdown")) {
      sendPanelResponse(id, token, 7, standing, PANEL_CONFIRM_SHUTDOWN, "Shut down the PC?");
    } else if (!strcmp(cid, "shutdown_confirm")) {
      runAndGetFirstLine("power shutdown", lastPanelResult, sizeof lastPanelResult);
      sendPanelResponse(id, token, 7, standing, PANEL_NORMAL, lastPanelResult);
    } else if (!strcmp(cid, "hibernate")) {
      sendPanelResponse(id, token, 7, standing, PANEL_CONFIRM_HIBERNATE, "Hibernate the PC?");
    } else if (!strcmp(cid, "hibernate_confirm")) {
      runAndGetFirstLine("power hibernate", lastPanelResult, sizeof lastPanelResult);
      sendPanelResponse(id, token, 7, standing, PANEL_NORMAL, lastPanelResult);
    } else if (!strncmp(cid, "door_", 5)) {
      char cmd[32];
      snprintf(cmd, sizeof cmd, "door %s", cid + 5);
      runAndGetFirstLine(cmd, lastPanelResult, sizeof lastPanelResult);
      sendPanelResponse(id, token, 7, standing, PANEL_NORMAL, lastPanelResult);
    }
  }
}

// ---- gateway ----
static void sendJson(cJSON *root) {
  char *s = cJSON_PrintUnformatted(root);
  if (s) {
    if (ws && esp_websocket_client_is_connected(ws)) {
      esp_websocket_client_send_text(ws, s, strlen(s), pdMS_TO_TICKS(2000));
    }
    free(s);
  }
}

static void heartbeat() {
  cJSON *d = cJSON_CreateObject();
  cJSON_AddNumberToObject(d, "op", 1);
  if (seq >= 0) {
    cJSON_AddNumberToObject(d, "d", seq);
  } else {
    cJSON_AddNullToObject(d, "d");
  }
  sendJson(d);
  cJSON_Delete(d);
  lastBeat = nowMs();
  gotAck = false;
}

static void identify() {
  cJSON *d = cJSON_CreateObject();
  cJSON_AddNumberToObject(d, "op", 2);
  cJSON *p = cJSON_AddObjectToObject(d, "d");
  cJSON_AddStringToObject(p, "token", DISCORD_TOKEN);
  cJSON_AddNumberToObject(p, "intents", 0); // interactions arrive without any intent
  cJSON *props = cJSON_AddObjectToObject(p, "properties");
  cJSON_AddStringToObject(props, "os", "esp32");
  cJSON_AddStringToObject(props, "browser", NET_HOSTNAME);
  cJSON_AddStringToObject(props, "device", NET_HOSTNAME);
  sendJson(d);
  cJSON_Delete(d);
}

static void handleGatewayMessage(const char *jsonStr, size_t len) {
  cJSON *d = cJSON_ParseWithLength(jsonStr, len);
  if (!d) return;

  cJSON *jOp = cJSON_GetObjectItem(d, "op");
  int op = jOp ? jOp->valueint : -1;

  cJSON *jS = cJSON_GetObjectItem(d, "s");
  if (jS && !cJSON_IsNull(jS)) {
    seq = jS->valueint;
  }

  cJSON *data = cJSON_GetObjectItem(d, "d");

  switch (op) {
    case 10: { // HELLO
      cJSON *jInterval = data ? cJSON_GetObjectItem(data, "heartbeat_interval") : nullptr;
      heartbeatMs = (jInterval && jInterval->valueint > 0) ? (uint32_t)jInterval->valueint : 41250;
      uint32_t jitter = heartbeatMs > 0 ? (esp_random() % heartbeatMs) : 0;
      lastBeat = nowMs() - jitter; // first beat after a jittered wait
      gotAck = true;
      identify();
      break;
    }
    case 11: // heartbeat ACK
      gotAck = true;
      break;
    case 1: // server asks for a beat now
      heartbeat();
      break;
    case 7: // reconnect requested
    case 9: // invalid session
      logMsg("discord: gateway asks to %s", op == 7 ? "reconnect" : "start over (invalid session)");
      requestReconnect = true;
      break;
    case 0: {
      cJSON *jT = cJSON_GetObjectItem(d, "t");
      const char *t = (jT && jT->valuestring) ? jT->valuestring : "";
      if (!strcmp(t, "READY")) {
        bool first = !ready;
        ready = true;
        const char *username = "?";
        if (data) {
          cJSON *user = cJSON_GetObjectItem(data, "user");
          if (user) {
            cJSON *uName = cJSON_GetObjectItem(user, "username");
            if (uName && uName->valuestring) username = uName->valuestring;
          }
        }
        logMsg("discord: online as %s", username);
        static bool announced;
        if (first && !announced) {
          announced = true;
          postChannelEmbed(DC_PC, "ESP32 PC Control is online", (netUp() && netIp()[0]) ? netIp() : "Wi-Fi down", 0x57F287);
        }
      } else if (!strcmp(t, "INTERACTION_CREATE")) {
        if (data) onInteraction(data);
      }
      break;
    }
    default:
      break;
  }

  cJSON_Delete(d);
}

static void onGatewayEvent(void *, esp_event_base_t, int32_t event_id, void *event_data) {
  esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
  switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
      logMsg("discord: gateway connected");
      break;
    case WEBSOCKET_EVENT_ERROR:
      logMsg("discord: gateway error");
      break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
      if (ready) logMsg("discord: gateway disconnected, reconnecting");
      ready = false;
      heartbeatMs = 0;
      wsBufLen = 0;
      wsMsgDropped = false;
      break;
    case WEBSOCKET_EVENT_DATA: {
      if (!wsBuf) break;
      if (data->payload_offset == 0) {
        wsBufLen = 0;
        wsMsgDropped = false;
        if (data->op_code != 1) return; // Only handle complete text frames (op_code 1)
        if (data->payload_len >= WS_BUF_SIZE) {
          wsMsgDropped = true;
          static bool loggedDrop;
          if (!loggedDrop) {
            loggedDrop = true;
            logMsg("discord: gateway message exceeds buffer (%d B), dropped", data->payload_len);
          }
          return;
        }
      }
      if (wsMsgDropped) return;
      if (data->payload_offset + data->data_len >= WS_BUF_SIZE) {
        wsMsgDropped = true;
        return;
      }
      if (data->data_len > 0) {
        memcpy(wsBuf + data->payload_offset, data->data_ptr, data->data_len);
      }
      wsBufLen = data->payload_offset + data->data_len;
      if (wsBufLen >= data->payload_len && data->payload_len > 0) {
        wsBuf[wsBufLen] = '\0';
        handleGatewayMessage(wsBuf, wsBufLen);
        wsBufLen = 0;
      }
      break;
    }
    default:
      break;
  }
}

static void botTask(void *) {
  while (!netUp()) sleepMs(500);
  registerCommands();

  esp_websocket_client_config_t ws_cfg = {};
  ws_cfg.uri = "wss://gateway.discord.gg/?v=10&encoding=json";
  ws_cfg.crt_bundle_attach = esp_crt_bundle_attach;
  ws_cfg.reconnect_timeout_ms = 5000;
  ws_cfg.network_timeout_ms = 10000;
  ws_cfg.buffer_size = 4096;
  ws_cfg.enable_close_reconnect = true;
  ws_cfg.task_core_id_set = true;
  ws_cfg.task_core_id = 0;
  ws_cfg.task_stack = 12288;   // gateway handlers run here and make TLS REST calls

  ws = esp_websocket_client_init(&ws_cfg);
  if (ws) {
    esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, onGatewayEvent, nullptr);
    esp_err_t err = esp_websocket_client_start(ws);
    if (err != ESP_OK) logMsg("discord: gateway start failed (%s)", esp_err_to_name(err));
  } else {
    logMsg("discord: gateway client init failed (internal RAM free %u)",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  }

  PcState lastPc = pcState();
  uint32_t lastLogPostMs = 0, lastCtlSyncMs = 0;
  bool ctlSynced = false;
  for (;;) {
    if (requestReconnect) {
      requestReconnect = false;
      if (ws) esp_websocket_client_close(ws, pdMS_TO_TICKS(1000));
    }
    if (heartbeatMs && nowMs() - lastBeat >= heartbeatMs) {
      if (!gotAck) {
        logMsg("discord: no heartbeat ACK, reconnecting");
        if (ws) esp_websocket_client_close(ws, pdMS_TO_TICKS(1000));
      } else {
        heartbeat();
      }
    }
    PcState pc = pcState();
    if (pc != lastPc && ready) {
      lastPc = pc;
      if (pc == PC_ON) postChannelEmbed(DC_PC, "PC is on", nullptr, 0x57F287);
      else if (pc == PC_OFF) postChannelEmbed(DC_PC, "PC is asleep or off", nullptr, 0xE67E22);
      else postChannelEmbed(DC_PC, "PC state unknown", nullptr, 0x99AAB5);
      lastCtlSyncMs = 0;   // the panel shows the PC state too
    }
    NotifyMsg nmsg;
    while (ready && notifyQueue && xQueueReceive(notifyQueue, &nmsg, 0) == pdTRUE) {
      DiscordChannel ch = (DiscordChannel)nmsg.ch;
      if (!nmsg.plain) {
        postChannelEmbed(ch, nmsg.text, nmsg.desc, nmsg.color);
      } else if (!strcmp(nmsg.text, "PC reset")) {
        postChannelEmbed(ch, "PC reset", nullptr, 0xED4245);
      } else if (!strncmp(nmsg.text, "door B", 6)) { // from cmdDoor: "door Bn"
        char title[32];
        snprintf(title, sizeof title, "Door button %.4s pressed", nmsg.text + 6);
        postChannelEmbed(ch, title, nullptr, 0x5865F2);
      } else {
        postChannelEmbed(ch, nmsg.text, nullptr, 0x5865F2);
      }
    }
    // #logging: every 10 s, sooner while a backlog is waiting
    if (ready && logRing && logLen &&
        nowMs() - lastLogPostMs >= (logLen > LOG_POST_MAX ? 2000u : 10000u)) {
      lastLogPostMs = nowMs();
      postLogBatch();
    }
    // #control: posted once online, refreshed every 10 min and on PC changes
    if (ready && DISCORD_CH_CONTROL[0] && (!ctlSynced || nowMs() - lastCtlSyncMs >= 10 * 60 * 1000)) {
      ctlSynced = true;
      lastCtlSyncMs = nowMs();
      controlPanelSync();
    }
    if (panelActive && panelToken[0] &&
        (nowMs() - panelTouchedMs >= ACTIONS_PANEL_TTL_MS || nowMs() - panelOpenedMs >= PANEL_TOKEN_LIFE_MS)) {
      deleteActivePanel();
    }
    sleepMs(20);
  }
}

void discordInit() {
  if (!DISCORD_TOKEN[0]) { logMsg("discord: off (no DISCORD_TOKEN in secrets.h)"); return; }
  reqDone = xSemaphoreCreateBinary();
  restMutex[0] = xSemaphoreCreateMutex();
  restMutex[1] = xSemaphoreCreateMutex();
  notifyQueue = xQueueCreateWithCaps(NOTIFY_QUEUE_DEPTH, sizeof(NotifyMsg), MALLOC_CAP_SPIRAM);
  if (!notifyQueue) notifyQueue = xQueueCreate(NOTIFY_QUEUE_DEPTH, sizeof(NotifyMsg));
  wsBuf = (char *)heap_caps_malloc(WS_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!wsBuf) wsBuf = (char *)malloc(WS_BUF_SIZE);
  if (DISCORD_CH_LOG[0]) logRing = (char *)heap_caps_malloc(LOG_RING_SIZE, MALLOC_CAP_SPIRAM);
  loadControlPanelId();
  xTaskCreatePinnedToCore(botTask, "discord", 10240, nullptr, 1, &botTaskHandle, 0);
  logMsg("discord: bot starting");
}

bool discordUp() { return ready; }
