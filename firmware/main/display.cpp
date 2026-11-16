#include "display.h"
#include "display_config.h"
#include <nvs.h>

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_st7789.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <string.h>
#include <algorithm>
#include <math.h>

#include <LovyanGFX.hpp>

#include "gif_store.h"
#include "screens.h"
#include "log.h"
#include "sys.h"
#include "vis.h"

#define BAND 8
#define INTERNAL_RESERVE (160 * 1024)   // Wi-Fi + two TLS sessions need it

Panel panels[N_PANELS];

static uint8_t *dmaBuf[2];
static volatile uint32_t txDone;
static uint32_t txIssued;
static QueueHandle_t flushQueue;
static QueueHandle_t gifQueue;      // GIF panels to decode before flush
static TaskHandle_t flushTaskHandle;   // the only task that waits on DMA
static volatile float busWirePct = 0.0f;

float busLoadPct() { return busWirePct; }

static bool IRAM_ATTR onTxDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *) {
  txDone = txDone + 1;   // only this ISR writes it
  BaseType_t woken = pdFALSE;
  if (flushTaskHandle) vTaskNotifyGiveFromISR(flushTaskHandle, &woken);
  return woken == pdTRUE;
}

uint32_t displaySpinUs;   // perf: time the flush task spends blocked on DMA
uint32_t gifDecodeUs, gifDecodeN, gifPackUs;   // perf: gifdec time, frames decoded, flush time packing GIF pixels

// Blocks (core 0 free for other work) until at most inFlight transfers are
// queued. Each finished transfer notifies; the timeout only guards against a
// lost interrupt.
static void waitTx(uint32_t inFlight) {
  if (txIssued - txDone <= inFlight) return;
  uint32_t t = nowUs();
  while (txIssued - txDone > inFlight) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
  displaySpinUs += nowUs() - t;
}

// CASET/RASET for a canvas rect, shifted by the panel's RAM offsets
static void setWindow(Panel &p, int x, int y, int w, int h) {
  const PanelConfig &cfg = displayPanels[&p - panels];
  x += cfg.colStart;
  y += cfg.rowStart;
  uint8_t ca[4] = {(uint8_t)(x >> 8), (uint8_t)x, (uint8_t)((x + w - 1) >> 8), (uint8_t)(x + w - 1)};
  uint8_t ra[4] = {(uint8_t)(y >> 8), (uint8_t)y, (uint8_t)((y + h - 1) >> 8), (uint8_t)(y + h - 1)};
  esp_lcd_panel_io_tx_param(p.io, 0x2A, ca, 4);   // CASET
  esp_lcd_panel_io_tx_param(p.io, 0x2B, ra, 4);   // RASET
}

// ---- Music-reactive cover border (vis.h) ----
// An 8 px ring around the media cover with rounded corners (radius 16 out, 8
// in, the cover's own), owned by this task: pushed as 4 strips
// at 30 fps straight from a colour table (no canvas, no PSRAM reads), and
// painted over the canvas wherever a face push crosses it, so face redraws
// neither flicker nor push it. drawMedia asks for it per frame
// (displaySetRing); it is applied when that frame is flushed. One panel at a
// time.
#define RING_T 8
#define RING_R 16       // outer corner radius; the cover's is RING_R - RING_T, same centres
#define RING_MS 33
#define RING_BLEND 20   // px over which neighbouring colours blend
struct RingGeom { int16_t x, y, w, h; };   // outer rect, x and w even; w 0 = none
static RingGeom ringReq[N_PANELS];   // core 1, read when that panel's frame is flushed
static RingGeom ringCur;             // this task
static int ringPanel = -1;
static uint16_t ringLut[2 * (TFT_W + TFT_H)];   // RGB444 per perimeter position, clockwise from top left
static bool ringDark = true;
static uint32_t ringDueMs, ringLastMs;

// heavy bass .. upper high
static const uint8_t ringColors[VIS_BANDS][3] = {
  {0xc8, 0, 0}, {0xff, 0, 0}, {0, 0, 0xc8}, {0, 0, 0xff}, {0, 0xc8, 0}, {0, 0xff, 0}};

void displaySetRing(LGFX_Sprite &c, int x, int y, int w, int h) {
  for (int i = 0; i < N_PANELS; i++)
    if (panels[i].canvas == &c) ringReq[i] = {(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h};
}

// Top-left corner box of the ring, (x, y) from the corner: ring coverage
// 0..255 (antialiased at both radii) and whether the cover is behind the
// inner edge (else black, outside the outer edge). The other corners mirror it.
static uint8_t cornerA[RING_R][RING_R];
static bool cornerArt[RING_R][RING_R];

static void cornerTables() {
  static bool done;
  if (done) return;
  done = true;
  for (int y = 0; y < RING_R; y++)
    for (int x = 0; x < RING_R; x++) {
      float d = hypotf(RING_R - (x + 0.5f), RING_R - (y + 0.5f));
      float out = RING_R + 0.5f - d, in = d - (RING_R - RING_T) + 0.5f;
      float a = std::min(std::max(std::min(out, in), 0.0f), 1.0f);
      cornerA[y][x] = (uint8_t)(a * 255.0f + 0.5f);
      cornerArt[y][x] = d < RING_R - RING_T / 2;
    }
}

static inline void put2(uint8_t *o, uint16_t a, uint16_t c) {
  o[0] = a >> 4;                // R1 G1
  o[1] = (a << 4) | (c >> 8);   // B1 R2
  o[2] = c;                     // G2 B2
}

static inline void set444(uint8_t *row, int i, uint16_t c) {
  uint8_t *o = row + (i >> 1) * 3;
  if (!(i & 1)) {
    o[0] = c >> 4;                          // R1 G1
    o[1] = (o[1] & 0x0F) | (uint8_t)(c << 4);   // B1
  } else {
    o[1] = (o[1] & 0xF0) | (c >> 8);        // R2
    o[2] = c;                               // G2 B2
  }
}

// canvas/sent pixel (byte-swapped RGB565) -> RGB444, as push444 packs it
static inline uint16_t to444(uint16_t sw) {
  uint16_t a = __builtin_bswap16(sw);
  return (uint16_t)((a >> 12) << 8 | ((a >> 7) & 15) << 4 | ((a >> 1) & 15));
}

static inline uint16_t scale444(uint16_t c, int a) {   // a 0..255
  return (uint16_t)((((c >> 8) & 15) * a + 127) / 255 << 8 | (((c >> 4) & 15) * a + 127) / 255 << 4 |
                    ((c & 15) * a + 127) / 255);
}

static inline uint16_t add444(uint16_t a, uint16_t b) {
  int r = std::min(((a >> 8) & 15) + ((b >> 8) & 15), 15), g = std::min(((a >> 4) & 15) + ((b >> 4) & 15), 15),
      bl = std::min((a & 15) + (b & 15), 15);
  return (uint16_t)(r << 8 | g << 4 | bl);
}

// The ring's colour at ring-relative (rx, ry) over the pixel behind it
// (byte-swapped RGB565), or -1 where the cover shows untouched. Perimeter
// positions run clockwise from the top left: top, right, bottom (right to
// left), left (bottom to top).
static int ringColor(const RingGeom &g, int rx, int ry, uint16_t behind) {
  int W = g.w, H = g.h;
  int pos = ry < RING_T ? rx : ry >= H - RING_T ? W + H + (W - 1 - rx) : rx < W / 2 ? 2 * W + H + (H - 1 - ry) : W + ry;
  uint16_t c = ringLut[pos];
  int cx = rx < RING_R ? rx : rx >= W - RING_R ? W - 1 - rx : -1;
  int cy = ry < RING_R ? ry : ry >= H - RING_R ? H - 1 - ry : -1;
  if (cx >= 0 && cy >= 0) {
    int a = cornerA[cy][cx];
    uint16_t r = a == 255 ? c : scale444(c, a);
    return cornerArt[cy][cx] ? add444(to444(behind), r) : r;   // the cover's corner is already faded to black
  }
  if (rx < RING_T || rx >= W - RING_T || ry < RING_T || ry >= H - RING_T) return c;
  return -1;
}

// Paint the ring over one packed RGB444 row (panel row y, columns [x, x+w),
// x even) whose source pixels (byte-swapped RGB565, panel row y from column
// 0) are src. Per pixel only in the RING_R corner boxes; elsewhere straight
// from the table, two pixels at a time.
static void ringRow(uint8_t *row, int y, int x, int w, const uint16_t *src) {
  const RingGeom &g = ringCur;
  int ry = y - g.y, W = g.w, H = g.h;
  if (ry < 0 || ry >= H) return;
  int a = std::max(x, (int)g.x) - g.x, b = std::min(x + w, g.x + W) - g.x;   // ring columns [a, b), even
  if (a >= b) return;
  int off = g.x - x;   // row pixel of ring column 0, even
  auto perPixel = [&](int r0, int r1) {
    for (int rx = std::max(a, r0); rx < std::min(b, r1); rx++) {
      int c = ringColor(g, rx, ry, src[g.x + rx]);
      if (c >= 0) set444(row, rx + off, (uint16_t)c);
    }
  };
  if (ry < RING_T || ry >= H - RING_T) {   // top or bottom: the whole width
    perPixel(0, RING_R);
    perPixel(W - RING_R, W);
    bool top = ry < RING_T;
    int base = W + H;
    for (int rx = std::max(a, RING_R); rx < std::min(b, W - RING_R); rx += 2)
      put2(row + (rx + off) / 2 * 3, ringLut[top ? rx : base + W - 1 - rx], ringLut[top ? rx + 1 : base + W - 2 - rx]);
  } else if (ry < RING_R || ry >= H - RING_R) {   // the rest of the corner boxes
    perPixel(0, RING_R);
    perPixel(W - RING_R, W);
  } else {   // a side: RING_T pixels of one colour each side
    uint16_t cl = ringLut[2 * W + H + (H - 1 - ry)], cr = ringLut[W + ry];
    for (int rx = a; rx < std::min(b, RING_T); rx += 2) put2(row + (rx + off) / 2 * 3, cl, cl);
    for (int rx = std::max(a, W - RING_T); rx < b; rx += 2) put2(row + (rx + off) / 2 * 3, cr, cr);
  }
}

// Panel screenshot (cmdShot): the ring over a copy of the canvas (byte-swapped
// RGB565). Reads this task's state from another task: a torn colour at worst.
void displayShotRing(int panel, uint16_t *buf) {
  if (panel != ringPanel || !ringCur.w) return;
  const RingGeom g = ringCur;
  for (int ry = 0; ry < g.h; ry++)
    for (int rx = 0; rx < g.w; rx++) {
      uint16_t *px = buf + (size_t)(g.y + ry) * TFT_W + g.x + rx;
      int c = ringColor(g, rx, ry, *px);
      if (c < 0) continue;
      int r = (c >> 8) & 15, gr = (c >> 4) & 15, bl = c & 15;
      uint16_t v = (uint16_t)((r << 12) | (r >> 3 << 11) | (gr << 7) | (gr >> 2 << 5) | (bl << 1) | (bl >> 3));
      *px = __builtin_bswap16(v);
    }
}

// The colour table: the bands in order around the ring, each as long as its
// share of the lengths (a band under 10% of the longest is left out),
// turning once in 120 s (0.5 RPM); RING_BLEND px of blending at each
// boundary. Each region is lit 60..100% by its band's volume against the
// song; the whole ring by the longest length (1 while music plays, falling
// to 0 on silence: the fade). One band alone fills the ring.
static void ringCompute(const float lv[VIS_BANDS], const float vol[VIS_BANDS], bool any) {
  const RingGeom &g = ringCur;
  int P = 2 * (g.w + g.h);
  if (!any) {
    memset(ringLut, 0, P * sizeof ringLut[0]);
    return;
  }
  float mx = 0;
  for (int b = 0; b < VIS_BANDS; b++) mx = std::max(mx, lv[b]);
  int seg[VIS_BANDS], n = 0;
  float len[VIS_BANDS], sum = 0;
  for (int b = 0; b < VIS_BANDS; b++)
    if (lv[b] >= mx * 0.1f) { seg[n] = b; len[n] = lv[b]; sum += lv[b]; n++; }
  float start[VIS_BANDS + 1];
  start[0] = 0;
  for (int i = 0; i < n; i++) start[i + 1] = start[i] + len[i] / sum * P;
  start[n] = (float)P;
  float off = (float)(nowMs() % 120000) / 120000.0f * P;
  float k = mx / 255.0f * 15.0f;   // 8-bit colour * fade -> 4 bits
  float cs[VIS_BANDS][3];           // each region's colour at its brightness
  for (int i = 0; i < n; i++) {
    float v = vol[seg[i]];
    float m = 0.6f + 0.4f * (v < 0 ? 0 : v > 1 ? 1 : v);
    for (int c = 0; c < 3; c++) cs[i][c] = ringColors[seg[i]][c] * m;
  }
  for (int p = 0; p < P; p++) {
    float u = p - off;
    if (u < 0) u += P;
    int i = 0;
    while (i < n - 1 && u >= start[i + 1]) i++;
    const float *c = cs[i];
    float r = c[0], gr = c[1], bl = c[2];
    if (n > 1) {
      float d0 = u - start[i], d1 = start[i + 1] - u;
      int j = -1;
      float t = 1;
      if (d0 < RING_BLEND / 2 && d0 <= d1) { j = (i + n - 1) % n; t = 0.5f + d0 / RING_BLEND; }
      else if (d1 < RING_BLEND / 2) { j = (i + 1) % n; t = 0.5f + d1 / RING_BLEND; }
      if (j >= 0) {
        const float *o = cs[j];
        r = r * t + o[0] * (1 - t);
        gr = gr * t + o[1] * (1 - t);
        bl = bl * t + o[2] * (1 - t);
      }
    }
    int r4 = (int)(r * k + 0.5f), g4 = (int)(gr * k + 0.5f), b4 = (int)(bl * k + 0.5f);
    ringLut[p] = (uint16_t)(std::min(r4, 15) << 8 | std::min(g4, 15) << 4 | std::min(b4, 15));
  }
}

// One ring strip. The sides (RING_R wide) carry the cover's edge columns, so
// they start from what is on the glass (sent); the top and bottom are all
// ring. The strip is packed while the previous one is still on the wire.
static void pushStrip(Panel &p, int x, int y, int w, int h, bool fromSent) {
  waitTx(1);   // the buffer we fill next is free
  uint8_t *b = dmaBuf[txIssued & 1], *o = b;
  size_t rowBytes = (size_t)w * 3 / 2;
  for (int r = 0; r < h; r++, o += rowBytes) {
    const uint16_t *srow = p.sent + (size_t)(y + r) * TFT_W;
    if (fromSent) {
      uint8_t *q = o;
      for (int k = x; k < x + w; k += 2, q += 3) {
        uint16_t a = to444(srow[k]), c = to444(srow[k + 1]);
        q[0] = a >> 4;
        q[1] = (uint8_t)(a << 4) | (c >> 8);
        q[2] = c;
      }
    }
    ringRow(o, y + r, x, w, srow);
  }
  waitTx(0);   // CASET/RASET go out after the previous strip
  setWindow(p, x, y, w, h);
  txIssued++;
  esp_lcd_panel_io_tx_color(p.io, 0x2C, b, o - b);
  p.bytes += o - b;
}

// sent over the ring made stale, so the next diff repaints it from the canvas
static void ringForget(Panel &p, const RingGeom &g) {
  for (int y = g.y; y < g.y + g.h; y++) {
    uint16_t *row = p.sent + (size_t)y * TFT_W;
    if (y < g.y + RING_T || y >= g.y + g.h - RING_T) {
      for (int x = g.x; x < g.x + g.w; x++) row[x] = 0xA5A5;
    } else {
      for (int x = g.x; x < g.x + RING_R; x++) row[x] = 0xA5A5;
      for (int x = g.x + g.w - RING_R; x < g.x + g.w; x++) row[x] = 0xA5A5;
    }
  }
}

// A face frame of panel idx is about to be flushed: take its ring request
static void ringApply(Panel &p, int idx) {
  RingGeom g = ringReq[idx];
  ringReq[idx] = {0, 0, 0, 0};   // one-shot: views that never call screensDraw (stats, bench) drop it
  cornerTables();
  bool ok = g.w >= 2 * RING_R + 2 && g.h >= 2 * RING_R + 2 && g.x >= 0 && g.y >= 0 && g.x + g.w <= TFT_W &&
            g.y + g.h <= TFT_H && !(g.x & 1) && !(g.w & 1) && !p.gifMode && visEnabled();
  if (!ok) g = {0, 0, 0, 0};
  if (ringPanel >= 0 && ringPanel != idx) return;   // on another panel already
  if (ringPanel < 0 && !g.w) return;
  if (!memcmp(&g, &ringCur, sizeof g)) return;
  if (ringPanel == idx && ringCur.w) ringForget(p, ringCur);
  ringCur = g;
  ringPanel = g.w ? idx : -1;
  if (ringPanel >= 0) {
    float lv[VIS_BANDS], vol[VIS_BANDS];
    bool any = visLevels(lv, vol);
    ringCompute(lv, vol, any);   // the face push below paints the ring with it
    ringDark = !any;
    ringDueMs = nowMs() + RING_MS;
    ringLastMs = 0;
  }
}

static void ringTick() {
  if (ringPanel < 0) return;
  if (panels[ringPanel].gifMode) {   // a GIF face took the panel (even before its first frame)
    ringPanel = -1;
    ringCur = {0, 0, 0, 0};
    return;
  }
  // core 1 reset this panel's sent (new face, wake): the strips would carry
  // garbage from it until the panel's next flush repaints everything
  if (panels[ringPanel].sentReset) return;
  uint32_t now = nowMs();
  if ((int32_t)(now - ringDueMs) < 0) return;
  ringDueMs += RING_MS;
  if ((int32_t)(now - ringDueMs) >= 0) ringDueMs = now + RING_MS;   // fell behind: no burst
  float lv[VIS_BANDS], vol[VIS_BANDS];
  bool any = visLevels(lv, vol);
  if ((!any && ringDark) || backlightIsIdle()) {
    ringLastMs = 0;
    return;
  }
  uint32_t t0 = nowUs();
  ringCompute(lv, vol, any);
  Panel &p = panels[ringPanel];
  const RingGeom &g = ringCur;
  pushStrip(p, g.x, g.y, g.w, RING_T, false);
  pushStrip(p, g.x + g.w - RING_R, g.y + RING_T, RING_R, g.h - 2 * RING_T, true);
  pushStrip(p, g.x, g.y + g.h - RING_T, g.w, RING_T, false);
  pushStrip(p, g.x, g.y + RING_T, RING_R, g.h - 2 * RING_T, true);
  waitTx(0);
  uint32_t us = nowUs() - t0;
  p.pushUs += us;
  visRingPushed(us, ringLastMs ? now - ringLastMs : 0);
  ringLastMs = any ? now : 0;
  ringDark = !any;
}

// Called where a long flush can pause (between diff bands, GIF rects): a
// ring frame that is due goes out now instead of after the whole frame
static void ringIfDue() {
  if (ringPanel >= 0 && (int32_t)(nowMs() - ringDueMs) >= 0) {
    waitTx(0);
    ringTick();
  }
}

// Push canvas rect (x, y, w, h) of panel p as RGB444. x and w must be even.
static void push444(Panel &p, int x, int y, int w, int h) {
  const uint16_t *src = (const uint16_t *)p.canvas->getBuffer();
  waitTx(0);
  setWindow(p, x, y, w, h);
  size_t rowBytes = (size_t)w * 3 / 2;
  int rowsPerChunk = std::max(1, (int)(CHUNK_BYTES / rowBytes));
  bool first = true;
  for (int r0 = 0; r0 < h; r0 += rowsPerChunk) {
    int rows = std::min(rowsPerChunk, h - r0);
    if (r0 > 0 && ringPanel >= 0 && (int32_t)(nowMs() - ringDueMs) >= 0) {
      // a long push (a whole face is ~35 ms) would make the ring miss a
      // frame: put it out between chunks, then reopen the window for the rest.
      // The side strips read sent: the rows already out go into it first.
      waitTx(0);
      for (int r = 0; r < r0; r++)
        memcpy(p.sent + (size_t)(y + r) * TFT_W + x, src + (size_t)(y + r) * TFT_W + x, w * 2);
      ringTick();
      setWindow(p, x, y + r0, w, h - r0);
      first = true;
    }
    waitTx(1);                                    // the buffer we fill next is free
    uint8_t *b = dmaBuf[txIssued & 1], *o = b;
    bool ring = ringPanel == &p - panels;
    for (int r = 0; r < rows; r++) {
      const uint16_t *s = src + (size_t)(y + r0 + r) * TFT_W + x;
      uint8_t *rowStart = o;
      for (int k = 0; k < w; k += 2) {
        uint16_t a = __builtin_bswap16(s[k]);
        uint16_t c = __builtin_bswap16(s[k + 1]);
        o[0] = ((a >> 8) & 0xF0) | ((a >> 7) & 0x0F);               // R1 G1
        o[1] = ((a << 3) & 0xF0) | (c >> 12);                       // B1 R2
        o[2] = ((c >> 3) & 0xF0) | ((c >> 1) & 0x0F);               // G2 B2
        o += 3;
      }
      if (ring) ringRow(rowStart, y + r0 + r, x, w, src + (size_t)(y + r0 + r) * TFT_W);
    }
    txIssued++;
    esp_lcd_panel_io_tx_color(p.io, first ? 0x2C : 0x3C, b, o - b);   // RAMWR, then RAMWRC
    first = false;
  }
  p.bytes += rowBytes * h;
}

// Changed x range of canvas vs sent over rows [y, y+n): false = unchanged.
static bool bandDiff(Panel &p, int y, int n, int &x0, int &x1) {
  const uint32_t *c = (const uint32_t *)p.canvas->getBuffer(), *s = (const uint32_t *)p.sent;
  const int words = TFT_W / 2;
  int lo = words, hi = -1;
  for (int r = y; r < y + n; r++) {
    const uint32_t *cr = c + (size_t)r * words, *sr = s + (size_t)r * words;
    int a = 0;
    while (a < lo && cr[a] == sr[a]) a++;
    if (a == words) continue;
    lo = std::min(lo, a);
    int b = words - 1;
    while (b > hi && cr[b] == sr[b]) b--;
    hi = std::max(hi, b);
  }
  if (hi < 0) return false;
  x0 = lo * 2;
  x1 = hi * 2 + 1;
  return true;
}

// Push the changed parts of canvas rows [yStart, yEnd) (yStart a multiple of BAND).
static void flushRows(Panel &p, int yStart, int yEnd) {
  int runY = -1, rx0 = TFT_W, rx1 = -1;
  for (int y = yStart; y <= yEnd; y += BAND) {
    int x0, x1;
    ringIfDue();
    bool dirty = y < yEnd && bandDiff(p, y, std::min(BAND, yEnd - y), x0, x1);
    if (dirty) {
      if (runY < 0) runY = y;
      rx0 = std::min(rx0, x0);
      rx1 = std::max(rx1, x1);
    } else if (runY >= 0) {                     // a run of dirty bands ended: push it
      int h = y - runY, w = rx1 - rx0 + 1;
      push444(p, rx0, runY, w, h);
      for (int r = runY; r < y; r++)
        memcpy(p.sent + (size_t)r * TFT_W + rx0, (const uint16_t *)p.canvas->getBuffer() + (size_t)r * TFT_W + rx0, w * 2);
      runY = -1; rx0 = TFT_W; rx1 = -1;
    }
  }
}

static uint32_t displayFlush(Panel &p) {
  uint32_t t0 = nowUs();
  p.sentReset = false;   // this flush repaints from the canvas whatever sent says
  ringApply(p, &p - panels);
  flushRows(p, 0, TFT_H);
  waitTx(0);
  uint32_t us = nowUs() - t0;
  p.pushUs += us;
  p.frames++;
  return us;
}

// n indices through the palette to RGB444 at o (word aligned), 2 px in 3
// bytes. A run that starts word aligned goes 8 px at a time: two word loads,
// three word stores.
static void packPixels(uint8_t *o, const uint8_t *s, const uint16_t *pal, size_t n) {
  if (!((uintptr_t)s & 3)) {
    const uint32_t *in = (const uint32_t *)s;
    uint32_t *out = (uint32_t *)o;
    for (; n >= 8; n -= 8) {
      uint32_t a = in[0], b = in[1];
      in += 2;
      uint32_t x0 = (uint32_t)pal[a & 255] << 12 | pal[(a >> 8) & 255];    // 2 px, 24 bits, first px on top
      uint32_t x1 = (uint32_t)pal[(a >> 16) & 255] << 12 | pal[a >> 24];
      uint32_t x2 = (uint32_t)pal[b & 255] << 12 | pal[(b >> 8) & 255];
      uint32_t x3 = (uint32_t)pal[(b >> 16) & 255] << 12 | pal[b >> 24];
      out[0] = __builtin_bswap32(x0 << 8 | x1 >> 16);                      // the 12 bytes, first on the wire lowest
      out[1] = __builtin_bswap32(x1 << 16 | x2 >> 8);
      out[2] = __builtin_bswap32(x2 << 24 | x3);
      out += 3;
    }
    s = (const uint8_t *)in;
    o = (uint8_t *)out;
  }
  for (; n >= 2; n -= 2, s += 2, o += 3) {
    uint16_t a = pal[s[0]], c = pal[s[1]];
    o[0] = a >> 4;                                                  // R1 G1
    o[1] = (a << 4) | (c >> 8);                                     // B1 R2
    o[2] = c;                                                       // G2 B2
  }
}

// Rows [r0, r1) of a GIF rect at panel (gx, gy), rw wide, whose indices start
// at src (row-major, rw a row): packed into the DMA chunks as they go out.
// Each chunk is packed while the one before it is on the wire, the previous
// rect's last chunk included: most rects fit one chunk.
static void pushGifRows(Panel &p, const uint8_t *src, const uint16_t *pal, int gx, int gy, int rw, int r0, int r1) {
  if (r1 <= r0) return;
  const size_t chunkPx = CHUNK_BYTES / 3 * 2;   // a multiple of 8
  size_t n = (size_t)rw * (r1 - r0);
  const uint8_t *s = src + (size_t)r0 * rw;
  for (size_t i = 0; i < n; i += chunkPx) {
    size_t m = std::min(chunkPx, n - i);
    waitTx(1);
    uint8_t *b = dmaBuf[txIssued & 1];
    uint32_t t = nowUs();
    packPixels(b, s + i, pal, m);
    gifPackUs += nowUs() - t;
    if (!i) {          // the window waits for the bus to go idle
      waitTx(0);
      setWindow(p, gx, gy + r0, rw, r1 - r0);
    }
    txIssued++;
    esp_lcd_panel_io_tx_color(p.io, i ? 0x3C : 0x2C, b, m * 3 / 2);
  }
  p.bytes += n * 3 / 2;
}

// GIF rect minus the overlay band: up to two slices above and below it
static void pushGifRect(Panel &p, const uint8_t *src, const uint16_t *pal, int rx, int ry, int rw, int rh, int x0, int y0) {
  int gx = x0 + rx, gy = y0 + ry;
  if (p.bandY1 <= p.bandY0) { pushGifRows(p, src, pal, gx, gy, rw, 0, rh); return; }
  pushGifRows(p, src, pal, gx, gy, rw, 0, std::clamp(p.bandY0 - gy, 0, rh));
  pushGifRows(p, src, pal, gx, gy, rw, std::clamp(p.bandY1 - gy, 0, rh), rh);
}

// Overlay band over a GIF: canvas pixels, except C_GIF_KEY ones, which take
// the GIF pixel underneath. Pushed whole (full width) whenever either side
// of it changed.
static void pushBand(Panel &p, const uint16_t *pal, int x0, int y0) {
  const uint16_t key = __builtin_bswap16(C_GIF_KEY);   // sprite stores big-endian
  const uint16_t *canvas = (const uint16_t *)p.canvas->getBuffer();
  int yA = p.bandY0, yB = p.bandY1;
  waitTx(0);
  setWindow(p, 0, yA, TFT_W, yB - yA);
  const size_t rowBytes = (size_t)TFT_W * 3 / 2;
  int rowsPerChunk = std::max(1, (int)(CHUNK_BYTES / rowBytes));
  bool first = true;
  for (int r0 = yA; r0 < yB; r0 += rowsPerChunk) {
    int rows = std::min(rowsPerChunk, yB - r0);
    waitTx(1);
    uint8_t *b = dmaBuf[txIssued & 1], *o = b;
    for (int y = r0; y < r0 + rows; y++) {
      const uint16_t *s = canvas + (size_t)y * TFT_W;
      int gy = y - y0;
      const uint8_t *g = (gy >= 0 && gy < gif.h) ? p.gifCur + (size_t)gy * gif.w : nullptr;
      for (int x = 0; x < TFT_W; x += 2) {
        uint16_t px[2];
        for (int k = 0; k < 2; k++) {
          uint16_t c = s[x + k];
          if (c != key) { px[k] = __builtin_bswap16(c); continue; }
          int gx = x + k - x0;
          if (!g || gx < 0 || gx >= gif.w) { px[k] = 0; continue; }
          uint16_t q = pal[g[gx]];
          px[k] = (uint16_t)(((q & 0xF00) << 4) | ((q & 0xF0) << 3) | ((q & 0xF) << 1));   // RGB444 back in 565 slots
        }
        uint16_t a = px[0], c = px[1];
        o[0] = ((a >> 8) & 0xF0) | ((a >> 7) & 0x0F);               // R1 G1
        o[1] = ((a << 3) & 0xF0) | (c >> 12);                       // B1 R2
        o[2] = ((c >> 3) & 0xF0) | ((c >> 1) & 0x0F);               // G2 B2
        o += 3;
      }
    }
    txIssued++;
    esp_lcd_panel_io_tx_color(p.io, first ? 0x2C : 0x3C, b, o - b);
    first = false;
  }
  memcpy(p.sent + (size_t)yA * TFT_W, canvas + (size_t)yA * TFT_W, (size_t)(yB - yA) * TFT_W * 2);
  p.bytes += rowBytes * (yB - yA);
}

// gifdec has decoded frame gifFrame: its rects in gifRaw, or all of it in gifCur
static void pushGif(Panel &p) {
  if (!gif.n || !p.gifCur) return;
  uint32_t t0 = nowUs();
  uint16_t fIdx = p.gifFrame;
  int x0 = ((TFT_W - gif.w) / 2) & ~1;
  int y0 = (TFT_H - gif.h) / 2;
  const uint16_t *pal = gif.pal[gifSegOf(fIdx)];
  bool band = p.bandY1 > p.bandY0, bandDirty = false;
  if (!p.gifFull) {
    const uint8_t *s = p.gifRaw;
    uint8_t nr = gif.nRects[fIdx];
    for (uint8_t i = 0; i < nr; i++) {
      const GifRect &r = gif.rects[fIdx][i];
      ringIfDue();
      pushGifRect(p, s, pal, r.x, r.y, r.w, r.h, x0, y0);
      s += (size_t)r.w * r.h;
      if (y0 + r.y < p.bandY1 && y0 + r.y + r.h > p.bandY0) bandDirty = true;
    }
  } else {
    pushGifRect(p, p.gifCur, pal, 0, 0, gif.w, gif.h, x0, y0);
    bandDirty = true;
  }
  if (band) {   // the overlay sits on the GIF: re-mix it when the GIF or the overlay changed
    int bx0, bx1;
    if (!bandDirty) bandDirty = bandDiff(p, p.bandY0, p.bandY1 - p.bandY0, bx0, bx1);
    if (bandDirty) pushBand(p, pal, x0, y0);
  }
  waitTx(0);
  p.gifShown = fIdx;
  uint32_t us = nowUs() - t0;
  p.pushUs += us;
  p.frames++;
}

static void flushTask(void *) {
  uint32_t winStart = nowMs();
  for (;;) {
    int idx;
    TickType_t wait = pdMS_TO_TICKS(10);
    if (ringPanel >= 0) {
      int32_t d = (int32_t)(ringDueMs - nowMs());
      wait = d <= 0 ? 0 : pdMS_TO_TICKS(std::min<int32_t>(d, 10));
    }
    if (xQueueReceive(flushQueue, &idx, wait) == pdTRUE) {
      if (idx >= 0 && idx < N_PANELS) {
        Panel &p = panels[idx];
        uint32_t bytesBefore = p.bytes;
        if (p.gifMode && idx == ringPanel) {   // a GIF took the panel: its whole frame covers the ring
          ringPanel = -1;
          ringCur = {0, 0, 0, 0};
        }
        if (p.gifMode) pushGif(p);
        else displayFlush(p);
        uint32_t frameBytes = p.bytes - bytesBefore;
        p.lastFrameBytes = frameBytes;
        if (p.emaBytes <= 0.0f) {
          p.emaBytes = (float)frameBytes;
        } else {
          p.emaBytes = 0.8f * p.emaBytes + 0.2f * (float)frameBytes;
        }
        p.busy = false;
      }
    }
    ringTick();
    uint32_t now = nowMs();
    if (now - winStart >= 1000) {
      float dt = (now - winStart) / 1000.0f;
      uint32_t totalBytes = 0;
      for (int i = 0; i < N_PANELS; i++) {
        Panel &p = panels[i];
        p.fps = p.frames / dt;
        p.kbps = p.bytes / 1024.0f / dt;
        p.busMs = p.pushUs / 1000.0f / dt;
        totalBytes += p.bytes;
        p.frames = 0;
        p.pushUs = 0;
        p.bytes = 0;
      }
      busWirePct = ((float)totalBytes / dt) * 8.0f * 100.0f / (float)displayBus.hz;
      winStart = now;
    }
  }
}

// Decodes GIF frames from flash for each panel (rects into gifRaw, all of the
// frame in gifCur for the next one to predict from), then hands the panel to flush, which
// packs the pixels to RGB444 on the way out. PSRAM bandwidth is what limits
// three panels: a pass that packed RGB444 into PSRAM here, for flush to DMA
// from there, and a second decoder on core 0 were both slower. Core 1 above the
// main task: below it gifdec only split the idle time with IDLE1 (10 fps), and
// level with it main's drawing of the other panels held one GIF panel to 13 fps.
static void gifDecodeTask(void *) {
  for (;;) {
    int idx;
    if (xQueueReceive(gifQueue, &idx, portMAX_DELAY) != pdTRUE) continue;
    Panel &p = panels[idx];
    if (!gif.n) {      // no pack (yet): nothing behind the frame tables
      p.busy = false;
      continue;
    }
    if (!p.gifCur) {   // first time on this panel
      p.gifCur = (uint8_t *)heap_caps_malloc((size_t)gif.w * gif.h, MALLOC_CAP_SPIRAM);
      p.gifRaw = (uint8_t *)heap_caps_malloc(gifRawBytes(), MALLOC_CAP_SPIRAM);
      if (!p.gifCur || !p.gifRaw) {
        static bool told;
        if (!told) logMsg("gif: no PSRAM for a panel buffer");
        told = true;
        free(p.gifCur);
        free(p.gifRaw);
        p.gifCur = p.gifRaw = nullptr;
        p.busy = false;
        continue;
      }
      p.gifShown = -1;
    }
    uint16_t f = p.gifFrame;
    uint32_t t0 = nowUs();
    // the frame before f on the glass and the band where it was: decode f's
    // rects; else (a segment switch, a new face, the band moved, a loop's
    // first frame) rebuild gifCur and push it whole
    p.gifFull = p.gifShown != (int16_t)gifPrevFrame(f) || gif.key[f] || p.bandY0 != p.gifKeepY0 || p.bandY1 != p.gifKeepY1;
    bool ok = p.gifFull ? gifBuildFrame(p.gifCur, p.gifRaw, f) : gifApplyFrame(p.gifCur, p.gifRaw, f);
    if (p.gifFull) {
      p.gifKeepY0 = p.bandY0;
      p.gifKeepY1 = p.bandY1;
    }
    gifDecodeUs += nowUs() - t0;
    gifDecodeN++;
    if (!ok) {
      static bool told;
      if (!told) logMsg("gif: frame %u did not decode", f);
      told = true;
      p.gifShown = -1;   // gifCur is partly written: the next frame rebuilds, the glass keeps the last one
      p.busy = false;
      continue;
    }
    xQueueSend(flushQueue, &idx, portMAX_DELAY);
    // panels queued back to back would keep this task from blocking at all,
    // and IDLE1, below it, feeds the task watchdog
    if (uxQueueMessagesWaiting(gifQueue)) vTaskDelay(1);
  }
}

void displaySubmit(int panel) {
  if (panel < 0 || panel >= N_PANELS) return;
  panels[panel].busy = true;
  xQueueSend(panels[panel].gifMode ? gifQueue : flushQueue, &panel, portMAX_DELAY);
}

void displayWait(int panel) {
  if (panel < 0 || panel >= N_PANELS) return;
  while (panels[panel].busy) vTaskDelay(1);
}

void displayWaitAll() {
  for (int i = 0; i < N_PANELS; i++) displayWait(i);
}

void displayInit() {
  // Hardware reset of all panels together; CS idles HIGH, the SPI driver
  // drives each panel's CS per transfer.
  for (int i = 0; i < N_PANELS; i++) {
    const auto &cfg = displayPanels[i];
    gpio_config_t io_conf = {};
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << cfg.cs) | (1ULL << cfg.rst);
    gpio_config(&io_conf);
    gpio_set_level((gpio_num_t)cfg.cs, 1);
    gpio_set_level((gpio_num_t)cfg.rst, 0);
  }
  sleepMs(20);
  for (int i = 0; i < N_PANELS; i++) {
    gpio_set_level((gpio_num_t)displayPanels[i].rst, 1);
  }
  sleepMs(150);

  spi_bus_config_t bus = {};
  bus.mosi_io_num = displayBus.mosi;
  bus.miso_io_num = -1;
  bus.sclk_io_num = displayBus.sck;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = CHUNK_BYTES + 16;
  // SPI ISR on the flush task's core. spi_bus_lock's bg_exit_core re-reads
  // acquiring_dev between its check and the resume: from the other core, the
  // flush task's polling RASET could end in that gap and the ISR takes NULL
  bus.isr_cpu_id = ESP_INTR_CPU_AFFINITY_0;
  ESP_ERROR_CHECK(spi_bus_initialize(displayBus.host, &bus, SPI_DMA_CH_AUTO));

  // Full drive (~40 mA) on the bus lines: three panel inputs + the 33R on a
  // long daisy chain want sharp edges.
  for (int p : {displayBus.sck, displayBus.mosi, displayBus.dc}) {
    gpio_set_drive_capability((gpio_num_t)p, GPIO_DRIVE_CAP_3);
  }

  // Allocate the two DMA chunk buffers BEFORE the canvases
  for (int i = 0; i < 2; i++) {
    dmaBuf[i] = (uint8_t *)heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_ERROR_CHECK(dmaBuf[i] ? ESP_OK : ESP_ERR_NO_MEM);   // stop here, not at the first flush
  }

  for (int i = 0; i < N_PANELS; i++) {
    const auto &cfg = displayPanels[i];
    Panel &p = panels[i];
    esp_lcd_panel_io_spi_config_t ioc = {};
    ioc.cs_gpio_num = cfg.cs;
    ioc.dc_gpio_num = displayBus.dc;
    ioc.spi_mode = 0;
    ioc.pclk_hz = displayBus.hz;
    ioc.trans_queue_depth = 4;
    ioc.on_color_trans_done = onTxDone;
    ioc.lcd_cmd_bits = 8;
    ioc.lcd_param_bits = 8;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)(uintptr_t)displayBus.host, &ioc, &p.io));

    esp_lcd_panel_dev_config_t pc = {};
    pc.reset_gpio_num = -1;               // per-panel RST lines handled above
    pc.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    pc.bits_per_pixel = 16;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(p.io, &pc, &p.lcd));

    // No esp_lcd_panel_reset(): with no RST pin it sends SWRESET and waits only
    // 20 ms, but the ST7789 ignores SLPOUT for 120 ms after a reset -- the panel
    // then stays asleep (backlight on, no picture). The hardware reset above
    // already left 150 ms.
    esp_lcd_panel_init(p.lcd);
    if (cfg.invert) esp_lcd_panel_invert_color(p.lcd, true);   // IPS ST7789
    const uint8_t colmod = 0x53;                               // 12 bpp RGB444 (esp_lcd set 16 bpp)
    esp_lcd_panel_io_tx_param(p.io, 0x3A, &colmod, 1);
    esp_lcd_panel_io_tx_param(p.io, 0x36, &cfg.madctl, 1);    // MADCTL: orientation / RGB order
    esp_lcd_panel_disp_on_off(p.lcd, true);

    size_t canvasBytes = (size_t)cfg.w * cfg.h * 2;
    uint16_t *buf = nullptr;
    bool isInternal = false;
    if (cfg.preferInternalCanvas && freeInternal() >= canvasBytes + INTERNAL_RESERVE) {
      buf = (uint16_t *)heap_caps_malloc(canvasBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      if (buf) {
        memset(buf, 0, canvasBytes);
        isInternal = true;
      }
    }
    if (!buf) {
      buf = (uint16_t *)heap_caps_malloc(canvasBytes, MALLOC_CAP_SPIRAM);
      if (buf) memset(buf, 0, canvasBytes);
      isInternal = false;
    }
    ESP_ERROR_CHECK(buf ? ESP_OK : ESP_ERR_NO_MEM);

    p.canvas = new LGFX_Sprite();
    p.canvas->setColorDepth(16);
    p.canvas->setBuffer(buf, cfg.w, cfg.h, 16);
    p.canvasInternal = isInternal;

    p.sent = (uint16_t *)heap_caps_malloc(canvasBytes, MALLOC_CAP_SPIRAM);
    ESP_ERROR_CHECK(p.sent ? ESP_OK : ESP_ERR_NO_MEM);
    memset(p.sent, 0xA5, canvasBytes);   // != any canvas: first flush sends all
    p.view = cfg.defaultView;
    p.busy = false;
    p.gifShown = -1;
    p.gifFrame = 0;
    p.gifCur = p.gifRaw = nullptr;
    p.gifKeepY0 = p.gifKeepY1 = 0;
    p.gifMode = false;
    p.bandY0 = p.bandY1 = 0;
    p.lastFrameBytes = 0;
    p.emaBytes = (float)(cfg.w * cfg.h * 3 / 2);
    p.deferred = 0;
    p.gifStretch = 1.0f;
  }

  logMsg("display: canvas 2 %s, 3 %s", panels[1].canvasInternal ? "internal" : "psram",
         panels[2].canvasInternal ? "internal" : "psram");

  flushQueue = xQueueCreate(N_PANELS, sizeof(int));
  gifQueue = xQueueCreate(N_PANELS, sizeof(int));
  xTaskCreatePinnedToCore(flushTask, "flush", 4096, nullptr, 2, &flushTaskHandle, 0);
  xTaskCreatePinnedToCore(gifDecodeTask, "gifdec", 3072, nullptr, 2, nullptr, 1);
}

// ---- backlights & NVS persistence ----
static uint8_t blPct[N_PANELS] = {100, 100, 100};   // user setting
static bool screensIdle;                            // PC asleep/off -> all dark
static bool forcedOff;                              // `display off` / Discord: dark until on

struct NvsSlot {
  uint8_t val;
  bool dirty;
  uint32_t lastWriteMs;
};

static NvsSlot nvsViews[N_PANELS];
static NvsSlot nvsBls[N_PANELS];

void displayNvsPoll() {
  uint32_t now = nowMs();
  for (int i = 0; i < N_PANELS; i++) {
    if (nvsViews[i].dirty && (now - nvsViews[i].lastWriteMs >= 2000)) {
      nvs_handle_t h;
      if (nvs_open("disp", NVS_READWRITE, &h) == ESP_OK) {
        char key[16];
        snprintf(key, sizeof(key), "view%d", i);
        nvs_set_u8(h, key, nvsViews[i].val);
        nvs_commit(h);
        nvs_close(h);
        nvsViews[i].lastWriteMs = now;
        nvsViews[i].dirty = false;
      }
    }
    if (nvsBls[i].dirty && (now - nvsBls[i].lastWriteMs >= 2000)) {
      nvs_handle_t h;
      if (nvs_open("disp", NVS_READWRITE, &h) == ESP_OK) {
        char key[16];
        snprintf(key, sizeof(key), "bl%d", i);
        nvs_set_u8(h, key, nvsBls[i].val);
        nvs_commit(h);
        nvs_close(h);
        nvsBls[i].lastWriteMs = now;
        nvsBls[i].dirty = false;
      }
    }
  }
}

View displayStoredView(int panel) {
  if (panel < 0 || panel >= N_PANELS) return VIEW_METRICS;
  nvs_handle_t h;
  uint8_t val = 0xFF;
  if (nvs_open("disp", NVS_READONLY, &h) == ESP_OK) {
    char key[16];
    snprintf(key, sizeof(key), "view%d", panel);
    nvs_get_u8(h, key, &val);
    nvs_close(h);
  }
  if (val < VIEW_COUNT) return (View)val;
  return displayPanels[panel].defaultView;
}

void displaySaveView(int panel, View v) {
  if (panel < 0 || panel >= N_PANELS) return;
  nvsViews[panel].val = (uint8_t)v;
  nvsViews[panel].dirty = true;
  displayNvsPoll();
}

uint8_t displayStoredBacklight(int panel) {
  if (panel < 0 || panel >= N_PANELS) return 100;
  nvs_handle_t h;
  uint8_t val = 0xFF;
  if (nvs_open("disp", NVS_READONLY, &h) == ESP_OK) {
    char key[16];
    snprintf(key, sizeof(key), "bl%d", panel);
    nvs_get_u8(h, key, &val);
    nvs_close(h);
  }
  if (val <= 100) return val;
  return 100;
}

void displaySaveBacklight(int panel, uint8_t pct) {
  if (panel < 0 || panel >= N_PANELS) return;
  nvsBls[panel].val = pct;
  nvsBls[panel].dirty = true;
  displayNvsPoll();
}

static uint8_t blCap = 100;   // night mode

static void blApply() {
  for (int i = 0; i < N_PANELS; i++) {
    uint8_t pct = blPct[i] < blCap ? blPct[i] : blCap;
    uint32_t duty = (screensIdle || forcedOff) ? 0 : (uint32_t)((uint64_t)PWM_MAX * pct / 100);
    pwmWrite(PWM_CH_BL(i), duty);
  }
}

void backlightInit() {
  for (int i = 0; i < N_PANELS; i++) {
    blPct[i] = displayStoredBacklight(i);
    pwmAttach(displayPanels[i].bl, PWM_CH_BL(i));
    gpio_set_drive_capability((gpio_num_t)displayPanels[i].bl, GPIO_DRIVE_CAP_3);
  }
  blApply();
}

void backlightSet(int panel, uint8_t pct) {
  if (panel < 0 || panel >= N_PANELS) return;
  blPct[panel] = pct;
  blApply();
}

uint8_t backlightPct(int panel) {
  if (panel < 0 || panel >= N_PANELS) return 0;
  return blPct[panel];
}

void backlightIdle(bool idle) {
  screensIdle = idle;
  blApply();
}

bool backlightIsIdle() { return screensIdle; }

void backlightForceOff(bool off) {
  forcedOff = off;
  blApply();
}

bool backlightForcedOff() { return forcedOff; }

void backlightCap(uint8_t pct) {
  blCap = pct > 100 ? 100 : pct;
  blApply();
}

uint8_t backlightCapPct() { return blCap; }
