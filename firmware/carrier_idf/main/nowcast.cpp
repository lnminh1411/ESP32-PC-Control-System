#include "nowcast.h"

#include <cJSON.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "log.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const float R1 = 10.0f;
static const float R2 = 25.0f;
static const float SQRT2_2 = 0.70710678f;

// 17-point ring grid relative offsets in km: East (X) and North (Y)
static const float GRID_X[17] = {
  0.0f,
  0.0f, R1 * SQRT2_2, R1, R1 * SQRT2_2, 0.0f, -R1 * SQRT2_2, -R1, -R1 * SQRT2_2,
  0.0f, R2 * SQRT2_2, R2, R2 * SQRT2_2, 0.0f, -R2 * SQRT2_2, -R2, -R2 * SQRT2_2
};

static const float GRID_Y[17] = {
  0.0f,
  R1, R1 * SQRT2_2, 0.0f, -R1 * SQRT2_2, -R1, -R1 * SQRT2_2, 0.0f, R1 * SQRT2_2,
  R2, R2 * SQRT2_2, 0.0f, -R2 * SQRT2_2, -R2, -R2 * SQRT2_2, 0.0f, R2 * SQRT2_2
};

NowcastEngine::NowcastEngine()
  : lat_(0.0), lon_(0.0), hasLoc_(false), alert_{}, cadenceMs_(30 * 60 * 1000),
    isDry_(true), lastSpeed_(0.0f) {
  memset(F_, 0, sizeof(F_));
}

void NowcastEngine::setLocation(double lat, double lon) {
  lat_ = lat;
  lon_ = lon;
  hasLoc_ = true;
}

int NowcastEngine::buildUrl(char *out, size_t maxLen) const {
  if (!hasLoc_) return 0;

  double cosLat = cos(lat_ * (M_PI / 180.0));
  if (fabs(cosLat) < 0.01) cosLat = 0.01;

  double lats[N_POINTS], lons[N_POINTS];
  for (int i = 0; i < N_POINTS; i++) {
    lats[i] = lat_ + (double)GRID_Y[i] / 111.139;
    double dLon = (double)GRID_X[i] / (111.139 * cosLat);
    double lon = lon_ + dLon;
    while (lon > 180.0) lon -= 360.0;
    while (lon < -180.0) lon += 360.0;
    lons[i] = lon;
  }

  int len = snprintf(out, maxLen, "https://api.open-meteo.com/v1/forecast?latitude=");
  for (int i = 0; i < N_POINTS; i++) {
    len += snprintf(out + len, maxLen - len, "%s%.5f", (i == 0) ? "" : ",", lats[i]);
  }
  len += snprintf(out + len, maxLen - len, "&longitude=");
  for (int i = 0; i < N_POINTS; i++) {
    len += snprintf(out + len, maxLen - len, "%s%.5f", (i == 0) ? "" : ",", lons[i]);
  }
  len += snprintf(out + len, maxLen - len,
                  "&minutely_15=precipitation&past_minutely_15=2&forecast_minutely_15=8");
  return len;
}

float NowcastEngine::interpolateField(int kIdx, float px, float py) const {
  float sumW = 0.0f;
  float sumVal = 0.0f;
  for (int i = 0; i < N_POINTS; i++) {
    float dx = px - GRID_X[i];
    float dy = py - GRID_Y[i];
    float dist2 = dx * dx + dy * dy;
    if (dist2 < 1e-4f) {
      return F_[kIdx][i];
    }
    float w = 1.0f / dist2;
    sumW += w;
    sumVal += w * F_[kIdx][i];
  }
  return (sumW > 0.0f) ? (sumVal / sumW) : 0.0f;
}

bool NowcastEngine::processResponse(const char *json, RainAlert *outAlert, uint32_t *outCadenceMs) {
  if (!json || !json[0]) return false;

  cJSON *root = cJSON_Parse(json);
  if (!root) return false;
  if (!cJSON_IsArray(root) || cJSON_GetArraySize(root) < N_POINTS) {
    cJSON_Delete(root);
    return false;
  }

  // 1. Populate precipitation field F[k][point] for k = -2..7 (s = 0..9)
  for (int p = 0; p < N_POINTS; p++) {
    cJSON *item = cJSON_GetArrayItem(root, p);
    cJSON *min15 = item ? cJSON_GetObjectItem(item, "minutely_15") : nullptr;
    cJSON *precip = min15 ? cJSON_GetObjectItem(min15, "precipitation") : nullptr;
    if (!precip || !cJSON_IsArray(precip)) {
      cJSON_Delete(root);
      return false;
    }
    int nPrecip = cJSON_GetArraySize(precip);
    for (int s = 0; s < N_STEPS; s++) {
      if (s < nPrecip) {
        cJSON *val = cJSON_GetArrayItem(precip, s);
        float v = (val && cJSON_IsNumber(val)) ? (float)val->valuedouble : 0.0f;
        F_[s][p] = (v > 0.0f) ? (v * 4.0f) : 0.0f; // mm/15min to mm/h
      } else {
        F_[s][p] = 0.0f;
      }
    }
  }
  cJSON_Delete(root);

  // Check if grid is dry for future steps k = 0..7 (s = 2..9)
  bool allDry = true;
  bool anyRainInGrid = false;
  for (int s = 2; s < N_STEPS; s++) {
    for (int p = 0; p < N_POINTS; p++) {
      if (F_[s][p] > 0.05f) {
        allDry = false;
        anyRainInGrid = true;
        break;
      }
    }
    if (anyRainInGrid) break;
  }

  if (allDry) {
    isDry_ = true;
    lastSpeed_ = 0.0f;
    alert_.active = false;
    alert_.etaMin = 0;
    alert_.mmh = 0.0f;
    alert_.from[0] = '\0';
    alert_.dryKm = 25;
    cadenceMs_ = 30 * 60 * 1000; // 30 min cadence when all dry
    if (outAlert) *outAlert = alert_;
    if (outCadenceMs) *outCadenceMs = cadenceMs_;
    return true;
  }

  isDry_ = false;

  // 2. Motion vector search: try v in {0} U {8 directions x 10, 20, 30, 45, 60 km/h}
  // error = sum over points and k=0..3 of (F[k+1](x) - F[k](x - v*15min))^2
  float bestError = 1e30f;
  float bestVx = 0.0f, bestVy = 0.0f, bestSpeed = 0.0f;

  // Candidate v = 0
  {
    float err = 0.0f;
    for (int p = 0; p < N_POINTS; p++) {
      for (int k = 0; k <= 3; k++) {
        int s = k + 2;
        float diff = F_[s + 1][p] - F_[s][p];
        err += diff * diff;
      }
    }
    bestError = err;
  }

  // Candidates: 8 directions x 5 speeds
  static const float SPEEDS[5] = {10.0f, 20.0f, 30.0f, 45.0f, 60.0f};
  static const float ANGLES_DEG[8] = {0.0f, 45.0f, 90.0f, 135.0f, 180.0f, 225.0f, 270.0f, 315.0f};

  for (float spd : SPEEDS) {
    for (float deg : ANGLES_DEG) {
      float rad = deg * (float)(M_PI / 180.0);
      float vx = spd * sinf(rad);
      float vy = spd * cosf(rad);
      float dx = vx * 0.25f; // 15 min displacement
      float dy = vy * 0.25f;

      float err = 0.0f;
      for (int p = 0; p < N_POINTS; p++) {
        float xp = GRID_X[p] - dx;
        float yp = GRID_Y[p] - dy;
        for (int k = 0; k <= 3; k++) {
          int s = k + 2;
          float fNext = F_[s + 1][p];
          float fPrev = interpolateField(s, xp, yp);
          float diff = fNext - fPrev;
          err += diff * diff;
        }
      }
      if (err < bestError) {
        bestError = err;
        bestVx = vx;
        bestVy = vy;
        bestSpeed = spd;
      }
    }
  }

  lastSpeed_ = bestSpeed;
  char fromCompass[3] = "";
  if (bestSpeed >= 1.0f) {
    float fromX = -bestVx;
    float fromY = -bestVy;
    float deg = atan2f(fromX, fromY) * (float)(180.0 / M_PI);
    if (deg < 0.0f) deg += 360.0f;
    int sector = (int)((deg + 22.5f) / 45.0f) % 8;
    static const char *SECTORS[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    snprintf(fromCompass, sizeof(fromCompass), "%s", SECTORS[sector]);
  }

  // 3. ETA: smallest t (5-min steps up to 90 min) where either advected field
  // F[0](home - v*t) or center forecast at t exceeds 0.5 mm/h
  bool etaFound = false;
  uint16_t foundEta = 0;

  for (int t = 0; t <= 90; t += 5) {
    int kFloor = t / 15;
    int kCeil = (kFloor >= 6) ? 6 : (kFloor + 1);
    float frac = (t % 15) / 15.0f;
    float centerVal = (1.0f - frac) * F_[kFloor + 2][0] + frac * F_[kCeil + 2][0];

    float advectX = -bestVx * ((float)t / 60.0f);
    float advectY = -bestVy * ((float)t / 60.0f);
    float advectVal = interpolateField(2, advectX, advectY); // k=0 is index 2

    float comb = (advectVal > centerVal) ? advectVal : centerVal;
    if (comb > 0.5f) {
      etaFound = true;
      foundEta = (uint16_t)t;
      break;
    }
  }

  if (etaFound) {
    // Intensity = max of the two over the next 30 min after ETA
    float maxInt = 0.0f;
    for (int t = (int)foundEta; t <= (int)foundEta + 30 && t <= 90; t += 5) {
      int kFloor = t / 15;
      int kCeil = (kFloor >= 6) ? 6 : (kFloor + 1);
      float frac = (t % 15) / 15.0f;
      float centerVal = (1.0f - frac) * F_[kFloor + 2][0] + frac * F_[kCeil + 2][0];

      float advectX = -bestVx * ((float)t / 60.0f);
      float advectY = -bestVy * ((float)t / 60.0f);
      float advectVal = interpolateField(2, advectX, advectY);

      float comb = (advectVal > centerVal) ? advectVal : centerVal;
      if (comb > maxInt) maxInt = comb;
    }

    // 4. RainAlert: active when ETA <= 60 min (or raining now: ETA 0)
    if (foundEta <= 60) {
      alert_.active = true;
      alert_.etaMin = foundEta;
      alert_.mmh = maxInt;
      snprintf(alert_.from, sizeof(alert_.from), "%s", fromCompass);
      alert_.dryKm = 0;
      cadenceMs_ = 5 * 60 * 1000; // 5 min cadence when alert active
    } else {
      alert_.active = false;
      alert_.etaMin = foundEta;
      alert_.mmh = maxInt;
      snprintf(alert_.from, sizeof(alert_.from), "%s", fromCompass);
      alert_.dryKm = 0;
      cadenceMs_ = 10 * 60 * 1000; // rain in grid -> 10 min
    }
  } else {
    alert_.active = false;
    alert_.etaMin = 0;
    alert_.mmh = 0.0f;
    alert_.from[0] = '\0';
    alert_.dryKm = anyRainInGrid ? 0 : 25;
    cadenceMs_ = anyRainInGrid ? (10 * 60 * 1000) : (30 * 60 * 1000);
  }

  if (outAlert) *outAlert = alert_;
  if (outCadenceMs) *outCadenceMs = cadenceMs_;
  return true;
}
