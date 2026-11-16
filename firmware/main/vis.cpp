#include "vis.h"

#include <freertos/FreeRTOS.h>
#include <nvs.h>
#include <string.h>

#include "log.h"
#include "sys.h"

// latest frame: written by the TinyUSB task, read by flush (both core 0, but
// different tasks)
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
// the last frames as they came, for the 100 ms average
#define HIST 8
static uint8_t histLv[HIST][VIS_CH];
static uint32_t histMs[HIST];
static int histHead, histN;
static uint32_t lastMs;

// stream counters since boot or `vis reset`
static uint32_t nFrames, nLost, gapMax, gapSum, gap50, gap100, sinceMs;
static int lastSeq = -1;
// ring counters
static uint32_t nRing, ringUsSum, ringUsMax, ringGapMax, ringGap40;

static int8_t enabled = -1;   // -1 = not read from NVS yet

void visPut(uint8_t seq, const uint8_t *ch) {
  uint32_t now = nowMs();
  portENTER_CRITICAL(&mux);
  if (lastSeq >= 0) {
    uint32_t gap = now - lastMs;
    nLost += (uint8_t)(seq - lastSeq - 1);   // seq wraps at 256
    if (gap > gapMax) gapMax = gap;
    gapSum += gap;
    if (gap > 50) gap50++;
    if (gap > 100) gap100++;
  } else {
    sinceMs = now;
  }
  lastSeq = seq;
  histHead = (histHead + 1) % HIST;
  memcpy(histLv[histHead], ch, VIS_CH);
  histMs[histHead] = now;
  if (histN < HIST) histN++;
  lastMs = now;
  nFrames++;
  portEXIT_CRITICAL(&mux);
}

// What the ring shows: the frames of the last AVG_MS averaged (raw 30 Hz
// levels flicker); a level reached is held HOLD_MS, then falls no faster than
// full scale in FALL_MS, so a pause or the end of the music fades out
// instead of cutting. A frame older than STALE_MS counts as silence.
#define AVG_MS 100
#define HOLD_MS 100
#define FALL_MS 500
#define STALE_MS 250
bool visLevels(float len[VIS_BANDS], float vol[VIS_BANDS]) {
  static float shown[VIS_CH];
  static uint32_t lastCallMs, peakMs[VIS_CH];
  uint8_t lv[HIST][VIS_CH];
  uint32_t ms[HIST];
  int head, n;
  portENTER_CRITICAL(&mux);
  memcpy(lv, histLv, sizeof lv);
  memcpy(ms, histMs, sizeof ms);
  head = histHead;
  n = histN;
  portEXIT_CRITICAL(&mux);
  uint32_t now = nowMs();
  uint32_t dt = now - lastCallMs;
  if (dt > 200) dt = 200;
  lastCallMs = now;

  float target[VIS_CH] = {0};
  int used = 0;
  for (int k = 0; k < n; k++) {
    int i = (head - k + HIST) % HIST;
    uint32_t age = now - ms[i];
    if (age > (k ? AVG_MS : STALE_MS)) break;   // the newest counts up to STALE_MS, the rest within AVG_MS
    for (int b = 0; b < VIS_CH; b++) target[b] += lv[i][b];
    used++;
  }
  bool any = false;
  for (int b = 0; b < VIS_CH; b++) {
    float t = used ? target[b] / used / 255.0f : 0.0f;
    if (t >= shown[b]) {
      shown[b] = t;
      peakMs[b] = now;
    } else if (now - peakMs[b] >= HOLD_MS) {
      float fall = shown[b] - (float)dt / FALL_MS;
      shown[b] = t > fall ? t : fall;
    }
    if (shown[b] < 0) shown[b] = 0;
    if (b < VIS_BANDS) {
      len[b] = shown[b];
      if (shown[b] > 0.004f) any = true;   // lengths decide dark: they fall to 0 on silence
    } else {
      vol[b - VIS_BANDS] = shown[b];
    }
  }
  return any;
}

bool visEnabled() {
  if (enabled < 0) {
    uint8_t v = 1;
    nvs_handle_t h;
    if (nvs_open("vis", NVS_READONLY, &h) == ESP_OK) {
      nvs_get_u8(h, "on", &v);
      nvs_close(h);
    }
    enabled = v ? 1 : 0;
  }
  return enabled == 1;
}

static void setEnabled(bool on) {
  enabled = on ? 1 : 0;
  nvs_handle_t h;
  if (nvs_open("vis", NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_u8(h, "on", on ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
  }
}

void visRingPushed(uint32_t us, uint32_t gapMs) {
  portENTER_CRITICAL(&mux);
  nRing++;
  ringUsSum += us;
  if (us > ringUsMax) ringUsMax = us;
  if (gapMs > ringGapMax) ringGapMax = gapMs;
  if (gapMs > 40) ringGap40++;
  portEXIT_CRITICAL(&mux);
}

static void stats() {
  portENTER_CRITICAL(&mux);
  uint32_t f = nFrames, lost = nLost, gmax = gapMax, gsum = gapSum, g50 = gap50, g100 = gap100;
  uint32_t secs = (nowMs() - sinceMs) / 1000, age = nowMs() - lastMs;
  uint32_t r = nRing, rsum = ringUsSum, rmax = ringUsMax, rgmax = ringGapMax, rg40 = ringGap40;
  portEXIT_CRITICAL(&mux);
  logMsg("vis: %s | stream %lu frames in %lu s (%.1f/s), lost %lu, gap avg %.1f max %lu ms, >50 ms %lu, >100 ms %lu, last %lu ms ago",
         visEnabled() ? "on" : "off", (unsigned long)f, (unsigned long)secs, secs ? (double)f / secs : 0.0,
         (unsigned long)lost, f > 1 ? (double)gsum / (f - 1) : 0.0, (unsigned long)gmax, (unsigned long)g50,
         (unsigned long)g100, (unsigned long)age);
  logMsg("vis: ring %lu frames, push avg %.2f max %.2f ms, gap max %lu ms, >40 ms %lu", (unsigned long)r,
         r ? rsum / 1000.0 / r : 0.0, rmax / 1000.0, (unsigned long)rgmax, (unsigned long)rg40);
}

void visConsole(const char *args) {
  while (*args == ' ') args++;
  if (!strcmp(args, "on") || !strcmp(args, "off")) {
    setEnabled(!strcmp(args, "on"));
    logMsg("vis: border %s (saved)", args);
  } else if (!strcmp(args, "reset")) {
    portENTER_CRITICAL(&mux);
    nFrames = nLost = gapMax = gapSum = gap50 = gap100 = 0;
    nRing = ringUsSum = ringUsMax = ringGapMax = ringGap40 = 0;
    lastSeq = -1;
    portEXIT_CRITICAL(&mux);
    logMsg("vis: counters reset");
  } else {
    stats();
  }
}
