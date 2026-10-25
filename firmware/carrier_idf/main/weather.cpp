#include "weather.h"

#include <string.h>

#include "log.h"
#include "views.h"

// Filled by the Open-Meteo client and the nowcast engine once Wi-Fi is set
// up; until then only `weather demo` puts data here.
static WeatherNow now_;
static RainAlert rainHome;

const WeatherNow &weatherNow() { return now_; }
const RainAlert &rainAlertHome() { return rainHome; }

static void loadDemo(bool rain) {
  WeatherNow w = {};
  w.valid = true;
  strcpy(w.place, "Home");
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
}

void weatherCommand(const char *a1, const char *a2) {
  if (a1 && !strcmp(a1, "demo")) {
    loadDemo(a2 && !strcmp(a2, "rain"));
    logMsg("weather: demo data loaded%s", rainHome.active ? " (rain incoming)" : "");
  } else if (a1 && !strcmp(a1, "clear")) {
    now_ = {};
    rainHome = {};
    logMsg("weather: cleared");
  } else if (!a1 || !strcmp(a1, "show")) {
    if (!now_.valid) { logMsg("weather: no data (needs Wi-Fi; `weather demo [rain]` for a sample)"); return; }
    logMsg("weather: %s %.1f C (%d/%d) wind %.0f km/h from %u deg, AQI %d | rain %s",
           now_.place, now_.temp, now_.tmax, now_.tmin, now_.windKmh, now_.windFromDeg, now_.aqi,
           rainHome.active ? "incoming" : "none");
    return;
  } else {
    logMsg("weather: usage weather <show|demo [rain]|clear>");
    return;
  }
  viewsMarkDirty(-1);
}
