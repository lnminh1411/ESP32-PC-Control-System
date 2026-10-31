#include "radar.h"

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lgfx/utility/lgfx_pngle.h"
#include "net.h"
#include "settings_or_example.h"

static char lastErr[64];
const char *radarLastError() { return lastErr; }

// RainViewer's "Universal Blue" scheme (color 2), rain rows: dBZ -> RGB. With
// smoothing off (options 0_0) every pixel is one of these exactly. Higher than
// 75 dBZ draws as 75's green.
static const struct { int8_t dbz; uint32_t rgb; } PALETTE[] = {
    {-10, 0x636159}, {-9, 0x66635a}, {-8, 0x69665c}, {-7, 0x6c685d}, {-6, 0x6f6b5f}, {-5, 0x726e61},
    {-4, 0x757062}, {-3, 0x787364}, {-2, 0x7c7565}, {-1, 0x7f7867}, {0, 0x827b69}, {1, 0x857d6a},
    {2, 0x88806c}, {3, 0x8b826d}, {4, 0x8e856f}, {5, 0x928871}, {6, 0x9e9375}, {7, 0xaa9e79},
    {8, 0xb6a97e}, {9, 0xc2b482}, {10, 0xcec087}, {11, 0xd2c48b}, {12, 0xd6c88f}, {13, 0xdacc93},
    {14, 0xded097}, {15, 0x88ddee}, {16, 0x6cd1eb}, {17, 0x51c5e8}, {18, 0x36bae5}, {19, 0x1baee2},
    {20, 0x00a3e0}, {21, 0x009ad5}, {22, 0x0091ca}, {23, 0x0088bf}, {24, 0x007fb4}, {25, 0x0077aa},
    {26, 0x0070a3}, {27, 0x00699c}, {28, 0x006295}, {29, 0x005b8e}, {30, 0x005588}, {31, 0x005180},
    {32, 0x004e78}, {33, 0x004a70}, {34, 0x004768}, {35, 0xffee00}, {36, 0xffe000}, {37, 0xffd200},
    {38, 0xffc500}, {39, 0xffb700}, {40, 0xffaa00}, {41, 0xff9f00}, {42, 0xff9500}, {43, 0xff8b00},
    {44, 0xff8100}, {45, 0xff4400}, {46, 0xf23600}, {47, 0xe62800}, {48, 0xd91b00}, {49, 0xcd0d00},
    {50, 0xc10000}, {51, 0xa80000}, {52, 0x8f0000}, {53, 0x760000}, {54, 0x5d0000}, {55, 0xffaaff},
    {56, 0xff9fff}, {57, 0xff95ff}, {58, 0xff8bff}, {59, 0xff81ff}, {60, 0xff77ff}, {61, 0xff6cff},
    {62, 0xff62ff}, {63, 0xff58ff}, {64, 0xff4eff}, {65, 0xffffff}, {75, 0x00ff00},
};

static int dbzOf(uint32_t rgb) {
  for (auto &p : PALETTE)
    if (p.rgb == rgb) return p.dbz;
  return -128;   // not rain (snow rows, or a colour we don't know)
}

float radarDbzToMmh(int dbz) {
  if (dbz <= -128) return 0.0f;
  return powf(powf(10.0f, dbz / 10.0f) / 200.0f, 1.0f / 1.6f);   // Z = 200 R^1.6
}

bool radarRainingHere(const RadarObs &o) {
  return o.valid && o.dbzHere >= RADAR_RAIN_DBZ && o.pxHere >= RADAR_HERE_MIN_PX;
}

struct Buf { char *buf; size_t cap, len; bool full; };

static esp_err_t onData(esp_http_client_event_t *evt) {
  Buf *b = (Buf *)evt->user_data;
  if (evt->event_id != HTTP_EVENT_ON_DATA || !b) return ESP_OK;
  size_t n = evt->data_len;
  if (b->len + n + 1 > b->cap) {
    b->full = true;
    n = b->cap - 1 - b->len;
  }
  memcpy(b->buf + b->len, evt->data, n);
  b->len += n;
  b->buf[b->len] = 0;
  return ESP_OK;
}

// GET into buf; the length, 0 on failure. A tile has binary in it: the
// length is the answer's, not strlen's
static size_t get(const char *url, char *buf, size_t cap) {
  Buf b = {buf, cap, 0, false};
  buf[0] = 0;
  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.timeout_ms = 10000;
  cfg.event_handler = onData;
  cfg.user_data = &b;
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (!c) { snprintf(lastErr, sizeof lastErr, "no HTTP client"); return 0; }
  esp_err_t err = esp_http_client_perform(c);
  int status = esp_http_client_get_status_code(c);
  esp_http_client_cleanup(c);
  if (err != ESP_OK) { snprintf(lastErr, sizeof lastErr, "%s", esp_err_to_name(err)); return 0; }
  netNoteTraffic();
  if (status < 200 || status >= 300 || !b.len) { snprintf(lastErr, sizeof lastErr, "HTTP %d", status); return 0; }
  if (b.full) { snprintf(lastErr, sizeof lastErr, "answer over %u bytes", (unsigned)cap); return 0; }
  return b.len;
}

// The tile decode: the place is the centre pixel (128, 128)
struct Scan {
  const uint8_t *png;
  size_t len, pos;
  int hereR2;               // RADAR_HERE_KM in pixels, squared
  int dbzHere, pxHere;
  int nearR2;               // nearest echo so far, pixels squared
};

static uint32_t readCb(void *user, uint8_t *buf, uint32_t len) {
  Scan *s = (Scan *)user;
  size_t n = s->len - s->pos;
  if (n > len) n = len;
  if (buf) memcpy(buf, s->png + s->pos, n);   // null: pngle skipping a chunk
  s->pos += n;
  return (uint32_t)n;
}

static void drawCb(void *user, uint32_t x, uint32_t y, uint_fast8_t divX, size_t len, const uint8_t *argb) {
  Scan *s = (Scan *)user;
  const uint32_t *px = (const uint32_t *)argb;   // A | R<<8 | G<<16 | B<<24
  int dy = (int)y - 128;
  for (size_t i = 0; i < len; i++) {
    uint32_t p = px[i];
    if (!(p & 0xFF)) continue;   // transparent: no echo
    int dbz = dbzOf(((p >> 8) & 0xFF) << 16 | ((p >> 16) & 0xFF) << 8 | (p >> 24));
    if (dbz < RADAR_RAIN_DBZ) continue;
    int dx = (int)(x + i * divX) - 128;
    int r2 = dx * dx + dy * dy;
    if (r2 < s->nearR2) s->nearR2 = r2;
    if (r2 <= s->hereR2) {
      s->pxHere++;
      if (dbz > s->dbzHere) s->dbzHere = dbz;
    }
  }
}

bool radarFetch(double lat, double lon, char *buf, size_t cap, RadarObs *out) {
  if (!get("https://api.rainviewer.com/public/weather-maps.json", buf, cap)) return false;
  cJSON *root = cJSON_Parse(buf);
  cJSON *host = root ? cJSON_GetObjectItem(root, "host") : nullptr;
  cJSON *past = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "radar"), "past");
  cJSON *last = cJSON_IsArray(past) ? cJSON_GetArrayItem(past, cJSON_GetArraySize(past) - 1) : nullptr;
  cJSON *path = cJSON_GetObjectItem(last, "path"), *t = cJSON_GetObjectItem(last, "time");
  if (!cJSON_IsString(host) || !cJSON_IsString(path) || !cJSON_IsNumber(t)) {
    snprintf(lastErr, sizeof lastErr, "no radar frame in the list");
    cJSON_Delete(root);
    return false;
  }
  char url[192];
  snprintf(url, sizeof url, "%.64s%.48s/256/7/%.4f/%.4f/2/0_0.png", host->valuestring, path->valuestring, lat, lon);
  uint32_t frame = (uint32_t)t->valuedouble;
  cJSON_Delete(root);

  size_t n = get(url, buf, cap);
  if (!n) return false;
  double kmPerPx = 40075.0 * cos(lat * M_PI / 180.0) / (128.0 * 256.0);
  int hereR = (int)(RADAR_HERE_KM / kmPerPx + 0.5);
  Scan s = {(const uint8_t *)buf, n, 0, hereR * hereR, -128, 0, INT32_MAX};
  pngle_t *p = lgfx_pngle_new();
  if (!p) { snprintf(lastErr, sizeof lastErr, "no memory for the PNG decoder"); return false; }
  int ok = lgfx_pngle_prepare(p, readCb, &s);
  uint32_t w = lgfx_pngle_get_width(p), h = lgfx_pngle_get_height(p);
  if (ok >= 0 && w == 256 && h == 256) ok = lgfx_pngle_decomp(p, drawCb);
  lgfx_pngle_destroy(p);
  if (ok < 0 || w != 256 || h != 256) {
    snprintf(lastErr, sizeof lastErr, "tile didn't decode (%d, %ux%u)", ok, (unsigned)w, (unsigned)h);
    return false;
  }

  RadarObs o = {};
  o.valid = true;
  o.fetchedUnix = (uint32_t)time(nullptr);
  o.frameUnix = frame;
  o.lat = lat;
  o.lon = lon;
  o.dbzHere = (int8_t)s.dbzHere;
  o.pxHere = (uint8_t)(s.pxHere > 255 ? 255 : s.pxHere);
  o.mmhHere = radarDbzToMmh(s.dbzHere);
  o.nearKm = s.nearR2 == INT32_MAX ? -1 : (int16_t)(sqrt((double)s.nearR2) * kmPerPx + 0.5);
  *out = o;
  lastErr[0] = 0;
  return true;
}
