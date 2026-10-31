#include "owm.h"

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <nvs.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "log.h"
#include "net.h"
#include "settings_or_example.h"

static const float RAIN_MMH = OWM_RAIN_MMH;   // what counts as rain: the Open-Meteo nowcast's threshold

static uint32_t callDay;              // UTC day of callsToday
static uint32_t callsToday;
static uint32_t backoffUntil;         // UTC: a rejected key or a rate limit pauses calls
static char lastErr[64];

bool owmEnabled() { return OWM_API_KEY[0] != 0; }
uint32_t owmCallsToday() { return callsToday; }
const char *owmLastError() { return lastErr; }

void owmSaveCount() {
  nvs_handle_t h;
  if (nvs_open("wx", NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_u32(h, "o_day", callDay);
  nvs_set_u32(h, "o_calls", callsToday);
  nvs_commit(h);
  nvs_close(h);
}

void owmInit() {
  nvs_handle_t h;
  if (nvs_open("wx", NVS_READONLY, &h) != ESP_OK) return;
  nvs_get_u32(h, "o_day", &callDay);
  nvs_get_u32(h, "o_calls", &callsToday);
  nvs_close(h);
}

static bool rollDay(uint32_t now) {
  if (now < 1700000000) return false;   // no clock: no budget to check against
  uint32_t day = now / 86400;
  if (day != callDay) {
    callDay = day;
    callsToday = 0;
    owmSaveCount();
  }
  return true;
}

bool owmBudgetLeft() {
  uint32_t now = (uint32_t)time(nullptr);
  if (!owmEnabled() || !rollDay(now)) return false;
  if (backoffUntil && now < backoffUntil) return false;
  // paced over the UTC day (07:00 local): no more than the day's share so far
  // plus 60, so a rainy morning can't spend the night's calls (the day ends
  // at 07:00 here, right when an early shower matters)
  uint32_t share = (uint32_t)((uint64_t)OWM_DAILY_CALLS * (now % 86400) / 86400) + 60;
  return callsToday < OWM_DAILY_CALLS && callsToday < share;
}

struct OwmBuf { char *buf; size_t cap, len; };

static esp_err_t onData(esp_http_client_event_t *evt) {
  OwmBuf *b = (OwmBuf *)evt->user_data;
  if (evt->event_id == HTTP_EVENT_ON_DATA && b && b->len + 1 < b->cap) {
    size_t n = evt->data_len;
    if (b->len + n >= b->cap) n = b->cap - 1 - b->len;
    memcpy(b->buf + b->len, evt->data, n);
    b->len += n;
    b->buf[b->len] = 0;
  }
  return ESP_OK;
}

// One counted call; the URL carries the key, so it is never logged
static bool owmGet(const char *url, char *buf, size_t cap) {
  OwmBuf b = {buf, cap, 0};
  buf[0] = 0;
  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.timeout_ms = 10000;
  cfg.event_handler = onData;
  cfg.user_data = &b;
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (!c) { snprintf(lastErr, sizeof lastErr, "no HTTP client"); return false; }
  esp_err_t err = esp_http_client_perform(c);
  int status = esp_http_client_get_status_code(c);
  esp_http_client_cleanup(c);
  if (err != ESP_OK) {
    snprintf(lastErr, sizeof lastErr, "%s", esp_err_to_name(err));
    return false;
  }
  callsToday++;   // OpenWeather counts every answered request
  netNoteTraffic();
  if (callsToday % 10 == 0) owmSaveCount();
  uint32_t now = (uint32_t)time(nullptr);
  if (status == 401) {
    snprintf(lastErr, sizeof lastErr, "key rejected (401): subscribed to One Call 4.0?");
    backoffUntil = now + 30 * 60;
    return false;
  }
  if (status == 429) {
    snprintf(lastErr, sizeof lastErr, "rate limited (429)");
    backoffUntil = now + 15 * 60;
    return false;
  }
  if (status < 200 || status >= 300 || !b.len) {
    snprintf(lastErr, sizeof lastErr, "HTTP %d", status);
    return false;
  }
  lastErr[0] = 0;
  return true;
}

bool owmFetchMinutely(double lat, double lon, char *buf, size_t cap, OwmMinutely *out) {
  if (!owmBudgetLeft()) return false;
  char url[256];
  snprintf(url, sizeof url, "https://api.openweathermap.org/data/4.0/onecall/timeline/1min?lat=%.4f&lon=%.4f&appid=%s",
           lat, lon, OWM_API_KEY);
  if (!owmGet(url, buf, cap)) return false;

  cJSON *root = cJSON_Parse(buf);
  cJSON *data = root ? cJSON_GetObjectItem(root, "data") : nullptr;
  if (!cJSON_IsArray(data) || !cJSON_GetArraySize(data)) {
    snprintf(lastErr, sizeof lastErr, "no minutes in the answer");
    cJSON_Delete(root);
    return false;
  }
  OwmMinutely m = {};
  m.lat = lat;
  m.lon = lon;
  m.fetchedUnix = (uint32_t)time(nullptr);
  cJSON *rec;
  cJSON_ArrayForEach(rec, data) {
    if (m.n >= 60) break;
    cJSON *dt = cJSON_GetObjectItem(rec, "dt"), *p = cJSON_GetObjectItem(rec, "precipitation");
    if (!m.n) {
      m.dt0 = cJSON_IsNumber(dt) ? (uint32_t)dt->valuedouble : m.fetchedUnix;
      cJSON *al = cJSON_GetObjectItem(rec, "alerts"), *id;
      cJSON_ArrayForEach(id, al) {
        if (m.nAlerts < 4 && cJSON_IsString(id))
          snprintf(m.alerts[m.nAlerts++], sizeof m.alerts[0], "%s", id->valuestring);
      }
    }
    m.mm[m.n++] = cJSON_IsNumber(p) ? (float)p->valuedouble : 0.0f;
  }
  cJSON_Delete(root);
  m.valid = true;
  *out = m;
  return true;
}

bool owmFetchAlert(const char *id, char *buf, size_t cap, OwmAlert *out) {
  if (!owmBudgetLeft()) return false;
  char url[256];
  snprintf(url, sizeof url, "https://api.openweathermap.org/data/4.0/onecall/alert/%.40s?appid=%s", id, OWM_API_KEY);
  if (!owmGet(url, buf, cap)) return false;
  cJSON *root = cJSON_Parse(buf);
  cJSON *a = root ? cJSON_GetObjectItem(root, "data") : nullptr;
  if (cJSON_IsArray(a)) a = cJSON_GetArrayItem(a, 0);
  if (!a) a = root;
  OwmAlert r = {};
  cJSON *v;
  if ((v = cJSON_GetObjectItem(a, "event")) && cJSON_IsString(v)) snprintf(r.event, sizeof r.event, "%s", v->valuestring);
  if ((v = cJSON_GetObjectItem(a, "sender_name")) && cJSON_IsString(v)) snprintf(r.sender, sizeof r.sender, "%s", v->valuestring);
  if ((v = cJSON_GetObjectItem(a, "start")) && cJSON_IsNumber(v)) r.startUnix = (uint32_t)v->valuedouble;
  if ((v = cJSON_GetObjectItem(a, "end")) && cJSON_IsNumber(v)) r.endUnix = (uint32_t)v->valuedouble;
  // description: [{"language": "en", "description": "..."}], or a plain string
  v = cJSON_GetObjectItem(a, "description");
  if (cJSON_IsArray(v)) v = cJSON_GetObjectItem(cJSON_GetArrayItem(v, 0), "description");
  if (cJSON_IsString(v)) snprintf(r.text, sizeof r.text, "%s", v->valuestring);
  cJSON_Delete(root);
  if (!r.event[0]) {
    snprintf(lastErr, sizeof lastErr, "alert without an event name");
    return false;
  }
  *out = r;
  return true;
}

bool owmRainSummary(const OwmMinutely &m, uint32_t nowUnix, OwmRain *out) {
  *out = {};
  if (!m.valid || !m.n) return false;
  // minutes already past drop off the front
  int skip = nowUnix > m.dt0 ? (int)((nowUnix - m.dt0) / 60) : 0;
  if (skip >= m.n) return false;
  out->coversUntil = m.dt0 + (uint32_t)m.n * 60;
  int first = -1, last = -1, run = 0;
  for (int i = skip; i < m.n; i++) {
    if (m.mm[i] < RAIN_MMH) { run = 0; continue; }
    if (first < 0) first = i;
    last = i;
    if (++run > out->run) out->run = (uint8_t)run;
    if (m.mm[i] > out->peak) out->peak = m.mm[i];
  }
  if (first < 0) return true;   // dry for the rest of the hour
  out->sees = true;
  out->rainingNow = first == skip;
  out->startUnix = out->rainingNow ? nowUnix : m.dt0 + (uint32_t)first * 60;
  if (last + 10 < m.n) out->endUnix = m.dt0 + (uint32_t)(last + 1) * 60;
  return true;
}
