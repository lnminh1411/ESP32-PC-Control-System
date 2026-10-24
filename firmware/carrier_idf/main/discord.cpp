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
#include <stdio.h>
#include <string.h>

#include "commands.h"
#include "config.h"
#include "display.h"
#include "log.h"
#include "net.h"
#include "optos.h"
#include "pc_state.h"
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
static SemaphoreHandle_t restMutex;

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
#define NOTIFY_QUEUE_DEPTH 6
#define NOTIFY_MSG_LEN 48

struct NotifyMsg {
  char text[NOTIFY_MSG_LEN];
};

static QueueHandle_t notifyQueue;

void discordNotify(const char *what) {
  if (!DISCORD_TOKEN[0] || !notifyQueue || !what) return;
  NotifyMsg msg;
  snprintf(msg.text, sizeof msg.text, "%s", what);
  xQueueSend(notifyQueue, &msg, 0);
}

// ---- REST ----
struct HttpRespBuf {
  char text[128];
  int len;
};

static esp_err_t httpEventHandler(esp_http_client_event_t *evt) {
  if (evt->event_id == HTTP_EVENT_ON_DATA) {
    HttpRespBuf *rb = (HttpRespBuf *)evt->user_data;
    if (rb && rb->len < (int)sizeof(rb->text) - 1) {
      int copyLen = evt->data_len;
      if (rb->len + copyLen > (int)sizeof(rb->text) - 1) {
        copyLen = (int)sizeof(rb->text) - 1 - rb->len;
      }
      memcpy(rb->text + rb->len, evt->data, copyLen);
      rb->len += copyLen;
      rb->text[rb->len] = '\0';
    }
  }
  return ESP_OK;
}

static int rest(const char *method, const char *path, const char *body) {
  if (restMutex) xSemaphoreTake(restMutex, portMAX_DELAY);
  static esp_http_client_handle_t client;
  static HttpRespBuf respBuf;
  char url[256];
  snprintf(url, sizeof url, "%s%s", API, path);

  respBuf.len = 0;
  respBuf.text[0] = 0;

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
    client = esp_http_client_init(&cfg);
    if (!client) {
      if (restMutex) xSemaphoreGive(restMutex);
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

  esp_err_t err = esp_http_client_perform(client);
  if (err != ESP_OK) {
    esp_http_client_close(client);
    respBuf.len = 0;
    respBuf.text[0] = 0;
    err = esp_http_client_perform(client);
  }

  int code = esp_http_client_get_status_code(client);
  if (err != ESP_OK) {
    logMsg("discord: %s %s -> err %s", method, path, esp_err_to_name(err));
    if (restMutex) xSemaphoreGive(restMutex);
    return -1;
  }
  if (code < 200 || code >= 300) {
    logMsg("discord: %s %s -> %d %s", method, path, code, respBuf.text);
  }
  if (restMutex) xSemaphoreGive(restMutex);
  return code;
}

static void postChannelEmbed(const char *title, const char *desc, uint32_t color) {
  if (!DISCORD_CHANNEL_ID[0]) return;
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
  snprintf(path, sizeof path, "/channels/%s/messages", DISCORD_CHANNEL_ID);
  rest("POST", path, body);
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
  char delPath[256];
  snprintf(delPath, sizeof delPath, "/webhooks/%s/%s/messages/@original", DISCORD_APP_ID, panelToken);
  rest("DELETE", delPath, "");
  panelActive = false;
  panelToken[0] = 0;
}

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
enum PanelView { PANEL_NORMAL, PANEL_CONFIRM_RESET };

static void addButton(cJSON *row, const char *label, const char *id, int style) {
  cJSON *b = cJSON_CreateObject();
  cJSON_AddNumberToObject(b, "type", 2);
  cJSON_AddStringToObject(b, "label", label);
  cJSON_AddStringToObject(b, "custom_id", id);
  cJSON_AddNumberToObject(b, "style", style); // 1 blurple, 2 grey, 4 red
  cJSON_AddItemToArray(row, b);
}

static cJSON *addRow(cJSON *comps) {
  cJSON *r = cJSON_CreateObject();
  cJSON_AddNumberToObject(r, "type", 1);
  cJSON *rowComps = cJSON_AddArrayToObject(r, "components");
  cJSON_AddItemToArray(comps, r);
  return rowComps;
}

static void buildEmbed(cJSON *embed, const char *footerText) {
  cJSON_AddStringToObject(embed, "title", "ESP32 PC Control");
  cJSON_AddNumberToObject(embed, "color", 0x5865F2);

  cJSON *fields = cJSON_AddArrayToObject(embed, "fields");

  cJSON *fPc = cJSON_CreateObject();
  cJSON_AddStringToObject(fPc, "name", "PC");
  cJSON_AddStringToObject(fPc, "value", (pcState() == PC_ON) ? "on" : (pcState() == PC_OFF) ? "asleep" : "no host");
  cJSON_AddBoolToObject(fPc, "inline", true);
  cJSON_AddItemToArray(fields, fPc);

  char blStr[32];
  snprintf(blStr, sizeof blStr, "%u / %u / %u %%", backlightPct(0), backlightPct(1), backlightPct(2));
  cJSON *fBl = cJSON_CreateObject();
  cJSON_AddStringToObject(fBl, "name", "Backlights");
  cJSON_AddStringToObject(fBl, "value", blStr);
  cJSON_AddBoolToObject(fBl, "inline", true);
  cJSON_AddItemToArray(fields, fBl);

  uint32_t s = nowMs() / 1000;
  char upStr[32];
  snprintf(upStr, sizeof upStr, "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60), (unsigned long)(s % 60));
  cJSON *fUp = cJSON_CreateObject();
  cJSON_AddStringToObject(fUp, "name", "Uptime");
  cJSON_AddStringToObject(fUp, "value", upStr);
  cJSON_AddBoolToObject(fUp, "inline", true);
  cJSON_AddItemToArray(fields, fUp);

  char wifiStr[48];
  if (netUp() && netIp()[0]) snprintf(wifiStr, sizeof wifiStr, "%s (%d dBm)", netIp(), netRssi());
  else snprintf(wifiStr, sizeof wifiStr, "down");
  cJSON *fWifi = cJSON_CreateObject();
  cJSON_AddStringToObject(fWifi, "name", "Wi-Fi");
  cJSON_AddStringToObject(fWifi, "value", wifiStr);
  cJSON_AddBoolToObject(fWifi, "inline", true);
  cJSON_AddItemToArray(fields, fWifi);

  char doorStr[32];
  int dp = doorPressed();
  if (dp > 0) snprintf(doorStr, sizeof doorStr, "B%d pressed", dp);
  else snprintf(doorStr, sizeof doorStr, "idle");
  cJSON *fDoor = cJSON_CreateObject();
  cJSON_AddStringToObject(fDoor, "name", "Door remote");
  cJSON_AddStringToObject(fDoor, "value", doorStr);
  cJSON_AddBoolToObject(fDoor, "inline", true);
  cJSON_AddItemToArray(fields, fDoor);

  const char *ft = (footerText && footerText[0]) ? footerText :
                   (lastPanelResult[0] ? lastPanelResult : "Panel closes by itself after 2 minutes");
  cJSON *footer = cJSON_AddObjectToObject(embed, "footer");
  cJSON_AddStringToObject(footer, "text", ft);
}

static void buildComponents(cJSON *data, PanelView view) {
  cJSON *comps = cJSON_AddArrayToObject(data, "components");
  if (view == PANEL_NORMAL) {
    cJSON *r1 = addRow(comps);
    addButton(r1, "Door 1", "door_1", 1);
    addButton(r1, "Door 2", "door_2", 1);
    addButton(r1, "Door 3", "door_3", 1);
    addButton(r1, "Door 4", "door_4", 1);

    cJSON *r2 = addRow(comps);
    addButton(r2, "Refresh", "refresh", 2);
    addButton(r2, "PC reset", "reset", 4);
    addButton(r2, "Close", "close", 2);
  } else if (view == PANEL_CONFIRM_RESET) {
    cJSON *r1 = addRow(comps);
    addButton(r1, "Yes, reset", "reset_confirm", 4);
    addButton(r1, "Cancel", "refresh", 2);
  }
}

static void sendPanelResponse(const char *id, const char *token, int callbackType, PanelView view, const char *footerText) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "type", callbackType); // 4 = reply, 7 = update
  cJSON *data = cJSON_AddObjectToObject(root, "data");
  if (callbackType == 4) {
    cJSON_AddNumberToObject(data, "flags", 64); // ephemeral
  }
  cJSON *embeds = cJSON_AddArrayToObject(data, "embeds");
  cJSON *embed = cJSON_CreateObject();
  buildEmbed(embed, footerText);
  cJSON_AddItemToArray(embeds, embed);
  buildComponents(data, view);

  char *body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);

  char path[256];
  snprintf(path, sizeof path, "/interactions/%s/%s/callback", id, token);
  rest("POST", path, body);
  free(body);
}

static void replyEphemeralText(const char *id, const char *token, const char *text) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "type", 4);
  cJSON *data = cJSON_AddObjectToObject(root, "data");
  cJSON_AddStringToObject(data, "content", text);
  cJSON_AddNumberToObject(data, "flags", 64);
  char *body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);

  char path[256];
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
      sendPanelResponse(id, token, 4, PANEL_NORMAL, nullptr);
      if (hadOld && oldToken[0] && DISCORD_APP_ID[0]) {
        char delPath[256];
        snprintf(delPath, sizeof delPath, "/webhooks/%s/%s/messages/@original", DISCORD_APP_ID, oldToken);
        rest("DELETE", delPath, "");
      }
    } else {
      replyEphemeralText(id, token, "Unknown command.");
    }
  } else if (type == 3) { // button
    cJSON *jCid = data ? cJSON_GetObjectItem(data, "custom_id") : nullptr;
    const char *cid = (jCid && jCid->valuestring) ? jCid->valuestring : "";
    if (!strcmp(cid, "close")) {
      cJSON *r = cJSON_CreateObject();
      cJSON_AddNumberToObject(r, "type", 6); // DEFERRED_UPDATE_MESSAGE
      char *body = cJSON_PrintUnformatted(r);
      cJSON_Delete(r);

      char path[256];
      snprintf(path, sizeof path, "/interactions/%s/%s/callback", id, token);
      rest("POST", path, body);
      free(body);

      const char *delToken = token[0] ? token : panelToken;
      if (DISCORD_APP_ID[0] && delToken[0]) {
        char delPath[256];
        snprintf(delPath, sizeof delPath, "/webhooks/%s/%s/messages/@original", DISCORD_APP_ID, delToken);
        rest("DELETE", delPath, "");
      }
      panelActive = false;
      panelToken[0] = 0;
      lastPanelResult[0] = 0;
      return;
    }

    panelTouchedMs = nowMs();

    if (!strcmp(cid, "refresh")) {
      snprintf(lastPanelResult, sizeof lastPanelResult, "Refreshed");
      sendPanelResponse(id, token, 7, PANEL_NORMAL, "Refreshed");
    } else if (!strcmp(cid, "reset")) {
      sendPanelResponse(id, token, 7, PANEL_CONFIRM_RESET, "Reset the PC?");
    } else if (!strcmp(cid, "reset_confirm")) {
      runAndGetFirstLine("reset", lastPanelResult, sizeof lastPanelResult);
      sendPanelResponse(id, token, 7, PANEL_NORMAL, lastPanelResult);
    } else if (!strncmp(cid, "door_", 5)) {
      char cmd[32];
      snprintf(cmd, sizeof cmd, "door %s", cid + 5);
      runAndGetFirstLine(cmd, lastPanelResult, sizeof lastPanelResult);
      sendPanelResponse(id, token, 7, PANEL_NORMAL, lastPanelResult);
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
          postChannelEmbed("ESP32 PC Control is online", (netUp() && netIp()[0]) ? netIp() : "Wi-Fi down", 0x57F287);
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
    esp_websocket_client_start(ws);
  }

  PcState lastPc = pcState();
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
      if (pc == PC_ON) postChannelEmbed("PC is on", nullptr, 0x57F287);
      else if (pc == PC_OFF) postChannelEmbed("PC is asleep or off", nullptr, 0xE67E22);
      else postChannelEmbed("PC state unknown", nullptr, 0x99AAB5);
    }
    NotifyMsg nmsg;
    while (notifyQueue && xQueueReceive(notifyQueue, &nmsg, 0) == pdTRUE) {
      if (!ready || !DISCORD_CHANNEL_ID[0]) continue;
      if (!strcmp(nmsg.text, "PC reset")) {
        postChannelEmbed("PC reset", nullptr, 0xED4245);
      } else if (!strncmp(nmsg.text, "door B", 6)) { // from cmdDoor: "door Bn"
        char title[32];
        snprintf(title, sizeof title, "Door button %s pressed", nmsg.text + 6);
        postChannelEmbed(title, nullptr, 0x5865F2);
      } else {
        postChannelEmbed(nmsg.text, nullptr, 0x5865F2);
      }
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
  restMutex = xSemaphoreCreateMutex();
  notifyQueue = xQueueCreate(NOTIFY_QUEUE_DEPTH, sizeof(NotifyMsg));
  wsBuf = (char *)heap_caps_malloc(WS_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!wsBuf) wsBuf = (char *)malloc(WS_BUF_SIZE);
  xTaskCreatePinnedToCore(botTask, "discord", 12288, nullptr, 1, nullptr, 0);
  logMsg("discord: bot starting");
}

bool discordUp() { return ready; }
