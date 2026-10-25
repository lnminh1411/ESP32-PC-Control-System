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
#include "discord.h"
#include "log.h"
#include "net.h"
#include "nowcast.h"
#include "owntracks.h"
#include "pushover.h"
#include "secrets_or_example.h"
#include "sys.h"
#include "views.h"

static WeatherNow now_;
static RainAlert rainHome;
static RainAlert rainPhone;

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
};
static uint16_t loggedErrors;
static char lastError[96];               // shown by `weather show`
static esp_err_t lastHttpErr;
static volatile bool homeChanged;        // new home: poll right away
static volatile bool testAlertPending;   // sent from the weather task: HTTPS must not block the main loop

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

static void loadDemo(bool rain) {
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
  if (rain) {
    rainHome.active = true;
    rainHome.etaMin = 25;
    rainHome.mmh = 2.1f;
    strcpy(rainHome.from, "NW");
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

static bool httpGet(const char *url, char *buf, size_t cap, int *outStatus) {
  HttpBuffer hb = {buf, cap, 0};
  buf[0] = '\0';

  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.timeout_ms = 10000;
  cfg.event_handler = httpEventCb;
  cfg.user_data = &hb;

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client) return false;

  esp_err_t err = esp_http_client_perform(client);
  lastHttpErr = err;
  int status = esp_http_client_get_status_code(client);
  if (outStatus) *outStatus = status;
  esp_http_client_cleanup(client);

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
  char url[256];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.5f&longitude=%.5f"
           "&current=temperature_2m,weather_code,is_day,wind_speed_10m,wind_direction_10m"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min&timezone=auto&forecast_days=4",
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
  if (tzSec && cJSON_IsNumber(tzSec)) {
    boardTimeSetUtcOffset(tzSec->valueint);
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

  bool started = alert.active && !rainHome.active;
  if (wxMutex) xSemaphoreTake(wxMutex, portMAX_DELAY);
  rainHome = alert;
  if (wxMutex) xSemaphoreGive(wxMutex);
  wxDirty = true;

  if (started) {
    char msg[128];
    snprintf(msg, sizeof msg, "In ~%u min, %.1f mm/h%s%s", (unsigned)alert.etaMin, alert.mmh,
             alert.from[0] ? ", from " : "", alert.from);
    char title[80];
    snprintf(title, sizeof title, "Rain soon at %s", homeName);
    discordPost(DC_WEATHER, title, msg, 0x3498DB);
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

// Morning forecast to #weather-announcement: the first good forecast after
// 06:00, once a day (the day is kept in NVS so a reboot does not repeat it)
static void postMorningForecast() {
  struct tm t;
  if (!now_.valid || !boardLocalTime(&t) || t.tm_hour < 6) return;
  uint32_t day = (uint32_t)(t.tm_year + 1900) * 1000 + t.tm_yday;
  static uint32_t postedDay;
  nvs_handle_t h;
  if (!postedDay && nvs_open("wx", NVS_READONLY, &h) == ESP_OK) {
    nvs_get_u32(h, "fc_day", &postedDay);
    nvs_close(h);
  }
  if (postedDay == day) return;
  postedDay = day;
  if (nvs_open("wx", NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_u32(h, "fc_day", day);
    nvs_commit(h);
    nvs_close(h);
  }

  WeatherNow w;
  if (wxMutex) xSemaphoreTake(wxMutex, portMAX_DELAY);
  w = now_;
  if (wxMutex) xSemaphoreGive(wxMutex);
  char desc[320];
  int n = snprintf(desc, sizeof desc, "%d\u00b0 / %d\u00b0, %s\nWind %.0f km/h", w.tmax, w.tmin, iconWord(w.icon), w.windKmh);
  if (w.aqi >= 0) n += snprintf(desc + n, sizeof desc - n, ", AQI %d", w.aqi);
  for (int i = 0; i < 3 && n < (int)sizeof desc; i++)
    if (w.days[i].name[0])
      n += snprintf(desc + n, sizeof desc - n, "\n%s: %d\u00b0 / %d\u00b0, %s", w.days[i].name, w.days[i].tmax, w.days[i].tmin,
                    iconWord(w.days[i].icon));
  char title[80];
  snprintf(title, sizeof title, "Today in %s", w.place[0] ? w.place : "Home");
  discordPost(DC_WEATHER, title, desc, 0x3498DB);
  logMsg("weather: morning forecast posted");
}

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

  if (wxMutex) xSemaphoreTake(wxMutex, portMAX_DELAY);
  rainPhone = alert;
  if (wxMutex) xSemaphoreGive(wxMutex);

  // Rain notification for away phone
  if (alert.active) {
    phoneDryStartMs = 0;
    if (!phoneAlertSent) {
      char msg[128];
      if (alert.from[0]) {
        snprintf(msg, sizeof(msg), "Rain at your location in ~%u min (%.1f mm/h, from %s)",
                 (unsigned)alert.etaMin, alert.mmh, alert.from);
      } else {
        snprintf(msg, sizeof(msg), "Rain at your location in ~%u min (%.1f mm/h)",
                 (unsigned)alert.etaMin, alert.mmh);
      }
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

static void weatherTask(void *) {
  size_t httpBufCap = 48 * 1024;
  char *httpBuf = (char *)heap_caps_malloc(httpBufCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!httpBuf) {
    httpBufCap = 32 * 1024;
    httpBuf = (char *)malloc(httpBufCap);
  }

  uint32_t lastForecastMs = 0;
  uint32_t lastHomeNowcastMs = 0;
  uint32_t lastPhoneNowcastMs = 0;
  uint32_t lastNvsSaveMs = nowMs();
  bool prevNetUp = false;
  bool forceImmediate = false;

  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));

    bool currentNet = netUp();
    if ((currentNet && !prevNetUp) || homeChanged) {
      homeChanged = false;
      forceImmediate = true;
    }
    prevNetUp = currentNet;

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

    // 1. Forecast & Air Quality: every 30 min, on Wi-Fi connect, or on home location change
    if (forceImmediate || lastForecastMs == 0 || (curMs - lastForecastMs >= 30 * 60 * 1000)) {
      if (checkBudgetForecast()) {
        fetchForecast(httpBuf, httpBufCap);
        postMorningForecast();
      }
      if (checkBudgetForecast()) {
        fetchAirQuality(httpBuf, httpBufCap);
      }
      lastForecastMs = curMs;
      // failed: try again in a minute, not in 30
      if (loggedErrors & (ERR_FC_HTTP | ERR_FC_PARSE)) lastForecastMs = curMs - 29 * 60 * 1000;
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

    forceImmediate = false;

    // 4. Budget persistence every 15 min
    if (curMs - lastNvsSaveMs >= 15 * 60 * 1000) {
      saveBudgetNvs();
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
  applyConfiguredHome();
  owntracksInit();
  pushoverInit();

  xTaskCreatePinnedToCore(weatherTask, "weather", 12288, nullptr, 1, &wxTaskHandle, 0);
  logMsg("weather: service initialized%s", homeConfigured ? "" : " (no home set; use `weather home <lat>,<lon> [name]`)");
}

void weatherPoll() {
  if (netUp()) {
    boardTimeStartSntp();
    owntracksStart();
  }

  if (wxDirty) {
    wxDirty = false;
    viewsMarkDirty(-1);
  }
}

void weatherCommand(const char *a1, const char *a2) {
  if (a1 && !strcmp(a1, "demo")) {
    loadDemo(a2 && !strcmp(a2, "rain"));
    logMsg("weather: demo data loaded%s", rainHome.active ? " (rain incoming)" : "");
  } else if (a1 && !strcmp(a1, "clear")) {
    if (wxMutex) xSemaphoreTake(wxMutex, portMAX_DELAY);
    now_ = {};
    rainHome = {};
    rainPhone = {};
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
    if (rainHome.active) {
      logMsg("weather: home engine: rain in ~%u min (%.1f mm/h, from %s) | cadence %u min",
             (unsigned)rainHome.etaMin, rainHome.mmh, rainHome.from[0] ? rainHome.from : "--",
             (unsigned)(homeNowcast.cadenceMs() / 60000));
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

    // Budget
    logMsg("weather: budget: %u/9500 calls today (home: %u, phone: %u, forecast: %u)",
           (unsigned)budgetTotalCalls, (unsigned)budgetHomeCalls,
           (unsigned)budgetPhoneCalls, (unsigned)budgetFcCalls);
    return;
  } else {
    logMsg("weather: usage weather <show|demo [rain]|clear|home <lat>,<lon> [name]|test-alert>");
    return;
  }

  viewsMarkDirty(-1);
}
