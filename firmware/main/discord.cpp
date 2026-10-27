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
#include <math.h>
#include <nvs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "board_time.h"
#include "commands.h"
#include "config.h"
#include "aictl.h"
#include "display.h"
#include "hwctl.h"
#include "log.h"
#include "monctl.h"
#include "net.h"
#include "optos.h"
#include "pc_state.h"
#include "night.h"
#include "pcstats.h"
#include "screens.h"
#include "settings_or_example.h"
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
  PANEL_CONFIRM_REBOOT,
  PANEL_CONFIRM_AI_FORCE,
  PANEL_CONFIRM_AI_RELOAD,
  PANEL_CONFIRM_AI_GATEWAY
};

// Panel tabs; the #control panel and the /actions panel each remember theirs
enum PanelCat : uint8_t { CAT_DOOR, CAT_PC, CAT_RGB, CAT_FAN, CAT_MON, CAT_AI, CAT_BOARD, CAT_DISPLAY, CAT_COUNT };
static const char *const CAT_LABEL[CAT_COUNT] = {"Door", "PC Control", "PC Case RGB LEDs", "PC Fan Control", "PC Monitor Control",
                                                 "Local Agent Control", "ESP32 Board Control", "ESP32 Display Control"};
static const char *const CAT_ID[CAT_COUNT] = {"tab_door", "tab_pc", "tab_rgb", "tab_fan", "tab_mon", "tab_ai", "tab_board", "tab_display"};
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

// Status LED brightness in 10 % steps (saved); night mode caps it at 20 %
static void addLedSelect(cJSON *comps) {
  cJSON *sel = cJSON_CreateObject();
  cJSON_AddNumberToObject(sel, "type", 3);
  cJSON_AddStringToObject(sel, "custom_id", "led_bright");
  char ph[40];
  snprintf(ph, sizeof ph, "Status LED max: %u %%", (unsigned)ledBrightness());
  cJSON_AddStringToObject(sel, "placeholder", ph);
  cJSON *opts = cJSON_AddArrayToObject(sel, "options");
  for (int v = 10; v <= 100; v += 10) {
    cJSON *op = cJSON_CreateObject();
    char label[32], val[8];
    snprintf(label, sizeof label, "Status LED max %d %%", v);
    snprintf(val, sizeof val, "%d", v);
    cJSON_AddStringToObject(op, "label", label);
    cJSON_AddStringToObject(op, "value", val);
    if (v == ledBrightness()) cJSON_AddBoolToObject(op, "default", true);
    cJSON_AddItemToArray(opts, op);
  }
  cJSON_AddItemToArray(addRow(comps), sel);
}

// Brightness for every panel in 5 % steps (saved, as `bl all` does), plus Off
static void addBrightnessSelect(cJSON *comps) {
  cJSON *sel = cJSON_CreateObject();
  cJSON_AddNumberToObject(sel, "type", 3);
  cJSON_AddStringToObject(sel, "custom_id", "bright");
  bool same = true;
  for (int i = 1; i < N_PANELS; i++) same = same && backlightPct(i) == backlightPct(0);
  int cur = backlightForcedOff() ? 0 : same ? backlightPct(0) : -1;
  char ph[40];
  if (cur < 0) snprintf(ph, sizeof ph, "Brightness: mixed");
  else snprintf(ph, sizeof ph, "Brightness: %d %%", cur);
  cJSON_AddStringToObject(sel, "placeholder", ph);
  cJSON *opts = cJSON_AddArrayToObject(sel, "options");
  for (int v = 0; v <= 100; v += 5) {
    cJSON *op = cJSON_CreateObject();
    char label[24], val[8];
    if (v) snprintf(label, sizeof label, "Brightness %d %%", v);
    else snprintf(label, sizeof label, "Displays off");
    snprintf(val, sizeof val, "%d", v);
    cJSON_AddStringToObject(op, "label", label);
    cJSON_AddStringToObject(op, "value", val);
    if (v == cur) cJSON_AddBoolToObject(op, "default", true);
    cJSON_AddItemToArray(opts, op);
  }
  cJSON_AddItemToArray(addRow(comps), sel);
}

// RGB tab (PcHwCtl via the PC, hwctl.h). Commands go to one group, PcHwCtl's
// names: all, main (motherboard + ARGB headers) or ram. A pick here holds
// until "Auto" hands the group back to PcHwCtl's own rules.
struct RgbOpt { const char *value, *label; };
static const RgbOpt RGB_GROUPS[] = {HW_RGB_GROUPS};
static const RgbOpt RGB_MODES[] = {
  {"auto", "Auto (PcHwCtl rules)"}, {"off", "Off"}, {"static", "Static color"},
  {"breathing", "Breathing"}, {"rainbow", "Rainbow"}, {"gradient", "Gradient"}};
static const RgbOpt RGB_COLORS[] = {
  {"#ff0000", "Red"}, {"#ff6000", "Orange"}, {"#ffd000", "Yellow"}, {"#00ff00", "Green"},
  {"#00ffd0", "Cyan"}, {"#0040ff", "Blue"}, {"#8000ff", "Purple"}, {"#ff00a0", "Pink"},
  {"#ffffff", "White"}, {"#ffb070", "Warm white"}};
static const RgbOpt RGB_BRIGHT[] = {{"0", "LEDs dark (0 %)"}, {"10", "Brightness 10 %"}, {"20", "Brightness 20 %"},
  {"30", "Brightness 30 %"}, {"40", "Brightness 40 %"}, {"50", "Brightness 50 %"}, {"60", "Brightness 60 %"},
  {"70", "Brightness 70 %"}, {"80", "Brightness 80 %"}, {"90", "Brightness 90 %"}, {"100", "Brightness 100 %"}};
#define RGB_N(a) ((int)(sizeof(a) / sizeof((a)[0])))
static uint8_t rgbGroup = 0;             // index into RGB_GROUPS
static char rgbColor[8] = "#ffffff";     // last color picked here
static bool rgbBreathing = false;        // a color pick keeps breathing going

static const char *rgbColorLabel(const char *hex) {
  for (const RgbOpt &o : RGB_COLORS)
    if (!strcmp(o.value, hex)) return o.label;
  return hex;
}

static void addRgbSelect(cJSON *comps, const char *id, const char *placeholder, const RgbOpt *opts, int n) {
  cJSON *sel = cJSON_CreateObject();
  cJSON_AddNumberToObject(sel, "type", 3);
  cJSON_AddStringToObject(sel, "custom_id", id);
  cJSON_AddStringToObject(sel, "placeholder", placeholder);
  cJSON *arr = cJSON_AddArrayToObject(sel, "options");
  for (int i = 0; i < n; i++) {
    cJSON *op = cJSON_CreateObject();
    cJSON_AddStringToObject(op, "label", opts[i].label);
    cJSON_AddStringToObject(op, "value", opts[i].value);
    cJSON_AddItemToArray(arr, op);
  }
  cJSON_AddItemToArray(addRow(comps), sel);
}

// Mode, color and brightness pickers; no option marked, so picking the same
// one again re-sends it
static void addRgbRows(cJSON *comps) {
  const char *g = RGB_GROUPS[rgbGroup].label;
  char ph[64];
  snprintf(ph, sizeof ph, "Mode for %s", g);
  addRgbSelect(comps, "rgb_mode", ph, RGB_MODES, RGB_N(RGB_MODES));
  snprintf(ph, sizeof ph, "Color for %s (last: %s)", g, rgbColorLabel(rgbColor));
  addRgbSelect(comps, "rgb_color", ph, RGB_COLORS, RGB_N(RGB_COLORS));
  float b = pcStat("hw.rgb.bright");   // one setting for every group in PcHwCtl
  if (isnan(b)) snprintf(ph, sizeof ph, "Brightness (all groups)");
  else snprintf(ph, sizeof ph, "Brightness: %d %% (all groups)", (int)lroundf(b));
  addRgbSelect(comps, "rgb_bright", ph, RGB_BRIGHT, RGB_N(RGB_BRIGHT));
}

// One rgb command from a panel pick; the footer gets PcHwCtl's answer
static bool rgbCommand(const char *cmd, const char *what) {
  char reply[96];
  bool ok = hwRun(cmd, 3000, reply, sizeof reply);
  if (ok) snprintf(lastPanelResult, sizeof lastPanelResult, "%s", what);
  else snprintf(lastPanelResult, sizeof lastPanelResult, "%.56s failed: %.56s", what, !strncmp(reply, "err ", 4) ? reply + 4 : reply);
  logMsg("discord: %s -> %s", cmd, reply);
  // what changed on the PC goes to the PC channel too
  if (ok) {
    bool fan = !strncmp(cmd, "fan ", 4);
    discordPost(DC_PC, fan ? "PC Fan Control" : "PC Case RGB LEDs", what, fan ? 0x1ABC9C : 0x9B59B6);
  }
  return ok;
}

// Local Agent Control tab (LocalAIHost via the PC, aictl.h). The service's
// status, squeezed by the PC into "model=asleep ctx=- want=128k idle=20
// keep=on busy=0 down=none last=-", is fetched when the tab opens, on Refresh
// and after each action; live model state comes from the hub's ai.* keys.
static char aiStatus[121] = "";          // last status words, "" = unknown
static char aiStatusErr[64] = "";        // why the last status failed
static int aiIdleOn = LOCAL_AI_IDLE_MIN; // auto-unload minutes to restore after "off"

static void aiRefreshStatus() {
  char out[121];
  if (aiRun("status", 8000, out, sizeof out)) {
    snprintf(aiStatus, sizeof aiStatus, "%s", out[2] == ' ' ? out + 3 : "");
    aiStatusErr[0] = 0;
  } else {
    aiStatus[0] = 0;
    snprintf(aiStatusErr, sizeof aiStatusErr, "%.60s", !strncmp(out, "err ", 4) ? out + 4 : out);
  }
}

// One word of aiStatus ("idle" -> "20"); "" when missing
static const char *aiField(const char *key, char *buf, size_t size) {
  buf[0] = 0;
  size_t kl = strlen(key);
  for (const char *p = aiStatus; *p;) {
    while (*p == ' ') p++;
    const char *e = strchr(p, ' ');
    size_t wl = e ? (size_t)(e - p) : strlen(p);
    if (wl > kl && !strncmp(p, key, kl) && p[kl] == '=') {
      size_t vl = wl - kl - 1;
      if (vl >= size) vl = size - 1;
      memcpy(buf, p + kl + 1, vl);
      buf[vl] = 0;
      return buf;
    }
    p += wl;
  }
  return buf;
}

static void addAiRows(cJSON *comps, cJSON *r) {
  char idle[8], keep[8], want[8];
  aiField("idle", idle, sizeof idle);
  aiField("keep", keep, sizeof keep);
  aiField("want", want, sizeof want);
  if (atoi(idle) > 0) aiIdleOn = atoi(idle);

  addButton(r, "Load", "ai_load", 3);
  addButton(r, "Unload", "ai_unload", 2);
  addButton(r, "Force unload", "ai_force", 4);
  addButton(r, "Reload", "ai_reload", 4);
  addButton(r, "Warm cache", "ai_warm", 2);

  r = addRow(comps);
  addButton(r, "Ctx 128K", "ai_ctx_default", !strcmp(want, "128k") ? 1 : 2);
  addButton(r, "Ctx 256K", "ai_ctx_heavy", !strcmp(want, "256k") ? 1 : 2);
  char label[40];
  if (!idle[0] || !strcmp(idle, "-")) snprintf(label, sizeof label, "Auto-unload: ?");
  else if (atoi(idle) == 0) snprintf(label, sizeof label, "Auto-unload: off");
  else snprintf(label, sizeof label, "Auto-unload: %s min", idle);
  addButton(r, label, "ai_idle", 2);
  snprintf(label, sizeof label, "Keep awake: %s", keep[0] && strcmp(keep, "-") ? keep : "?");
  addButton(r, label, "ai_keep", 2);

  r = addRow(comps);
  addButton(r, "Restart gateway", "ai_gw", 4);
  addButton(r, "Restart Web UI", "ai_webui", 2);
  addButton(r, "Restart proxy", "ai_proxy", 2);
  addButton(r, "Guard check", "ai_guard", 2);
}

// Fan tab (PcHwCtl via the PC, same relay as RGB). A speed pick is an
// override for the chosen hold time (PcHwCtl needs a ttl for fans, max 1 h);
// each fan's floorPct still wins (CPU 50 %, pump 100 %). Auto drops the
// override and any curve switch. Fan keys and curve names are PcHwCtl's
// config (C:\ProgramData\PcHwCtl\pchwctl.json). "All fans" is PcHwCtl's
// `all`, the GPUs included; "Case fans" is the three case fan keys, one
// command each. The GPU fans run their own curves (RX 6600 through the
// driver's OD8 fan curve, RTX 3050 through LHM) and get a Zero RPM menu in
// place of the curve menu.
static const RgbOpt FAN_TARGETS[] = {HW_FAN_TARGETS};
static const char *const FAN_CASE_KEYS[] = {HW_FAN_CASE_KEYS};
static const RgbOpt FAN_SHOWN[] = {HW_FAN_SHOWN};   // embed rows: state.txt keys use _ for -
static const RgbOpt FAN_ZERO_RPM[] = {{"on", "Zero RPM: on (stops when cool)"}, {"off", "Zero RPM: off (always spins)"},
  {"default", "Zero RPM: config default"}};
static const RgbOpt FAN_SPEEDS[] = {{"20", "Speed 20 %"}, {"30", "Speed 30 %"}, {"40", "Speed 40 %"}, {"50", "Speed 50 %"},
  {"60", "Speed 60 %"}, {"70", "Speed 70 %"}, {"80", "Speed 80 %"}, {"90", "Speed 90 %"}, {"100", "Speed 100 %"}};
static const RgbOpt FAN_CURVES[] = {{"graph", "Curve: graph"}, {"flat", "Curve: flat"}, {"auto", "Curve: auto"}};
static const int FAN_HOLD_MIN[] = {HW_FAN_HOLD_MIN};
static uint8_t fanTarget = 0;            // index into FAN_TARGETS
static uint8_t fanHold = 1;              // index into FAN_HOLD_MIN

static void addFanRows(cJSON *comps) {
  cJSON *sel = cJSON_CreateObject();
  cJSON_AddNumberToObject(sel, "type", 3);
  cJSON_AddStringToObject(sel, "custom_id", "fan_target");
  cJSON *arr = cJSON_AddArrayToObject(sel, "options");
  for (int i = 0; i < RGB_N(FAN_TARGETS); i++) {
    cJSON *op = cJSON_CreateObject();
    char label[40];
    snprintf(label, sizeof label, "Target: %s", FAN_TARGETS[i].label);
    cJSON_AddStringToObject(op, "label", label);
    cJSON_AddStringToObject(op, "value", FAN_TARGETS[i].value);
    if (i == fanTarget) cJSON_AddBoolToObject(op, "default", true);
    cJSON_AddItemToArray(arr, op);
  }
  cJSON_AddItemToArray(addRow(comps), sel);

  char ph[64];
  snprintf(ph, sizeof ph, "Speed for %s (hold %d min)", FAN_TARGETS[fanTarget].label, FAN_HOLD_MIN[fanHold]);
  addRgbSelect(comps, "fan_pct", ph, FAN_SPEEDS, RGB_N(FAN_SPEEDS));
  const char *t = FAN_TARGETS[fanTarget].value;
  if (!strncmp(t, "gpu-", 4)) {
    snprintf(ph, sizeof ph, "Zero RPM for %s", FAN_TARGETS[fanTarget].label);
    addRgbSelect(comps, "fan_zr", ph, FAN_ZERO_RPM, RGB_N(FAN_ZERO_RPM));
  } else {
    snprintf(ph, sizeof ph, "Curve for %s", FAN_TARGETS[fanTarget].label);
    addRgbSelect(comps, "fan_curve", ph, FAN_CURVES, RGB_N(FAN_CURVES));
  }
}

// PC Monitor Control tab (DDC/CI through the PC's user-session half,
// monctl.h). The monitors and their state come from the mon.* stats,
// numbered from the left; picks go to one monitor or to all of them.
static const RgbOpt MON_BRIGHT[] = {{"0", "Brightness 0 %"}, {"10", "Brightness 10 %"}, {"20", "Brightness 20 %"},
  {"30", "Brightness 30 %"}, {"40", "Brightness 40 %"}, {"50", "Brightness 50 %"}, {"60", "Brightness 60 %"},
  {"70", "Brightness 70 %"}, {"80", "Brightness 80 %"}, {"90", "Brightness 90 %"}, {"100", "Brightness 100 %"}};
static uint8_t monTarget = 0;            // 0 = all monitors, else the monitor's number

static int monCount() {
  float n = pcStat("mon.count");
  if (isnan(n) || n < 0) return 0;
  return n > 24 ? 24 : (int)n;           // a select menu holds 25 options, one is "All"
}

// "2 MSI PRO MP275 (main)"
static void monLabel(int n, char *buf, size_t size) {
  char k[24];
  snprintf(k, sizeof k, "mon.%d.name", n);
  const char *name = pcText(k);
  snprintf(k, sizeof k, "mon.%d.main", n);
  snprintf(buf, size, "%d %.40s%s", n, *name ? name : "Monitor", pcStat(k) >= 1 ? " (main)" : "");
}

static void monTargetLabel(char *buf, size_t size) {
  if (!monTarget || monTarget > monCount()) snprintf(buf, size, "All monitors");
  else monLabel(monTarget, buf, size);
}

static void addMonRows(cJSON *comps) {
  int n = monCount();
  if (monTarget > n) monTarget = 0;
  cJSON *sel = cJSON_CreateObject();
  cJSON_AddNumberToObject(sel, "type", 3);
  cJSON_AddStringToObject(sel, "custom_id", "mon_target");
  cJSON *arr = cJSON_AddArrayToObject(sel, "options");
  for (int i = 0; i <= n; i++) {
    cJSON *op = cJSON_CreateObject();
    char name[64], label[80], value[12];
    if (i) monLabel(i, name, sizeof name);
    else snprintf(name, sizeof name, "All monitors");
    snprintf(label, sizeof label, "Target: %s", name);
    if (i) snprintf(value, sizeof value, "%d", i);
    else snprintf(value, sizeof value, "all");
    cJSON_AddStringToObject(op, "label", label);
    cJSON_AddStringToObject(op, "value", value);
    if (i == monTarget) cJSON_AddBoolToObject(op, "default", true);
    cJSON_AddItemToArray(arr, op);
  }
  cJSON_AddItemToArray(addRow(comps), sel);

  char t[64], ph[88];
  monTargetLabel(t, sizeof t);
  snprintf(ph, sizeof ph, "Brightness for %s", t);
  addRgbSelect(comps, "mon_bright", ph, MON_BRIGHT, RGB_N(MON_BRIGHT));
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
    nightStatus(v, sizeof v);
    addField(fields, "Night mode", v);
  } else if (cat == CAT_RGB) {
    const char *mainRule = pcText("hw.rgb.main"), *ramRule = pcText("hw.rgb.ram");
    addField(fields, "PcHwCtl", pcStat("hw.ok") >= 1 ? "state feed live" : "no state (not running?)");
    addField(fields, "Motherboard + ARGB", *mainRule ? mainRule : "?");
    addField(fields, "RAM", *ramRule ? ramRule : "?");
    float b = pcStat("hw.rgb.bright");
    if (isnan(b)) snprintf(v, sizeof v, "?");
    else snprintf(v, sizeof v, "%d %%", (int)lroundf(b));
    addField(fields, "Brightness", v);
    addField(fields, "Panel targets", RGB_GROUPS[rgbGroup].label);
  } else if (cat == CAT_FAN) {
    addField(fields, "PcHwCtl", pcStat("hw.ok") >= 1 ? "state feed live" : "no state (not running?)");
    for (const RgbOpt &f : FAN_SHOWN) {
      char k[32];
      snprintf(k, sizeof k, "hw.%s.pct", f.value);
      float pct = pcStat(k);
      snprintf(k, sizeof k, "hw.%s.rpm", f.value);
      float rpm = pcStat(k);
      snprintf(k, sizeof k, "hw.%s.temp", f.value);
      float temp = pcStat(k);
      snprintf(k, sizeof k, "hw.%s.ovr", f.value);
      bool ovr = pcStat(k) >= 1;   // a speed override from here or another client
      snprintf(k, sizeof k, "hw.%s.park", f.value);
      bool parked = pcStat(k) >= 1;   // GPU Zero RPM: stopped right now
      snprintf(k, sizeof k, "hw.%s.zr", f.value);
      float zr = pcStat(k);           // GPU Zero RPM setting, NaN for case fans
      if (isnan(pct)) snprintf(v, sizeof v, "?");
      else if (parked) snprintf(v, sizeof v, "stopped (Zero RPM), %.0f C", isnan(temp) ? 0.0f : temp);
      else snprintf(v, sizeof v, "%.0f %%%s, %.0f rpm, %.0f C%s", pct, ovr ? " (held)" : "", isnan(rpm) ? 0.0f : rpm,
                    isnan(temp) ? 0.0f : temp, isnan(zr) ? "" : zr >= 1 ? ", ZR on" : ", ZR off");
      addField(fields, f.label, v);
    }
    float mix = pcStat("hw.mix");
    if (!isnan(mix)) { snprintf(v, sizeof v, "%.0f C", mix); addField(fields, "Mix temp", v); }
    snprintf(v, sizeof v, "%s, hold %d min", FAN_TARGETS[fanTarget].label, FAN_HOLD_MIN[fanHold]);
    addField(fields, "Panel targets", v);
  } else if (cat == CAT_MON) {
    int n = monCount();
    if (!n) addField(fields, "Monitors", pcStat("mon.count") == 0 ? "none answer DDC/CI" : "no data (nobody signed in?)");
    for (int i = 1; i <= n; i++) {
      char name[64], k[24];
      monLabel(i, name, sizeof name);
      snprintf(k, sizeof k, "mon.%d.power", i);
      float power = pcStat(k);
      snprintf(k, sizeof k, "mon.%d.bright", i);
      float bright = pcStat(k);
      if (isnan(power) || power < 0) snprintf(v, sizeof v, "no DDC answer");
      else if (power < 1) snprintf(v, sizeof v, "off");
      else if (isnan(bright) || bright < 0) snprintf(v, sizeof v, "on");
      else snprintf(v, sizeof v, "on, %.0f %%", bright);
      addField(fields, name, v);
    }
    char t[64];
    monTargetLabel(t, sizeof t);
    addField(fields, "Panel targets", t);
  } else if (cat == CAT_AI) {
    char w1[32], w2[32];
    const char *model = pcText("ai.model_name");
    if (!*model) model = aiField("model", w1, sizeof w1);
    addField(fields, "Model", *model ? model : "?");
    const char *phase = pcText("ai.phase_name");
    addField(fields, "Phase", *phase ? phase : "?");
    aiField("ctx", w1, sizeof w1);
    aiField("want", w2, sizeof w2);
    if (!w2[0]) snprintf(v, sizeof v, "?");
    else if (!strcmp(w1, "-")) snprintf(v, sizeof v, "next load %s", w2);
    else snprintf(v, sizeof v, "%.12s (next load %.12s)", w1, w2);
    addField(fields, "Context", v);
    aiField("idle", w1, sizeof w1);
    double unloadIn = pcStat("ai.unload_in_s");
    if (!w1[0] || !strcmp(w1, "-")) snprintf(v, sizeof v, "?");
    else if (atoi(w1) == 0) snprintf(v, sizeof v, "off");
    else if (!isnan(unloadIn) && unloadIn > 0)
      snprintf(v, sizeof v, "after %s min (in %d:%02d)", w1, (int)unloadIn / 60, (int)unloadIn % 60);
    else snprintf(v, sizeof v, "after %s min idle", w1);
    addField(fields, "Auto-unload", v);
    addField(fields, "Keep awake", aiField("keep", w1, sizeof w1)[0] ? w1 : "?");
    aiField("down", w1, sizeof w1);
    if (!aiStatus[0]) snprintf(v, sizeof v, "unknown: %.50s", aiStatusErr[0] ? aiStatusErr : "no status yet");
    else if (!strcmp(w1, "none")) snprintf(v, sizeof v, "all running");
    else snprintf(v, sizeof v, "down: %s", w1);
    addField(fields, "Services", v);
    aiField("last", w1, sizeof w1);
    if (w1[0] && strcmp(w1, "-")) addField(fields, "Last action", w1);
    double vram = pcStat("ai.vram_mb"), temp = pcStat("ai.gpu_temp");
    if (!isnan(vram)) {
      if (!isnan(temp)) snprintf(v, sizeof v, "%.0f MB, %.0f C", vram, temp);
      else snprintf(v, sizeof v, "%.0f MB", vram);
      addField(fields, "VRAM", v);
    }
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
    if (ledMode() == LED_OFF) snprintf(v, sizeof v, "off");
    else if (nightEngaged()) snprintf(v, sizeof v, "on, max %u %% (night: %d %%)", (unsigned)ledBrightness(), NIGHT_STATUS_LED_PCT);
    else snprintf(v, sizeof v, "on, max %u %%", (unsigned)ledBrightness());
    addField(fields, "Status LED", v);
  } else {
    for (int i = 0; i < N_PANELS; i++) {
      char name[12];
      snprintf(name, sizeof name, "Panel %d", i + 1);
      snprintf(v, sizeof v, "%s, %u %%", screensForce[i] == FACE_NONE ? "auto" : faceLabel(screensForce[i]), backlightPct(i));
      addField(fields, name, v);
    }
    char nightDisp[40];
    snprintf(nightDisp, sizeof nightDisp, "night mode (%d %%, PC-off screens)", NIGHT_PANEL_PCT);
    addField(fields, "Displays", backlightForcedOff() ? "off" : backlightIsIdle() ? "dark (PC asleep)" :
                                 nightEngaged() ? nightDisp : "on");
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
    case PANEL_CONFIRM_AI_FORCE: confirmRow(comps, "Yes, force unload", "ai_force_confirm", 4); return;
    case PANEL_CONFIRM_AI_RELOAD: confirmRow(comps, "Yes, reload", "ai_reload_confirm", 4); return;
    case PANEL_CONFIRM_AI_GATEWAY: confirmRow(comps, "Yes, restart the gateway", "ai_gw_confirm", 4); return;
    case PANEL_NORMAL: break;
  }

  // row 1: the category dropdown; the tab's buttons follow, Refresh and
  // Close (the /actions panel only) at the end
  uint8_t cat = panelCat(standing);
  cJSON *sel = cJSON_CreateObject();
  cJSON_AddNumberToObject(sel, "type", 3);
  cJSON_AddStringToObject(sel, "custom_id", "cat");
  cJSON *opts = cJSON_AddArrayToObject(sel, "options");
  for (int i = 0; i < CAT_COUNT; i++) {
    cJSON *op = cJSON_CreateObject();
    cJSON_AddStringToObject(op, "label", CAT_LABEL[i]);
    cJSON_AddStringToObject(op, "value", CAT_ID[i]);
    if (i == cat) cJSON_AddBoolToObject(op, "default", true);
    cJSON_AddItemToArray(opts, op);
  }
  cJSON_AddItemToArray(addRow(comps), sel);

  cJSON *r = addRow(comps);
  if (cat == CAT_DOOR) {
    addButton(r, "Door 1", "door_1", 1);
    addButton(r, "Door 2", "door_2", 1);
    addButton(r, "Door 3", "door_3", 1);
    addButton(r, "Door 4", "door_4", 1);
    r = addRow(comps);
  } else if (cat == CAT_PC) {
    addButton(r, "Wake PC", "wake", 3);
    addButton(r, "Lock", "lock", 1);
    addButton(r, "Sleep", "sleep", 2);
    addButton(r, "Hibernate", "hibernate", 2);
    r = addRow(comps);
    addButton(r, "Restart", "restart", 4);
    addButton(r, "Shut down", "shutdown", 4);
    addButton(r, "PC reset", "reset", 4);
    r = addRow(comps);
    addButton(r, "Screenshot", "shot", 2);   // Refresh/Close share this row
    addButton(r, nightEnabled() ? "Night auto: on" : "Night auto: off", "night", nightEnabled() ? 1 : 2);
    addButton(r, nightEngaged() ? "End night now" : "Start night now", "night_now", nightEngaged() ? 3 : 2);
  } else if (cat == CAT_RGB) {
    // 5 rows: tabs, mode, color, brightness, then target group + Refresh/Close
    cJSON_DeleteItemFromArray(comps, cJSON_GetArraySize(comps) - 1);
    addRgbRows(comps);
    r = addRow(comps);
    for (int i = 0; i < RGB_N(RGB_GROUPS); i++) {
      char bid[16];
      snprintf(bid, sizeof bid, "rgbg_%s", RGB_GROUPS[i].value);
      addButton(r, RGB_GROUPS[i].label, bid, i == rgbGroup ? 1 : 2);
    }
  } else if (cat == CAT_FAN) {
    // 5 rows: tabs, target, speed, curve, then Hold/Auto + Refresh/Close
    cJSON_DeleteItemFromArray(comps, cJSON_GetArraySize(comps) - 1);
    addFanRows(comps);
    r = addRow(comps);
    char label[24];
    snprintf(label, sizeof label, "Hold: %d min", FAN_HOLD_MIN[fanHold]);
    addButton(r, label, "fan_hold", 2);
    addButton(r, "Auto (release)", "fan_release", 1);
  } else if (cat == CAT_MON) {
    // 4 rows: tabs, target, brightness, then Screen on/off + Refresh/Close
    cJSON_DeleteItemFromArray(comps, cJSON_GetArraySize(comps) - 1);
    addMonRows(comps);
    r = addRow(comps);
    addButton(r, "All screens on", "mon_on", 3);
    addButton(r, "All screens off", "mon_off", 4);
  } else if (cat == CAT_AI) {
    // 5 rows: tabs, model, context/idle, services, Refresh/Close
    addAiRows(comps, r);
    r = addRow(comps);
  } else if (cat == CAT_BOARD) {
    // 4 rows: tabs, status LED brightness, LED on/off, board buttons + Refresh/Close
    cJSON_DeleteItemFromArray(comps, cJSON_GetArraySize(comps) - 1);
    addLedSelect(comps);
    r = addRow(comps);
    bool ledOff = ledMode() == LED_OFF;
    addButton(r, ledOff ? "Status LED: off" : "Status LED: on", "led_toggle", ledOff ? 2 : 1);
    r = addRow(comps);
    addButton(r, "Restart board", "reboot", 4);
    addButton(r, "Refresh weather", "wx_refresh", 2);
    addButton(r, "Reconnect Discord", "dc_reconnect", 2);
  } else {
    // 5 rows max, all dropdowns: tabs, 3 screen pickers, brightness. Refresh
    // sits in the tab dropdown instead; the /actions panel closes by itself
    cJSON_DeleteItemFromArray(comps, cJSON_GetArraySize(comps) - 1);
    cJSON *op = cJSON_CreateObject();
    cJSON_AddStringToObject(op, "label", "Refresh");
    cJSON_AddStringToObject(op, "value", "refresh");
    cJSON_AddItemToArray(opts, op);
    for (int i = 0; i < N_PANELS; i++) addFaceSelect(comps, i);
    addBrightnessSelect(comps);
    return;
  }
  addButton(r, "Refresh", "refresh", 2);
  if (!standing) addButton(r, "Close", "close", 2);
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

// A panel action only waits for the PC to accept the command; the state it
// changes reaches the board 1-3 s later (PcHwCtl's state file, then the PC's
// next poll and tick), so the panel is drawn once more a few seconds after.
// One pending redraw: a newer click replaces it. Uses the click's token, which
// can edit the message for 15 minutes (standing panel included).
static struct {
  bool pending, standing;
  uint32_t atMs;
  char token[256];
} panelLater;

static void panelRefreshLater(bool standing, const char *token) {
  if (!DISCORD_APP_ID[0] || !token || !token[0]) return;
  panelLater.pending = true;
  panelLater.standing = standing;
  panelLater.atMs = nowMs() + 3000;
  snprintf(panelLater.token, sizeof panelLater.token, "%s", token);
}

static void aiRefreshStatus();
static void panelLaterService() {
  if (!panelLater.pending || (int32_t)(nowMs() - panelLater.atMs) < 0) return;
  panelLater.pending = false;
  if (panelCat(panelLater.standing) == CAT_AI) aiRefreshStatus();
  sendPanelResponse("", panelLater.token, 7, panelLater.standing, PANEL_NORMAL, lastPanelResult);
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

// One LocalAIHost command from a tab button; the footer gets its answer
static void aiAction(const char *cmd, const char *what) {
  char reply[121];
  bool ok = aiRun(cmd, 12000, reply, sizeof reply);
  if (ok) snprintf(lastPanelResult, sizeof lastPanelResult, "%.80s%s", what, strstr(reply, "started") ? " (started)" : "");
  else snprintf(lastPanelResult, sizeof lastPanelResult, "%.50s failed: %.60s", what, !strncmp(reply, "err ", 4) ? reply + 4 : reply);
  logMsg("discord: ai %s -> %s", cmd, reply);
  if (ok) discordPost(DC_PC, "Local Agent Control", lastPanelResult, 0x5865F2);
}

// Local Agent Control buttons; true when cid was one of them
static bool aiButton(const char *cid, const char *id, const char *token, bool standing) {
  if (strncmp(cid, "ai_", 3)) return false;
  struct Confirm { const char *cid; PanelView view; const char *ask; };
  static const Confirm CONFIRMS[] = {
    {"ai_force", PANEL_CONFIRM_AI_FORCE, "Force unload the model? A running request is cut off."},
    {"ai_reload", PANEL_CONFIRM_AI_RELOAD, "Reload the model? A running request is cut off."},
    {"ai_gw", PANEL_CONFIRM_AI_GATEWAY, "Restart the OpenClaw gateway? It kills an active run."}};
  for (const Confirm &c : CONFIRMS)
    if (!strcmp(cid, c.cid)) { sendPanelResponse(id, token, 7, standing, c.view, c.ask); return true; }

  struct Act { const char *cid, *cmd, *what; };
  static const Act ACTS[] = {
    {"ai_load", "model load", "Model load"},
    {"ai_unload", "model unload", "Model unload"},
    {"ai_force_confirm", "model unload force", "Model force unload"},
    {"ai_reload_confirm", "model reload", "Model reload"},
    {"ai_warm", "gateway warm", "Gateway cache warm-up"},
    {"ai_ctx_default", "model ctx default", "Context 128K"},
    {"ai_ctx_heavy", "model ctx heavy", "Context 256K"},
    {"ai_gw_confirm", "restart gateway", "Gateway restart"},
    {"ai_webui", "restart webui", "Web UI restart"},
    {"ai_proxy", "restart wake-proxy", "Wake proxy restart"},
    {"ai_guard", "guard check", "Guard check"}};
  char cmd[40], what[48], w[8];
  cmd[0] = 0;
  for (const Act &a : ACTS)
    if (!strcmp(cid, a.cid)) { snprintf(cmd, sizeof cmd, "%s", a.cmd); snprintf(what, sizeof what, "%s", a.what); }
  if (!strcmp(cid, "ai_idle")) {
    bool off = atoi(aiField("idle", w, sizeof w)) == 0 && w[0] && strcmp(w, "-");
    if (off) snprintf(cmd, sizeof cmd, "model idle %d", aiIdleOn);
    else snprintf(cmd, sizeof cmd, "model idle 0");
    if (off) snprintf(what, sizeof what, "Auto-unload after %d min", aiIdleOn);
    else snprintf(what, sizeof what, "Auto-unload off");
  } else if (!strcmp(cid, "ai_keep")) {
    bool on = !strcmp(aiField("keep", w, sizeof w), "on");
    snprintf(cmd, sizeof cmd, "model keepawake %s", on ? "off" : "on");
    snprintf(what, sizeof what, "Keep awake %s", on ? "off" : "on");
  }
  if (!cmd[0]) {
    snprintf(lastPanelResult, sizeof lastPanelResult, "Unknown button");
  } else {
    aiAction(cmd, what);
    panelRefreshLater(standing, token);   // status again once the service moved
  }
  sendPanelResponse(id, token, 7, standing, PANEL_NORMAL, lastPanelResult);
  return true;
}

// Fan tab selects and buttons; true when cid was one of them
static bool fanInteraction(const char *cid, cJSON *jVal, const char *id, const char *token, bool standing) {
  if (strncmp(cid, "fan_", 4)) return false;
  const char *val = jVal && cJSON_IsString(jVal) ? jVal->valuestring : "";
  const char *f = FAN_TARGETS[fanTarget].value, *label = FAN_TARGETS[fanTarget].label;
  char cmd[64] = "", what[64];
  if (!strcmp(cid, "fan_target")) {
    for (int i = 0; i < RGB_N(FAN_TARGETS); i++)
      if (!strcmp(val, FAN_TARGETS[i].value)) fanTarget = (uint8_t)i;
    snprintf(lastPanelResult, sizeof lastPanelResult, "Fan picks now go to %s", FAN_TARGETS[fanTarget].label);
  } else if (!strcmp(cid, "fan_hold")) {
    fanHold = (uint8_t)((fanHold + 1) % RGB_N(FAN_HOLD_MIN));
    snprintf(lastPanelResult, sizeof lastPanelResult, "Speed picks now hold %d min", FAN_HOLD_MIN[fanHold]);
  } else if (!strcmp(cid, "fan_pct")) {
    int pct = atoi(val);
    if (pct < 20) pct = 20;
    if (pct > 100) pct = 100;
    snprintf(cmd, sizeof cmd, "fan set %s %d %d", f, pct, FAN_HOLD_MIN[fanHold] * 60);
    snprintf(what, sizeof what, "%s: %d %% for %d min", label, pct, FAN_HOLD_MIN[fanHold]);
  } else if (!strcmp(cid, "fan_curve")) {
    for (const RgbOpt &c : FAN_CURVES)
      if (!strcmp(val, c.value)) {
        snprintf(cmd, sizeof cmd, "fan curve %s %s", f, c.value);
        snprintf(what, sizeof what, "%s: curve %s", label, c.value);
      }
  } else if (!strcmp(cid, "fan_zr")) {
    for (const RgbOpt &z : FAN_ZERO_RPM)
      if (!strcmp(val, z.value) && !strncmp(f, "gpu-", 4)) {
        snprintf(cmd, sizeof cmd, "fan zerorpm %s %s", f, z.value);
        snprintf(what, sizeof what, "%s: Zero RPM %s", label, z.value);
      }
  } else if (!strcmp(cid, "fan_release")) {
    snprintf(cmd, sizeof cmd, "fan release %s", f);
    snprintf(what, sizeof what, "%s back to PcHwCtl curves", label);
  }
  if (cmd[0] && !strcmp(f, "case")) {
    // not a PcHwCtl key: the same command for each case fan, one post
    const char *at = strstr(cmd, " case");   // "fan set case 40 900" -> "fan set " + key + " 40 900"
    bool all = true;
    char one[64], reply[96];
    for (const char *key : FAN_CASE_KEYS) {
      snprintf(one, sizeof one, "%.*s %s%s", (int)(at - cmd), cmd, key, at + 5);
      bool ok = hwRun(one, 3000, reply, sizeof reply);
      logMsg("discord: %s -> %s", one, reply);
      if (!ok) {
        snprintf(lastPanelResult, sizeof lastPanelResult, "%.56s failed: %.56s", what, !strncmp(reply, "err ", 4) ? reply + 4 : reply);
        all = false;
        break;
      }
    }
    if (all) {
      snprintf(lastPanelResult, sizeof lastPanelResult, "%.100s", what);
      discordPost(DC_PC, "PC Fan Control", what, 0x1ABC9C);
      panelRefreshLater(standing, token);
    }
  } else if (cmd[0] && rgbCommand(cmd, what)) panelRefreshLater(standing, token);
  sendPanelResponse(id, token, 7, standing, PANEL_NORMAL, lastPanelResult);
  return true;
}

// One monitor command from a panel pick; the footer gets the PC's answer
static bool monCommand(const char *cmd, const char *what) {
  char reply[96];
  bool ok = monRun(cmd, 5000, reply, sizeof reply);
  if (ok) snprintf(lastPanelResult, sizeof lastPanelResult, "%.100s", what);
  else snprintf(lastPanelResult, sizeof lastPanelResult, "%.56s failed: %.56s", what, !strncmp(reply, "err ", 4) ? reply + 4 : reply);
  logMsg("discord: mon %s -> %s", cmd, reply);
  if (ok) discordPost(DC_PC, "PC Monitor Control", what, 0x3498DB);
  return ok;
}

// Monitor tab selects and buttons; true when cid was one of them
static bool monInteraction(const char *cid, cJSON *jVal, const char *id, const char *token, bool standing) {
  if (strncmp(cid, "mon_", 4)) return false;
  const char *val = jVal && cJSON_IsString(jVal) ? jVal->valuestring : "";
  char t[64], target[12], cmd[48] = "", what[96];
  monTargetLabel(t, sizeof t);
  if (monTarget && monTarget <= monCount()) snprintf(target, sizeof target, "%d", monTarget);
  else snprintf(target, sizeof target, "all");
  if (!strcmp(cid, "mon_target")) {
    monTarget = !strcmp(val, "all") ? 0 : (uint8_t)atoi(val);
    if (monTarget > monCount()) monTarget = 0;
    monTargetLabel(t, sizeof t);
    snprintf(lastPanelResult, sizeof lastPanelResult, "Monitor picks now go to %s", t);
  } else if (!strcmp(cid, "mon_bright")) {
    int pct = atoi(val);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    snprintf(cmd, sizeof cmd, "bright %s %d", target, pct);
    snprintf(what, sizeof what, "%s: brightness %d %%", t, pct);
  } else if (!strcmp(cid, "mon_on") || !strcmp(cid, "mon_off")) {
    bool on = !strcmp(cid, "mon_on");
    // Windows' own display off: every display at once, any input wakes them
    snprintf(cmd, sizeof cmd, "power all %s", on ? "on" : "off");
    snprintf(what, sizeof what, "All screens %s", on ? "on" : "off");
  }
  if (cmd[0] && monCommand(cmd, what)) panelRefreshLater(standing, token);
  sendPanelResponse(id, token, 7, standing, PANEL_NORMAL, lastPanelResult);
  return true;
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
      // deferred by ackInteraction when the app id is known: edit it, else answer
      sendPanelResponse(id, token, DISCORD_APP_ID[0] ? 7 : 4, false, PANEL_NORMAL, nullptr);
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

    if (!standing) panelTouchedMs = nowMs();   // acknowledged already (ackInteraction)

    cJSON *jVals = data ? cJSON_GetObjectItem(data, "values") : nullptr;
    cJSON *jVal = (jVals && cJSON_IsArray(jVals)) ? cJSON_GetArrayItem(jVals, 0) : nullptr;
    if (jVal && cJSON_IsString(jVal) && !strcmp(cid, "led_bright")) {
      int v = atoi(jVal->valuestring);
      if (v < 10) v = 10;
      if (v > 100) v = 100;
      ledBrightnessSet((uint8_t)v);
      ledSettingsSave();
      snprintf(lastPanelResult, sizeof lastPanelResult, "Status LED max %d %%", v);
      sendPanelResponse(id, token, 7, standing, PANEL_NORMAL, lastPanelResult);
      return;
    }
    if (jVal && cJSON_IsString(jVal) && !strncmp(cid, "face_", 5)) {
      // Display tab: a screen picked for one panel (not saved, `face` command)
      char cmd[32];
      snprintf(cmd, sizeof cmd, "face %s %s", cid + 5, jVal->valuestring);
      runAndGetFirstLine(cmd, lastPanelResult, sizeof lastPanelResult);
      sendPanelResponse(id, token, 7, standing, PANEL_NORMAL, lastPanelResult);
      return;
    }
    if (jVal && cJSON_IsString(jVal) && !strcmp(cid, "bright")) {
      int v = atoi(jVal->valuestring);
      char ignore[64];
      if (v <= 0) {
        runAndGetFirstLine("display off", lastPanelResult, sizeof lastPanelResult);
      } else {
        char cmd[24];
        snprintf(cmd, sizeof cmd, "bl all %d", v);
        runAndGetFirstLine("display on", ignore, sizeof ignore);
        runAndGetFirstLine(cmd, ignore, sizeof ignore);
        snprintf(lastPanelResult, sizeof lastPanelResult, "Brightness %d %% on every panel", v);
      }
      sendPanelResponse(id, token, 7, standing, PANEL_NORMAL, lastPanelResult);
      return;
    }
    if (fanInteraction(cid, jVal, id, token, standing)) return;
    if (monInteraction(cid, jVal, id, token, standing)) return;
    if (jVal && cJSON_IsString(jVal) && !strncmp(cid, "rgb_", 4)) {
      // RGB tab picks: the footer says whether PcHwCtl took it; the new state
      // shows on the redraw a few seconds later
      const char *val = jVal->valuestring, *g = RGB_GROUPS[rgbGroup].value;
      char cmd[64] = "", what[64];
      if (!strcmp(cid, "rgb_mode")) {
        if (!strcmp(val, "auto")) {
          snprintf(cmd, sizeof cmd, "rgb release %s", g);
          snprintf(what, sizeof what, "RGB %s back to PcHwCtl rules", g);
        } else if (!strcmp(val, "static") || !strcmp(val, "breathing")) {
          snprintf(cmd, sizeof cmd, "rgb mode %s %s %s", g, val, rgbColor);
          snprintf(what, sizeof what, "RGB %s: %s %s", g, val, rgbColorLabel(rgbColor));
        } else {
          snprintf(cmd, sizeof cmd, "rgb mode %s %.12s", g, val);
          snprintf(what, sizeof what, "RGB %s: %.12s", g, val);
        }
        rgbBreathing = !strcmp(val, "breathing");
      } else if (!strcmp(cid, "rgb_color") && val[0] == '#' && strlen(val) == 7) {
        snprintf(rgbColor, sizeof rgbColor, "%s", val);
        const char *mode = rgbBreathing ? "breathing" : "static";
        snprintf(cmd, sizeof cmd, "rgb mode %s %s %s", g, mode, rgbColor);
        snprintf(what, sizeof what, "RGB %s: %s %s", g, mode, rgbColorLabel(rgbColor));
      } else if (!strcmp(cid, "rgb_bright")) {
        int bright = atoi(val);
        if (bright < 0) bright = 0;
        if (bright > 100) bright = 100;
        snprintf(cmd, sizeof cmd, "rgb brightness %d", bright);
        snprintf(what, sizeof what, "RGB brightness %d %%", bright);
      }
      if (cmd[0] && rgbCommand(cmd, what)) panelRefreshLater(standing, token);
      sendPanelResponse(id, token, 7, standing, PANEL_NORMAL, lastPanelResult);
      return;
    }
    if (jVal && cJSON_IsString(jVal) && !strcmp(cid, "cat") && !strcmp(jVal->valuestring, "refresh")) {
      snprintf(lastPanelResult, sizeof lastPanelResult, "Refreshed");
      sendPanelResponse(id, token, 7, standing, PANEL_NORMAL, "Refreshed");
      return;
    }
    for (int i = 0; jVal && cJSON_IsString(jVal) && !strcmp(cid, "cat") && i < CAT_COUNT; i++) {
      if (!strcmp(jVal->valuestring, CAT_ID[i])) {
        panelCat(standing) = (uint8_t)i;
        lastPanelResult[0] = 0;
        if (i == CAT_AI) aiRefreshStatus();
        sendPanelResponse(id, token, 7, standing, PANEL_NORMAL, nullptr);
        return;
      }
    }

    if (aiButton(cid, id, token, standing)) {
      // handled
    } else if (!strncmp(cid, "rgbg_", 5)) {
      for (int i = 0; i < RGB_N(RGB_GROUPS); i++)
        if (!strcmp(cid + 5, RGB_GROUPS[i].value)) rgbGroup = (uint8_t)i;
      snprintf(lastPanelResult, sizeof lastPanelResult, "RGB picks now go to %s", RGB_GROUPS[rgbGroup].label);
      sendPanelResponse(id, token, 7, standing, PANEL_NORMAL, lastPanelResult);
    } else if (!strcmp(cid, "refresh")) {
      if (panelCat(standing) == CAT_AI) aiRefreshStatus();
      snprintf(lastPanelResult, sizeof lastPanelResult, "Refreshed");
      sendPanelResponse(id, token, 7, standing, PANEL_NORMAL, "Refreshed");
    } else if (!strcmp(cid, "lock")) {
      runAndGetFirstLine("power lock", lastPanelResult, sizeof lastPanelResult);
      if (!lastPanelResult[0]) snprintf(lastPanelResult, sizeof lastPanelResult, "Lock sent to the PC");
      sendPanelResponse(id, token, 7, standing, PANEL_NORMAL, lastPanelResult);
    } else if (!strcmp(cid, "night")) {
      nightSetEnabled(!nightEnabled());
      char win[40];
      nightWindowText(win, sizeof win);
      if (nightEnabled()) snprintf(lastPanelResult, sizeof lastPanelResult, "Night mode on: %s without input", win);
      else snprintf(lastPanelResult, sizeof lastPanelResult, "Night mode off");
      sendPanelResponse(id, token, 7, standing, PANEL_NORMAL, lastPanelResult);
    } else if (!strcmp(cid, "led_toggle")) {
      bool off = ledMode() != LED_OFF;
      ledModeSet(off ? LED_OFF : LED_AUTO);
      ledSettingsSave();
      snprintf(lastPanelResult, sizeof lastPanelResult, "Status LED %s", off ? "off" : "on");
      sendPanelResponse(id, token, 7, standing, PANEL_NORMAL, lastPanelResult);
    } else if (!strcmp(cid, "night_now")) {
      bool start = !nightEngaged();
      nightManual(start);
      snprintf(lastPanelResult, sizeof lastPanelResult, "%s",
               start ? "Night mode started: input on the PC or End ends it" : "Night mode ended");
      sendPanelResponse(id, token, 7, standing, PANEL_NORMAL, lastPanelResult);
    } else if (!strcmp(cid, "shot")) {
      runAndGetFirstLine("screenshot", lastPanelResult, sizeof lastPanelResult);
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

static void heartbeatService() {
  if (!heartbeatMs || nowMs() - lastBeat < heartbeatMs) return;
  if (!gotAck) {
    logMsg("discord: no heartbeat ACK, reconnecting");
    if (ws) esp_websocket_client_close(ws, pdMS_TO_TICKS(1000));
  } else {
    heartbeat();
  }
}

// ---- interaction queue ----
// Gateway events arrive in the websocket task, which only copies each
// interaction into ixQueue. The bot task acknowledges it at once (Discord's
// 3 s deadline), then handles it; while a handler waits on the PC it keeps
// acknowledging new ones and sending heartbeats (discordWaitTick). So the
// websocket is never held up by a slow command, and every REST call runs in
// the bot task on its one TLS client.
#define IX_QUEUE_DEPTH 6
static QueueHandle_t ixQueue;             // char * JSON copies in PSRAM
struct IxPending { char *json; bool handle; };
static IxPending ixPending[IX_QUEUE_DEPTH];
static int ixPendingCount;

static void ixEnqueue(cJSON *d) {
  char *text = cJSON_PrintUnformatted(d);
  if (!text) return;
  size_t n = strlen(text) + 1;
  char *copy = (char *)heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
  if (copy) memcpy(copy, text, n);
  free(text);
  if (!copy || !ixQueue || xQueueSend(ixQueue, &copy, 0) != pdTRUE) {
    logMsg("discord: interaction dropped (queue full or no memory)");
    heap_caps_free(copy);
  }
}

static const char *ixUser(cJSON *d) {
  cJSON *member = cJSON_GetObjectItem(d, "member");
  cJSON *u = member ? cJSON_GetObjectItem(member, "user") : cJSON_GetObjectItem(d, "user");
  cJSON *uId = u ? cJSON_GetObjectItem(u, "id") : nullptr;
  return uId && cJSON_IsString(uId) ? uId->valuestring : "";
}

// The 3 s answer; false when that answer was the whole reply
static bool ackInteraction(cJSON *d) {
  cJSON *jId = cJSON_GetObjectItem(d, "id"), *jToken = cJSON_GetObjectItem(d, "token");
  const char *id = jId && cJSON_IsString(jId) ? jId->valuestring : "";
  const char *token = jToken && cJSON_IsString(jToken) ? jToken->valuestring : "";
  if (strcmp(ixUser(d), DISCORD_OWNER_ID) != 0) {
    replyEphemeralText(id, token, "Not allowed.");
    return false;
  }
  if (!DISCORD_APP_ID[0]) return true;   // no app id, no edits: the handler answers in place
  cJSON *jType = cJSON_GetObjectItem(d, "type");
  cJSON *data = cJSON_GetObjectItem(d, "data");
  int type = jType ? jType->valueint : 0;
  if (type == 2) {
    cJSON *jName = data ? cJSON_GetObjectItem(data, "name") : nullptr;
    if (!jName || !cJSON_IsString(jName) || strcmp(jName->valuestring, "actions")) {
      replyEphemeralText(id, token, "Unknown command.");
      return false;
    }
    char path[640];
    snprintf(path, sizeof path, "/interactions/%s/%s/callback", id, token);
    rest("POST", path, "{\"type\":5,\"data\":{\"flags\":64}}");   // "thinking", ephemeral
    return true;
  }
  if (type == 3) {
    cJSON *jCid = data ? cJSON_GetObjectItem(data, "custom_id") : nullptr;
    bool close = jCid && cJSON_IsString(jCid) && !strcmp(jCid->valuestring, "close");
    if (!close) ackButton(id, token);    // Close answers with its own deferred update
    return true;
  }
  return false;
}

// Takes new interactions off the queue and acknowledges them (bot task only)
static void ixPump() {
  heartbeatService();
  char *json;
  while (ixQueue && xQueueReceive(ixQueue, &json, 0) == pdTRUE) {
    cJSON *d = cJSON_Parse(json);
    if (ixPendingCount >= IX_QUEUE_DEPTH) {
      // backlog full: answer now rather than let Discord's 3 s run out
      cJSON *jId = d ? cJSON_GetObjectItem(d, "id") : nullptr, *jToken = d ? cJSON_GetObjectItem(d, "token") : nullptr;
      if (jId && cJSON_IsString(jId) && jToken && cJSON_IsString(jToken))
        replyEphemeralText(jId->valuestring, jToken->valuestring, "Busy, try again in a few seconds.");
      cJSON_Delete(d);
      heap_caps_free(json);
      continue;
    }
    bool handle = d && ackInteraction(d);
    cJSON_Delete(d);
    ixPending[ixPendingCount++] = {json, handle};
  }
}

void discordWaitTick() {
  if (botTaskHandle && xTaskGetCurrentTaskHandle() == botTaskHandle) ixPump();
}

static void ixService() {
  ixPump();
  while (ixPendingCount) {
    IxPending p = ixPending[0];
    memmove(ixPending, ixPending + 1, (ixPendingCount - 1) * sizeof ixPending[0]);
    ixPendingCount--;
    if (p.handle) {
      cJSON *d = cJSON_Parse(p.json);
      if (d) onInteraction(d);
      cJSON_Delete(d);
    }
    heap_caps_free(p.json);
    ixPump();
  }
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
      netNoteTraffic();
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
        if (data) ixEnqueue(data);   // handled in the bot task
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

// The gateway client; when it can't be created (low internal RAM at boot)
// the bot loop tries again every 30 s
static void gatewayStart() {
  esp_websocket_client_config_t ws_cfg = {};
  ws_cfg.uri = "wss://gateway.discord.gg/?v=10&encoding=json";
  ws_cfg.crt_bundle_attach = esp_crt_bundle_attach;
  ws_cfg.reconnect_timeout_ms = 5000;
  ws_cfg.network_timeout_ms = 10000;
  ws_cfg.buffer_size = 4096;
  ws_cfg.enable_close_reconnect = true;
  ws_cfg.task_core_id_set = true;
  ws_cfg.task_core_id = 0;
  ws_cfg.task_stack = 8192;    // TLS reads and gateway parsing; interactions run in the bot task

  ws = esp_websocket_client_init(&ws_cfg);
  if (ws) {
    esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, onGatewayEvent, nullptr);
    esp_err_t err = esp_websocket_client_start(ws);
    if (err != ESP_OK) logMsg("discord: gateway start failed (%s)", esp_err_to_name(err));
  } else {
    logMsg("discord: gateway client init failed (internal RAM free %u), retrying in 30 s",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  }
}

static void botTask(void *) {
  while (!netUp()) sleepMs(500);
  registerCommands();
  gatewayStart();
  uint32_t lastGatewayTryMs = nowMs();

  PcState lastPc = pcState();
  uint32_t lastLogPostMs = 0, lastCtlSyncMs = 0;
  bool ctlSynced = false;
  for (;;) {
    if (!ws && netUp() && nowMs() - lastGatewayTryMs >= 30000) {
      lastGatewayTryMs = nowMs();
      gatewayStart();
    }
    if (requestReconnect) {
      requestReconnect = false;
      if (ws) esp_websocket_client_close(ws, pdMS_TO_TICKS(1000));
    }
    ixService();   // heartbeat, then button clicks and slash commands
    panelLaterService();
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
  if (!DISCORD_TOKEN[0]) { logMsg("discord: off (no DISCORD_TOKEN in settings.h)"); return; }
  reqDone = xSemaphoreCreateBinary();
  restMutex[0] = xSemaphoreCreateMutex();
  restMutex[1] = xSemaphoreCreateMutex();
  ixQueue = xQueueCreate(IX_QUEUE_DEPTH, sizeof(char *));
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
