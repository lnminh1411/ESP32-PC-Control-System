#include "owntracks.h"

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <math.h>
#include <mqtt_client.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "log.h"
#include "net.h"
#include "secrets_or_example.h"
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

static void mqttEventHandler(void *, esp_event_base_t, int32_t event_id, void *event_data) {
  esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
  switch (event_id) {
    case MQTT_EVENT_CONNECTED: {
      errLogged = false;
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
      cJSON *root = cJSON_ParseWithLength(msg, msgLen);
      if (bigMsg) {
        free(bigMsg);
        bigMsg = nullptr;
      }
      if (!root) break;

      cJSON *type = cJSON_GetObjectItem(root, "_type");
      if (type && cJSON_IsString(type) && !strcmp(type->valuestring, "location")) {
        cJSON *jlat = cJSON_GetObjectItem(root, "lat");
        cJSON *jlon = cJSON_GetObjectItem(root, "lon");
        cJSON *jtst = cJSON_GetObjectItem(root, "tst");
        cJSON *jacc = cJSON_GetObjectItem(root, "acc");

        if (jlat && jlon && jtst && cJSON_IsNumber(jlat) && cJSON_IsNumber(jlon)) {
          if (locMutex) xSemaphoreTake(locMutex, portMAX_DELAY);
          phoneLat = jlat->valuedouble;
          phoneLon = jlon->valuedouble;
          phoneTst = (uint32_t)(jtst->valuedouble > 0.0 ? jtst->valuedouble : 0.0);
          phoneAcc = (jacc && cJSON_IsNumber(jacc)) ? (uint32_t)jacc->valuedouble : 0;
          phoneReceivedMs = nowMs();
          hasLocation = true;
          if (locMutex) xSemaphoreGive(locMutex);
          logMsg("owntracks: location updated (acc %u m)", (unsigned)phoneAcc);
        }
      }
      cJSON_Delete(root);
      break;
    }
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
}

void owntracksStart() {
  if (!MQTT_URI[0] || clientStarted || !netUp()) return;
  if (!locMutex) locMutex = xSemaphoreCreateMutex();

  esp_mqtt_client_config_t cfg = {};
  cfg.broker.address.uri = MQTT_URI;
  cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.credentials.username = MQTT_USER;
  cfg.credentials.authentication.password = MQTT_PASS;
  cfg.network.timeout_ms = 10000;
  cfg.session.keepalive = 120;   // seconds; the phone publishes, the board only listens
  cfg.task.stack_size = 5120;    // default 6144; the TLS handshake runs on this task, check `bench usage`
  cfg.buffer.size = 512;         // default 1024; bigger messages are joined in MQTT_EVENT_DATA
  cfg.buffer.out_size = 256;     // only CONNECT, SUBSCRIBE and PINGREQ go out

  mqttClient = esp_mqtt_client_init(&cfg);
  if (!mqttClient) {
    logMsg("owntracks: failed to initialize MQTT client");
    return;
  }

  esp_mqtt_client_register_event(mqttClient, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, mqttEventHandler, nullptr);
  esp_mqtt_client_start(mqttClient);
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
    return (diff >= -120 && diff < 2 * 3600); // valid if < 2 hours old
  }
  return (nowMs() - rxMs < 2 * 3600 * 1000);
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

bool owntracksIsMerged(double homeLat, double homeLon) {
  if (!owntracksEnabled() || !owntracksValid()) return true;
  double d = owntracksDistanceTo(homeLat, homeLon);
  return (d >= 0.0 && d < 1.0);
}
