// Tropical cyclone tracker from GDACS (gdacs.org, no key). The weather task
// does the fetching; this file parses and judges a storm against home.
//   list:  current cyclones worldwide, ~6 KB, every 30 min
//   track: one storm's past and forecast positions, ~350 KB, fetched only
//          when GDACS publishes a new advisory (episode) for a nearby storm
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "settings_or_example.h"

#include "weather.h"

constexpr float CYCLONE_RANGE_KM = CYCLONE_HOME_KM;     // "at home": centre within this distance
constexpr float CYCLONE_WATCH_KM = CYCLONE_FETCH_KM;    // storms further away are not fetched
constexpr int CYCLONE_MAX_POINTS = 64;

struct CycloneCandidate {
  uint32_t eventId, episodeId;
  char name[20];
  uint16_t windKmh;   // GDACS severity: the storm's peak wind
  float lat, lon;     // latest position
};

struct CycloneTrack {
  uint32_t eventId, episodeId;
  char name[20];
  uint16_t windKmh;
  int n;
  uint32_t t[CYCLONE_MAX_POINTS];   // unix UTC, ascending
  float lat[CYCLONE_MAX_POINTS], lon[CYCLONE_MAX_POINTS];
  char cat[CYCLONE_MAX_POINTS][4];  // GDACS class at that point: TD, TS, HU, ...
};

void cycloneListUrl(char *out, size_t cap, time_t now);
void cycloneTrackUrl(char *out, size_t cap, uint32_t eventId, uint32_t episodeId);

// Current cyclones within CYCLONE_WATCH_KM of home, nearest first. Returns the count.
int cycloneParseList(const char *json, double homeLat, double homeLon, CycloneCandidate *out, int max);

// One storm's track from its GDACS geometry. False = no usable points.
bool cycloneParseTrack(const char *json, size_t len, const CycloneCandidate &c, time_t now, CycloneTrack *out);

// Where the storm is relative to home at `now`; out->tracked is false for
// storms that never reach typhoon strength within range of home.
void cycloneJudge(const CycloneTrack &tr, double homeLat, double homeLon, time_t now, Cyclone *out);

float cycloneDistKm(double lat1, double lon1, double lat2, double lon2);
