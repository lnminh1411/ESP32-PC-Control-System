#include "cyclone.h"

#include <esp_heap_caps.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Both GDACS documents are scanned as text, feature by feature: the track is
// ~350 KB of GeoJSON (cJSON would need megabytes) and the list is small but
// cJSON nodes would come out of the scarce internal RAM.

static const char FEATURE[] = "{\"type\":\"Feature\"";
constexpr int64_t CYCLONE_MAX_AHEAD_S = 15 * 86400;   // track points further ahead are dropped

static const char *findIn(const char *b, const char *e, const char *needle) {
  size_t n = strlen(needle);
  for (const char *p = b; p + n <= e; p++)
    if (*p == *needle && !memcmp(p, needle, n)) return p;
  return nullptr;
}

static bool numAfter(const char *b, const char *e, const char *key, double *out) {
  const char *p = findIn(b, e, key);
  if (!p) return false;
  p += strlen(key);
  char *end;
  double v = strtod(p, &end);
  if (end == p) return false;
  *out = v;
  return true;
}

static bool strAfter(const char *b, const char *e, const char *key, char *out, size_t cap) {
  const char *p = findIn(b, e, key);
  if (!p) return false;
  p += strlen(key);
  size_t n = 0;
  while (p < e && *p != '"' && n + 1 < cap) out[n++] = *p++;
  out[n] = 0;
  return true;
}

float cycloneDistKm(double lat1, double lon1, double lat2, double lon2) {
  const double R = 6371.0, D = M_PI / 180.0;
  double dLat = (lat2 - lat1) * D, dLon = (lon2 - lon1) * D;
  double a = sin(dLat / 2) * sin(dLat / 2) + cos(lat1 * D) * cos(lat2 * D) * sin(dLon / 2) * sin(dLon / 2);
  return (float)(2.0 * R * atan2(sqrt(a), sqrt(1.0 - a)));
}

// Days since 1970-01-01 for a civil date (proleptic Gregorian)
static int64_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  int64_t yoe = y - era * 400;
  int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

void cycloneListUrl(char *out, size_t cap, time_t now) {
  time_t from = now - 10 * 86400, to = now + 86400;
  struct tm a, b;
  gmtime_r(&from, &a);
  gmtime_r(&to, &b);
  snprintf(out, cap,
           "https://www.gdacs.org/gdacsapi/api/events/geteventlist/SEARCH?eventlist=TC"
           "&fromDate=%04d-%02d-%02d&toDate=%04d-%02d-%02d",
           a.tm_year + 1900, a.tm_mon + 1, a.tm_mday, b.tm_year + 1900, b.tm_mon + 1, b.tm_mday);
}

void cycloneTrackUrl(char *out, size_t cap, uint32_t eventId, uint32_t episodeId) {
  snprintf(out, cap, "https://www.gdacs.org/gdacsapi/api/polygons/getgeometry?eventtype=TC&eventid=%u&episodeid=%u",
           (unsigned)eventId, (unsigned)episodeId);
}

int cycloneParseList(const char *json, double homeLat, double homeLon, CycloneCandidate *out, int max) {
  int n = 0;
  float dist[8];
  const char *end = json + strlen(json);
  const char *fs = strstr(json, FEATURE);
  while (fs) {
    const char *fe = findIn(fs + 1, end, FEATURE);
    const char *stop = fe ? fe : end;
    double ev, ep, sev = 0, lon, lat;
    char cur[8] = "", name[32] = "";
    const char *geo = findIn(fs, stop, "\"coordinates\":[");
    bool ok = geo && numAfter(fs, stop, "\"eventid\":", &ev) && numAfter(fs, stop, "\"episodeid\":", &ep) &&
              strAfter(fs, stop, "\"iscurrent\":\"", cur, sizeof cur) && !strcmp(cur, "true") &&
              strAfter(fs, stop, "\"eventname\":\"", name, sizeof name);
    if (ok) {
      char *e2;
      lon = strtod(geo + 15, &e2);
      ok = *e2 == ',';
      lat = ok ? strtod(e2 + 1, nullptr) : 0;
    }
    numAfter(fs, stop, "\"severity\":", &sev);
    if (ok) {
      float d = cycloneDistKm(homeLat, homeLon, lat, lon);
      if (d <= CYCLONE_WATCH_KM) {
        // "KAJIKI-25" -> "KAJIKI"
        char *dash = strrchr(name, '-');
        if (dash && dash != name) *dash = 0;
        CycloneCandidate c = {};
        c.eventId = (uint32_t)ev;
        c.episodeId = (uint32_t)ep;
        snprintf(c.name, sizeof c.name, "%s", name);
        c.windKmh = (uint16_t)(sev > 0 && sev < 600 ? sev : 0);
        c.lat = (float)lat;
        c.lon = (float)lon;
        // insert nearest first; a full list drops the furthest
        int at = n;
        while (at > 0 && dist[at - 1] > d) at--;
        if (at < max && max <= 8) {
          int last = n < max ? n : max - 1;
          for (int i = last; i > at; i--) { out[i] = out[i - 1]; dist[i] = dist[i - 1]; }
          out[at] = c;
          dist[at] = d;
          if (n < max) n++;
        }
      }
    }
    fs = fe;
  }
  return n;
}

bool cycloneParseTrack(const char *json, size_t len, const CycloneCandidate &c, time_t now, CycloneTrack *out) {
  memset(out, 0, sizeof *out);
  out->eventId = c.eventId;
  out->episodeId = c.episodeId;
  snprintf(out->name, sizeof out->name, "%s", c.name);
  out->windKmh = c.windKmh;

  struct tm nowTm;
  gmtime_r(&now, &nowTm);
  struct Seg { float lon1, lat1, lon2, lat2; char cat[4]; };
  Seg *segs = (Seg *)heap_caps_malloc(sizeof(Seg) * CYCLONE_MAX_POINTS, MALLOC_CAP_SPIRAM);
  if (!segs) return false;
  int nSeg = 0;

  const char *end = json + len;
  const char *fs = findIn(json, end, FEATURE);
  while (fs) {
    const char *fe = findIn(fs + 1, end, FEATURE);
    const char *stop = fe ? fe : end;
    char cls[32] = "";
    strAfter(fs, stop, "\"Class\":\"", cls, sizeof cls);
    if (!strncmp(cls, "Point_Polygon_Point_", 20) && out->n < CYCLONE_MAX_POINTS) {
      // the marker polygon's bbox centre is the position; key = MMDDhhmm UTC
      double b[4];
      char key[12] = "";
      const char *p = findIn(fs, stop, "\"bbox\":[");
      bool ok = p && strAfter(fs, stop, "\"key\":\"", key, sizeof key) && strlen(key) == 8;
      for (int i = 0; i < 8 && ok; i++) ok = key[i] >= '0' && key[i] <= '9';
      if (ok) {
        p += 8;
        for (int i = 0; i < 4 && ok; i++) {
          char *e2;
          b[i] = strtod(p, &e2);
          ok = e2 != p;
          p = e2 + 1;
        }
      }
      int mo = 0, d = 0, h = 0, mi = 0;
      if (ok) {
        mo = (key[0] - '0') * 10 + key[1] - '0';
        d = (key[2] - '0') * 10 + key[3] - '0';
        h = (key[4] - '0') * 10 + key[5] - '0';
        mi = (key[6] - '0') * 10 + key[7] - '0';
        double lat = (b[1] + b[3]) / 2, lon = (b[0] + b[2]) / 2;
        ok = mo >= 1 && mo <= 12 && d >= 1 && d <= 31 && h <= 23 && mi <= 59 && fabs(lat) <= 90 && fabs(lon) <= 180;
      }
      uint32_t t = 0;
      if (ok) {
        int y = nowTm.tm_year + 1900;
        if (mo > nowTm.tm_mon + 1 + 6) y--;        // December storm read in January
        else if (mo + 6 < nowTm.tm_mon + 1) y++;   // the other way round
        t = (uint32_t)(daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60);
        // cycloneJudge walks 15-min steps from now to the last point: a forecast
        // runs days, not months
        ok = (int64_t)t <= (int64_t)now + CYCLONE_MAX_AHEAD_S;
      }
      if (ok) {
        // insertion sort by time (GDACS lists them in order; this keeps it robust)
        int at = out->n;
        while (at > 0 && out->t[at - 1] > t) {
          out->t[at] = out->t[at - 1];
          out->lat[at] = out->lat[at - 1];
          out->lon[at] = out->lon[at - 1];
          at--;
        }
        out->t[at] = t;
        out->lon[at] = (float)((b[0] + b[2]) / 2);
        out->lat[at] = (float)((b[1] + b[3]) / 2);
        out->n++;
      }
    } else if (!strncmp(cls, "Line_Line_", 10) && nSeg < CYCLONE_MAX_POINTS) {
      Seg s = {};
      const char *p = findIn(fs, stop, "\"coordinates\":[[");
      if (p && strAfter(fs, stop, "\"polygonlabel\":\"", s.cat, sizeof s.cat)) {
        // [[lon1,lat1],[lon2,lat2]]: older point first
        if (sscanf(p + 16, "%f,%f],[%f,%f", &s.lon1, &s.lat1, &s.lon2, &s.lat2) == 4) segs[nSeg++] = s;
      }
    }
    fs = fe;
  }

  // a point's class: the segment that ends there, else one that starts there
  for (int i = 0; i < out->n; i++) {
    for (int pass = 0; pass < 2 && !out->cat[i][0]; pass++) {
      for (int k = 0; k < nSeg; k++) {
        float lo = pass ? segs[k].lon1 : segs[k].lon2, la = pass ? segs[k].lat1 : segs[k].lat2;
        if (fabsf(lo - out->lon[i]) < 0.15f && fabsf(la - out->lat[i]) < 0.15f) {
          memcpy(out->cat[i], segs[k].cat, sizeof out->cat[i]);
          break;
        }
      }
    }
  }
  heap_caps_free(segs);
  return out->n > 0;
}

static void posAt(const CycloneTrack &tr, uint32_t t, float *lat, float *lon) {
  if (t <= tr.t[0]) { *lat = tr.lat[0]; *lon = tr.lon[0]; return; }
  for (int i = 1; i < tr.n; i++) {
    if (t <= tr.t[i]) {
      float f = (float)(t - tr.t[i - 1]) / (float)(tr.t[i] - tr.t[i - 1]);
      *lat = tr.lat[i - 1] + f * (tr.lat[i] - tr.lat[i - 1]);
      *lon = tr.lon[i - 1] + f * (tr.lon[i] - tr.lon[i - 1]);
      return;
    }
  }
  *lat = tr.lat[tr.n - 1];
  *lon = tr.lon[tr.n - 1];
}

static int nearestPoint(const CycloneTrack &tr, uint32_t t) {
  int best = 0;
  for (int i = 1; i < tr.n; i++)
    if (labs((long)tr.t[i] - (long)t) < labs((long)tr.t[best] - (long)t)) best = i;
  return best;
}

// TD / TS stay "Storm"; HU (GDACS's word for typhoon strength), TY, STY and
// Saffir-Simpson categories count as typhoons
static bool typhoonClass(const char *cat) {
  return cat[0] == 'H' || !strcmp(cat, "TY") || !strcmp(cat, "STY") || (cat[0] == 'C' && cat[1] >= '1' && cat[1] <= '5');
}

void cycloneJudge(const CycloneTrack &tr, double homeLat, double homeLon, time_t now, Cyclone *out) {
  *out = {};
  if (tr.n <= 0) return;
  snprintf(out->name, sizeof out->name, "%s", tr.name);
  out->windKmh = tr.windKmh;

  uint32_t tNow = (uint32_t)now, last = tr.t[tr.n - 1];
  bool stale = tNow > last + 12 * 3600;   // GDACS stopped updating it
  float la, lo;
  posAt(tr, tNow, &la, &lo);
  float dNow = cycloneDistKm(homeLat, homeLon, la, lo);
  out->distKm = (uint16_t)(dNow < 65000 ? dNow : 65000);
  bool inRange = !stale && dNow <= CYCLONE_RANGE_KM;

  // walk the forecast in 15-min steps: first arrival in range, closest approach
  float closest = dNow;
  uint32_t closestT = tNow, arrive = 0;
  for (uint32_t t = tNow > tr.t[0] ? tNow : tr.t[0]; !stale && t <= last; t += 900) {
    posAt(tr, t, &la, &lo);
    float d = cycloneDistKm(homeLat, homeLon, la, lo);
    if (d < closest) { closest = d; closestT = t; }
    if (!arrive && d <= CYCLONE_RANGE_KM) arrive = t;
  }
  out->closestKm = (uint16_t)(closest < 65000 ? closest : 65000);

  // strength where it matters: now when in range, else at arrival
  int pi = nearestPoint(tr, inRange ? tNow : arrive ? arrive : closestT);
  const char *cat = tr.cat[pi];
  bool typhoon = cat[0] ? typhoonClass(cat) : tr.windKmh >= 118;
  // GDACS gives the peak wind of the whole storm, not at arrival
  out->superTyphoon = typhoon && (!strcmp(cat, "STY") || tr.windKmh >= 184);
  out->active = typhoon && inRange;
  out->tracked = typhoon && (inRange || arrive);
  out->arriveUnix = inRange ? 0 : arrive;
}
