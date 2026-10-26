#include "nowcast.h"

#include <cJSON.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

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
                  "&minutely_15=precipitation&past_minutely_15=2&forecast_minutely_15=8&timeformat=unixtime");
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

  // Slot times (unix, UTC) from the centre point: the ETA counts from the clock,
  // not from the slot, or it would sit still for up to 15 min and then jump
  uint32_t slot2 = 0;
  {
    cJSON *item = cJSON_GetArrayItem(root, 0);
    cJSON *min15 = item ? cJSON_GetObjectItem(item, "minutely_15") : nullptr;
    cJSON *times = min15 ? cJSON_GetObjectItem(min15, "time") : nullptr;
    cJSON *t2 = (times && cJSON_GetArraySize(times) > 2) ? cJSON_GetArrayItem(times, 2) : nullptr;
    if (t2 && cJSON_IsNumber(t2)) slot2 = (uint32_t)t2->valuedouble;
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
    alert_ = {};
    alert_.dryKm = 25;
    cadenceMs_ = 15 * 60 * 1000; // all dry: the 15-min data can't change faster
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

  // 3. Timing. A slot's value at time T is the rain of the 15 min before T,
  // so it stands for T - 7.5 min; t counts minutes from that point of slot 2
  // (k = 0). Rain at t is the larger of the centre forecast and the k = 0
  // field advected by the motion vector.
  time_t nowT = time(nullptr);
  bool clockOk = nowT >= 1700000000 && slot2 > 0;
  uint32_t base = clockOk ? slot2 - 450 : 0;
  float tNow = clockOk ? (float)((double)nowT - (double)base) / 60.0f : 0.0f;
  if (tNow < 0.0f) tNow = 0.0f;
  if (tNow > 90.0f) tNow = 90.0f;

  auto rainAt = [&](float t) -> float {
    float k = t / 15.0f;
    if (k > 7.0f) k = 7.0f;
    int kf = (int)k;
    int kc = kf < 7 ? kf + 1 : 7;
    float frac = k - (float)kf;
    float centerVal = (1.0f - frac) * F_[kf + 2][0] + frac * F_[kc + 2][0];
    float advectVal = interpolateField(2, -bestVx * t / 60.0f, -bestVy * t / 60.0f);
    return (advectVal > centerVal) ? advectVal : centerVal;
  };

  static const float HORIZON = 105.0f;   // slot 9's midpoint
  bool rainingNow = rainAt(tNow) > 0.5f;
  float tStart = -1.0f;
  for (float t = tNow; t <= HORIZON; t += 5.0f)
    if (rainAt(t) > 0.5f) { tStart = t; break; }

  alert_ = {};
  if (tStart >= 0.0f) {
    // end: first step under 0.3 mm/h (hysteresis below the 0.5 start)
    float tEnd = -1.0f, maxInt = 0.0f;
    for (float t = tStart; t <= HORIZON; t += 5.0f) {
      float r = rainAt(t);
      if (r < 0.3f) { tEnd = t; break; }
      if (r > maxInt) maxInt = r;
    }
    uint16_t eta = (uint16_t)(tStart - tNow + 0.5f);
    alert_.raining = rainingNow;
    alert_.active = !rainingNow && eta <= 60;
    alert_.etaMin = rainingNow ? 0 : eta;
    alert_.startUnix = clockOk ? (rainingNow ? (uint32_t)nowT : base + (uint32_t)(tStart * 60.0f)) : 0;
    alert_.endUnix = (clockOk && tEnd >= 0.0f) ? base + (uint32_t)(tEnd * 60.0f) : 0;
    alert_.mmh = maxInt;
    snprintf(alert_.from, sizeof(alert_.from), "%s", fromCompass);
    // warning or rain: 5 min catches an hourly model update soon after it lands
    cadenceMs_ = (alert_.active || alert_.raining) ? 5 * 60 * 1000 : 10 * 60 * 1000;
  } else {
    alert_.dryKm = anyRainInGrid ? 0 : 25;
    cadenceMs_ = anyRainInGrid ? (10 * 60 * 1000) : (15 * 60 * 1000);
  }

  if (outAlert) *outAlert = alert_;
  if (outCadenceMs) *outCadenceMs = cadenceMs_;
  return true;
}
