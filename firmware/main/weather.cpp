#include "weather.h"

#include <cJSON.h>
#include <ctype.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <math.h>
#include <nvs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "board_time.h"
#include "cyclone.h"
#include "discord.h"
#include "log.h"
#include "net.h"
#include "nowcast.h"
#include "owm.h"
#include "owntracks.h"
#include "pushover.h"
#include "secrets_or_example.h"
#include "sys.h"
#include "views.h"

static WeatherNow now_;
static RainAlert rainHome;
static RainAlert rainPhone;
static Cyclone cyclone_;

// Next hours from the forecast: storm signs and the rain's end past the nowcast
struct HourWx {
  uint32_t unix;   // end of the hour (UTC); its values cover the hour before
  float mm;        // precipitation in that hour
  uint8_t code;    // WMO weather code
  uint8_t gust;    // km/h, capped at 255
};
static HourWx hours_[12];
static int nHours_;
static uint32_t forecastAtMs;   // last good forecast

// Cyclone tracks of storms near home (PSRAM), refreshed per GDACS advisory
static CycloneTrack *tracks_;
static constexpr int MAX_TRACKS = 3;
static int trackShown = -1;     // index in tracks_ behind cyclone_

// `weather demo`: polls leave the demo data alone and nothing is posted
static uint32_t demoUntilMs;
static bool demoOn() { return demoUntilMs && (int32_t)(demoUntilMs - nowMs()) > 0; }

// Discord colours: approaching > 20 min, close or here, cleared
static constexpr uint32_t COL_YELLOW = 0xF1C40F;
static constexpr uint32_t COL_RED = 0xE74C3C;
static constexpr uint32_t COL_GREEN = 0x2ECC71;
static constexpr uint32_t COL_BLUE = 0x3498DB;

static SemaphoreHandle_t wxMutex;
static TaskHandle_t wxTaskHandle;
static volatile bool wxDirty;

// Location configuration
static double homeLat;
static double homeLon;
static char homeName[48] = "Home";
static bool homeConfigured;

// Nowcast engines
static NowcastEngine homeNowcast;
static NowcastEngine phoneNowcast;

// Budget state (<= 10000 daily, hard cap 9500)
static uint32_t currentUtcDay;
static uint32_t budgetTotalCalls;
static uint32_t budgetHomeCalls;
static uint32_t budgetPhoneCalls;
static uint32_t budgetFcCalls;
static bool homeThrottled;
static bool phoneThrottled;

// Phone notification state (1 message per rain event, re-arm after 60 min dry)
static bool phoneAlertSent;
static uint32_t phoneDryStartMs;

// Error logging masks (log once per kind)
enum ErrMask : uint16_t {
  ERR_FC_HTTP     = 1 << 0,
  ERR_FC_PARSE    = 1 << 1,
  ERR_AQI_HTTP    = 1 << 2,
  ERR_AQI_PARSE   = 1 << 3,
  ERR_HOME_HTTP   = 1 << 4,
  ERR_HOME_PARSE  = 1 << 5,
  ERR_PHONE_HTTP  = 1 << 6,
  ERR_PHONE_PARSE = 1 << 7,
  ERR_TC_HTTP     = 1 << 8,
  ERR_TC_TRACK    = 1 << 9,
  ERR_OWM         = 1 << 10,
};
static uint16_t loggedErrors;
static char lastError[96];               // shown by `weather show`
static esp_err_t lastHttpErr;
static volatile bool homeChanged;        // new home: poll right away
static volatile bool testAlertPending;   // sent from the weather task: HTTPS must not block the main loop
static volatile bool cycloneRefresh;     // `weather storms`: fetch GDACS again now

static void logErrorOnce(uint16_t mask, const char *msg) {
  snprintf(lastError, sizeof lastError, "%s", msg);
  if (!(loggedErrors & mask)) {
    loggedErrors |= mask;
    logMsg("%s", msg);
  }
}

static void clearError(uint16_t mask) {
  loggedErrors &= ~mask;
}

const WeatherNow &weatherNow() { return now_; }
const RainAlert &rainAlertHome() { return rainHome; }
const RainAlert &rainAlertPhone() { return rainPhone; }

const char *rainSrcName(const RainAlert &a) {
  uint8_t s = a.src & (RAIN_SRC_OPEN_METEO | RAIN_SRC_OPENWEATHER);
  return s == RAIN_SRC_OPENWEATHER ? "OpenWeather" : s == RAIN_SRC_OPEN_METEO ? "Open-Meteo" : s ? "both" : "--";
}

// Open-Meteo's own nowcast results; rainHome / rainPhone are these merged
// with OpenWeather's minute-by-minute hour (owm.h) for the place it covers
static RainAlert omHome, omPhone;
static OwmMinutely owmLast;          // the last good OpenWeather hour
static bool owmOnPhone;              // ...for the phone, not home
static char owmSeen[8][40];          // alert IDs already posted
static uint8_t owmSeenNext;
const Cyclone &weatherCyclone() { return cyclone_; }

const char *wxTierName(WxTier t) {
  switch (t) {
    case TIER_LIGHT_RAIN: return "Light Rain";
    case TIER_HEAVY_SHOWER: return "Heavy Shower";
    case TIER_STORM: return "Storm";
    case TIER_TYPHOON: return "Typhoon";
    case TIER_SUPER_TYPHOON: return "Super Typhoon";
    default: return "Rain";
  }
}

// struct tm read as UTC -> unix (newlib has no timegm)
static int64_t utcMkTime(const struct tm *t) {
  int y = t->tm_year + 1900, m = t->tm_mon + 1;
  y -= m <= 2;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  int64_t yoe = y - era * 400;
  int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + t->tm_mday - 1;
  int64_t days = era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
  return days * 86400 + t->tm_hour * 3600 + t->tm_min * 60 + t->tm_sec;
}

static bool clockValid(time_t *now) {
  *now = time(nullptr);
  return *now >= 1700000000;
}

int rainEtaMin(const RainAlert &a) {
  if (!a.active && !a.raining) return -1;
  if (a.raining) return 0;
  time_t now;
  if (!a.startUnix || !clockValid(&now)) return a.etaMin;
  long d = ((long)a.startUnix - (long)now + 59) / 60;
  return d > 0 ? (int)d : 0;
}

int rainLeftMin(const RainAlert &a) {
  time_t now;
  if (!a.endUnix || !clockValid(&now)) return -1;
  long d = ((long)a.endUnix - (long)now + 59) / 60;
  return d > 0 ? (int)d : 0;
}

static WxIcon wmoToWxIcon(int code) {
  if (code <= 1) return WX_CLEAR;
  if (code == 2) return WX_PARTLY;
  if (code == 3) return WX_CLOUDY;
  if (code == 45 || code == 48) return WX_FOG;
  if (code >= 51 && code <= 57) return WX_DRIZZLE;
  if (code >= 61 && code <= 67) return WX_RAIN;
  if (code >= 71 && code <= 77) return WX_SNOW;
  if (code >= 80 && code <= 82) return WX_SHOWERS;
  if (code >= 85 && code <= 86) return WX_SNOW;
  if (code >= 95 && code <= 99) return WX_STORM;
  return WX_CLOUDY;
}

static const char *calcDayName(int y, int m, int d) {
  static const char *names[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y -= 1;
  int dow = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
  return names[dow];
}

// mode: 0 dry, 1 heavy shower in 25 min, 2 typhoon at home
static void loadDemo(int mode) {
  WeatherNow w = {};
  w.valid = true;
  snprintf(w.place, sizeof(w.place), "%s", homeName[0] ? homeName : "Home");
  strcpy(w.updated, "21:40");
  w.temp = 29.0f;
  w.tmax = 33;
  w.tmin = 25;
  w.icon = WX_RAIN;
  w.isDay = true;
  w.windKmh = 12.0f;
  w.windFromDeg = 225;
  w.aqi = 42;
  const WeatherDay days[3] = {{"Mon", WX_RAIN, 33, 24}, {"Tue", WX_CLEAR, 33, 24}, {"Wed", WX_SHOWERS, 31, 24}};
  memcpy(w.days, days, sizeof days);

  if (wxMutex) xSemaphoreTake(wxMutex, portMAX_DELAY);
  now_ = w;
  rainHome = {};
  cyclone_ = {};
  time_t now;
  bool clock = clockValid(&now);
  if (mode == 1) {
    rainHome.active = true;
    rainHome.etaMin = 25;
    rainHome.startUnix = clock ? (uint32_t)now + 25 * 60 : 0;
    rainHome.endUnix = clock ? (uint32_t)now + 95 * 60 : 0;
    rainHome.mmh = 9.2f;
    rainHome.tier = TIER_HEAVY_SHOWER;
    rainHome.src = RAIN_SRC_OPEN_METEO | RAIN_SRC_OPENWEATHER;
    strcpy(rainHome.from, "NW");
  } else if (mode == 2) {
    cyclone_.tracked = cyclone_.active = true;
    strcpy(cyclone_.name, "KAJIKI");
    cyclone_.distKm = 80;
    cyclone_.closestKm = 45;
    cyclone_.windKmh = 150;
  } else {
    rainHome.dryKm = 40;
  }
  if (wxMutex) xSemaphoreGive(wxMutex);
  wxDirty = true;
}

// HTTP buffer in PSRAM
struct HttpBuffer {
  char *buf;
  size_t cap;
  size_t len;
};

static esp_err_t httpEventCb(esp_http_client_event_t *evt) {
  if (evt->event_id == HTTP_EVENT_ON_DATA) {
    HttpBuffer *hb = (HttpBuffer *)evt->user_data;
    if (hb && hb->buf && hb->len + 1 < hb->cap) {
      size_t copyLen = evt->data_len;
      if (hb->len + copyLen >= hb->cap) {
        copyLen = hb->cap - 1 - hb->len;
      }
      memcpy(hb->buf + hb->len, evt->data, copyLen);
      hb->len += copyLen;
      hb->buf[hb->len] = '\0';
    }
  }
  return ESP_OK;
}

// timeoutMs: GDACS can take over 10 s to start sending a large track
static bool httpGet(const char *url, char *buf, size_t cap, int *outStatus, int timeoutMs = 10000) {
  HttpBuffer hb = {buf, cap, 0};
  buf[0] = '\0';

  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.timeout_ms = timeoutMs;
  cfg.buffer_size_tx = 1024;   // the nowcast's 17-point URL is ~550 bytes
  cfg.event_handler = httpEventCb;
  cfg.user_data = &hb;

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client) return false;

  esp_err_t err = esp_http_client_perform(client);
  lastHttpErr = err;
  int status = esp_http_client_get_status_code(client);
  if (outStatus) *outStatus = status;
  esp_http_client_cleanup(client);
  if (err == ESP_OK && status > 0) netNoteTraffic();

  return (err == ESP_OK && status >= 200 && status < 300 && hb.len > 0);
}

static void saveBudgetNvs() {
  time_t now = time(nullptr);
  if (now < 1700000000) return;
  uint32_t curDay = (uint32_t)(now / 86400);

  nvs_handle_t h;
  if (nvs_open("wx", NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_u32(h, "b_day", curDay);
    nvs_set_u32(h, "b_tot", budgetTotalCalls);
    nvs_set_u32(h, "b_home", budgetHomeCalls);
    nvs_set_u32(h, "b_phone", budgetPhoneCalls);
    nvs_set_u32(h, "b_fc", budgetFcCalls);
    nvs_commit(h);
    nvs_close(h);
  }
}

static bool checkBudgetNowcast(bool isPhoneEngine) {
  time_t now = time(nullptr);
  uint32_t curDay = (now >= 1700000000) ? (uint32_t)(now / 86400) : 0;

  if (curDay != 0 && curDay != currentUtcDay) {
    currentUtcDay = curDay;
    budgetTotalCalls = 0;
    budgetHomeCalls = 0;
    budgetPhoneCalls = 0;
    budgetFcCalls = 0;
    homeThrottled = false;
    phoneThrottled = false;
    saveBudgetNvs();
  }

  if (budgetTotalCalls + 17 > 9500) {
    return false;
  }

  double share = 0.0;
  bool merged = owntracksIsMerged(homeLat, homeLon);
  if (merged) {
    share = isPhoneEngine ? 0.0 : 1.0;
  } else {
    share = isPhoneEngine ? 0.65 : 0.35;
  }

  if (share <= 0.0) return false;

  double elapsedFraction = 1.0;
  if (now >= 1700000000) {
    uint32_t secToday = (uint32_t)(now % 86400);
    elapsedFraction = (double)secToday / 86400.0;
  }

  uint32_t usedByEngine = isPhoneEngine ? budgetPhoneCalls : budgetHomeCalls;
  double allowed = share * (elapsedFraction * 9100.0 + 300.0);

  if ((double)(usedByEngine + 17) <= allowed) {
    if (isPhoneEngine) phoneThrottled = false;
    else homeThrottled = false;
    return true;
  }

  if (isPhoneEngine) {
    if (!phoneThrottled) {
      phoneThrottled = true;
      logMsg("weather: phone engine throttled by budget (%u used)", (unsigned)budgetPhoneCalls);
    }
  } else {
    if (!homeThrottled) {
      homeThrottled = true;
      logMsg("weather: home engine throttled by budget (%u used)", (unsigned)budgetHomeCalls);
    }
  }
  return false;
}

static bool checkBudgetForecast() {
  if (budgetTotalCalls + 1 > 9500) return false;
  return true;
}

static void fetchForecast(char *buf, size_t cap) {
  char url[320];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.5f&longitude=%.5f"
           "&current=temperature_2m,weather_code,is_day,wind_speed_10m,wind_direction_10m"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min"
           "&hourly=precipitation,weather_code,wind_gusts_10m&forecast_hours=12"
           "&timezone=auto&forecast_days=4",
           homeLat, homeLon);

  int status = 0;
  if (!httpGet(url, buf, cap, &status)) {
    char err[80];
    snprintf(err, sizeof(err), "weather: forecast HTTP failed (%d, %s)", status, esp_err_to_name(lastHttpErr));
    logErrorOnce(ERR_FC_HTTP, err);
    return;
  }
  clearError(ERR_FC_HTTP);
  budgetTotalCalls++;
  budgetFcCalls++;

  cJSON *root = cJSON_Parse(buf);
  if (!root) {
    logErrorOnce(ERR_FC_PARSE, "weather: forecast JSON parse failed");
    return;
  }
  clearError(ERR_FC_PARSE);

  cJSON *tzSec = cJSON_GetObjectItem(root, "utc_offset_seconds");
  int32_t utcOffset = 0;
  if (tzSec && cJSON_IsNumber(tzSec)) {
    utcOffset = tzSec->valueint;
    boardTimeSetUtcOffset(utcOffset);
  }

  // hourly: local "YYYY-MM-DDTHH:MM" stamps, each value covering the hour before
  cJSON *hourly = cJSON_GetObjectItem(root, "hourly");
  if (hourly) {
    cJSON *ht = cJSON_GetObjectItem(hourly, "time");
    cJSON *hp = cJSON_GetObjectItem(hourly, "precipitation");
    cJSON *hc = cJSON_GetObjectItem(hourly, "weather_code");
    cJSON *hg = cJSON_GetObjectItem(hourly, "wind_gusts_10m");
    int n = ht ? cJSON_GetArraySize(ht) : 0;
    HourWx hw[12] = {};
    int k = 0;
    for (int i = 0; i < n && k < 12; i++) {
      const char *ts = cJSON_GetArrayItem(ht, i)->valuestring;
      int y, mo, d, h, mi;
      if (!ts || sscanf(ts, "%d-%d-%dT%d:%d", &y, &mo, &d, &h, &mi) != 5) continue;
      struct tm t = {};
      t.tm_year = y - 1900;
      t.tm_mon = mo - 1;
      t.tm_mday = d;
      t.tm_hour = h;
      t.tm_min = mi;
      // the stamps are local: read them as UTC, then take the zone off
      cJSON *p = hp ? cJSON_GetArrayItem(hp, i) : nullptr;
      cJSON *c = hc ? cJSON_GetArrayItem(hc, i) : nullptr;
      cJSON *g = hg ? cJSON_GetArrayItem(hg, i) : nullptr;
      hw[k].unix = (uint32_t)(utcMkTime(&t) - utcOffset);
      hw[k].mm = (p && cJSON_IsNumber(p)) ? (float)p->valuedouble : 0.0f;
      hw[k].code = (c && cJSON_IsNumber(c)) ? (uint8_t)c->valueint : 0;
      double gv = (g && cJSON_IsNumber(g)) ? g->valuedouble : 0.0;
      hw[k].gust = (uint8_t)(gv > 255 ? 255 : gv < 0 ? 0 : gv);
      k++;
    }
    if (wxMutex) xSemaphoreTake(wxMutex, portMAX_DELAY);
    memcpy(hours_, hw, sizeof hw);
    nHours_ = k;
    if (wxMutex) xSemaphoreGive(wxMutex);
  }

  cJSON *curr = cJSON_GetObjectItem(root, "current");
  cJSON *daily = cJSON_GetObjectItem(root, "daily");

  if (curr && daily) {
    WeatherNow w = {};
    w.valid = true;
    snprintf(w.place, sizeof(w.place), "%s", homeName);

    struct tm tmNow;
    if (boardLocalTime(&tmNow)) {
      snprintf(w.updated, sizeof(w.updated), "%02d:%02d", tmNow.tm_hour, tmNow.tm_min);
    } else {
      strcpy(w.updated, "--:--");
    }

    cJSON *temp = cJSON_GetObjectItem(curr, "temperature_2m");
    cJSON *wcode = cJSON_GetObjectItem(curr, "weather_code");
    cJSON *isDay = cJSON_GetObjectItem(curr, "is_day");
    cJSON *wspd = cJSON_GetObjectItem(curr, "wind_speed_10m");
    cJSON *wdir = cJSON_GetObjectItem(curr, "wind_direction_10m");

    w.temp = temp ? (float)temp->valuedouble : 0.0f;
    w.icon = wcode ? wmoToWxIcon(wcode->valueint) : WX_CLEAR;
    w.isDay = isDay ? (isDay->valueint == 1) : true;
    w.windKmh = wspd ? (float)wspd->valuedouble : 0.0f;
    w.windFromDeg = wdir ? (uint16_t)wdir->valuedouble : 0;

    cJSON *tmaxArr = cJSON_GetObjectItem(daily, "temperature_2m_max");
    cJSON *tminArr = cJSON_GetObjectItem(daily, "temperature_2m_min");
    cJSON *codeArr = cJSON_GetObjectItem(daily, "weather_code");
    cJSON *timeArr = cJSON_GetObjectItem(daily, "time");

    if (tmaxArr && cJSON_GetArraySize(tmaxArr) > 0) {
      w.tmax = (int8_t)round(cJSON_GetArrayItem(tmaxArr, 0)->valuedouble);
    }
    if (tminArr && cJSON_GetArraySize(tminArr) > 0) {
      w.tmin = (int8_t)round(cJSON_GetArrayItem(tminArr, 0)->valuedouble);
    }

    for (int i = 0; i < 3; i++) {
      int idx = i + 1;
      if (timeArr && idx < cJSON_GetArraySize(timeArr)) {
        const char *ts = cJSON_GetArrayItem(timeArr, idx)->valuestring;
        int y = 0, m = 0, d = 0;
        if (ts && sscanf(ts, "%d-%d-%d", &y, &m, &d) == 3) {
          snprintf(w.days[i].name, sizeof(w.days[i].name), "%s", calcDayName(y, m, d));
        }
      }
      if (codeArr && idx < cJSON_GetArraySize(codeArr)) {
        w.days[i].icon = wmoToWxIcon(cJSON_GetArrayItem(codeArr, idx)->valueint);
      }
      if (tmaxArr && idx < cJSON_GetArraySize(tmaxArr)) {
        w.days[i].tmax = (int8_t)round(cJSON_GetArrayItem(tmaxArr, idx)->valuedouble);
      }
      if (tminArr && idx < cJSON_GetArraySize(tminArr)) {
        w.days[i].tmin = (int8_t)round(cJSON_GetArrayItem(tminArr, idx)->valuedouble);
      }
    }

    if (wxMutex) xSemaphoreTake(wxMutex, portMAX_DELAY);
    w.aqi = now_.aqi; // preserve existing AQI until AQI call completes
    now_ = w;
    if (wxMutex) xSemaphoreGive(wxMutex);
    forecastAtMs = nowMs();
    wxDirty = true;
  }

  cJSON_Delete(root);
}

static void fetchAirQuality(char *buf, size_t cap) {
  char url[160];
  snprintf(url, sizeof(url),
           "https://air-quality-api.open-meteo.com/v1/air-quality?latitude=%.5f&longitude=%.5f&current=us_aqi",
           homeLat, homeLon);

  int status = 0;
  if (!httpGet(url, buf, cap, &status)) {
    char err[64];
    snprintf(err, sizeof(err), "weather: AQI HTTP failed (%d)", status);
    logErrorOnce(ERR_AQI_HTTP, err);
    return;
  }
  clearError(ERR_AQI_HTTP);
  budgetTotalCalls++;
  budgetFcCalls++;

  cJSON *root = cJSON_Parse(buf);
  if (!root) {
    logErrorOnce(ERR_AQI_PARSE, "weather: AQI JSON parse failed");
    return;
  }
  clearError(ERR_AQI_PARSE);

  cJSON *curr = cJSON_GetObjectItem(root, "current");
  if (curr) {
    cJSON *aqiVal = cJSON_GetObjectItem(curr, "us_aqi");
    if (aqiVal && cJSON_IsNumber(aqiVal)) {
      if (wxMutex) xSemaphoreTake(wxMutex, portMAX_DELAY);
      now_.aqi = (int16_t)aqiVal->valueint;
      if (wxMutex) xSemaphoreGive(wxMutex);
      wxDirty = true;
    }
  }

  cJSON_Delete(root);
}

static uint32_t homePolls;   // good home nowcasts so far

// Past the nowcast's ~105 min the rain's end comes from the hourly forecast:
// the first dry hour (under 0.2 mm) starting 30+ min after the rain does
static uint32_t endFromHourly(uint32_t start) {
  for (int i = 0; i < nHours_; i++) {
    uint32_t hStart = hours_[i].unix - 3600;
    if (hStart >= start + 30 * 60 && hours_[i].mm < 0.2f) return hStart;
  }
  return 0;
}

// Light Rain / Heavy Shower by intensity (7.6 mm/h is "heavy rain"); Storm
// when the forecast has thunder or 62+ km/h gusts (Beaufort 8) while it rains
static WxTier rainTier(const RainAlert &a) {
  uint32_t from = a.startUnix, to = a.endUnix ? a.endUnix : a.startUnix + 2 * 3600;
  float mm = a.mmh;
  for (int i = 0; i < nHours_; i++) {
    uint32_t hEnd = hours_[i].unix, hStart = hEnd - 3600;
    if (from ? (hEnd <= from || hStart >= to) : i > 1) continue;   // no clock: the next two hours
    if (hours_[i].code >= 95 || hours_[i].gust >= 62) return TIER_STORM;
    if (hours_[i].mm > mm) mm = hours_[i].mm;
  }
  return mm >= 7.6f ? TIER_HEAVY_SHOWER : TIER_LIGHT_RAIN;
}

// OpenWeather's hour counts for 5 min after it was fetched, for its place only
static bool owmFresh(bool phone, uint32_t now) {
  return owmLast.valid && owmOnPhone == phone && now - owmLast.fetchedUnix < 5 * 60;
}

static void mergeOwm(RainAlert *a, bool phone) {
  a->src = (a->active || a->raining) ? RAIN_SRC_OPEN_METEO : 0;
  uint32_t now = (uint32_t)time(nullptr);
  if (owmFresh(phone, now)) owmMergeRain(a, owmLast, now);
}

// rainHome from Open-Meteo's nowcast and OpenWeather's hour, after either
static void publishHome() {
  RainAlert a = omHome;
  mergeOwm(&a, false);
  if ((a.active || a.raining) && a.startUnix && !a.endUnix) a.endUnix = endFromHourly(a.startUnix);
  if (a.active || a.raining) a.tier = rainTier(a);
  if (demoOn()) return;
  if (wxMutex) xSemaphoreTake(wxMutex, portMAX_DELAY);
  rainHome = a;
  homePolls++;
  if (wxMutex) xSemaphoreGive(wxMutex);
  wxDirty = true;
}

static void runHomeNowcast(char *buf, size_t cap) {
  char url[1024];
  homeNowcast.setLocation(homeLat, homeLon);
  if (!homeNowcast.buildUrl(url, sizeof(url))) return;

  int status = 0;
  if (!httpGet(url, buf, cap, &status)) {
    char err[64];
    snprintf(err, sizeof(err), "weather: home nowcast HTTP failed (%d)", status);
    logErrorOnce(ERR_HOME_HTTP, err);
    return;
  }
  clearError(ERR_HOME_HTTP);
  budgetTotalCalls += 17;
  budgetHomeCalls += 17;

  RainAlert alert = {};
  uint32_t cadence = 0;
  if (!homeNowcast.processResponse(buf, &alert, &cadence)) {
    logErrorOnce(ERR_HOME_PARSE, "weather: home nowcast process failed");
    return;
  }
  clearError(ERR_HOME_PARSE);
  omHome = alert;
  publishHome();
}

// "35 min", "1 h 10 min", "2 h"
static void fmtDur(char *out, size_t cap, long min) {
  if (min < 60) snprintf(out, cap, "%ld min", min);
  else if (min % 60 == 0 || min >= 600) snprintf(out, cap, "%ld h", (min + 30) / 60);
  else snprintf(out, cap, "%ld h %ld min", min / 60, min % 60);
}

// "2 d 6 h" for a day or more, else fmtDur
static void fmtLong(char *out, size_t cap, long sec) {
  long min = sec / 60;
  if (min < 24 * 60) fmtDur(out, cap, min);
  else snprintf(out, cap, "%ld d %ld h", min / 1440, (min % 1440) / 60);
}

// local "16:40", or "Tue 16:40"
static void fmtClock(char *out, size_t cap, uint32_t unixT, bool withDay) {
  static const char *DAYS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  struct tm t;
  if (!unixT || !boardLocalTimeAt((time_t)unixT, &t)) snprintf(out, cap, "--:--");
  else if (withDay) snprintf(out, cap, "%s %02d:%02d", DAYS[t.tm_wday], t.tm_hour, t.tm_min);
  else snprintf(out, cap, "%02d:%02d", t.tm_hour, t.tm_min);
}

// Rain warnings to #weather-announcement. While rain is on its way: a post
// whenever the ETA has moved 5+ min from the last one (counting down or
// re-forecast) or the tier goes up. While it rains: a post when the expected
// end moves 5+ min. Two dry polls in a row after either: "cleared".
static struct {
  WxTier tier;          // tier of the last post; NONE = no warning out
  bool raining;         // the last post said it rains now
  int etaPosted;        // ETA in that post, in 5-min steps
  uint32_t endPosted;   // rain end in that post
  uint32_t pollsSeen;   // homePolls at the last check
  uint8_t dryPolls;     // polls without rain since
} warn;

static void rainWarnTick() {
  RainAlert a = rainHome;   // this task is the only writer
  bool newPoll = homePolls != warn.pollsSeen;
  warn.pollsSeen = homePolls;
  if (cyclone_.active || demoOn()) return;   // the typhoon posts cover it

  char title[112], desc[256], dur[24], clk[16];
  if (a.active) {
    warn.dryPolls = 0;
    int eta = rainEtaMin(a);
    int eta5 = (eta + 2) / 5 * 5;
    if (warn.tier != TIER_NONE && !warn.raining && a.tier <= warn.tier && abs(eta5 - warn.etaPosted) < 5) return;
    snprintf(title, sizeof title, "%s in ~%d min | %s", wxTierName(a.tier), eta, homeName);
    int n;
    if (a.startUnix && a.endUnix) {
      fmtDur(dur, sizeof dur, (long)(a.endUnix - a.startUnix) / 60);
      fmtClock(clk, sizeof clk, a.endUnix, false);
      n = snprintf(desc, sizeof desc, "Lasts ~%s (until ~%s)", dur, clk);
    } else {
      n = snprintf(desc, sizeof desc, a.startUnix ? "No end in the 12 h forecast" : "Length unknown (no clock yet)");
    }
    snprintf(desc + n, sizeof desc - n, "\n%.1f mm/h%s%s\nSeen by %s", a.mmh, a.from[0] ? ", moving in from " : "", a.from,
             rainSrcName(a));
    discordPost(DC_WEATHER, title, desc, eta > 20 ? COL_YELLOW : COL_RED);
    logMsg("weather: warning posted: %s", title);
    warn.tier = a.tier;
    warn.raining = false;
    warn.etaPosted = eta5;
  } else if (a.raining) {
    warn.dryPolls = 0;
    long shift = labs((long)a.endUnix - (long)warn.endPosted);
    bool endKnownChanged = (a.endUnix == 0) != (warn.endPosted == 0);
    if (warn.raining && a.tier <= warn.tier && !endKnownChanged && shift < 300) return;
    snprintf(title, sizeof title, "%s now | %s", wxTierName(a.tier), homeName);
    int left = rainLeftMin(a);
    int n;
    if (left >= 0) {
      fmtDur(dur, sizeof dur, left);
      fmtClock(clk, sizeof clk, a.endUnix, false);
      n = snprintf(desc, sizeof desc, "Until ~%s (~%s left)", clk, dur);
    } else {
      n = snprintf(desc, sizeof desc, "No end in the 12 h forecast");
    }
    snprintf(desc + n, sizeof desc - n, "\n%.1f mm/h\nSeen by %s", a.mmh, rainSrcName(a));
    discordPost(DC_WEATHER, title, desc, COL_RED);
    logMsg("weather: warning posted: %s", title);
    warn.tier = a.tier;
    warn.raining = true;
    warn.endPosted = a.endUnix;
  } else if (warn.tier != TIER_NONE && newPoll && ++warn.dryPolls >= 2) {
    snprintf(title, sizeof title, "Weather Warning Cleared | %s", homeName);
    discordPost(DC_WEATHER, title, warn.raining ? "The rain is over." : "The rain is no longer expected.", COL_GREEN);
    logMsg("weather: warning cleared");
    uint32_t seen = warn.pollsSeen;
    warn = {};
    warn.pollsSeen = seen;
  }
}

// Typhoon posts: one per GDACS advisory while it is more than 6 h out, one a
// minute in the last 6 h, one an hour while it is here, "cleared" after.
static struct {
  uint32_t eventId;        // storm the posts are about
  char name[20];
  uint32_t episodePosted;  // advisory of the last "heading here" post
  uint32_t minutePostMs, hourPostMs;
  bool warned, activePosted;
} ty;

static void cycloneTick() {
  time_t now;
  if (!tracks_ || !clockValid(&now) || demoOn()) return;
  Cyclone best = {};
  int bestIdx = -1;
  for (int i = 0; i < MAX_TRACKS; i++) {
    if (!tracks_[i].n) continue;
    Cyclone c;
    cycloneJudge(tracks_[i], homeLat, homeLon, now, &c);
    if (!c.tracked) continue;
    bool better = bestIdx < 0 || (c.active && !best.active) ||
                  (!c.active && !best.active && c.arriveUnix < best.arriveUnix);
    if (better) {
      best = c;
      bestIdx = i;
    }
  }
  bool changed = best.tracked != cyclone_.tracked || best.active != cyclone_.active;
  if (wxMutex) xSemaphoreTake(wxMutex, portMAX_DELAY);
  cyclone_ = best;
  trackShown = bestIdx;
  if (wxMutex) xSemaphoreGive(wxMutex);
  if (changed) wxDirty = true;

  char title[112], desc[200], when[24], clk[24];
  uint32_t ms = nowMs();
  const char *kind = best.superTyphoon ? "Super Typhoon" : "Typhoon";
  if (bestIdx >= 0) {
    const CycloneTrack &tr = tracks_[bestIdx];
    if (ty.eventId != tr.eventId) {
      ty = {};
      ty.eventId = tr.eventId;
      snprintf(ty.name, sizeof ty.name, "%s", tr.name);
    }
    if (best.active) {
      if (ty.activePosted && ms - ty.hourPostMs < 3600 * 1000) return;
      snprintf(title, sizeof title, "%s %s Active | %s", kind, best.name, homeName);
      snprintf(desc, sizeof desc, "Centre ~%u km away, peak wind %u km/h", best.distKm, best.windKmh);
      discordPost(DC_WEATHER, title, desc, COL_RED);
      ty.activePosted = ty.warned = true;
      ty.hourPostMs = ms;
    } else {
      long left = (long)best.arriveUnix - (long)now;
      fmtLong(when, sizeof when, left > 0 ? left : 0);
      fmtClock(clk, sizeof clk, best.arriveUnix, true);
      if (left <= 6 * 3600) {
        if (ty.minutePostMs && ms - ty.minutePostMs < 60 * 1000) return;
        snprintf(title, sizeof title, "%s %s in ~%s | %s", kind, best.name, when, homeName);
        snprintf(desc, sizeof desc, "Arrives ~%s, closest ~%u km\nPeak wind %u km/h", clk, best.closestKm, best.windKmh);
        discordPost(DC_WEATHER, title, desc, left > 20 * 60 ? COL_YELLOW : COL_RED);
        ty.minutePostMs = ms;
      } else {
        if (ty.episodePosted == tr.episodeId) return;
        snprintf(title, sizeof title, "%s %s heading for %s", kind, best.name, homeName);
        snprintf(desc, sizeof desc, "Arrives ~%s (in ~%s), closest ~%u km\nPeak wind %u km/h (GDACS advisory %u)", clk,
                 when, best.closestKm, best.windKmh, (unsigned)tr.episodeId);
        discordPost(DC_WEATHER, title, desc, COL_YELLOW);
        ty.episodePosted = tr.episodeId;
        logMsg("weather: %s", title);
      }
      ty.warned = true;
    }
  } else if (ty.warned) {
    snprintf(title, sizeof title, "Weather Warning Cleared | %s", homeName);
    snprintf(desc, sizeof desc, ty.activePosted ? "Typhoon %s has passed." : "Typhoon %s is no longer heading here.", ty.name);
    discordPost(DC_WEATHER, title, desc, COL_GREEN);
    logMsg("weather: typhoon %s cleared", ty.name);
    ty = {};
  }
}

// Current cyclones from GDACS; a nearby storm's track is fetched again only
// when GDACS publishes a new advisory for it (about every 6 h)
static void pollCyclones(char *buf, size_t cap) {
  time_t now;
  if (!clockValid(&now)) return;
  if (!tracks_) tracks_ = (CycloneTrack *)heap_caps_calloc(MAX_TRACKS, sizeof(CycloneTrack), MALLOC_CAP_SPIRAM);
  if (!tracks_) return;

  char url[200];
  cycloneListUrl(url, sizeof url, now);
  int status = 0;
  if (!httpGet(url, buf, cap, &status)) {
    char err[64];
    snprintf(err, sizeof err, "weather: GDACS list HTTP failed (%d)", status);
    logErrorOnce(ERR_TC_HTTP, err);
    return;
  }
  clearError(ERR_TC_HTTP);
  CycloneCandidate cand[MAX_TRACKS];
  int n = cycloneParseList(buf, homeLat, homeLon, cand, MAX_TRACKS);

  for (int i = 0; i < MAX_TRACKS; i++) {   // storms no longer listed (or no longer near)
    bool listed = false;
    for (int k = 0; k < n; k++) listed |= tracks_[i].eventId == cand[k].eventId;
    if (!listed) memset(&tracks_[i], 0, sizeof(CycloneTrack));
  }
  for (int k = 0; k < n; k++) {
    int slot = -1, freeSlot = -1;
    for (int i = 0; i < MAX_TRACKS; i++) {
      if (tracks_[i].eventId == cand[k].eventId) slot = i;
      else if (!tracks_[i].eventId && freeSlot < 0) freeSlot = i;
    }
    if (slot >= 0 && tracks_[slot].episodeId == cand[k].episodeId) continue;   // have this advisory
    int dst = slot >= 0 ? slot : freeSlot;
    if (dst < 0) continue;
    // ~350 KB of GeoJSON: a PSRAM buffer just for this fetch
    const size_t bigCap = 1024 * 1024;
    char *big = (char *)heap_caps_malloc(bigCap, MALLOC_CAP_SPIRAM);
    if (!big) continue;
    cycloneTrackUrl(url, sizeof url, cand[k].eventId, cand[k].episodeId);
    bool got = httpGet(url, big, bigCap, &status, 30000);
    size_t len = strlen(big);
    if (got && cycloneParseTrack(big, len, cand[k], now, &tracks_[dst])) {
      clearError(ERR_TC_TRACK);
      logMsg("weather: cyclone %s: %d track points from %u KB, advisory %u, %.0f km away", tracks_[dst].name,
             tracks_[dst].n, (unsigned)(len / 1024), (unsigned)cand[k].episodeId,
             cycloneDistKm(homeLat, homeLon, cand[k].lat, cand[k].lon));
    } else {
      char err[96];
      snprintf(err, sizeof err, "weather: GDACS track for %s failed (HTTP %d, %s, %u KB)", cand[k].name, status,
               esp_err_to_name(lastHttpErr), (unsigned)(len / 1024));
      logErrorOnce(ERR_TC_TRACK, err);
    }
    heap_caps_free(big);
  }
}

static const char *iconWord(WxIcon i) {
  switch (i) {
    case WX_CLEAR: return "clear";
    case WX_PARTLY: return "partly cloudy";
    case WX_CLOUDY: return "cloudy";
    case WX_FOG: return "fog";
    case WX_DRIZZLE: return "drizzle";
    case WX_RAIN: return "rain";
    case WX_SHOWERS: return "showers";
    case WX_SNOW: return "snow";
    case WX_STORM: return "thunderstorms";
  }
  return "";
}

// Weather update to #weather-announcement every 2 h from 06:00 to 20:00; the
// 06:00 one adds the 3-day forecast. The slot is kept in NVS so a reboot in
// the same hour does not post it twice.
static void postWeatherUpdate() {
  struct tm t;
  if (!now_.valid || !boardLocalTime(&t)) return;
  if (t.tm_hour < 6 || t.tm_hour > 20 || (t.tm_hour & 1)) return;
  if (!forecastAtMs || nowMs() - forecastAtMs > 10 * 60 * 1000) return;   // wait for fresh data
  uint32_t slot = (uint32_t)(t.tm_year + 1900) * 10000 + t.tm_yday * 24 + t.tm_hour;
  static uint32_t posted;
  nvs_handle_t h;
  if (!posted && nvs_open("wx", NVS_READONLY, &h) == ESP_OK) {
    nvs_get_u32(h, "upd_slot", &posted);
    nvs_close(h);
  }
  if (posted == slot) return;
  posted = slot;
  if (nvs_open("wx", NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_u32(h, "upd_slot", slot);
    nvs_commit(h);
    nvs_close(h);
  }

  WeatherNow w;
  if (wxMutex) xSemaphoreTake(wxMutex, portMAX_DELAY);
  w = now_;
  if (wxMutex) xSemaphoreGive(wxMutex);
  char desc[480], clk[16];
  int n = snprintf(desc, sizeof desc, "%.0f\u00b0 (%d\u00b0 / %d\u00b0), %s\nWind %.0f km/h", (double)w.temp, w.tmax, w.tmin,
                   iconWord(w.icon), w.windKmh);
  if (w.aqi >= 0) n += snprintf(desc + n, sizeof desc - n, ", AQI %d", w.aqi);
  const RainAlert &r = rainHome;
  if (r.raining) {
    fmtClock(clk, sizeof clk, r.endUnix, false);
    n += snprintf(desc + n, sizeof desc - n, r.endUnix ? "\n%s now, until ~%s" : "\n%s now", wxTierName(r.tier), clk);
  } else if (r.active) {
    n += snprintf(desc + n, sizeof desc - n, "\n%s in ~%d min", wxTierName(r.tier), rainEtaMin(r));
  } else {
    n += snprintf(desc + n, sizeof desc - n, "\nNo rain in the next 2 h");
  }
  if (cyclone_.tracked && n < (int)sizeof desc) {
    if (cyclone_.active) {
      n += snprintf(desc + n, sizeof desc - n, "\nTyphoon %s active", cyclone_.name);
    } else {
      fmtClock(clk, sizeof clk, cyclone_.arriveUnix, true);
      n += snprintf(desc + n, sizeof desc - n, "\nTyphoon %s expected ~%s", cyclone_.name, clk);
    }
  }
  if (t.tm_hour == 6) {
    for (int i = 0; i < 3 && n < (int)sizeof desc; i++)
      if (w.days[i].name[0])
        n += snprintf(desc + n, sizeof desc - n, "\n%s: %d\u00b0 / %d\u00b0, %s", w.days[i].name, w.days[i].tmax,
                      w.days[i].tmin, iconWord(w.days[i].icon));
  }
  char title[96];
  snprintf(title, sizeof title, "Currently in %s | %02d:%02d", w.place[0] ? w.place : "Home", t.tm_hour, t.tm_min);
  discordPost(DC_WEATHER, title, desc, COL_BLUE);
  logMsg("weather: update posted (%02d:%02d)", t.tm_hour, t.tm_min);
}

static void publishPhone();

static void runPhoneNowcast(char *buf, size_t cap) {
  char url[1024];
  if (!phoneNowcast.buildUrl(url, sizeof(url))) return;

  int status = 0;
  if (!httpGet(url, buf, cap, &status)) {
    char err[64];
    snprintf(err, sizeof(err), "weather: phone nowcast HTTP failed (%d)", status);
    logErrorOnce(ERR_PHONE_HTTP, err);
    return;
  }
  clearError(ERR_PHONE_HTTP);
  budgetTotalCalls += 17;
  budgetPhoneCalls += 17;

  RainAlert alert = {};
  uint32_t cadence = 0;
  if (!phoneNowcast.processResponse(buf, &alert, &cadence)) {
    logErrorOnce(ERR_PHONE_PARSE, "weather: phone nowcast process failed");
    return;
  }
  clearError(ERR_PHONE_PARSE);
  omPhone = alert;
  publishPhone();
}

// rainPhone from both sources, and the "rain where you are" notification
static void publishPhone() {
  RainAlert alert = omPhone;
  mergeOwm(&alert, true);
  if (wxMutex) xSemaphoreTake(wxMutex, portMAX_DELAY);
  rainPhone = alert;
  if (wxMutex) xSemaphoreGive(wxMutex);

  // Rain notification for away phone
  if (alert.active || alert.raining) {
    phoneDryStartMs = 0;
    if (!phoneAlertSent) {
      char msg[128];
      if (alert.raining) {
        snprintf(msg, sizeof(msg), "Raining at your location now (%.1f mm/h)", alert.mmh);
      } else if (alert.from[0]) {
        snprintf(msg, sizeof(msg), "Rain at your location in ~%u min (%.1f mm/h, from %s)",
                 (unsigned)alert.etaMin, alert.mmh, alert.from);
      } else {
        snprintf(msg, sizeof(msg), "Rain at your location in ~%u min (%.1f mm/h)",
                 (unsigned)alert.etaMin, alert.mmh);
      }
      size_t n = strlen(msg);
      snprintf(msg + n, sizeof msg - n, " [%s]", rainSrcName(alert));
      discordPost(DC_WEATHER, "Rain soon where you are", msg, 0x3498DB);
      pushoverSend("Rain soon", msg);
      phoneAlertSent = true;
      logMsg("weather: phone rain alert sent: %s", msg);
    }
  } else {
    if (phoneAlertSent) {
      if (phoneDryStartMs == 0) {
        phoneDryStartMs = nowMs();
      } else if (nowMs() - phoneDryStartMs >= 60 * 60 * 1000) {
        phoneAlertSent = false;
        phoneDryStartMs = 0;
        logMsg("weather: phone rain alert re-armed after 60 min dry");
      }
    }
  }
}

static void setHomeLocation(double lat, double lon, const char *name) {
  homeLat = lat;
  homeLon = lon;
  snprintf(homeName, sizeof(homeName), "%s", name ? name : "Home");
  homeConfigured = true;

  nvs_handle_t h;
  if (nvs_open("wx", NVS_READWRITE, &h) == ESP_OK) {
    char latStr[24], lonStr[24];
    snprintf(latStr, sizeof(latStr), "%.5f", lat);
    snprintf(lonStr, sizeof(lonStr), "%.5f", lon);
    nvs_set_str(h, "home_lat", latStr);
    nvs_set_str(h, "home_lon", lonStr);
    nvs_set_str(h, "home_name", homeName);
    nvs_commit(h);
    nvs_close(h);
  }

  homeChanged = true;
  if (wxTaskHandle) xTaskNotifyGive(wxTaskHandle);
}

static void loadNvsConfig() {
  nvs_handle_t h;
  if (nvs_open("wx", NVS_READONLY, &h) == ESP_OK) {
    char latStr[24] = {0}, lonStr[24] = {0}, nameStr[48] = {0};
    size_t len = sizeof(latStr);
    if (nvs_get_str(h, "home_lat", latStr, &len) == ESP_OK) {
      len = sizeof(lonStr);
      if (nvs_get_str(h, "home_lon", lonStr, &len) == ESP_OK) {
        homeLat = strtod(latStr, nullptr);
        homeLon = strtod(lonStr, nullptr);
        homeConfigured = true;
        len = sizeof(nameStr);
        if (nvs_get_str(h, "home_name", nameStr, &len) == ESP_OK) {
          snprintf(homeName, sizeof(homeName), "%s", nameStr);
        }
      }
    }

    // the clock is usually unset at boot: restore the saved day's counts and
    // let the day check in checkBudgetNowcast reset them if the day moved on
    uint32_t savedDay = 0;
    if (nvs_get_u32(h, "b_day", &savedDay) == ESP_OK) {
      currentUtcDay = savedDay;
      nvs_get_u32(h, "b_tot", &budgetTotalCalls);
      nvs_get_u32(h, "b_home", &budgetHomeCalls);
      nvs_get_u32(h, "b_phone", &budgetPhoneCalls);
      nvs_get_u32(h, "b_fc", &budgetFcCalls);
    }
    nvs_close(h);
  }
}

// A weather alert OpenWeather names for the first time: its details once, to
// #weather-announcement (one alert call per poll at most)
static void owmAlertsTick(char *buf, size_t cap) {
  for (int i = 0; i < owmLast.nAlerts; i++) {
    const char *id = owmLast.alerts[i];
    bool seen = false;
    for (auto &s : owmSeen) seen = seen || !strcmp(s, id);
    if (seen) continue;
    OwmAlert al;
    if (!owmFetchAlert(id, buf, cap, &al)) return;   // budget or error: next poll
    snprintf(owmSeen[owmSeenNext], sizeof owmSeen[0], "%s", id);
    owmSeenNext = (owmSeenNext + 1) % 8;
    char title[112], desc[320], s[24], e[24];
    fmtClock(s, sizeof s, al.startUnix, true);
    fmtClock(e, sizeof e, al.endUnix, true);
    snprintf(title, sizeof title, "%.60s | %s", al.event, owmOnPhone ? "where you are" : homeName);
    snprintf(desc, sizeof desc, "%s to %s%s%.48s\n%.200s", s, e, al.sender[0] ? ", from " : "", al.sender, al.text);
    discordPost(DC_WEATHER, title, desc, COL_RED);
    logMsg("weather: OpenWeather alert posted: %s", al.event);
    return;
  }
}

static void owmTick(char *buf, size_t cap, bool force, uint32_t *lastMs, uint32_t curMs) {
  bool onPhone = owntracksEnabled() && owntracksValid() && owntracksAgeSec() < 30 * 60 &&
                 !owntracksIsMerged(homeLat, homeLon);
  double lat = homeLat, lon = homeLon;
  if (onPhone) owntracksGetLocation(&lat, &lon, nullptr, nullptr);
  const RainAlert &t = onPhone ? rainPhone : rainHome;
  uint32_t now = (uint32_t)time(nullptr);
  bool alertNow = owmLast.valid && owmLast.nAlerts && now - owmLast.fetchedUnix < 10 * 60;
  uint32_t every = (t.active || t.raining || alertNow) ? 90 * 1000 : 120 * 1000;
  if (onPhone != owmOnPhone) force = true;   // moved: the new place right away
  if (!force && *lastMs && curMs - *lastMs < every) return;
  if (!owmBudgetLeft()) return;
  *lastMs = curMs;
  OwmMinutely m;
  if (!owmFetchMinutely(lat, lon, buf, cap, &m)) {
    char err[96];
    snprintf(err, sizeof err, "weather: OpenWeather: %s", owmLastError());
    logErrorOnce(ERR_OWM, err);
    return;
  }
  clearError(ERR_OWM);
  bool moved = onPhone != owmOnPhone;
  owmLast = m;
  owmOnPhone = onPhone;
  publishHome();                        // home gains or loses OpenWeather
  if (onPhone || moved) publishPhone();
  owmAlertsTick(buf, cap);
}

static void weatherTask(void *) {
  size_t httpBufCap = 48 * 1024;
  char *httpBuf = (char *)heap_caps_malloc(httpBufCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!httpBuf) {
    httpBufCap = 32 * 1024;
    httpBuf = (char *)malloc(httpBufCap);
  }

  uint32_t lastForecastMs = 0;
  uint32_t lastAqiMs = 0;
  uint32_t lastCycloneMs = 0;
  uint32_t lastJudgeMs = 0;
  uint32_t lastHomeNowcastMs = 0;
  uint32_t lastPhoneNowcastMs = 0;
  uint32_t lastOwmMs = 0;
  uint32_t lastNvsSaveMs = nowMs();
  bool prevNetUp = false;
  uint32_t netUpMs = 0;
  bool forceImmediate = false;

  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));

    bool currentNet = netUp();
    if ((currentNet && !prevNetUp) || homeChanged) {
      homeChanged = false;
      forceImmediate = true;
    }
    if (currentNet && !prevNetUp) netUpMs = nowMs();
    prevNetUp = currentNet;

    // After Wi-Fi comes up, let Discord's TLS handshakes finish first (or
    // 30 s): handshakes running side by side left ~3 KB internal RAM at boot
    if (currentNet && !discordUp() && nowMs() - netUpMs < 30000) continue;

    if (currentNet && testAlertPending) {
      testAlertPending = false;
      const char *testMsg = "Rain at your location in ~25 min (2.5 mm/h, from NW) [TEST]";
      discordPost(DC_WEATHER, "Rain soon where you are", testMsg, 0x3498DB);
      pushoverSend("Rain soon", testMsg);
      logMsg("weather: test-alert sent");
    }

    if (!currentNet || !homeConfigured || !httpBuf) {
      continue;
    }

    uint32_t curMs = nowMs();

    // 1. Forecast every 5 min, air quality every 30, both at once on Wi-Fi
    // connect or a home change
    if (forceImmediate || lastForecastMs == 0 || (curMs - lastForecastMs >= 5 * 60 * 1000)) {
      if (checkBudgetForecast()) fetchForecast(httpBuf, httpBufCap);
      lastForecastMs = curMs;
      // failed: try again in a minute
      if (loggedErrors & (ERR_FC_HTTP | ERR_FC_PARSE)) lastForecastMs = curMs - 4 * 60 * 1000;
    }
    if (forceImmediate || lastAqiMs == 0 || (curMs - lastAqiMs >= 30 * 60 * 1000)) {
      if (checkBudgetForecast()) fetchAirQuality(httpBuf, httpBufCap);
      lastAqiMs = curMs;
    }

    // 1b. Cyclones near home: GDACS list every 30 min (5 after a failure)
    if (forceImmediate || cycloneRefresh || lastCycloneMs == 0 || (curMs - lastCycloneMs >= 30 * 60 * 1000)) {
      if (cycloneRefresh && tracks_) memset(tracks_, 0, MAX_TRACKS * sizeof(CycloneTrack));   // refetch the tracks too
      cycloneRefresh = false;
      clearError(ERR_TC_HTTP | ERR_TC_TRACK);   // log this round's errors again
      pollCyclones(httpBuf, httpBufCap);
      lastCycloneMs = curMs;
      if (loggedErrors & ERR_TC_HTTP) lastCycloneMs = curMs - 25 * 60 * 1000;
      lastJudgeMs = 0;
    }

    // 2. Home Nowcast engine
    uint32_t homeCadence = homeNowcast.cadenceMs();
    if (forceImmediate || lastHomeNowcastMs == 0 || (curMs - lastHomeNowcastMs >= homeCadence)) {
      if (checkBudgetNowcast(false)) {
        runHomeNowcast(httpBuf, httpBufCap);
        lastHomeNowcastMs = curMs;
      }
    }

    // 3. Phone Nowcast engine (only while away, not merged)
    if (owntracksEnabled() && owntracksValid() && !owntracksIsMerged(homeLat, homeLon)) {
      double pLat = 0.0, pLon = 0.0;
      owntracksGetLocation(&pLat, &pLon, nullptr, nullptr);
      phoneNowcast.setLocation(pLat, pLon);

      uint32_t phoneCadence = phoneNowcast.cadenceMs();
      if (lastPhoneNowcastMs == 0 || (curMs - lastPhoneNowcastMs >= phoneCadence)) {
        if (checkBudgetNowcast(true)) {
          runPhoneNowcast(httpBuf, httpBufCap);
          lastPhoneNowcastMs = curMs;
        }
      }
    } else {
      phoneAlertSent = false;
      phoneDryStartMs = 0;
    }

    // 3b. OpenWeather's minute-by-minute hour for one place: the phone while
    // it is away and reported in the last 30 min, else home. Every 2 min,
    // every 90 s while rain or a weather alert is near; 960 calls a day.
    if (owmEnabled()) owmTick(httpBuf, httpBufCap, forceImmediate, &lastOwmMs, curMs);

    forceImmediate = false;

    // 4. Warnings and the 2-hourly update; storms re-judged every 30 s
    if (!lastJudgeMs || curMs - lastJudgeMs >= 30 * 1000) {
      cycloneTick();
      lastJudgeMs = curMs;
    }
    rainWarnTick();
    postWeatherUpdate();

    // 5. Budget persistence every 15 min
    if (curMs - lastNvsSaveMs >= 15 * 60 * 1000) {
      saveBudgetNvs();
      owmSaveCount();
      lastNvsSaveMs = curMs;
    }
  }
}

// WEATHER_HOME from secrets.h ("lat,lon[,name]"), when set, wins at boot.
// The name is everything after the second comma (commas allowed), trimmed,
// with one pair of surrounding ' or " quotes removed.
static void applyConfiguredHome() {
  const char *cfg = WEATHER_HOME;
  if (!cfg[0]) return;
  char *end;
  double lat = strtod(cfg, &end);
  bool ok = end != cfg && *end == ',';
  const char *p = end + 1;
  double lon = ok ? strtod(p, &end) : 0;
  ok = ok && end != p && (*end == ',' || *end == 0 || isspace((unsigned char)*end));
  char name[48] = "Home";
  if (ok) {
    while (isspace((unsigned char)*end)) end++;
    if (*end == ',') {
      const char *s = end + 1;
      while (isspace((unsigned char)*s)) s++;
      size_t n = strlen(s);
      while (n && isspace((unsigned char)s[n - 1])) n--;
      if (n >= 2 && (s[0] == '\'' || s[0] == '"') && s[n - 1] == s[0]) { s++; n -= 2; }
      if (n >= sizeof name) n = sizeof name - 1;
      if (n) { memcpy(name, s, n); name[n] = 0; }
    } else if (*end) {
      ok = false;
    }
  }
  if (!ok || lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
    logMsg("weather: WEATHER_HOME in secrets.h is not \"lat,lon[,name]\"; ignored");
    return;
  }
  if (homeConfigured && fabs(homeLat - lat) < 1e-5 && fabs(homeLon - lon) < 1e-5 && !strcmp(homeName, name)) return;
  setHomeLocation(lat, lon, name);
  logMsg("weather: home from secrets.h (%s)", homeName);
}

void weatherInit() {
  wxMutex = xSemaphoreCreateMutex();
  loadNvsConfig();
  owmInit();
  applyConfiguredHome();
  owntracksInit();
  pushoverInit();

  xTaskCreatePinnedToCore(weatherTask, "weather", 9216, nullptr, 1, &wxTaskHandle, 0);
  logMsg("weather: service initialized%s", homeConfigured ? "" : " (no home set; use `weather home <lat>,<lon> [name]`)");
}

void weatherPoll() {
  if (netUp()) {
    boardTimeStartSntp();
    owntracksStart();
  }

  // live countdowns: redraw once a minute while a warning is up
  static uint32_t lastMinute;
  time_t now;
  if ((rainHome.active || rainHome.raining || cyclone_.tracked) && clockValid(&now) && (uint32_t)(now / 60) != lastMinute) {
    lastMinute = (uint32_t)(now / 60);
    wxDirty = true;
  }

  if (wxDirty) {
    wxDirty = false;
    viewsMarkDirty(-1);
  }
}

void weatherCommand(const char *a1, const char *a2) {
  if (a1 && !strcmp(a1, "demo")) {
    int mode = !a2 ? 0 : !strcmp(a2, "rain") ? 1 : !strcmp(a2, "typhoon") ? 2 : 0;
    loadDemo(mode);
    demoUntilMs = nowMs() + 5 * 60 * 1000;
    logMsg("weather: demo data loaded%s for 5 min (no Discord posts)",
           mode == 1 ? " (heavy shower in 25 min)" : mode == 2 ? " (typhoon here)" : "");
  } else if (a1 && !strcmp(a1, "clear")) {
    if (wxMutex) xSemaphoreTake(wxMutex, portMAX_DELAY);
    now_ = {};
    rainHome = {};
    rainPhone = {};
    cyclone_ = {};
    demoUntilMs = 0;
    if (wxMutex) xSemaphoreGive(wxMutex);
    wxDirty = true;
    logMsg("weather: cleared");
  } else if (a1 && !strcmp(a1, "home")) {
    if (!a2) {
      logMsg("weather: usage weather home <lat>,<lon> [name]");
      return;
    }
    char a2Buf[64];
    snprintf(a2Buf, sizeof(a2Buf), "%s", a2);
    char *c1 = strchr(a2Buf, ',');
    if (!c1) {
      logMsg("weather: invalid coordinates format (use <lat>,<lon>)");
      return;
    }
    *c1 = '\0';
    char *lonPart = c1 + 1;
    char name[24] = "Home";
    char *c2 = strchr(lonPart, ',');
    if (c2) {
      *c2 = '\0';
      snprintf(name, sizeof(name), "%s", c2 + 1);
    } else {
      char *a3 = strtok(nullptr, " \t");
      if (a3 && a3[0]) {
        snprintf(name, sizeof(name), "%s", a3);
      }
    }
    for (char *p = name; *p; p++) {
      if (*p == '_') *p = ' ';
    }
    double lat = strtod(a2Buf, nullptr);
    double lon = strtod(lonPart, nullptr);
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
      logMsg("weather: coordinates out of range (-90..90, -180..180)");
      return;
    }
    setHomeLocation(lat, lon, name);
    logMsg("weather: home set to %s (%.5f, %.5f)", name, lat, lon);
  } else if (a1 && !strcmp(a1, "refresh")) {
    homeChanged = true;     // forecast, AQI and nowcast at once
    cycloneRefresh = true;  // and GDACS
    if (wxTaskHandle) xTaskNotifyGive(wxTaskHandle);
    logMsg("weather: refresh queued");
    return;
  } else if (a1 && !strcmp(a1, "storms")) {
    cycloneRefresh = true;
    if (wxTaskHandle) xTaskNotifyGive(wxTaskHandle);
    logMsg("weather: cyclone refresh queued");
    return;
  } else if (a1 && !strcmp(a1, "owm-check")) {
    // the merge on a made-up OpenWeather hour (rain from minute N), home's
    // Open-Meteo result untouched and nothing posted: `weather owm-check 12`
    int from = a2 ? atoi(a2) : 10;
    OwmMinutely m = {};
    m.valid = true;
    m.fetchedUnix = (uint32_t)time(nullptr);
    m.dt0 = m.fetchedUnix;
    m.n = 60;
    for (int i = 0; i < 60; i++) m.mm[i] = (i >= from && i < from + 20) ? 4.0f : 0.0f;
    RainAlert r = omHome;
    r.src = (r.active || r.raining) ? RAIN_SRC_OPEN_METEO : 0;
    owmMergeRain(&r, m, m.fetchedUnix);
    char st[16], en[16];
    fmtClock(st, sizeof st, r.startUnix, false);
    fmtClock(en, sizeof en, r.endUnix, false);
    logMsg("weather: owm-check rain from min %d: active %d raining %d eta %u start %s end %s %.1f mm/h, seen by %s", from,
           r.active, r.raining, (unsigned)r.etaMin, st, en, r.mmh, rainSrcName(r));
    return;
  } else if (a1 && !strcmp(a1, "test-alert")) {
    testAlertPending = true;
    if (wxTaskHandle) xTaskNotifyGive(wxTaskHandle);
    logMsg("weather: test-alert queued%s", netUp() ? "" : " (sends when Wi-Fi is up)");
  } else if (!a1 || !strcmp(a1, "show")) {
    if (now_.valid) {
      logMsg("weather: %s %.1f C (%d/%d) wind %.0f km/h from %u deg, AQI %d | rain %s",
             now_.place, now_.temp, now_.tmax, now_.tmin, now_.windKmh, now_.windFromDeg,
             now_.aqi, rainHome.active ? "incoming" : "none");
    } else {
      logMsg("weather: %s (no forecast data yet; %s)", homeConfigured ? homeName : "Home",
             !netUp() ? "needs Wi-Fi" : !homeConfigured ? "no home set" : "fetching");
    }
    if (loggedErrors) logMsg("weather: last error: %s [%s]", lastError, esp_err_to_name(lastHttpErr));

    // Home engine
    if (rainHome.active || rainHome.raining) {
      char s[16], e[16];
      fmtClock(s, sizeof s, rainHome.startUnix, false);
      fmtClock(e, sizeof e, rainHome.endUnix, false);
      logMsg("weather: home engine: %s %s (%s to %s, %.1f mm/h, from %s) | cadence %u min", wxTierName(rainHome.tier),
             rainHome.raining ? "now" : "soon", s, rainHome.endUnix ? e : "12 h+", rainHome.mmh,
             rainHome.from[0] ? rainHome.from : "--", (unsigned)(homeNowcast.cadenceMs() / 60000));
    } else if (rainHome.dryKm > 0) {
      logMsg("weather: home engine: clear, radar dry within %u km | cadence %u min",
             (unsigned)rainHome.dryKm, (unsigned)(homeNowcast.cadenceMs() / 60000));
    } else {
      logMsg("weather: home engine: clear | cadence %u min",
             (unsigned)(homeNowcast.cadenceMs() / 60000));
    }

    // Phone engine
    if (!owntracksEnabled()) {
      logMsg("weather: phone engine: off (MQTT_URI empty)");
    } else if (!owntracksValid()) {
      uint32_t age = owntracksAgeSec();
      if (age == UINT32_MAX) {
        logMsg("weather: phone engine: waiting for location");
      } else {
        logMsg("weather: phone engine: location expired (age %u s)", (unsigned)age);
      }
    } else if (owntracksIsMerged(homeLat, homeLon)) {
      double dist = owntracksDistanceTo(homeLat, homeLon);
      logMsg("weather: phone engine: merged with home (dist %.2f km, age %u s)",
             dist, (unsigned)owntracksAgeSec());
    } else {
      double dist = owntracksDistanceTo(homeLat, homeLon);
      if (rainPhone.active) {
        logMsg("weather: phone engine: away (dist %.2f km, age %u s) | rain in ~%u min (%.1f mm/h, from %s), cadence %u min",
               dist, (unsigned)owntracksAgeSec(), (unsigned)rainPhone.etaMin, rainPhone.mmh,
               rainPhone.from[0] ? rainPhone.from : "--", (unsigned)(phoneNowcast.cadenceMs() / 60000));
      } else {
        logMsg("weather: phone engine: away (dist %.2f km, age %u s) | clear%s, cadence %u min",
               dist, (unsigned)owntracksAgeSec(),
               (rainPhone.dryKm > 0) ? " (dry 25 km)" : "",
               (unsigned)(phoneNowcast.cadenceMs() / 60000));
      }
    }

    // OpenWeather
    if (!owmEnabled()) {
      logMsg("weather: OpenWeather: off (OWM_API_KEY empty)");
    } else {
      int first = -1;
      float peak = 0;
      for (int i = 0; i < owmLast.n; i++) {
        if (owmLast.mm[i] >= 0.5f && first < 0) first = i;
        if (owmLast.mm[i] > peak) peak = owmLast.mm[i];
      }
      char when[24] = "--";
      if (owmLast.valid) fmtClock(when, sizeof when, owmLast.fetchedUnix, false);
      logMsg("weather: OpenWeather: %s, last %s: %s, peak %.1f mm/h, %u alert(s) | %u/%u calls today%s%s",
             owmOnPhone ? "phone" : "home", when, !owmLast.valid ? "no data yet" : first < 0 ? "dry for 60 min" :
             first == 0 ? "raining" : "rain within the hour", peak, (unsigned)owmLast.nAlerts,
             (unsigned)owmCallsToday(), (unsigned)OWM_DAILY_CALLS, owmLastError()[0] ? " | " : "", owmLastError());
    }
    logMsg("weather: rain seen by: home %s, phone %s", rainSrcName(rainHome), rainSrcName(rainPhone));

    // Cyclones (GDACS)
    int nTracks = 0;
    for (int i = 0; tracks_ && i < MAX_TRACKS; i++) {
      if (!tracks_[i].n) continue;
      nTracks++;
      Cyclone c;
      time_t now;
      if (!clockValid(&now)) break;
      cycloneJudge(tracks_[i], homeLat, homeLon, now, &c);
      char a[24];
      fmtClock(a, sizeof a, c.arriveUnix, true);
      logMsg("weather: cyclone %s%s: %u km now, closest %u km, peak %u km/h, %s", tracks_[i].name,
             i == trackShown ? " (shown)" : "", c.distKm, c.closestKm, c.windKmh,
             c.active ? "TYPHOON HERE" : c.tracked ? a : "not a typhoon threat");
    }
    if (!nTracks) logMsg("weather: cyclones: none within %.0f km", CYCLONE_WATCH_KM);

    // Budget
    logMsg("weather: budget: %u/9500 calls today (home: %u, phone: %u, forecast: %u)",
           (unsigned)budgetTotalCalls, (unsigned)budgetHomeCalls,
           (unsigned)budgetPhoneCalls, (unsigned)budgetFcCalls);
    return;
  } else {
    logMsg("weather: usage weather <show|refresh|demo [rain|typhoon]|clear|storms|home <lat>,<lon> [name]|test-alert>");
    return;
  }

  viewsMarkDirty(-1);
}
