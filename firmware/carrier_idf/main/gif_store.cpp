#include "gif_store.h"

#include <Arduino.h>
#include <FFat.h>
#include <esp32-hal-psram.h>

#include "display.h"

#define BAND 8

Gif gif;
static bool fatOk;

bool gifFsInit() {
  fatOk = FFat.begin(true);
  return fatOk;
}

static bool readExact(void *dst, size_t len, uint32_t timeoutMs = 5000) {
  uint8_t *p = (uint8_t *)dst;
  uint32_t last = millis();
  while (len) {
    int avail = HOST_UART.available();
    if (avail > 0) {
      size_t got = HOST_UART.readBytes(p, min((size_t)avail, len));
      p += got;
      len -= got;
      last = millis();
    } else if (millis() - last > timeoutMs) {
      return false;
    }
  }
  return true;
}

static void packFrame(uint8_t *dst, const uint8_t *srcRawBe, uint16_t w, uint16_t pw, uint16_t h) {
  size_t dstStride = (size_t)pw * 3 / 2;
  for (uint16_t y = 0; y < h; y++) {
    const uint8_t *s = srcRawBe + (size_t)y * w * 2;
    uint8_t *o = dst + (size_t)y * dstStride;
    for (uint16_t x = 0; x < pw; x += 2) {
      uint16_t a = (x < w) ? (((uint16_t)s[x * 2] << 8) | s[x * 2 + 1]) : 0;
      uint16_t c = (x + 1 < w) ? (((uint16_t)s[(x + 1) * 2] << 8) | s[(x + 1) * 2 + 1]) : 0;
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
    lo = min(lo, a);
    int b = pairs - 1;
    while (b > hi) {
      int off = b * 3;
      if (cr[off] != pr[off] || cr[off + 1] != pr[off + 1] || cr[off + 2] != pr[off + 2]) break;
      b--;
    }
    hi = max(hi, b);
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
    bool dirty = !end && bandDiffGif(cur, prv, y, min(BAND, gif.h - y), gif.pw, x0, x1);
    if (dirty) {
      if (runY < 0) runY = y;
      rx0 = min(rx0, x0);
      rx1 = max(rx1, x1);
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

static bool allocFrames() {
  if (gif.w == 0 || gif.w > TFT_W || gif.h == 0 || gif.h > TFT_H || gif.n == 0 || gif.n > MAX_FRAMES) {
    HOST_UART.printf("ERR bad size %ux%u x%u\n", gif.w, gif.h, gif.n);
    return false;
  }
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
    gif.packed[i] = (uint8_t *)ps_malloc(pBytes);
    if (!gif.packed[i]) { HOST_UART.printf("ERR psram alloc frame %u\n", i); return false; }
  }
  return true;
}

bool gifReceive(void (*onFrame)(int idx)) {
  displayWaitAll();
  uint16_t hdr[3];
  if (!readExact(hdr, sizeof hdr)) { HOST_UART.println("ERR header timeout"); return false; }
  gif.w = hdr[0]; gif.h = hdr[1]; gif.n = hdr[2];
  if (gif.n == 0 || gif.n > MAX_FRAMES) { HOST_UART.printf("ERR bad count %u\n", gif.n); return false; }
  if (!readExact(gif.durMs, gif.n * 2)) { HOST_UART.println("ERR duration timeout"); return false; }
  if (!allocFrames()) return false;

  size_t rawBytes = (size_t)gif.w * gif.h * 2;
  uint8_t *temp = (uint8_t *)ps_malloc(rawBytes);
  if (!temp) { HOST_UART.println("ERR psram alloc temp"); return false; }

  File f = fatOk ? FFat.open(GIF_FILE, FILE_WRITE) : File();
  bool fileOk = (bool)f;
  if (fileOk) {
    uint16_t h[3] = {gif.w, gif.h, gif.n};
    fileOk = f.write((const uint8_t *)"GIF5", 4) == 4 &&
             f.write((const uint8_t *)h, 6) == 6 &&
             f.write((const uint8_t *)gif.durMs, gif.n * 2) == gif.n * 2u;
  }
  HOST_UART.println("OK");

  for (uint16_t i = 0; i < gif.n;) {
    uint32_t sum = 0, want;
    if (!readExact(temp, rawBytes) || !readExact(&want, 4)) {
      HOST_UART.printf("ERR frame %u timeout\n", i);
      if (f) f.close();
      free(temp);
      return false;
    }
    for (size_t k = 0; k < rawBytes; k++) sum += temp[k];
    if (sum != want) { HOST_UART.printf("E%u\n", i); continue; }
    if (fileOk) {
      if (f.write(temp, rawBytes) != rawBytes) fileOk = false;
    }
    packFrame(gif.packed[i], temp, gif.w, gif.pw, gif.h);
    if (onFrame) onFrame(i);
    HOST_UART.printf("K%u\n", i);
    i++;
  }
  if (f) f.close();
  free(temp);
  HOST_UART.println(fileOk ? "SAVED" : "ERR save");
  for (uint16_t i = 0; i < gif.n; i++) computeFrameRects(i);
  return true;
}

bool gifLoad() {
  if (!fatOk || !FFat.exists(GIF_FILE)) return false;
  File f = FFat.open(GIF_FILE, FILE_READ);
  char magic[4];
  uint16_t h[3];
  bool ok = f.read((uint8_t *)magic, 4) == 4 && !memcmp(magic, "GIF5", 4) && f.read((uint8_t *)h, 6) == 6;
  if (!ok) { f.close(); return false; }
  gif.w = h[0]; gif.h = h[1]; gif.n = h[2];
  if (!allocFrames()) { f.close(); gif.n = 0; return false; }
  ok = f.read((uint8_t *)gif.durMs, gif.n * 2) == gif.n * 2u;
  if (!ok) { f.close(); gif.n = 0; return false; }

  size_t rawBytes = (size_t)gif.w * gif.h * 2;
  uint8_t *temp = (uint8_t *)ps_malloc(rawBytes);
  if (!temp) { f.close(); gif.n = 0; return false; }

  for (uint16_t i = 0; ok && i < gif.n; i++) {
    if (f.read(temp, rawBytes) != rawBytes) { ok = false; break; }
    packFrame(gif.packed[i], temp, gif.w, gif.pw, gif.h);
  }
  free(temp);
  f.close();
  if (!ok) { gif.n = 0; return false; }

  for (uint16_t i = 0; i < gif.n; i++) computeFrameRects(i);
  return true;
}
