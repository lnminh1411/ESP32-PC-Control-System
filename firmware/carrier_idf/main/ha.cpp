#include "ha.h"

#include <Arduino.h>
#include <freertos/queue.h>
#include <esp_mac.h>
#include <mqtt_client.h>

#include "commands.h"
#include "config.h"
#include "display.h"
#include "log.h"
#include "net.h"
#include "pc_state.h"
#include "secrets_or_example.h"
#include "views.h"

#define DISC_PREFIX "homeassistant"

static esp_mqtt_client_handle_t mqtt;
static volatile bool connected, sendDiscovery;
static QueueHandle_t cmdQueue;            // command lines from the MQTT task -> loop()
static char devId[24];                    // esp32pc_<mac>
static char t_cmd[40], t_state[40], t_avail[40], t_log[40];

struct CmdLine { char s[64]; };

static void pub(const char *topic, const char *payload, bool retain) {
  if (mqtt && connected) esp_mqtt_client_enqueue(mqtt, topic, payload, 0, 1, retain, true);
}

// ---- discovery ----
static void announce(const char *component, const char *obj, const char *name, const char *extra) {
  char topic[96], json[900];
  snprintf(topic, sizeof topic, DISC_PREFIX "/%s/%s/%s/config", component, devId, obj);
  snprintf(json, sizeof json,
           "{\"~\":\"%s\",\"name\":\"%s\",\"uniq_id\":\"%s_%s\",\"avty_t\":\"~/avail\","
           "\"dev\":{\"ids\":[\"%s\"],\"name\":\"ESP32 PC Control\",\"mf\":\"DIY\","
           "\"mdl\":\"ESP32-S3 carrier rev 3.1\",\"sw\":\"" __DATE__ "\"},%s}",
           MQTT_BASE, name, devId, obj, devId, extra);
  pub(topic, json, true);
}

static void announceAll() {
  char obj[16], name[40], extra[600];
  for (int i = 1; i <= 4; i++) {
    snprintf(obj, sizeof obj, "door_%d", i);
    snprintf(name, sizeof name, "Door button %d", i);
    snprintf(extra, sizeof extra, "\"cmd_t\":\"~/cmd\",\"pl_prs\":\"door %d\",\"icon\":\"mdi:remote\"", i);
    announce("button", obj, name, extra);
  }
  announce("button", "pc_reset", "PC reset", "\"cmd_t\":\"~/cmd\",\"pl_prs\":\"reset\",\"dev_cla\":\"restart\"");
  for (int i = 1; i <= N_PANELS; i++) {
    snprintf(obj, sizeof obj, "backlight_%d", i);
    snprintf(name, sizeof name, "Screen %d backlight", i);
    snprintf(extra, sizeof extra,
             "\"cmd_t\":\"~/cmd\",\"pl_on\":\"bl %d on\",\"pl_off\":\"bl %d off\",\"on_cmd_type\":\"brightness\","
             "\"stat_t\":\"~/state\",\"stat_val_tpl\":\"{{ 'ON' if value_json.bl%d > 0 else 'OFF' }}\","
             "\"bri_cmd_t\":\"~/cmd\",\"bri_cmd_tpl\":\"bl %d {{ value }}\",\"bri_scl\":100,"
             "\"bri_stat_t\":\"~/state\",\"bri_val_tpl\":\"{{ value_json.bl%d }}\",\"icon\":\"mdi:monitor\"",
             i, i, i, i, i);
    announce("light", obj, name, extra);
    snprintf(obj, sizeof obj, "view_%d", i);
    snprintf(name, sizeof name, "Screen %d view", i);
    snprintf(extra, sizeof extra,
             "\"cmd_t\":\"~/cmd\",\"cmd_tpl\":\"view %d {{ value }}\",\"stat_t\":\"~/state\","
             "\"val_tpl\":\"{{ value_json.view%d }}\",\"ops\":[\"gif\",\"status\",\"stats\"],\"icon\":\"mdi:monitor-dashboard\"",
             i, i);
    announce("select", obj, name, extra);
  }
  announce("select", "status_led", "Status LED",
           "\"cmd_t\":\"~/cmd\",\"cmd_tpl\":\"led {{ value }}\",\"stat_t\":\"~/state\","
           "\"val_tpl\":\"{{ value_json.led }}\",\"ops\":[\"auto\",\"on\",\"off\"],\"ent_cat\":\"config\",\"icon\":\"mdi:led-on\"");
  announce("sensor", "pc_state", "PC state",
           "\"stat_t\":\"~/state\",\"val_tpl\":\"{{ value_json.pc }}\",\"dev_cla\":\"enum\","
           "\"ops\":[\"on\",\"asleep\",\"no host\"],\"icon\":\"mdi:desktop-tower\"");
  announce("sensor", "last_message", "Last message", "\"stat_t\":\"~/log\",\"icon\":\"mdi:message-text\"");
  announce("sensor", "uptime", "Uptime",
           "\"stat_t\":\"~/state\",\"val_tpl\":\"{{ value_json.up }}\",\"dev_cla\":\"duration\",\"unit_of_meas\":\"s\","
           "\"stat_cla\":\"total_increasing\",\"ent_cat\":\"diagnostic\"");
  announce("sensor", "wifi_signal", "Wi-Fi signal",
           "\"stat_t\":\"~/state\",\"val_tpl\":\"{{ value_json.rssi }}\",\"dev_cla\":\"signal_strength\","
           "\"unit_of_meas\":\"dBm\",\"stat_cla\":\"measurement\",\"ent_cat\":\"diagnostic\"");
  announce("sensor", "ip", "IP address", "\"stat_t\":\"~/state\",\"val_tpl\":\"{{ value_json.ip }}\",\"ent_cat\":\"diagnostic\",\"icon\":\"mdi:ip-network\"");
  announce("sensor", "chip_temp", "Chip temperature",
           "\"stat_t\":\"~/state\",\"val_tpl\":\"{{ value_json.temp }}\",\"dev_cla\":\"temperature\","
           "\"unit_of_meas\":\"\\u00b0C\",\"stat_cla\":\"measurement\",\"ent_cat\":\"diagnostic\"");
  announce("sensor", "bus_load", "SPI bus load",
           "\"stat_t\":\"~/state\",\"val_tpl\":\"{{ value_json.bus }}\",\"unit_of_meas\":\"%\","
           "\"stat_cla\":\"measurement\",\"ent_cat\":\"diagnostic\",\"icon\":\"mdi:speedometer\"");
  announce("sensor", "gif_fps", "GIF frame rate",
           "\"stat_t\":\"~/state\",\"val_tpl\":\"{{ value_json.fps }}\",\"unit_of_meas\":\"fps\","
           "\"stat_cla\":\"measurement\",\"ent_cat\":\"diagnostic\",\"icon\":\"mdi:filmstrip\"");
  announce("sensor", "psram_free", "Free PSRAM",
           "\"stat_t\":\"~/state\",\"val_tpl\":\"{{ value_json.psram }}\",\"unit_of_meas\":\"kB\","
           "\"stat_cla\":\"measurement\",\"ent_cat\":\"diagnostic\",\"icon\":\"mdi:memory\"");
}

// ---- state ----
static const char *pcWord(PcState s) { return s == PC_ON ? "on" : s == PC_OFF ? "asleep" : "no host"; }
static const char *ledWord(LedMode m) { return m == LED_AUTO ? "auto" : m == LED_ON ? "on" : m == LED_SWEEP ? "sweep" : "off"; }

// The fields that change on a command or a PC event; a change here is
// published at once, the slow ones (uptime, signal, temp...) every 10 s.
static void fastFields(char *out, size_t n) {
  snprintf(out, n, "\"pc\":\"%s\",\"bl1\":%u,\"bl2\":%u,\"bl3\":%u,\"view1\":\"%s\",\"view2\":\"%s\",\"view3\":\"%s\",\"led\":\"%s\"",
           pcWord(pcState()), backlightPct(0), backlightPct(1), backlightPct(2),
           viewName(panels[0].view), viewName(panels[1].view), viewName(panels[2].view), ledWord(ledMode()));
}

static void publishState(const char *fast) {
  float gifFps = 0;
  for (const Panel &p : panels) if (p.view == VIEW_GIF && p.fps > gifFps) gifFps = p.fps;
  char json[400];
  snprintf(json, sizeof json, "{%s,\"up\":%lu,\"rssi\":%d,\"ip\":\"%s\",\"temp\":%.1f,\"bus\":%.0f,\"fps\":%.1f,\"psram\":%u}",
           fast, (unsigned long)(millis() / 1000), netRssi(), netIp(), temperatureRead(), busLoadPct(), gifFps,
           (unsigned)(ESP.getFreePsram() / 1024));
  pub(t_state, json, true);
}

// ---- MQTT events (MQTT task) ----
static void onMqtt(void *, esp_event_base_t, int32_t id, void *data) {
  esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;
  switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
      connected = true;
      esp_mqtt_client_subscribe(mqtt, t_cmd, 1);
      esp_mqtt_client_subscribe(mqtt, DISC_PREFIX "/status", 1);   // HA birth message
      sendDiscovery = true;
      break;
    case MQTT_EVENT_DISCONNECTED:
      connected = false;
      break;
    case MQTT_EVENT_DATA: {
      if (e->current_data_offset || e->data_len != e->total_data_len) break;   // ignore fragmented payloads
      if (e->topic_len == (int)strlen(t_cmd) && !strncmp(e->topic, t_cmd, e->topic_len)) {
        CmdLine c;
        int n = min(e->data_len, (int)sizeof c.s - 1);
        memcpy(c.s, e->data, n);
        c.s[n] = 0;
        xQueueSend(cmdQueue, &c, 0);
      } else if (e->data_len == 6 && !strncmp(e->data, "online", 6)) {
        sendDiscovery = true;            // HA restarted: announce again
      }
      break;
    }
    default:
      break;
  }
}

static void logToMqtt(const char *line) { pub(t_log, line, false); }

void haInit() {
  if (!MQTT_HOST[0]) { logMsg("ha: off (no MQTT_HOST in secrets.h)"); return; }
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(devId, sizeof devId, "esp32pc_%02x%02x%02x", mac[3], mac[4], mac[5]);
  snprintf(t_cmd, sizeof t_cmd, MQTT_BASE "/cmd");
  snprintf(t_state, sizeof t_state, MQTT_BASE "/state");
  snprintf(t_avail, sizeof t_avail, MQTT_BASE "/avail");
  snprintf(t_log, sizeof t_log, MQTT_BASE "/log");
  cmdQueue = xQueueCreate(8, sizeof(CmdLine));

  static char uri[96];
  snprintf(uri, sizeof uri, "mqtt://%s:%d", MQTT_HOST, MQTT_PORT);
  esp_mqtt_client_config_t cfg = {};
  cfg.broker.address.uri = uri;
  cfg.credentials.client_id = devId;
  if (MQTT_USER[0]) cfg.credentials.username = MQTT_USER;
  if (MQTT_PASS[0]) cfg.credentials.authentication.password = MQTT_PASS;
  cfg.session.last_will.topic = t_avail;
  cfg.session.last_will.msg = "offline";
  cfg.session.last_will.qos = 1;
  cfg.session.last_will.retain = 1;
  cfg.session.keepalive = 30;
  cfg.buffer.size = 2048;
  cfg.network.reconnect_timeout_ms = 5000;
  mqtt = esp_mqtt_client_init(&cfg);
  esp_mqtt_client_register_event(mqtt, MQTT_EVENT_ANY, onMqtt, nullptr);
  logSetHook(logToMqtt);
  logMsg("ha: broker %s, device %s", uri, devId);
}

void haPoll() {
  if (!mqtt) return;
  static bool started, wasConnected;
  if (!started && netUp()) { started = true; esp_mqtt_client_start(mqtt); }   // needs the network up
  if (connected != wasConnected) {
    wasConnected = connected;
    logMsg(connected ? "ha: connected to the broker" : "ha: broker connection lost, retrying");
  }
  if (!connected) return;

  CmdLine c;
  while (xQueueReceive(cmdQueue, &c, 0) == pdTRUE) commandRun(c.s);

  static char lastFast[200];
  static uint32_t lastFull;
  char fast[200];
  fastFields(fast, sizeof fast);
  if (sendDiscovery) {
    sendDiscovery = false;
    announceAll();
    pub(t_avail, "online", true);
    lastFast[0] = 0;                      // and a full state right after
  }
  if (strcmp(fast, lastFast) || millis() - lastFull > 10000) {
    strcpy(lastFast, fast);
    lastFull = millis();
    publishState(fast);
  }
}

bool haUp() { return connected; }
