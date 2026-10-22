#include "gif_store.h"

#include <Arduino.h>
#include <FFat.h>
#include <esp32-hal-psram.h>

Gif gif;
static bool fatOk;

static size_t frameBytes() { return (size_t)gif.w * gif.h * 2; }

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

static bool allocFrames() {
  if (gif.w == 0 || gif.w > TFT_W || gif.h == 0 || gif.h > TFT_H || gif.n == 0 || gif.n > MAX_FRAMES) {
    HOST_UART.printf("ERR bad size %ux%u x%u\n", gif.w, gif.h, gif.n);
    return false;
  }
  for (uint16_t i = 0; i < MAX_FRAMES; i++) { free(gif.frames[i]); gif.frames[i] = nullptr; }
  for (uint16_t i = 0; i < gif.n; i++) {
    gif.frames[i] = (uint16_t *)ps_malloc(frameBytes());
    if (!gif.frames[i]) { HOST_UART.printf("ERR psram alloc frame %u\n", i); return false; }
  }
  return true;
}

bool gifReceive(void (*onFrame)(const uint16_t *frame)) {
  uint16_t hdr[3];
  if (!readExact(hdr, sizeof hdr)) { HOST_UART.println("ERR header timeout"); return false; }
  gif.w = hdr[0]; gif.h = hdr[1]; gif.n = hdr[2];
  if (gif.n == 0 || gif.n > MAX_FRAMES) { HOST_UART.printf("ERR bad count %u\n", gif.n); return false; }
  if (!readExact(gif.durMs, gif.n * 2)) { HOST_UART.println("ERR duration timeout"); return false; }
  if (!allocFrames()) return false;
  HOST_UART.println("OK");
  for (uint16_t i = 0; i < gif.n;) {
    uint32_t sum = 0, want;
    if (!readExact(gif.frames[i], frameBytes()) || !readExact(&want, 4)) { HOST_UART.printf("ERR frame %u timeout\n", i); return false; }
    const uint8_t *p = (const uint8_t *)gif.frames[i];
    for (size_t k = 0; k < frameBytes(); k++) sum += p[k];
    if (sum != want) { HOST_UART.printf("E%u\n", i); continue; }
    onFrame(gif.frames[i]);
    HOST_UART.printf("K%u\n", i);
    i++;
  }
  File f = fatOk ? FFat.open(GIF_FILE, FILE_WRITE) : File();
  bool ok = f;
  if (ok) {
    uint16_t h[3] = {gif.w, gif.h, gif.n};
    ok = f.write((const uint8_t *)"GIF5", 4) == 4 && f.write((uint8_t *)h, 6) == 6 &&
         f.write((uint8_t *)gif.durMs, gif.n * 2) == gif.n * 2u;
    for (uint16_t i = 0; ok && i < gif.n; i++) ok = f.write((uint8_t *)gif.frames[i], frameBytes()) == frameBytes();
    f.close();
  }
  HOST_UART.println(ok ? "SAVED" : "ERR save");
  return true;
}

bool gifLoad() {
  if (!fatOk || !FFat.exists(GIF_FILE)) return false;
  File f = FFat.open(GIF_FILE, FILE_READ);
  char magic[4];
  uint16_t h[3];
  bool ok = f.read((uint8_t *)magic, 4) == 4 && !memcmp(magic, "GIF5", 4) && f.read((uint8_t *)h, 6) == 6;
  if (ok) {
    gif.w = h[0]; gif.h = h[1]; gif.n = h[2];
    ok = allocFrames() && f.read((uint8_t *)gif.durMs, gif.n * 2) == gif.n * 2u;
    for (uint16_t i = 0; ok && i < gif.n; i++) ok = f.read((uint8_t *)gif.frames[i], frameBytes()) == frameBytes();
  }
  f.close();
  if (!ok) gif.n = 0;
  return ok;
}
