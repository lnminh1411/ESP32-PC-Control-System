#include "display.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp32-hal-psram.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_st7789.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "gif_store.h"
#include "log.h"

#define BAND 8
#define INTERNAL_RESERVE (160 * 1024)   // Wi-Fi + two TLS sessions need it

Panel panels[N_PANELS];

PsCanvas::PsCanvas(uint16_t w, uint16_t h, bool internal) : GFXcanvas16(w, h, false), _isInternal(false) {
  size_t bytes = (size_t)w * h * 2;
  if (internal && heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >= bytes + INTERNAL_RESERVE) {
    buffer = (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buffer) {
      memset(buffer, 0, bytes);
      _isInternal = true;
    }
  }
  if (!buffer) {
    buffer = (uint16_t *)ps_calloc((size_t)w * h, 2);
    _isInternal = false;
  }
}

static uint8_t *dmaBuf[2];
static volatile uint32_t txDone;
static uint32_t txIssued;
static QueueHandle_t flushQueue;
static volatile float busWirePct = 0.0f;

float busLoadPct() { return busWirePct; }

static bool IRAM_ATTR onTxDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *) {
  txDone = txDone + 1;   // only this ISR writes it
  return false;
}

uint32_t displaySpinUs;   // perf: time spent waiting for DMA (temporary)

static void waitTx(uint32_t inFlight) {
  if (txIssued - txDone <= inFlight) return;
  uint32_t t = micros();
  while (txIssued - txDone > inFlight) {
  }
  displaySpinUs += micros() - t;
}

// Push canvas rect (x, y, w, h) of panel p as RGB444. x and w must be even.
static void push444(Panel &p, int x, int y, int w, int h) {
  const uint16_t *src = p.canvas->getBuffer();
  waitTx(0);
  uint8_t ca[4] = {(uint8_t)(x >> 8), (uint8_t)x, (uint8_t)((x + w - 1) >> 8), (uint8_t)(x + w - 1)};
  uint8_t ra[4] = {(uint8_t)(y >> 8), (uint8_t)y, (uint8_t)((y + h - 1) >> 8), (uint8_t)(y + h - 1)};
  esp_lcd_panel_io_tx_param(p.io, 0x2A, ca, 4);   // CASET
  esp_lcd_panel_io_tx_param(p.io, 0x2B, ra, 4);   // RASET
  size_t rowBytes = (size_t)w * 3 / 2;
  int rowsPerChunk = max(1, (int)(CHUNK_BYTES / rowBytes));
  bool first = true;
  for (int r0 = 0; r0 < h; r0 += rowsPerChunk) {
    int rows = min(rowsPerChunk, h - r0);
    waitTx(1);                                    // the buffer we fill next is free
    uint8_t *b = dmaBuf[txIssued & 1], *o = b;
    for (int r = 0; r < rows; r++) {
      const uint16_t *s = src + (size_t)(y + r0 + r) * TFT_W + x;
      for (int k = 0; k < w; k += 2) {
        uint16_t a = s[k], c = s[k + 1];
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
    lo = min(lo, a);
    int b = words - 1;
    while (b > hi && cr[b] == sr[b]) b--;
    hi = max(hi, b);
  }
  if (hi < 0) return false;
  x0 = lo * 2;
  x1 = hi * 2 + 1;
  return true;
}

static uint32_t displayFlush(Panel &p) {
  uint32_t t0 = micros();
  int runY = -1, rx0 = TFT_W, rx1 = -1;
  for (int y = 0; y <= TFT_H; y += BAND) {
    int x0, x1;
    bool dirty = y < TFT_H && bandDiff(p, y, min(BAND, TFT_H - y), x0, x1);
    if (dirty) {
      if (runY < 0) runY = y;
      rx0 = min(rx0, x0);
      rx1 = max(rx1, x1);
    } else if (runY >= 0) {                     // a run of dirty bands ended: push it
      int h = y - runY, w = rx1 - rx0 + 1;
      push444(p, rx0, runY, w, h);
      for (int r = runY; r < y; r++)
        memcpy(p.sent + (size_t)r * TFT_W + rx0, p.canvas->getBuffer() + (size_t)r * TFT_W + rx0, w * 2);
      runY = -1; rx0 = TFT_W; rx1 = -1;
    }
  }
  waitTx(0);
  uint32_t us = micros() - t0;
  p.pushUs += us;
  p.frames++;
  return us;
}

static void pushGifRect(Panel &p, const uint8_t *framePacked, int rx, int ry, int rw, int rh, int x0, int y0) {
  int gx = x0 + rx;
  int gy = y0 + ry;
  waitTx(0);
  uint8_t ca[4] = {(uint8_t)(gx >> 8), (uint8_t)gx, (uint8_t)((gx + rw - 1) >> 8), (uint8_t)(gx + rw - 1)};
  uint8_t ra[4] = {(uint8_t)(gy >> 8), (uint8_t)gy, (uint8_t)((gy + rh - 1) >> 8), (uint8_t)(gy + rh - 1)};
  esp_lcd_panel_io_tx_param(p.io, 0x2A, ca, 4);   // CASET
  esp_lcd_panel_io_tx_param(p.io, 0x2B, ra, 4);   // RASET
  size_t frameStride = (size_t)gif.pw * 3 / 2;
  size_t xByteOffset = (size_t)rx * 3 / 2;
  size_t rowBytes = (size_t)rw * 3 / 2;
  int rowsPerChunk = max(1, (int)(CHUNK_BYTES / rowBytes));
  bool first = true;
  for (int r0 = 0; r0 < rh; r0 += rowsPerChunk) {
    int rows = min(rowsPerChunk, rh - r0);
    waitTx(1);
    uint8_t *b = dmaBuf[txIssued & 1], *o = b;
    for (int r = 0; r < rows; r++) {
      const uint8_t *s = framePacked + (size_t)(ry + r0 + r) * frameStride + xByteOffset;
      memcpy(o, s, rowBytes);
      o += rowBytes;
    }
    txIssued++;
    esp_lcd_panel_io_tx_color(p.io, first ? 0x2C : 0x3C, b, o - b);
    first = false;
  }
  p.bytes += rowBytes * rh;
}

static void pushGif(Panel &p) {
  if (!gif.n || !gif.packed[p.gifFrame]) return;
  uint32_t t0 = micros();
  uint16_t fIdx = p.gifFrame;
  int x0 = ((TFT_W - gif.pw) / 2) & ~1;
  int y0 = (TFT_H - gif.h) / 2;
  uint16_t prevIdx = (fIdx == 0) ? (gif.n - 1) : (fIdx - 1);
  if (p.gifShown == (int16_t)prevIdx) {
    uint8_t nr = gif.nRects[fIdx];
    for (uint8_t i = 0; i < nr; i++) {
      const GifRect &r = gif.rects[fIdx][i];
      pushGifRect(p, gif.packed[fIdx], r.x, r.y, r.w, r.h, x0, y0);
    }
  } else {
    pushGifRect(p, gif.packed[fIdx], 0, 0, gif.pw, gif.h, x0, y0);
  }
  waitTx(0);
  p.gifShown = fIdx;
  uint32_t us = micros() - t0;
  p.pushUs += us;
  p.frames++;
}

static void flushTask(void *) {
  uint32_t winStart = millis();
  for (;;) {
    int idx;
    if (xQueueReceive(flushQueue, &idx, pdMS_TO_TICKS(10)) == pdTRUE) {
      if (idx >= 0 && idx < N_PANELS) {
        Panel &p = panels[idx];
        if (p.view == VIEW_GIF) pushGif(p);
        else displayFlush(p);
        p.busy = false;
      }
    }
    uint32_t now = millis();
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
      busWirePct = ((float)totalBytes / dt) * 8.0f * 100.0f / (float)SPI_HZ;
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
  // Hardware reset of all three together; CS idles HIGH, the SPI driver
  // drives each panel's CS per transfer.
  for (int i = 0; i < N_PANELS; i++) {
    gpio_set_level((gpio_num_t)PIN_CS[i], 1);
    pinMode(PIN_CS[i], OUTPUT);
    pinMode(PIN_RST[i], OUTPUT);
    digitalWrite(PIN_RST[i], LOW);
  }
  delay(20);
  for (int i = 0; i < N_PANELS; i++) digitalWrite(PIN_RST[i], HIGH);
  delay(150);

  spi_bus_config_t bus = {};
  bus.mosi_io_num = PIN_MOSI;
  bus.miso_io_num = -1;
  bus.sclk_io_num = PIN_SCK;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = CHUNK_BYTES + 16;
  ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
  // Full drive (~40 mA) on the bus lines: three panel inputs + the 33R on a
  // long daisy chain want sharp edges.
  for (int p : {PIN_SCK, PIN_MOSI, PIN_DC})
    gpio_set_drive_capability((gpio_num_t)p, GPIO_DRIVE_CAP_3);

  // Allocate the two DMA chunk buffers BEFORE the canvases
  for (int i = 0; i < 2; i++)
    dmaBuf[i] = (uint8_t *)heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

  for (int i = 0; i < N_PANELS; i++) {
    Panel &p = panels[i];
    esp_lcd_panel_io_spi_config_t ioc = {};
    ioc.cs_gpio_num = PIN_CS[i];
    ioc.dc_gpio_num = PIN_DC;
    ioc.spi_mode = 0;
    ioc.pclk_hz = SPI_HZ;
    ioc.trans_queue_depth = 4;
    ioc.on_color_trans_done = onTxDone;
    ioc.lcd_cmd_bits = 8;
    ioc.lcd_param_bits = 8;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &ioc, &p.io));

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
    esp_lcd_panel_invert_color(p.lcd, true);   // IPS ST7789
    const uint8_t colmod = 0x53;               // 12 bpp RGB444 (esp_lcd set 16 bpp)
    esp_lcd_panel_io_tx_param(p.io, 0x3A, &colmod, 1);
    esp_lcd_panel_disp_on_off(p.lcd, true);    // MADCTL 0: the orientation the panels need

    p.canvas = new PsCanvas(TFT_W, TFT_H, i > 0);   // panels 2 and 3 ask internal, panel 1 stays psram
    p.sent = (uint16_t *)ps_malloc((size_t)TFT_W * TFT_H * 2);
    memset(p.sent, 0xA5, (size_t)TFT_W * TFT_H * 2);   // != any canvas: first flush sends all
    p.busy = false;
    p.gifShown = -1;
    p.gifFrame = 0;
  }

  logMsg("display: canvas 2 %s, 3 %s", panels[1].canvas->isInternal() ? "internal" : "psram",
         panels[2].canvas->isInternal() ? "internal" : "psram");

  flushQueue = xQueueCreate(N_PANELS, sizeof(int));
  xTaskCreatePinnedToCore(flushTask, "flush", 4096, nullptr, 2, nullptr, 0);
}

// ---- backlights ----
static uint8_t blPct[N_PANELS] = {100, 100, 100};   // user setting
static bool screensIdle;                            // PC asleep/off -> all dark

static void blApply() {
  for (int i = 0; i < N_PANELS; i++) ledcWrite(PIN_BL[i], screensIdle ? 0 : BL_MAX * blPct[i] / 100);
}

void backlightInit() {
  for (int i = 0; i < N_PANELS; i++) {
    ledcAttach(PIN_BL[i], BL_PWM_HZ, BL_PWM_BITS);
    gpio_set_drive_capability((gpio_num_t)PIN_BL[i], GPIO_DRIVE_CAP_3);
  }
  blApply();
}

void backlightSet(int panel, uint8_t pct) {
  blPct[panel] = pct;
  blApply();
}

uint8_t backlightPct(int panel) { return blPct[panel]; }

void backlightIdle(bool idle) {
  screensIdle = idle;
  blApply();
}

bool backlightIsIdle() { return screensIdle; }
