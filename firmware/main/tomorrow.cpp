#include "tomorrow.h"

#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <nvs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "net.h"
#include "weather.h"

static uint32_t callDay;              // UTC day of callsToday
static uint32_t callsToday;
static uint32_t callHour;             // UTC hour of callsHour
static uint32_t callsHour;
static uint32_t backoffUntil;         // UTC: a rejected key or a rate limit pauses calls
static char lastErr[64];

bool tioEnabled() { return TOMORROW_API_KEY[0] != 0; }
uint32_t tioCallsToday() { return callsToday; }
uint32_t tioCallsThisHour() { return callHour == (uint32_t)time(nullptr) / 3600 ? callsHour : 0; }
const char *tioLastError() { return lastErr; }

void tioSaveCount() {
  nvs_handle_t h;
  if (nvs_open("wx", NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_u32(h, "t_day", callDay);
  nvs_set_u32(h, "t_calls", callsToday);
  nvs_set_u32(h, "t_hour", callHour);
  nvs_set_u32(h, "t_hcalls", callsHour);
  nvs_commit(h);
  nvs_close(h);
}

void tioInit() {
  nvs_handle_t h;
  if (nvs_open("wx", NVS_READONLY, &h) != ESP_OK) return;
  nvs_get_u32(h, "t_day", &callDay);
  nvs_get_u32(h, "t_calls", &callsToday);
  nvs_get_u32(h, "t_hour", &callHour);
  nvs_get_u32(h, "t_hcalls", &callsHour);
  nvs_close(h);
}

static bool rollCounts(uint32_t now) {
  if (now < 1700000000) return false;   // no clock: no budget to check against
  uint32_t hour = now / 3600;
  if (hour != callHour) {
    callHour = hour;
    callsHour = 0;
  }
  uint32_t day = now / 86400;
  if (day != callDay) {
    callDay = day;
    callsToday = 0;
    tioSaveCount();
  }
  return true;
}

bool tioBudgetLeft() {
  uint32_t now = (uint32_t)time(nullptr);
  if (!tioEnabled() || !rollCounts(now)) return false;
  if (backoffUntil && now < backoffUntil) return false;
  // paced over the UTC day like OpenWeather (owmBudgetLeft)
  uint32_t share = (uint32_t)((uint64_t)TIO_DAILY_CALLS * (now % 86400) / 86400) + 30;
  return callsToday < TIO_DAILY_CALLS && callsToday < share && callsHour < TIO_HOURLY_CALLS;
}

struct TioBuf { char *buf; size_t cap, len; };

static esp_err_t onData(esp_http_client_event_t *evt) {
  TioBuf *b = (TioBuf *)evt->user_data;
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
static bool tioGet(const char *url, char *buf, size_t cap) {
  TioBuf b = {buf, cap, 0};
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
  callsToday++;
  callsHour++;
  netNoteTraffic();
  if (callsToday % 5 == 0) tioSaveCount();   // a reboot can't lose enough to pass the real 25 an hour
  uint32_t now = (uint32_t)time(nullptr);
  if (status == 401) {
    snprintf(lastErr, sizeof lastErr, "key rejected (401)");
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

bool tioFetchMinutely(double lat, double lon, char *buf, size_t cap, OwmMinutely *out) {
  if (!tioBudgetLeft()) return false;
  char url[256];
  snprintf(url, sizeof url, "https://api.tomorrow.io/v4/timelines?location=%.4f,%.4f&fields=precipitationIntensity"
           "&timesteps=1m&startTime=now&endTime=nowPlus1h&units=metric&apikey=%s", lat, lon, TOMORROW_API_KEY);
  if (!tioGet(url, buf, cap)) return false;

  // {"data":{"timelines":[{..."intervals":[{"startTime":"2026-10-31T00:23:00Z",
  // "values":{"precipitationIntensity":0}},...]}]}}, 61 of them a minute apart.
  // Read as text: in cJSON each minute is four nodes of internal RAM for one number
  static const char KEY[] = "\"precipitationIntensity\":";
  static const char START[] = "\"startTime\":\"";
  const char *p = strstr(buf, "\"intervals\"");
  p = p ? strstr(p, START) : nullptr;
  struct tm t = {};
  if (!p || sscanf(p + sizeof START - 1, "%d-%d-%dT%d:%d", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min) != 5) {
    snprintf(lastErr, sizeof lastErr, "no minutes in the answer");
    return false;
  }
  t.tm_year -= 1900;
  t.tm_mon -= 1;
  OwmMinutely m = {};
  m.lat = lat;
  m.lon = lon;
  m.fetchedUnix = (uint32_t)time(nullptr);
  m.dt0 = (uint32_t)utcMkTime(&t);
  while (m.n < 60 && (p = strstr(p, KEY))) {
    p += sizeof KEY - 1;
    m.mm[m.n++] = strtof(p, nullptr);
  }
  if (!m.n) {
    snprintf(lastErr, sizeof lastErr, "no minutes in the answer");
    return false;
  }
  m.valid = true;
  *out = m;
  return true;
}
