#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "weather.h"

// 17-point ring grid advection nowcasting engine.
class NowcastEngine {
public:
  NowcastEngine();

  void setLocation(double lat, double lon);
  bool hasLocation() const { return hasLoc_; }
  double lat() const { return lat_; }
  double lon() const { return lon_; }

  // Build the 17-location comma-separated query URL into out. Returns written length.
  int buildUrl(char *out, size_t maxLen) const;

  // Process the Open-Meteo multi-location JSON response.
  // Computes motion vector, ETA, intensity, and sets RainAlert & adaptive cadence.
  bool processResponse(const char *json, RainAlert *outAlert, uint32_t *outCadenceMs);

  const RainAlert &alert() const { return alert_; }
  uint32_t cadenceMs() const { return cadenceMs_; }
  bool isDry() const { return isDry_; }
  float lastSpeed() const { return lastSpeed_; }

  // Upwind probe: N_PROBE points along the motion vector, where the rain that
  // reaches home in PROBE_MIN[i] minutes is now. Needs a nowcast that found
  // the rain moving (5+ km/h).
  static constexpr int N_PROBE = 8;
  bool canProbe() const { return hasLoc_ && !isDry_ && lastSpeed_ >= 5.0f; }
  int buildProbeUrl(char *out, size_t maxLen) const;
  // Rain end at home from the probe's current values, carried home unchanged
  // at the rain's speed: halfway between the last wet point and the first dry
  // one after it. Returns PROBE_* below; *endUnix set for PROBE_END.
  enum { PROBE_BAD, PROBE_NO_RAIN, PROBE_END, PROBE_BEYOND };
  int processProbe(const char *json, uint32_t nowUnix, bool rainingNow, uint32_t *endUnix) const;

private:
  static constexpr int N_POINTS = 17;
  static constexpr int N_STEPS = 10; // k = -2..7

  double lat_, lon_;
  bool hasLoc_;
  RainAlert alert_;
  uint32_t cadenceMs_;
  bool isDry_;
  float lastSpeed_;
  float lastVx_, lastVy_;   // motion vector, km/h east and north (where the rain goes)

  // Field values in mm/h: F_[step_idx][point_idx]
  // step_idx = k + 2 (k = -2..7 -> step_idx 0..9)
  float F_[N_STEPS][N_POINTS];

  // Inverse distance weighted interpolation of F_[kIdx] at arbitrary (px, py) in km
  float interpolateField(int kIdx, float px, float py) const;
};
