// RainViewer's radar mosaic: the newest frame (every 10 min, ~10 min late)
// read around one place. What the radar sees is rain falling now, not a
// model's guess, so weather.cpp takes it as "raining here" on its own: the
// case the models missed (10-29, 05:00). Free, no key. RADAR_ENABLED in
// settings.h turns it off.
#pragma once
#include <stddef.h>
#include <stdint.h>

struct RadarObs {
  bool valid;
  uint32_t fetchedUnix;     // when it was read (UTC)
  uint32_t frameUnix;       // the radar frame's time (UTC)
  double lat, lon;          // where
  int8_t dbzHere;           // strongest echo within RADAR_HERE_KM; -128 = none
  uint8_t pxHere;           // pixels within RADAR_HERE_KM at RADAR_RAIN_DBZ or more
  float mmhHere;            // dbzHere as rain (Marshall-Palmer)
  int16_t nearKm;           // nearest echo at RADAR_RAIN_DBZ or more; -1 = none on the tile
};

// Two calls: the frame list, then one 256 px tile centred on the place (zoom
// 7, ~1.1 km a pixel, ~140 km across). buf is scratch for both.
bool radarFetch(double lat, double lon, char *buf, size_t cap, RadarObs *out);
const char *radarLastError();   // "" when the last fetch worked
// Rain at the place, by the settings' thresholds
bool radarRainingHere(const RadarObs &o);
float radarDbzToMmh(int dbz);
