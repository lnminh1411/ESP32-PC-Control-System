#include "discord.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <WebSocketsClient.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include "commands.h"
#include "config.h"
#include "display.h"
#include "log.h"
#include "net.h"
#include "optos.h"
#include "pc_state.h"
#include "secrets_or_example.h"

// The core's built-in Mozilla CA bundle (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE).
extern const uint8_t caBundleStart[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t caBundleEnd[] asm("_binary_x509_crt_bundle_end");

#define API "https://discord.com/api/v10"

static WebSocketsClient ws;
static volatile bool ready;
static uint32_t heartbeatMs, lastBeat;
static int32_t seq = -1;
static bool gotAck = true;

// ---- hand-over to the main loop: one command at a time ----
static char reqLine[64];
static char reqReply[1500];
static volatile bool reqPending;
static SemaphoreHandle_t reqDone;

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
static int rest(const char *method, const String &path, const String &body) {
  static NetworkClientSecure tls;
  static bool tlsInit;
  if (!tlsInit) { tls.setCACertBundle(caBundleStart, caBundleEnd - caBundleStart); tlsInit = true; }
  HTTPClient http;
  http.setReuse(true);
  if (!http.begin(tls, String(API) + path)) return -1;
  http.addHeader("Authorization", "Bot " DISCORD_TOKEN);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("User-Agent", "DiscordBot (https://github.com/lnminh1411/ESP32-PC-Control-System, 1.0)");
  int code = http.sendRequest(method, body);
  if (code < 200 || code >= 300) logMsg("discord: %s %s -> %d %s", method, path.c_str(), code, http.getString().substring(0, 120).c_str());
  http.end();
  return code;
}

static void postChannelEmbed(const char *title, const char *desc, uint32_t color) {
  if (!DISCORD_CHANNEL_ID[0]) return;
  JsonDocument d;
  JsonObject embed = d["embeds"].to<JsonArray>().add<JsonObject>();
  embed["title"] = title;
  if (desc && desc[0]) embed["description"] = desc;
  embed["color"] = color;
  String body;
  serializeJson(d, body);
  rest("POST", String("/channels/") + DISCORD_CHANNEL_ID + "/messages", body);
}

// ---- actions panel tracking ----
static char panelToken[128];
static uint32_t panelTouchedMs, panelOpenedMs;
#define PANEL_TOKEN_LIFE_MS (14 * 60 * 1000UL)   // interaction tokens die at 15 min
static bool panelActive;
static char lastPanelResult[128];

static void deleteActivePanel() {
  if (!panelActive || !panelToken[0] || !DISCORD_APP_ID[0]) return;
  rest("DELETE", String("/webhooks/") + DISCORD_APP_ID + "/" + panelToken + "/messages/@original", "");
  panelActive = false;
  panelToken[0] = 0;
}

// ---- slash command registration ----
static void registerCommands() {
  JsonDocument d;
  JsonArray cmds = d.to<JsonArray>();
  JsonObject c = cmds.add<JsonObject>();
  c["name"] = "actions";
  c["description"] = "Open the ESP32 PC Control panel (only you see it)";

  String body;
  serializeJson(d, body);
  int code = rest("PUT", String("/applications/") + DISCORD_APP_ID + "/guilds/" + DISCORD_GUILD_ID + "/commands", body);
  if (code >= 200 && code < 300) logMsg("discord: slash commands registered");
}

// ---- UI builders ----
enum PanelView { PANEL_NORMAL, PANEL_CONFIRM_RESET };

static void addButton(JsonArray row, const char *label, const char *id, int style) {
  JsonObject b = row.add<JsonObject>();
  b["type"] = 2;
  b["label"] = label;
  b["custom_id"] = id;
  b["style"] = style; // 1 blurple, 2 grey, 4 red
}

static JsonArray addRow(JsonObject data) {
  JsonArray comps;
  if (!data["components"].is<JsonArray>()) {
    comps = data["components"].to<JsonArray>();
  } else {
    comps = data["components"].as<JsonArray>();
  }
  JsonObject r = comps.add<JsonObject>();
  r["type"] = 1;
  return r["components"].to<JsonArray>();
}

static void buildEmbed(JsonObject embed, const char *footerText) {
  embed["title"] = "ESP32 PC Control";
  embed["color"] = 0x5865F2;

  JsonArray fields = embed["fields"].to<JsonArray>();

  JsonObject fPc = fields.add<JsonObject>();
  fPc["name"] = "PC";
  fPc["value"] = (pcState() == PC_ON) ? "on" : (pcState() == PC_OFF) ? "asleep" : "no host";
  fPc["inline"] = true;

  char blStr[32];
  snprintf(blStr, sizeof blStr, "%u / %u / %u %%", backlightPct(0), backlightPct(1), backlightPct(2));
  JsonObject fBl = fields.add<JsonObject>();
  fBl["name"] = "Backlights";
  fBl["value"] = blStr;
  fBl["inline"] = true;

  uint32_t s = millis() / 1000;
  char upStr[32];
  snprintf(upStr, sizeof upStr, "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60), (unsigned long)(s % 60));
  JsonObject fUp = fields.add<JsonObject>();
  fUp["name"] = "Uptime";
  fUp["value"] = upStr;
  fUp["inline"] = true;

  char wifiStr[48];
  if (netUp() && netIp()[0]) snprintf(wifiStr, sizeof wifiStr, "%s (%d dBm)", netIp(), netRssi());
  else snprintf(wifiStr, sizeof wifiStr, "down");
  JsonObject fWifi = fields.add<JsonObject>();
  fWifi["name"] = "Wi-Fi";
  fWifi["value"] = wifiStr;
  fWifi["inline"] = true;

  char doorStr[32];
  int dp = doorPressed();
  if (dp > 0) snprintf(doorStr, sizeof doorStr, "B%d pressed", dp);
  else snprintf(doorStr, sizeof doorStr, "idle");
  JsonObject fDoor = fields.add<JsonObject>();
  fDoor["name"] = "Door remote";
  fDoor["value"] = doorStr;
  fDoor["inline"] = true;

  const char *ft = (footerText && footerText[0]) ? footerText :
                   (lastPanelResult[0] ? lastPanelResult : "Panel closes by itself after 2 minutes");
  JsonObject footer = embed["footer"].to<JsonObject>();
  footer["text"] = ft;
}

static void buildComponents(JsonObject data, PanelView view) {
  if (view == PANEL_NORMAL) {
    JsonArray r1 = addRow(data);
    addButton(r1, "Door 1", "door_1", 1);
    addButton(r1, "Door 2", "door_2", 1);
    addButton(r1, "Door 3", "door_3", 1);
    addButton(r1, "Door 4", "door_4", 1);

    JsonArray r2 = addRow(data);
    addButton(r2, "Refresh", "refresh", 2);
    addButton(r2, "PC reset", "reset", 4);
    addButton(r2, "Close", "close", 2);
  } else if (view == PANEL_CONFIRM_RESET) {
    JsonArray r1 = addRow(data);
    addButton(r1, "Yes, reset", "reset_confirm", 4);
    addButton(r1, "Cancel", "refresh", 2);
  }
}

static void sendPanelResponse(const char *id, const char *token, int callbackType, PanelView view, const char *footerText) {
  JsonDocument d;
  d["type"] = callbackType; // 4 = reply, 7 = update
  JsonObject data = d["data"].to<JsonObject>();
  if (callbackType == 4) {
    data["flags"] = 64; // ephemeral
  }
  JsonObject embed = data["embeds"].to<JsonArray>().add<JsonObject>();
  buildEmbed(embed, footerText);
  buildComponents(data, view);

  String body;
  serializeJson(d, body);
  rest("POST", String("/interactions/") + id + "/" + token + "/callback", body);
}

static void replyEphemeralText(const char *id, const char *token, const char *text) {
  JsonDocument d;
  d["type"] = 4;
  JsonObject data = d["data"].to<JsonObject>();
  data["content"] = text;
  data["flags"] = 64;
  String body;
  serializeJson(d, body);
  rest("POST", String("/interactions/") + id + "/" + token + "/callback", body);
}

static void onInteraction(JsonObject d) {
  const char *id = d["id"] | "", *token = d["token"] | "";
  const char *user = d["member"]["user"]["id"] | (d["user"]["id"] | "");
  if (strcmp(user, DISCORD_OWNER_ID)) {
    replyEphemeralText(id, token, "Not allowed.");
    return;
  }
  int type = d["type"] | 0;
  if (type == 2) { // slash command
    const char *name = d["data"]["name"] | "";
    if (!strcmp(name, "actions")) {
      // answer first (3 s deadline), then remove the previous panel
      char oldToken[sizeof panelToken];
      bool hadOld = panelActive;
      snprintf(oldToken, sizeof oldToken, "%s", panelToken);
      snprintf(panelToken, sizeof panelToken, "%s", token);
      panelTouchedMs = panelOpenedMs = millis();
      panelActive = true;
      lastPanelResult[0] = 0;
      sendPanelResponse(id, token, 4, PANEL_NORMAL, nullptr);
      if (hadOld && oldToken[0] && DISCORD_APP_ID[0])
        rest("DELETE", String("/webhooks/") + DISCORD_APP_ID + "/" + oldToken + "/messages/@original", "");
    } else {
      replyEphemeralText(id, token, "Unknown command.");
    }
  } else if (type == 3) { // button
    const char *cid = d["data"]["custom_id"] | "";
    if (!strcmp(cid, "close")) {
      JsonDocument r;
      r["type"] = 6; // DEFERRED_UPDATE_MESSAGE
      String body;
      serializeJson(r, body);
      rest("POST", String("/interactions/") + id + "/" + token + "/callback", body);

      const char *delToken = token[0] ? token : panelToken;
      if (DISCORD_APP_ID[0] && delToken[0]) {
        rest("DELETE", String("/webhooks/") + DISCORD_APP_ID + "/" + delToken + "/messages/@original", "");
      }
      panelActive = false;
      panelToken[0] = 0;
      lastPanelResult[0] = 0;
      return;
    }

    panelTouchedMs = millis();

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
static void sendJson(JsonDocument &d) {
  String s;
  serializeJson(d, s);
  ws.sendTXT(s);
}

static void heartbeat() {
  JsonDocument d;
  d["op"] = 1;
  if (seq >= 0) d["d"] = seq; else d["d"] = nullptr;
  sendJson(d);
  lastBeat = millis();
  gotAck = false;
}

static void identify() {
  JsonDocument d;
  d["op"] = 2;
  JsonObject p = d["d"].to<JsonObject>();
  p["token"] = DISCORD_TOKEN;
  p["intents"] = 0; // interactions arrive without any intent
  JsonObject props = p["properties"].to<JsonObject>();
  props["os"] = "esp32";
  props["browser"] = NET_HOSTNAME;
  props["device"] = NET_HOSTNAME;
  sendJson(d);
}

static void onGateway(WStype_t type, uint8_t *payload, size_t len) {
  if (type == WStype_DISCONNECTED) {
    if (ready) logMsg("discord: gateway disconnected, reconnecting");
    ready = false;
    heartbeatMs = 0;
    return;
  }
  if (type != WStype_TEXT) return;
  JsonDocument filter;
  filter["op"] = true;
  filter["t"] = true;
  filter["s"] = true;
  JsonObject fd = filter["d"].to<JsonObject>();
  fd["heartbeat_interval"] = true;
  fd["id"] = true;
  fd["token"] = true;
  fd["type"] = true;
  fd["data"]["name"] = true;
  fd["data"]["custom_id"] = true;
  fd["member"]["user"]["id"] = true;
  fd["user"]["id"] = true;
  fd["user"]["username"] = true;
  JsonDocument d;
  if (deserializeJson(d, payload, len, DeserializationOption::Filter(filter))) return;
  int op = d["op"] | -1;
  if (!d["s"].isNull()) seq = d["s"];
  switch (op) {
    case 10: // HELLO
      heartbeatMs = d["d"]["heartbeat_interval"] | 41250;
      lastBeat = millis() - random(heartbeatMs); // first beat after a jittered wait
      gotAck = true;
      identify();
      break;
    case 11: gotAck = true; break; // heartbeat ACK
    case 1: heartbeat(); break;    // server asks for a beat now
    case 7:                        // reconnect requested
    case 9:                        // invalid session
      ws.disconnect();
      break;
    case 0: {
      const char *t = d["t"] | "";
      if (!strcmp(t, "READY")) {
        bool first = !ready;
        ready = true;
        logMsg("discord: online as %s", d["d"]["user"]["username"] | "?");
        static bool announced;
        if (first && !announced) {
          announced = true;
          postChannelEmbed("ESP32 PC Control is online", netIp()[0] ? netIp() : "Wi-Fi down", 0x57F287);
        }
      } else if (!strcmp(t, "INTERACTION_CREATE")) {
        onInteraction(d["d"]);
      }
      break;
    }
  }
}

static void botTask(void *) {
  while (!netUp()) vTaskDelay(pdMS_TO_TICKS(500));
  registerCommands();
  ws.beginSslWithBundle("gateway.discord.gg", 443, "/?v=10&encoding=json", caBundleStart,
                        caBundleEnd - caBundleStart, "");
  ws.onEvent(onGateway);
  ws.setReconnectInterval(5000);
  PcState lastPc = pcState();
  for (;;) {
    ws.loop();
    if (heartbeatMs && millis() - lastBeat >= heartbeatMs) {
      if (!gotAck) { logMsg("discord: no heartbeat ACK, reconnecting"); ws.disconnect(); }
      else heartbeat();
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
      } else if (!strncmp(nmsg.text, "door B", 6)) {   // from cmdDoor: "door Bn"
        char title[32];
        snprintf(title, sizeof title, "Door button %s pressed", nmsg.text + 6);
        postChannelEmbed(title, nullptr, 0x5865F2);
      } else {
        postChannelEmbed(nmsg.text, nullptr, 0x5865F2);
      }
    }
    if (panelActive && panelToken[0] &&
        (millis() - panelTouchedMs >= ACTIONS_PANEL_TTL_MS || millis() - panelOpenedMs >= PANEL_TOKEN_LIFE_MS)) {
      deleteActivePanel();
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void discordInit() {
  if (!DISCORD_TOKEN[0]) { logMsg("discord: off (no DISCORD_TOKEN in secrets.h)"); return; }
  reqDone = xSemaphoreCreateBinary();
  notifyQueue = xQueueCreate(NOTIFY_QUEUE_DEPTH, sizeof(NotifyMsg));
  xTaskCreatePinnedToCore(botTask, "discord", 12288, nullptr, 1, nullptr, 0);
  logMsg("discord: bot starting");
}

bool discordUp() { return ready; }
