#include "owntracks.h"

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <math.h>
#include <mbedtls/base64.h>
#include <mqtt_client.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "log.h"
#include "net.h"
#include "settings_or_example.h"
#include "sys.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static esp_mqtt_client_handle_t mqttClient;
static SemaphoreHandle_t locMutex;

static double phoneLat;
static double phoneLon;
static uint32_t phoneTst;
static uint32_t phoneAcc;
static uint32_t phoneReceivedMs;
static bool hasLocation;
static bool clientStarted;
static volatile bool connected;
static char devTopic[80];     // the phone's own topic, owntracks/<user>/<device>, from its location messages
static uint32_t lastAskMs;    // last reportLocation command
static volatile bool askPending = true;   // a retained reportLocation may sit on the broker (one from before a restart too): cleared when a fix arrives
static bool errLogged;
static char *bigMsg;   // joins a message split across MQTT_EVENT_DATA events

static double haversineKm(double lat1, double lon1, double lat2, double lon2) {
  constexpr double R = 6371.0;
  double dLat = (lat2 - lat1) * (M_PI / 180.0);
  double dLon = (lon2 - lon1) * (M_PI / 180.0);
  double a = sin(dLat / 2.0) * sin(dLat / 2.0) +
             cos(lat1 * (M_PI / 180.0)) * cos(lat2 * (M_PI / 180.0)) *
             sin(dLon / 2.0) * sin(dLon / 2.0);
  double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
  return R * c;
}

// cJSON recurses once per level and this runs on the MQTT task's 4 KB stack.
// A location is 2 levels deep. Brackets inside strings don't count (a "]]]"
// string must not buy depth back), and more closers than openers is not JSON.
static bool jsonShallow(const char *s, int len) {
  int depth = 0;
  bool inStr = false;
  for (int i = 0; i < len; i++) {
    char c = s[i];
    if (inStr) {
      if (c == '\\') i++;
      else if (c == '"') inStr = false;
    } else if (c == '"') {
      inStr = true;
    } else if (c == '{' || c == '[') {
      if (++depth > 8) return false;
    } else if (c == '}' || c == ']') {
      if (--depth < 0) return false;
    }
  }
  return true;
}

// OwnTracks payload encryption: the app's key is its text, zero-padded to 32
// bytes; a message is {"_type":"encrypted","data":base64(nonce[24] || secretbox)}
static bool encrypted() { return OWNTRACKS_SECRET[0] != 0; }

static void secretKey(uint8_t key[crypto_secretbox_KEYBYTES]) {
  memset(key, 0, crypto_secretbox_KEYBYTES);
  size_t n = strlen(OWNTRACKS_SECRET);
  memcpy(key, OWNTRACKS_SECRET, n < crypto_secretbox_KEYBYTES ? n : crypto_secretbox_KEYBYTES);
}

// The plaintext JSON of an encrypted message (heap, caller frees), or null
static char *decryptData(const char *b64) {
  size_t b64Len = strlen(b64), rawLen = 0;
  if (b64Len > 4096) return nullptr;
  uint8_t *raw = (uint8_t *)malloc(b64Len);
  if (!raw) return nullptr;
  char *plain = nullptr;
  if (mbedtls_base64_decode(raw, b64Len, &rawLen, (const uint8_t *)b64, b64Len) == 0 &&
      rawLen > crypto_secretbox_NONCEBYTES + crypto_secretbox_MACBYTES) {
    size_t cLen = rawLen - crypto_secretbox_NONCEBYTES, pLen = cLen - crypto_secretbox_MACBYTES;
    plain = (char *)malloc(pLen + 1);
    uint8_t key[crypto_secretbox_KEYBYTES];
    secretKey(key);
    if (plain && crypto_secretbox_open_easy((uint8_t *)plain, raw + crypto_secretbox_NONCEBYTES, cLen, raw, key) == 0) {
      plain[pLen] = 0;
    } else {
      free(plain);
      plain = nullptr;
    }
    sodium_memzero(key, sizeof key);
  }
  free(raw);
  return plain;
}

// {"_type":"encrypted","data":...} for a plaintext message (heap), or null
static char *encryptJson(const char *json) {
  size_t pLen = strlen(json), rawLen = crypto_secretbox_NONCEBYTES + crypto_secretbox_MACBYTES + pLen;
  uint8_t *raw = (uint8_t *)malloc(rawLen);
  size_t b64Cap = (rawLen + 2) / 3 * 4 + 1, b64Len = 0;
  char *out = (char *)malloc(b64Cap + 40);
  if (!raw || !out) { free(raw); free(out); return nullptr; }
  uint8_t key[crypto_secretbox_KEYBYTES];
  secretKey(key);
  randombytes_buf(raw, crypto_secretbox_NONCEBYTES);
  crypto_secretbox_easy(raw + crypto_secretbox_NONCEBYTES, (const uint8_t *)json, pLen, raw, key);
  sodium_memzero(key, sizeof key);
  int n = snprintf(out, b64Cap + 40, "{\"_type\":\"encrypted\",\"data\":\"");
  mbedtls_base64_encode((uint8_t *)out + n, b64Cap, &b64Len, raw, rawLen);
  strcpy(out + n + b64Len, "\"}");
  free(raw);
  return out;
}

// A location fix from a parsed message
static void takeLocation(cJSON *root, esp_mqtt_event_handle_t event) {
  cJSON *type = cJSON_GetObjectItem(root, "_type");
  if (!type || !cJSON_IsString(type) || strcmp(type->valuestring, "location")) return;
  cJSON *jlat = cJSON_GetObjectItem(root, "lat");
  cJSON *jlon = cJSON_GetObjectItem(root, "lon");
  cJSON *jtst = cJSON_GetObjectItem(root, "tst");
  cJSON *jacc = cJSON_GetObjectItem(root, "acc");

  // a real position only: the nowcast's URL builder wraps the longitude
  // in a loop (an inf never leaves it) and prints every point with %.5f
  if (!jlat || !jlon || !jtst || !cJSON_IsNumber(jlat) || !cJSON_IsNumber(jlon) || !cJSON_IsNumber(jtst) ||
      !(fabs(jlat->valuedouble) <= 90.0) || !(fabs(jlon->valuedouble) <= 180.0)) return;   // NaN fails too
  uint32_t tst = (uint32_t)(jtst->valuedouble > 0.0 ? jtst->valuedouble : 0.0);
  if (hasLocation && tst < phoneTst) {   // an older fix replayed (a retained or recorded message)
    logMsg("owntracks: fix older than the last one, ignored");
    return;
  }
  if (locMutex) xSemaphoreTake(locMutex, portMAX_DELAY);
  phoneLat = jlat->valuedouble;
  phoneLon = jlon->valuedouble;
  phoneTst = tst;
  phoneAcc = (jacc && cJSON_IsNumber(jacc)) ? (uint32_t)jacc->valuedouble : 0;
  phoneReceivedMs = nowMs();
  hasLocation = true;
  // the device's topic: its /cmd subtopic takes reportLocation
  if (event->topic && event->topic_len > 0 && event->topic_len < (int)sizeof devTopic) {
    memcpy(devTopic, event->topic, event->topic_len);
    devTopic[event->topic_len] = 0;
  }
  if (locMutex) xSemaphoreGive(locMutex);
  logMsg("owntracks: location updated (acc %u m)", (unsigned)phoneAcc);
  if (askPending && devTopic[0]) {   // a joined message's last piece has no topic: never "/cmd"
    // answered (or moved): take the retained command off the broker
    char t[96];
    snprintf(t, sizeof t, "%s/cmd", devTopic);
    esp_mqtt_client_publish(mqttClient, t, "", 0, 1, 1);
    askPending = false;
  }
}

static void mqttEventHandler(void *, esp_event_base_t, int32_t event_id, void *event_data) {
  esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
  switch (event_id) {
    case MQTT_EVENT_CONNECTED: {
      errLogged = false;
      connected = true;
      const char *topic = OWNTRACKS_TOPIC[0] ? OWNTRACKS_TOPIC : "owntracks/#";
      esp_mqtt_client_subscribe(mqttClient, topic, 0);
      logMsg("owntracks: subscribed to %s", topic);
      if (strpbrk(topic, "#+"))
        logMsg("owntracks: wildcard topic also delivers events and other devices; set owntracks/<user>/<device>");
      break;
    }
    case MQTT_EVENT_DATA: {
      if (!event->data || event->data_len <= 0) break;
      // A message bigger than the 512 B buffer arrives in pieces: join them
      // in a heap copy (rare; a location is ~300-450 B)
      const char *msg = event->data;
      int msgLen = event->data_len;
      if (event->total_data_len > event->data_len) {
        if (event->total_data_len > 4096) break;
        if (event->current_data_offset == 0) {
          free(bigMsg);
          bigMsg = (char *)malloc(event->total_data_len);
        }
        if (!bigMsg || event->current_data_offset + event->data_len > event->total_data_len) break;
        memcpy(bigMsg + event->current_data_offset, event->data, event->data_len);
        if (event->current_data_offset + event->data_len < event->total_data_len) break;
        msg = bigMsg;
        msgLen = event->total_data_len;
      }
      cJSON *root = jsonShallow(msg, msgLen) ? cJSON_ParseWithLength(msg, msgLen) : nullptr;
      if (bigMsg) {
        free(bigMsg);
        bigMsg = nullptr;
      }
      if (!root) break;

      cJSON *type = cJSON_GetObjectItem(root, "_type");
      bool isEnc = type && cJSON_IsString(type) && !strcmp(type->valuestring, "encrypted");
      if (encrypted() && !isEnc) {
        // with a key set only sealed messages count: anyone who can publish on
        // the topic could otherwise move "you" (hibernate, away lock)
        static bool plainLogged;
        if (!plainLogged && type && cJSON_IsString(type) && !strcmp(type->valuestring, "location")) {
          plainLogged = true;
          logMsg("owntracks: unencrypted location ignored (OWNTRACKS_SECRET is set: set the same key in the app)");
        }
      } else if (isEnc) {
        cJSON *data = cJSON_GetObjectItem(root, "data");
        char *plain = encrypted() && data && cJSON_IsString(data) ? decryptData(data->valuestring) : nullptr;
        cJSON *inner = plain && jsonShallow(plain, strlen(plain)) ? cJSON_Parse(plain) : nullptr;
        if (inner) takeLocation(inner, event);
        else if (encrypted()) logMsg("owntracks: encrypted message that the key doesn't open, ignored");
        cJSON_Delete(inner);
        free(plain);
      } else {
        takeLocation(root, event);
      }
      cJSON_Delete(root);
      break;
    }
    case MQTT_EVENT_DISCONNECTED:
      connected = false;
      break;
    case MQTT_EVENT_ERROR: {
      if (!errLogged) {
        errLogged = true;
        logMsg("owntracks: MQTT error event");
      }
      break;
    }
    default:
      break;
  }
}

void owntracksInit() {
  if (!locMutex) locMutex = xSemaphoreCreateMutex();
  if (encrypted() && sodium_init() < 0) logMsg("owntracks: libsodium init failed");
}

void owntracksStart() {
  if (!MQTT_URI[0] || clientStarted || !netUp()) return;
  static uint32_t lastTryMs;   // called every main loop pass: a client that fails tries again in 30 s
  if (lastTryMs && nowMs() - lastTryMs < 30000) return;
  lastTryMs = nowMs() | 1;
  if (!locMutex) locMutex = xSemaphoreCreateMutex();

  esp_mqtt_client_config_t cfg = {};
  if (strstr(MQTT_URI, "://")) {
    cfg.broker.address.uri = MQTT_URI;
  } else {
    // a bare host name ("xxxx.emqxsl.com" as the console shows it): TLS on 8883
    cfg.broker.address.hostname = MQTT_URI;
    cfg.broker.address.transport = MQTT_TRANSPORT_OVER_SSL;
    cfg.broker.address.port = 8883;
  }
  cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.credentials.username = MQTT_USER;
  cfg.credentials.authentication.password = MQTT_PASS;
  cfg.network.timeout_ms = 10000;
  cfg.session.keepalive = 120;   // seconds; the phone publishes, the board listens (and asks, owntracksAskIfStale)
  // default 6144; peak 2.85 KB with the TLS handshake (bench usage, 2026-10-27),
  // plus room for opening an encrypted message (secretbox, base64 on the heap)
  cfg.task.stack_size = OWNTRACKS_SECRET[0] ? 5120 : 4096;
  cfg.buffer.size = 512;         // default 1024; bigger messages are joined in MQTT_EVENT_DATA
  cfg.buffer.out_size = 256;     // CONNECT, SUBSCRIBE, PINGREQ and the ~130 B reportLocation command

  mqttClient = esp_mqtt_client_init(&cfg);
  if (!mqttClient) {
    logMsg("owntracks: failed to initialize MQTT client");
    return;
  }

  esp_mqtt_client_register_event(mqttClient, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, mqttEventHandler, nullptr);
  if (esp_mqtt_client_start(mqttClient) != ESP_OK) {   // no task (low RAM)
    logMsg("owntracks: MQTT client did not start, retrying in 30 s");
    esp_mqtt_client_destroy(mqttClient);
    mqttClient = nullptr;
    return;
  }
  clientStarted = true;
  logMsg("owntracks: MQTT client started");
}

bool owntracksEnabled() {
  return MQTT_URI[0] != '\0';
}

bool owntracksValid() {
  if (!hasLocation) return false;
  if (locMutex) xSemaphoreTake(locMutex, portMAX_DELAY);
  uint32_t tst = phoneTst;
  uint32_t rxMs = phoneReceivedMs;
  if (locMutex) xSemaphoreGive(locMutex);

  time_t now = time(nullptr);
  if (now >= 1700000000) {
    int64_t diff = (int64_t)now - (int64_t)tst;
    return (diff >= -120 && diff < OWNTRACKS_MAX_AGE_S);
  }
  return (nowMs() - rxMs < OWNTRACKS_MAX_AGE_S * 1000);
}

bool owntracksGetLocation(double *lat, double *lon, uint32_t *tst, uint32_t *acc) {
  if (!hasLocation) return false;
  if (locMutex) xSemaphoreTake(locMutex, portMAX_DELAY);
  if (lat) *lat = phoneLat;
  if (lon) *lon = phoneLon;
  if (tst) *tst = phoneTst;
  if (acc) *acc = phoneAcc;
  if (locMutex) xSemaphoreGive(locMutex);
  return true;
}

uint32_t owntracksAgeSec() {
  if (!hasLocation) return UINT32_MAX;
  if (locMutex) xSemaphoreTake(locMutex, portMAX_DELAY);
  uint32_t tst = phoneTst;
  uint32_t rxMs = phoneReceivedMs;
  if (locMutex) xSemaphoreGive(locMutex);

  time_t now = time(nullptr);
  if (now >= 1700000000 && (time_t)tst <= now) {
    return (uint32_t)(now - tst);
  }
  return (nowMs() - rxMs) / 1000;
}

double owntracksDistanceTo(double homeLat, double homeLon) {
  if (!hasLocation) return -1.0;
  if (locMutex) xSemaphoreTake(locMutex, portMAX_DELAY);
  double pLat = phoneLat;
  double pLon = phoneLon;
  if (locMutex) xSemaphoreGive(locMutex);

  return haversineKm(homeLat, homeLon, pLat, pLon);
}

// Home under OWNTRACKS_HOME_KM, away over OWNTRACKS_AWAY_KM, the last answer
// between (a fix wandering around the line doesn't flip it, each flip costing
// an OpenWeather call and a fresh "where you are" warning). A fix vaguer than
// OWNTRACKS_SWITCH_ACC_M doesn't switch either way.
bool owntracksIsMerged(double homeLat, double homeLon) {
  if (!owntracksEnabled() || !owntracksValid()) return true;
  static volatile bool merged = true;   // callers in several tasks: a bool, set whole
  double d = owntracksDistanceTo(homeLat, homeLon);
  uint32_t acc = 0;
  owntracksGetLocation(nullptr, nullptr, nullptr, &acc);
  if (d < 0.0 || acc > OWNTRACKS_SWITCH_ACC_M) return merged;
  if (merged && d >= OWNTRACKS_AWAY_KM) merged = false;
  else if (!merged && d < OWNTRACKS_HOME_KM) merged = true;
  return merged;
}

// A phone that sits still reports nothing (OwnTracks publishes on movement),
// so after OWNTRACKS_ASK_AFTER_S without a fix the board publishes
// OwnTracks' remote command reportLocation to <device topic>/cmd, again every
// OWNTRACKS_ASK_AFTER_S until one arrives. The command is retained, so a phone
// that is not connected right then gets it when it next connects; the next
// fix clears it. The app answers when remote commands are on (reliable on
// Android, best effort on iOS); a fix stays valid OWNTRACKS_MAX_AGE_S either way.
void owntracksAskNow() {
  if (!clientStarted || !connected || !devTopic[0] || !hasLocation) return;
  lastAskMs = nowMs() | 1;
  char topic[96];
  snprintf(topic, sizeof topic, "%s/cmd", devTopic);
  // queued for the MQTT task: publish writes to the socket here, on the main
  // loop, for up to the network timeout
  const char *cmd = "{\"_type\":\"cmd\",\"action\":\"reportLocation\"}";
  char *sealed = encrypted() ? encryptJson(cmd) : nullptr;   // the app with a key set opens it like its own
  int id = encrypted() && !sealed ? -1 : esp_mqtt_client_enqueue(mqttClient, topic, sealed ? sealed : cmd, 0, 1, 1, true);
  free(sealed);
  if (id >= 0) askPending = true;
  logMsg("owntracks: fix %lu min old, asked the phone for its location%s", (unsigned long)(owntracksAgeSec() / 60),
         id < 0 ? " (publish failed)" : "");
}

void owntracksAskIfStale() {
  if (owntracksAgeSec() < OWNTRACKS_ASK_AFTER_S) return;
  if (lastAskMs && nowMs() - lastAskMs < OWNTRACKS_ASK_AFTER_S * 1000UL) return;
  owntracksAskNow();
}
