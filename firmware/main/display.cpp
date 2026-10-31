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

#include <LovyanGFX.hpp>

#include "gif_store.h"
#include "screens.h"
#include "log.h"
#include "sys.h"

#define BAND 8
#define INTERNAL_RESERVE (160 * 1024)   // Wi-Fi + two TLS sessions need it

Panel panels[N_PANELS];

static uint8_t *dmaBuf[2];
static volatile uint32_t txDone;
static uint32_t txIssued;
static QueueHandle_t flushQueue;
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
    waitTx(1);                                    // the buffer we fill next is free
    uint8_t *b = dmaBuf[txIssued & 1], *o = b;
    for (int r = 0; r < rows; r++) {
      const uint16_t *s = src + (size_t)(y + r0 + r) * TFT_W + x;
      for (int k = 0; k < w; k += 2) {
        uint16_t a = __builtin_bswap16(s[k]);
        uint16_t c = __builtin_bswap16(s[k + 1]);
        o[0] = ((a >> 8) & 0xF0) | ((a >> 7) & 0x0F);               // R1 G1
        o[1] = ((a << 3) & 0xF0) | (c >> 12);                       // B1 R2
        o[2] = ((c >> 3) & 0xF0) | ((c >> 1) & 0x0F);               // G2 B2
        o += 3;
      }
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
  flushRows(p, 0, TFT_H);
  waitTx(0);
  uint32_t us = nowUs() - t0;
  p.pushUs += us;
  p.frames++;
  return us;
}

static void pushGifRectRaw(Panel &p, const uint16_t *pal, int rx, int ry, int rw, int rh, int x0, int y0);

// GIF rect minus the overlay band: up to two slices above and below it
static void pushGifRect(Panel &p, const uint16_t *pal, int rx, int ry, int rw, int rh, int x0, int y0) {
  if (p.bandY1 <= p.bandY0) { pushGifRectRaw(p, pal, rx, ry, rw, rh, x0, y0); return; }
  int top = y0 + ry, bot = top + rh;                    // panel rows
  int a1 = std::min(bot, (int)p.bandY0), b0 = std::max(top, (int)p.bandY1);
  if (a1 > top) pushGifRectRaw(p, pal, rx, ry, rw, a1 - top, x0, y0);
  if (bot > b0) pushGifRectRaw(p, pal, rx, ry + (b0 - top), rw, bot - b0, x0, y0);
}

// Rect of the panel's current frame: indices through the palette to RGB444
static void pushGifRectRaw(Panel &p, const uint16_t *pal, int rx, int ry, int rw, int rh, int x0, int y0) {
  int gx = x0 + rx;
  int gy = y0 + ry;
  waitTx(0);
  setWindow(p, gx, gy, rw, rh);
  size_t rowBytes = (size_t)rw * 3 / 2;
  int rowsPerChunk = std::max(1, (int)(CHUNK_BYTES / rowBytes));
  bool first = true;
  for (int r0 = 0; r0 < rh; r0 += rowsPerChunk) {
    int rows = std::min(rowsPerChunk, rh - r0);
    waitTx(1);
    uint8_t *b = dmaBuf[txIssued & 1], *o = b;
    for (int r = 0; r < rows; r++) {
      const uint8_t *s = p.gifCur + (size_t)(ry + r0 + r) * gif.w + rx;
      for (int k = 0; k < rw; k += 2) {
        uint16_t a = pal[s[k]], c = pal[s[k + 1]];
        o[0] = a >> 4;                                              // R1 G1
        o[1] = (a << 4) | (c >> 8);                                 // B1 R2
        o[2] = c;                                                   // G2 B2
        o += 3;
      }
    }
    txIssued++;
    esp_lcd_panel_io_tx_color(p.io, first ? 0x2C : 0x3C, b, o - b);
    first = false;
  }
  p.bytes += rowBytes * rh;
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

static void pushGif(Panel &p) {
  if (!gif.n) return;
  if (!p.gifCur) {   // first time on this panel
    p.gifCur = (uint8_t *)heap_caps_malloc((size_t)gif.w * gif.h, MALLOC_CAP_SPIRAM);
    if (!p.gifCur) {
      static bool told;
      if (!told) logMsg("gif: no PSRAM for a panel buffer");
      told = true;
      return;
    }
    p.gifShown = -1;
  }
  uint32_t t0 = nowUs();
  uint16_t fIdx = p.gifFrame;
  int x0 = ((TFT_W - gif.w) / 2) & ~1;
  int y0 = (TFT_H - gif.h) / 2;
  uint16_t prevIdx = gifPrevFrame(fIdx);   // a segment switch is a full push
  const uint16_t *pal = gif.pal[gifSegOf(fIdx)];
  bool band = p.bandY1 > p.bandY0, bandDirty = false;
  if (p.gifShown == (int16_t)prevIdx) {
    gifApplyFrame(p.gifCur, fIdx);
    uint8_t nr = gif.nRects[fIdx];
    for (uint8_t i = 0; i < nr; i++) {
      const GifRect &r = gif.rects[fIdx][i];
      pushGifRect(p, pal, r.x, r.y, r.w, r.h, x0, y0);
      if (y0 + r.y < p.bandY1 && y0 + r.y + r.h > p.bandY0) bandDirty = true;
    }
  } else {
    gifBuildFrame(p.gifCur, fIdx);
    pushGifRect(p, pal, 0, 0, gif.w, gif.h, x0, y0);
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
    if (xQueueReceive(flushQueue, &idx, pdMS_TO_TICKS(10)) == pdTRUE) {
      if (idx >= 0 && idx < N_PANELS) {
        Panel &p = panels[idx];
        uint32_t bytesBefore = p.bytes;
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

void displaySubmit(int panel) {
  if (panel < 0 || panel >= N_PANELS) return;
  panels[panel].busy = true;
  xQueueSend(flushQueue, &panel, portMAX_DELAY);
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
  xTaskCreatePinnedToCore(flushTask, "flush", 4096, nullptr, 2, &flushTaskHandle, 0);
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
