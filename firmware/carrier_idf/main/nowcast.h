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

private:
  static constexpr int N_POINTS = 17;
  static constexpr int N_STEPS = 10; // k = -2..7

  double lat_, lon_;
  bool hasLoc_;
  RainAlert alert_;
  uint32_t cadenceMs_;
  bool isDry_;
  float lastSpeed_;

  // Field values in mm/h: F_[step_idx][point_idx]
  // step_idx = k + 2 (k = -2..7 -> step_idx 0..9)
  float F_[N_STEPS][N_POINTS];

  // Inverse distance weighted interpolation of F_[kIdx] at arbitrary (px, py) in km
  float interpolateField(int kIdx, float px, float py) const;
};
