// OpenWeather One Call 4.0: the next 60 minutes of rain, minute by minute,
// for one place (home, or the phone while it is away), plus the IDs of the
// weather alerts in force there. A second source next to Open-Meteo's
// nowcast; weather.cpp merges the two. Needs OWM_API_KEY in secrets.h (empty
// = off). Calls are budgeted on the board: 960 a day (UTC), the account's
// free tier is 1000.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "weather.h"

constexpr uint32_t OWM_DAILY_CALLS = 960;

struct OwmMinutely {
  bool valid;
  uint32_t fetchedUnix;     // when it was fetched (UTC)
  double lat, lon;          // where
  uint32_t dt0;             // the first minute (UTC)
  uint8_t n;                // minutes in mm[]
  float mm[60];             // precipitation, mm/h
  uint8_t nAlerts;
  char alerts[4][40];       // alert IDs in force (first minute)
};

struct OwmAlert {
  char event[48];           // "Heavy rain warning"
  char sender[48];
  uint32_t startUnix, endUnix;
  char text[200];           // description, first language, cut
};

bool owmEnabled();                       // a key is set
bool owmBudgetLeft();                    // calls left today (resets 00:00 UTC)
uint32_t owmCallsToday();
const char *owmLastError();              // "" when the last call worked
void owmInit();                          // NVS: today's count

// One timeline/1min call. buf is scratch for the response.
bool owmFetchMinutely(double lat, double lon, char *buf, size_t cap, OwmMinutely *out);
// One alert/{id} call
bool owmFetchAlert(const char *id, char *buf, size_t cap, OwmAlert *out);
void owmSaveCount();                     // persist today's count (every few calls is enough)

// Folds the minutely rain into a RainAlert built by Open-Meteo's nowcast:
// rain anywhere in the hour makes it active (or raining), OpenWeather's start
// time wins, the intensity is the higher of the two; dry only when both are.
// Sets the source bits (RAIN_SRC_*).
void owmMergeRain(RainAlert *a, const OwmMinutely &m, uint32_t nowUnix);
