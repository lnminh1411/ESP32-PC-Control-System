// Weather for the media screen while the PC is off, and rain warnings.
// Data comes from Open-Meteo over Wi-Fi (forecast + air quality); the rain
// nowcast engine fills RainAlert. Until Wi-Fi is set up `weather demo`
// loads sample data so the screens can be checked.
#pragma once
#include <stdint.h>

// WMO weather code groups the icons are picked from
enum WxIcon : uint8_t { WX_CLEAR, WX_PARTLY, WX_CLOUDY, WX_FOG, WX_DRIZZLE, WX_RAIN, WX_SHOWERS, WX_SNOW, WX_STORM };

struct WeatherDay {
  char name[4];          // "Mon"
  WxIcon icon;
  int8_t tmax, tmin;     // deg C
};

struct WeatherNow {
  bool valid;
  char place[48];        // location label, UTF-8
  char updated[6];       // "21:40" local
  float temp;            // deg C
  int8_t tmax, tmin;     // today
  WxIcon icon;
  bool isDay;
  float windKmh;
  uint16_t windFromDeg;  // meteorological: where the wind comes from
  int16_t aqi;           // US AQI, -1 = unknown
  WeatherDay days[3];    // next three days
};

// Warning tiers, mildest first. Rain tiers come from the nowcast and the
// hourly forecast; typhoon tiers from the GDACS cyclone tracker.
enum WxTier : uint8_t { TIER_NONE, TIER_LIGHT_RAIN, TIER_HEAVY_SHOWER, TIER_STORM, TIER_TYPHOON, TIER_SUPER_TYPHOON };

struct RainAlert {
  bool active;           // rain expected within 60 min (a warning; false once it rains)
  bool raining;          // raining at the last poll
  uint16_t etaMin;       // minutes until it starts, at the last poll (rainEtaMin() counts live)
  uint32_t startUnix;    // rain start (UTC); 0 = no rain in sight
  uint32_t endUnix;      // rain end estimate (UTC); 0 = beyond the forecast
  float mmh;             // expected intensity
  char from[3];          // compass direction it moves in from ("NW")
  uint16_t dryKm;        // when inactive: radius known to be dry (0 = unknown)
  WxTier tier;           // home only: LIGHT_RAIN..STORM while active, raining or watch
  uint8_t src;           // RAIN_SRC_* bits: who sees the rain
  uint8_t dry;           // RAIN_SRC_* bits: fresh sources that see none
  bool watch;            // rain possible but unconfirmed (one source, light or far off):
                         // shown, never posted; active and raining stay false
  bool endUnknown;       // no end, and the hourly forecast has no rain then (a shower)
  uint8_t endSrc;        // RAIN_END_*: where endUnix comes from
  uint8_t kind;          // RAIN_KIND_*: from the hourly weather codes around the start
};

// Rain end sources, also the slots of the end tracker (`weather show`).
// RANGE: the middle of the shower's usual length, when nothing else has an end
enum : uint8_t { RAIN_END_NONE, RAIN_END_NOWCAST, RAIN_END_OWM, RAIN_END_UPWIND, RAIN_END_HOURLY, RAIN_END_RANGE, RAIN_END_N };
const char *rainEndSrcName(uint8_t src);   // "nowcast", "OpenWeather", "upwind", "hourly", "range"
// Steady rain (WMO 51-67) can last hours; a shower (80-82, or rain the hourly
// forecast misses) and a thunderstorm (95-99) have a usual length
enum : uint8_t { RAIN_KIND_STEADY, RAIN_KIND_SHOWER, RAIN_KIND_THUNDER };
// The usual length of a shower or thunderstorm in minutes; false for steady rain
bool rainKindRange(uint8_t kind, int *loMin, int *hiMin);

enum : uint8_t { RAIN_SRC_OPEN_METEO = 1, RAIN_SRC_OPENWEATHER = 2 };
const char *rainSrcName(const RainAlert &a);   // "Open-Meteo", "OpenWeather", "both"
// "Seen by OpenWeather (Open-Meteo: dry)"; returns the length written
int rainSrcLine(char *out, int cap, const RainAlert &a);

// Tropical cyclone near home (GDACS track + forecast)
struct Cyclone {
  bool tracked;          // a typhoon whose track comes within range of home
  bool active;           // within range now
  bool superTyphoon;
  char name[20];         // "KAJIKI"
  uint32_t arriveUnix;   // first time within range (UTC); 0 = already in range
  uint16_t distKm;       // centre's distance from home now
  uint16_t closestKm;    // closest forecast approach
  uint16_t windKmh;      // peak wind GDACS reports for the storm
};

const WeatherNow &weatherNow();
const RainAlert &rainAlertHome();
bool weatherHomeLocation(double *lat, double *lon);   // false while no home is set
const RainAlert &rainAlertPhone();
const Cyclone &weatherCyclone();
const char *wxTierName(WxTier t);         // "Heavy Shower"
int rainEtaMin(const RainAlert &a);       // minutes to rain start, from the clock; -1 = none
int rainLeftMin(const RainAlert &a);      // minutes to rain end, from the clock; -1 = unknown
void weatherInit();
void weatherPoll();
void weatherCommand(const char *a1, const char *a2);   // `weather <demo|clear|show>`
