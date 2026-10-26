#include "gif_store.h"

#include <algorithm>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <esp_vfs_fat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "display.h"
#include "host_uart.h"
#include "sys.h"

#define BAND 8

Gif gif;
static bool fatOk;
static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;

// Frame tables in PSRAM: 6 KB of internal RAM back; the player reads a few
// rects per frame, nothing it would notice
struct GifTables {
  uint16_t durMs[MAX_FRAMES];
  uint8_t *packed[MAX_FRAMES];
  uint8_t nRects[MAX_FRAMES];
  GifRect rects[MAX_FRAMES][MAX_DIRTY_RECTS];
};

static bool gifTablesAlloc() {
  if (gif.packed) return true;
  GifTables *t = (GifTables *)heap_caps_calloc(1, sizeof(GifTables), MALLOC_CAP_SPIRAM);
  if (!t) return false;
  gif.durMs = t->durMs;
  gif.nRects = t->nRects;
  gif.rects = t->rects;
  gif.packed = t->packed;   // last: gifTablesAlloc() tests this one
  return true;
}

bool gifFsInit() {
  gifTablesAlloc();
  const esp_vfs_fat_mount_config_t mount_config = {
    .format_if_mount_failed = true,
    .max_files = 4,
    .allocation_unit_size = 4096,
    .disk_status_check_enable = false,
    .use_one_fat = false,
  };
  esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl("/ffat", "ffat", &mount_config, &s_wl_handle);
  fatOk = (err == ESP_OK);
  return fatOk;
}

static bool readExact(void *dst, size_t len, uint32_t timeoutMs = 5000) {
  uint8_t *p = (uint8_t *)dst;
  uint32_t last = nowMs();
  while (len) {
    int avail = hostUartAvailable();
    if (avail > 0) {
      size_t toRead = std::min((size_t)avail, len);
      size_t got = hostUartReadBytes(p, toRead, 0);
      if (got > 0) {
        p += got;
        len -= got;
        last = nowMs();
      }
    } else if (nowMs() - last > timeoutMs) {
      return false;
    } else {
      sleepMs(1);
    }
  }
  return true;
}

// Width of the source once scaled to the screen height, before the crop.
static int fitFullWidth(uint16_t sw, uint16_t sh) {
  return ((int)sw * TFT_H + sh / 2) / sh;
}

static inline uint16_t px565(const uint8_t *src, int sw, int x, int y) {
  const uint8_t *q = src + ((size_t)y * sw + x) * 2;
  return ((uint16_t)q[0] << 8) | q[1];
}

// Weighted mix of four RGB565 pixels; wx, wy are 0..256 towards b / the lower row.
static uint16_t mix565(uint16_t a, uint16_t b, uint16_t c, uint16_t d, uint32_t wx, uint32_t wy) {
  uint32_t wa = (256 - wx) * (256 - wy), wb = wx * (256 - wy), wc = (256 - wx) * wy, wd = wx * wy;
  uint32_t r = ((a >> 11) * wa + (b >> 11) * wb + (c >> 11) * wc + (d >> 11) * wd + 32768) >> 16;
  uint32_t g = (((a >> 5) & 63) * wa + ((b >> 5) & 63) * wb + ((c >> 5) & 63) * wc + ((d >> 5) & 63) * wd + 32768) >> 16;
  uint32_t bl = ((a & 31) * wa + (b & 31) * wb + (c & 31) * wc + (d & 31) * wd + 32768) >> 16;
  return (uint16_t)((r << 11) | (g << 5) | bl);
}

// Source RGB565-BE frame (sw x sh) -> gif.w x gif.h packed RGB444: bilinear
// scale to the screen height, centre crop to gif.w.
static void packFrame(uint8_t *dst, const uint8_t *srcRawBe, uint16_t sw, uint16_t sh) {
  const int fullW = fitFullWidth(sw, sh), cx = (fullW - gif.w) / 2;
  static uint16_t xs0[TFT_W + 1], xs1[TFT_W + 1];
  static uint16_t xw[TFT_W + 1];
  for (int x = 0; x < gif.pw; x++) {   // pixel-centre mapping, 8-bit fraction
    int32_t f = (int32_t)(((int64_t)(2 * (x + cx) + 1) * sw * 256) / (2 * fullW)) - 128;
    if (f < 0) f = 0;
    xs0[x] = f >> 8;
    xs1[x] = std::min(xs0[x] + 1, sw - 1);
    xw[x] = f & 255;
  }
  size_t dstStride = (size_t)gif.pw * 3 / 2;
  for (int y = 0; y < gif.h; y++) {
    int32_t f = (int32_t)(((int64_t)(2 * y + 1) * sh * 256) / (2 * gif.h)) - 128;
    if (f < 0) f = 0;
    int y0 = f >> 8, y1 = std::min(y0 + 1, sh - 1);
    uint32_t wy = f & 255;
    uint8_t *o = dst + (size_t)y * dstStride;
    for (int x = 0; x < gif.pw; x += 2) {
      uint16_t a = 0, c = 0;
      if (x < gif.w)
        a = mix565(px565(srcRawBe, sw, xs0[x], y0), px565(srcRawBe, sw, xs1[x], y0),
                   px565(srcRawBe, sw, xs0[x], y1), px565(srcRawBe, sw, xs1[x], y1), xw[x], wy);
      if (x + 1 < gif.w)
        c = mix565(px565(srcRawBe, sw, xs0[x + 1], y0), px565(srcRawBe, sw, xs1[x + 1], y0),
                   px565(srcRawBe, sw, xs0[x + 1], y1), px565(srcRawBe, sw, xs1[x + 1], y1), xw[x + 1], wy);
      o[0] = ((a >> 8) & 0xF0) | ((a >> 7) & 0x0F);               // R1 G1
      o[1] = ((a << 3) & 0xF0) | (c >> 12);                       // B1 R2
      o[2] = ((c >> 3) & 0xF0) | ((c >> 1) & 0x0F);               // G2 B2
      o += 3;
    }
  }
}

static bool bandDiffGif(const uint8_t *cur, const uint8_t *prev, int y, int n, int pw, int &x0, int &x1) {
  size_t rowStride = (size_t)pw * 3 / 2;
  int pairs = pw / 2;
  int lo = pairs, hi = -1;
  for (int r = y; r < y + n; r++) {
    const uint8_t *cr = cur + (size_t)r * rowStride;
    const uint8_t *pr = prev + (size_t)r * rowStride;
    int a = 0;
    while (a < lo) {
      int off = a * 3;
      if (cr[off] != pr[off] || cr[off + 1] != pr[off + 1] || cr[off + 2] != pr[off + 2]) break;
      a++;
    }
    if (a == pairs) continue;
    lo = std::min(lo, a);
    int b = pairs - 1;
    while (b > hi) {
      int off = b * 3;
      if (cr[off] != pr[off] || cr[off + 1] != pr[off + 1] || cr[off + 2] != pr[off + 2]) break;
      b--;
    }
    hi = std::max(hi, b);
  }
  if (hi < 0) return false;
  x0 = lo * 2;
  x1 = hi * 2 + 1;
  return true;
}

static void computeFrameRects(int i) {
  if (gif.n <= 1) {
    gif.nRects[i] = 1;
    gif.rects[i][0] = {0, 0, gif.pw, gif.h};
    return;
  }
  int prev = (i == 0) ? (gif.n - 1) : (i - 1);
  const uint8_t *cur = gif.packed[i];
  const uint8_t *prv = gif.packed[prev];
  int runY = -1, rx0 = gif.pw, rx1 = -1;
  uint8_t count = 0;
  for (int y = 0;; y += BAND) {   // h need not be a multiple of BAND: one extra pass closes the last run
    bool end = y >= gif.h;
    if (end) y = gif.h;
    int x0, x1;
    bool dirty = !end && bandDiffGif(cur, prv, y, std::min(BAND, gif.h - y), gif.pw, x0, x1);
    if (dirty) {
      if (runY < 0) runY = y;
      rx0 = std::min(rx0, x0);
      rx1 = std::max(rx1, x1);
    } else if (runY >= 0) {
      if (count < MAX_DIRTY_RECTS) {
        gif.rects[i][count++] = {(uint16_t)rx0, (uint16_t)runY, (uint16_t)(rx1 - rx0 + 1), (uint16_t)(y - runY)};
      } else {
        count = 9;   // more than 8 runs -> single full-frame rect
        break;
      }
      runY = -1; rx0 = gif.pw; rx1 = -1;
    }
    if (end) break;
  }
  if (count > MAX_DIRTY_RECTS) {
    gif.nRects[i] = 1;
    gif.rects[i][0] = {0, 0, gif.pw, gif.h};
  } else {
    gif.nRects[i] = count;
  }
}

// Sets gif.w / gif.h to the scaled size of an sw x sh source and allocates frames.
static bool allocFrames(uint16_t sw, uint16_t sh) {
  if (sw == 0 || sw > TFT_W || sh == 0 || sh > TFT_H || gif.n == 0 || gif.n > MAX_FRAMES) {
    hostUartPrintf("ERR bad size %ux%u x%u\n", sw, sh, gif.n);
    return false;
  }
  gif.h = TFT_H;
  gif.w = (uint16_t)std::min(fitFullWidth(sw, sh), TFT_W);
  displayWaitAll();
  for (uint16_t i = 0; i < MAX_FRAMES; i++) {
    free(gif.packed[i]);
    gif.packed[i] = nullptr;
  }
  // Until computeFrameRects() runs (upload preview), every frame is a full push,
  // and no panel's glass matches the new frames.
  for (uint16_t i = 0; i < MAX_FRAMES; i++) {
    gif.nRects[i] = 1;
    gif.rects[i][0] = {0, 0, (uint16_t)((gif.w + 1) & ~1), gif.h};
  }
  for (Panel &p : panels) p.gifShown = -1;
  gif.pw = (gif.w + 1) & ~1;
  size_t pBytes = (size_t)gif.pw * gif.h * 3 / 2;
  for (uint16_t i = 0; i < gif.n; i++) {
    gif.packed[i] = (uint8_t *)heap_caps_malloc(pBytes, MALLOC_CAP_SPIRAM);
    if (!gif.packed[i]) { hostUartPrintf("ERR psram alloc frame %u\n", i); return false; }
  }
  return true;
}

bool gifReceive(void (*onFrame)(int idx)) {
  if (!gifTablesAlloc()) { hostUartPrintln("ERR psram alloc tables"); return false; }
  displayWaitAll();
  uint16_t hdr[3];
  if (!readExact(hdr, sizeof hdr)) { hostUartPrintln("ERR header timeout"); return false; }
  uint16_t sw = hdr[0], sh = hdr[1];
  gif.n = hdr[2];
  if (gif.n == 0 || gif.n > MAX_FRAMES) { hostUartPrintf("ERR bad count %u\n", gif.n); return false; }
  if (!readExact(gif.durMs, gif.n * 2)) { hostUartPrintln("ERR duration timeout"); return false; }
  if (!allocFrames(sw, sh)) return false;

  size_t rawBytes = (size_t)sw * sh * 2;
  uint8_t *temp = (uint8_t *)heap_caps_malloc(rawBytes, MALLOC_CAP_SPIRAM);
  if (!temp) { hostUartPrintln("ERR psram alloc temp"); return false; }

  FILE *f = fatOk ? fopen(GIF_FILE, "wb") : nullptr;
  bool fileOk = (f != nullptr);
  if (fileOk) {
    uint16_t h[3] = {sw, sh, gif.n};
    fileOk = (fwrite("GIF5", 1, 4, f) == 4) &&
             (fwrite(h, 1, 6, f) == 6) &&
             (fwrite(gif.durMs, 1, gif.n * 2, f) == (size_t)(gif.n * 2));
  }
  hostUartPrintln("OK");

  for (uint16_t i = 0; i < gif.n;) {
    uint32_t sum = 0, want;
    if (!readExact(temp, rawBytes) || !readExact(&want, 4)) {
      hostUartPrintf("ERR frame %u timeout\n", i);
      if (f) fclose(f);
      free(temp);
      return false;
    }
    for (size_t k = 0; k < rawBytes; k++) sum += temp[k];
    if (sum != want) { hostUartPrintf("E%u\n", i); continue; }
    if (fileOk) {
      if (fwrite(temp, 1, rawBytes, f) != rawBytes) fileOk = false;
    }
    packFrame(gif.packed[i], temp, sw, sh);
    if (onFrame) onFrame(i);
    hostUartPrintf("K%u\n", i);
    esp_task_wdt_reset();   // a long upload is still progress
    i++;
  }
  if (f) fclose(f);
  free(temp);
  hostUartPrintln(fileOk ? "SAVED" : "ERR save");
  for (uint16_t i = 0; i < gif.n; i++) computeFrameRects(i);
  return true;
}

bool gifLoad() {
  if (!fatOk || !gifTablesAlloc()) return false;
  FILE *f = fopen(GIF_FILE, "rb");
  if (!f) return false;
  char magic[4];
  uint16_t h[3];
  bool ok = (fread(magic, 1, 4, f) == 4) && !memcmp(magic, "GIF5", 4) && (fread(h, 1, 6, f) == 6);
  if (!ok) { fclose(f); return false; }
  uint16_t sw = h[0], sh = h[1];
  gif.n = h[2];
  if (!allocFrames(sw, sh)) { fclose(f); gif.n = 0; return false; }
  ok = (fread(gif.durMs, 1, gif.n * 2, f) == (size_t)(gif.n * 2));
  if (!ok) { fclose(f); gif.n = 0; return false; }

  size_t rawBytes = (size_t)sw * sh * 2;
  uint8_t *temp = (uint8_t *)heap_caps_malloc(rawBytes, MALLOC_CAP_SPIRAM);
  if (!temp) { fclose(f); gif.n = 0; return false; }

  for (uint16_t i = 0; ok && i < gif.n; i++) {
    if (fread(temp, 1, rawBytes, f) != rawBytes) { ok = false; break; }
    packFrame(gif.packed[i], temp, sw, sh);
  }
  free(temp);
  fclose(f);
  if (!ok) { gif.n = 0; return false; }

  for (uint16_t i = 0; i < gif.n; i++) computeFrameRects(i);
  return true;
}
