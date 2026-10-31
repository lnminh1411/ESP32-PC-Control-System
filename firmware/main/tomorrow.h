// Tomorrow.io: the next 60 minutes of rain, minute by minute, for one place
// (home, or the phone while it is away). A third source next to Open-Meteo's
// nowcast and OpenWeather; weather.cpp merges the three. Needs TOMORROW_API_KEY
// in settings.h (empty = off). The free tier is 500 calls a day and 25 an hour;
// the board counts its calls and stays at TOMORROW_CALLS_PER_DAY (480) a day
// (UTC) and 20 an hour. The hour fits OwmMinutely and owmRainSummary (owm.h).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "owm.h"
#include "settings_or_example.h"

constexpr uint32_t TIO_DAILY_CALLS = TOMORROW_CALLS_PER_DAY;
constexpr uint32_t TIO_HOURLY_CALLS = 20;

bool tioEnabled();                       // a key is set
bool tioBudgetLeft();                    // calls left today (00:00 UTC) and this clock hour
uint32_t tioCallsToday();
uint32_t tioCallsThisHour();
const char *tioLastError();              // "" when the last call worked
void tioInit();                          // NVS: the counts
void tioSaveCount();                     // persist the counts (every few calls is enough)

// One /v4/timelines call for the hour's precipitationIntensity. buf is scratch
// for the response (~5 KB).
bool tioFetchMinutely(double lat, double lon, char *buf, size_t cap, OwmMinutely *out);
