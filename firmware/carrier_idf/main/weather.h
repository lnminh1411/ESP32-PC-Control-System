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

struct RainAlert {
  bool active;           // rain expected soon
  uint16_t etaMin;       // minutes until it starts
  float mmh;             // expected intensity
  char from[3];          // compass direction it moves in from ("NW")
  uint16_t dryKm;        // when inactive: radius known to be dry (0 = unknown)
};

const WeatherNow &weatherNow();
const RainAlert &rainAlertHome();
const RainAlert &rainAlertPhone();
void weatherInit();
void weatherPoll();
void weatherCommand(const char *a1, const char *a2);   // `weather <demo|clear|show>`
