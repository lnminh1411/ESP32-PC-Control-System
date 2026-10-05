// ============================================================
// S3 GIF stream test -- new-panel bring-up, raw RGB565 over serial.
// ============================================================
//
// The PC (Scripts/gif_stream.py) decodes the ORIGINAL GIF (no background
// removal, no palette, no RLE), scales it to the panel width and streams
// every frame as raw big-endian RGB565 over the USB-UART (CH343, COM port).
// Each frame is drawn the moment it lands and kept in PSRAM; once all frames are in, they are saved to the FAT
// partition (/gif.bin) and the sketch loops them from PSRAM at the GIF's own
// frame durations, logging FPS + push time. Pixels go out raw: ESP-IDF
// esp_lcd SPI + DMA, no GFX layer (Adafruit_GFX writePixels managed 27 fps,
// 33 ms/frame at 40 MHz -- slower than the GIF's 30 ms frames: stutter). On every later boot the saved
// GIF is loaded back into PSRAM and plays without the PC.
//
// Why not live 30fps over the wire: one 240x291 frame is 139,680 bytes; the
// UART tops out around 300 KB/s, i.e. ~2 fps. PSRAM (8MB) holds the whole
// 33-frame GIF (4.6MB), so the wire only has to carry it once.
//
// Protocol (host -> device unless noted):
//   device: "READY\n" after boot (then 1.5 s to start an upload, else it
//           plays the saved GIF; with nothing saved it keeps sending READY)
//   "GIF5" u16 w, u16 h, u16 n, n x u16 duration_ms      (little-endian)
//   device: "OK\n" or "ERR <reason>\n"
//   per frame: w*h*2 bytes RGB565 big-endian, u32 byte-sum (LE)
//   device: "K<i>\n" (stored) or "E<i>\n" (sum mismatch -> host resends)
//   device: "SAVED\n" (or "ERR save ..."), "PLAY\n", then
//           "FPS:.. push_ms:.." once a second
//   A new "GIF5" upload is accepted any time while playing.
//
// Backlight test on top: BL PWM sweeps 100% -> 0% over 2 s, back to 100%
// over 1 s, repeating (blSweepTask).
//
// Bench wiring (new screen, header order BL CS DC RST SDA SCL):
//   BL 4, CS 5, DC 6, RST 7, SDA/MOSI 15, SCL/SCK 16. Module VCC takes 5V
//   (brighter backlight) or 3.3V.
// 15/16 are not FSPI IO_MUX pins, so SPI runs through the GPIO matrix:
// SPI_HZ defaults to 80 MHz; build with -DSPI_HZ=40000000 if the picture
// breaks up (40 MHz is the documented GPIO-matrix ceiling).
//
// Board config (arduino-cli):
//   --fqbn esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB

#include <FFat.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_st7789.h>

#define TFT_BL   4    // backlight, PWM (HIGH = 100%)

#define BL_PWM_HZ   20000   // above hearing, no coil whine
#define BL_PWM_BITS 10
#define BL_MAX      ((1u << BL_PWM_BITS) - 1)
#define BL_FALL_MS  2000    // 100% -> 0%
#define BL_RISE_MS  1000    // 0% -> 100%
#define TFT_CS   5
#define TFT_DC   6
#define TFT_RST  7
#define TFT_MOSI 15   // panel "SDA"
#define TFT_SCK  16   // panel "SCL"

#define TFT_W 240
#define TFT_H 320
#ifndef SPI_HZ
#define SPI_HZ 80000000
#endif

#define BAUD 3000000
#define MAX_FRAMES 64
#define GIF_FILE "/gif.bin"   // "GIF5" + w,h,n + durations + frames

// Raw push: frames stay RGB565 big-endian in PSRAM and go to the panel as-is
// (no GFX layer). Each frame is cut into CHUNK_LINES-row slices that are
// memcpy'd into one of two internal DMA buffers while the other one is on
// the wire, so the SPI bus never waits for the CPU.
#define CHUNK_LINES 32
#define CHUNK_BYTES (TFT_W * CHUNK_LINES * 2)

static esp_lcd_panel_io_handle_t io;
static esp_lcd_panel_handle_t panel;
static uint8_t *dmaBuf[2];
static volatile uint32_t txDone;       // completed colour transfers (ISR)
static uint32_t txIssued;              // queued colour transfers

static uint16_t fw, fh, nFrames;
static uint16_t durMs[MAX_FRAMES];
static uint16_t *frames[MAX_FRAMES];
static bool fatOk;

static size_t frameBytes() { return (size_t)fw * fh * 2; }

static bool IRAM_ATTR onTxDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *) {
  txDone++;
  return false;
}

static bool readExact(void *dst, size_t len, uint32_t timeoutMs = 5000) {
  uint8_t *p = (uint8_t *)dst;
  uint32_t last = millis();
  while (len) {
    int avail = Serial.available();
    if (avail > 0) {
      size_t got = Serial.readBytes(p, min((size_t)avail, len));
      p += got;
      len -= got;
      last = millis();
    } else if (millis() - last > timeoutMs) {
      return false;
    }
  }
  return true;
}

static void waitTx(uint32_t inFlight) {  // until at most inFlight transfers are pending
  while (txIssued - txDone > inFlight) {
  }
}

// Stream w x h pixels into the window at (x, y). src = big-endian RGB565
// (row-major, w wide); src == nullptr fills with `fill` instead.
static void pushRect(int x, int y, int w, int h, const uint8_t *src, uint16_t fill = 0) {
  waitTx(0);
  uint8_t ca[4] = {(uint8_t)(x >> 8), (uint8_t)x, (uint8_t)((x + w - 1) >> 8), (uint8_t)(x + w - 1)};
  uint8_t ra[4] = {(uint8_t)(y >> 8), (uint8_t)y, (uint8_t)((y + h - 1) >> 8), (uint8_t)(y + h - 1)};
  esp_lcd_panel_io_tx_param(io, 0x2A, ca, 4);   // CASET
  esp_lcd_panel_io_tx_param(io, 0x2B, ra, 4);   // RASET
  size_t total = (size_t)w * h * 2, rowBytes = (size_t)w * 2;
  size_t chunk = (CHUNK_BYTES / rowBytes) * rowBytes;
  for (size_t off = 0; off < total; off += chunk) {
    size_t n = min(chunk, total - off);
    waitTx(1);                         // the buffer we are about to fill is free
    uint8_t *b = dmaBuf[txIssued & 1];
    if (src) {
      memcpy(b, src + off, n);
    } else {
      for (size_t k = 0; k < n; k += 2) { b[k] = fill >> 8; b[k + 1] = fill; }
    }
    txIssued++;
    esp_lcd_panel_io_tx_color(io, off ? 0x3C : 0x2C, b, n);  // RAMWR, then RAMWRC
  }
}

static void pushFrame(const uint16_t *buf) {
  pushRect((TFT_W - fw) / 2, (TFT_H - fh) / 2, fw, fh, (const uint8_t *)buf);
}

static void fillRect(int x, int y, int w, int h, uint16_t c) { pushRect(x, y, w, h, nullptr, c); }

// Backlight test: PWM duty 100% -> 0% linearly over BL_FALL_MS, back to 100%
// over BL_RISE_MS, forever. Own task on core 0, so it runs smoothly
// whatever the GIF loop (core 1) is doing.
static void blSweepTask(void *) {
  const uint32_t period = BL_FALL_MS + BL_RISE_MS;
  uint32_t t0 = millis();
  for (;;) {
    uint32_t t = (millis() - t0) % period;
    uint32_t duty = t < BL_FALL_MS ? BL_MAX - BL_MAX * t / BL_FALL_MS
                                   : BL_MAX * (t - BL_FALL_MS) / BL_RISE_MS;
    ledcWrite(TFT_BL, duty);
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

static void lcdInit() {
  spi_bus_config_t bus = {};
  bus.mosi_io_num = TFT_MOSI;
  bus.miso_io_num = -1;
  bus.sclk_io_num = TFT_SCK;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = CHUNK_BYTES + 16;
  ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

  esp_lcd_panel_io_spi_config_t ioc = {};
  ioc.cs_gpio_num = TFT_CS;
  ioc.dc_gpio_num = TFT_DC;
  ioc.spi_mode = 0;
  ioc.pclk_hz = SPI_HZ;
  ioc.trans_queue_depth = 4;
  ioc.on_color_trans_done = onTxDone;
  ioc.lcd_cmd_bits = 8;
  ioc.lcd_param_bits = 8;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &ioc, &io));

  esp_lcd_panel_dev_config_t pc = {};
  pc.reset_gpio_num = TFT_RST;
  pc.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  pc.bits_per_pixel = 16;
  ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &pc, &panel));
  esp_lcd_panel_reset(panel);
  esp_lcd_panel_init(panel);
  esp_lcd_panel_invert_color(panel, true);      // IPS ST7789: same INVON Adafruit sent
  // MADCTL 0 = Adafruit rotation 2 (the orientation this panel needs)
  esp_lcd_panel_disp_on_off(panel, true);

  for (int i = 0; i < 2; i++) dmaBuf[i] = (uint8_t *)heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
}

// Size check + PSRAM buffers for the current fw/fh/nFrames.
static bool allocFrames() {
  if (fw == 0 || fw > TFT_W || fh == 0 || fh > TFT_H || nFrames == 0 || nFrames > MAX_FRAMES) {
    Serial.printf("ERR bad size %ux%u x%u\n", fw, fh, nFrames);
    return false;
  }
  for (uint16_t i = 0; i < MAX_FRAMES; i++) {   // sizes may differ from the last GIF
    free(frames[i]);
    frames[i] = nullptr;
  }
  for (uint16_t i = 0; i < nFrames; i++) {
    frames[i] = (uint16_t *)ps_malloc(frameBytes());
    if (!frames[i]) { Serial.printf("ERR psram alloc frame %u (free %u)\n", i, ESP.getFreePsram()); return false; }
  }
  return true;
}

// Wait for the "GIF5" magic; announces READY once a second while idle.
static void waitMagic() {
  const char magic[] = "GIF5";
  uint8_t m = 0;
  uint32_t lastReady = 0;
  while (m < 4) {                      // resync on the magic, byte by byte
    if (Serial.available()) {
      m = (Serial.read() == magic[m]) ? m + 1 : 0;
    } else if (millis() - lastReady > 1000) {
      Serial.println("READY");
      lastReady = millis();
    }
  }
}

// Everything after the magic: header, frames (drawn as they land), save.
static bool receiveGif() {
  uint16_t hdr[3];
  if (!readExact(hdr, sizeof hdr)) { Serial.println("ERR header timeout"); return false; }
  fw = hdr[0]; fh = hdr[1]; nFrames = hdr[2];
  if (nFrames == 0 || nFrames > MAX_FRAMES) { Serial.printf("ERR bad count %u\n", nFrames); return false; }
  if (!readExact(durMs, nFrames * 2)) { Serial.println("ERR duration timeout"); return false; }
  if (!allocFrames()) return false;
  Serial.println("OK");

  for (uint16_t i = 0; i < nFrames;) {
    uint32_t sum = 0, want;
    if (!readExact(frames[i], frameBytes()) || !readExact(&want, 4)) {
      Serial.printf("ERR frame %u timeout\n", i);
      return false;
    }
    const uint8_t *p = (const uint8_t *)frames[i];
    for (size_t k = 0; k < frameBytes(); k++) sum += p[k];
    if (sum != want) { Serial.printf("E%u\n", i); continue; }  // host resends
    pushFrame(frames[i]);
    Serial.printf("K%u\n", i);
    i++;
  }

  File f = fatOk ? FFat.open(GIF_FILE, FILE_WRITE) : File();
  bool ok = f;
  if (ok) {
    uint16_t h[3] = {fw, fh, nFrames};
    ok = f.write((const uint8_t *)"GIF5", 4) == 4 && f.write((uint8_t *)h, 6) == 6 &&
         f.write((uint8_t *)durMs, nFrames * 2) == nFrames * 2u;
    for (uint16_t i = 0; ok && i < nFrames; i++)
      ok = f.write((uint8_t *)frames[i], frameBytes()) == frameBytes();
    f.close();
  }
  if (ok) Serial.println("SAVED");
  else Serial.printf("ERR save (fat %s, free %u)\n", fatOk ? "ok" : "not mounted",
                     fatOk ? (unsigned)(FFat.totalBytes() - FFat.usedBytes()) : 0u);
  return true;
}

static bool loadGif() {
  if (!fatOk || !FFat.exists(GIF_FILE)) return false;
  File f = FFat.open(GIF_FILE, FILE_READ);
  char magic[4];
  uint16_t h[3];
  bool ok = f.read((uint8_t *)magic, 4) == 4 && !memcmp(magic, "GIF5", 4) && f.read((uint8_t *)h, 6) == 6;
  if (ok) {
    fw = h[0]; fh = h[1]; nFrames = h[2];
    ok = allocFrames() && f.read((uint8_t *)durMs, nFrames * 2) == nFrames * 2u;
    for (uint16_t i = 0; ok && i < nFrames; i++)
      ok = f.read((uint8_t *)frames[i], frameBytes()) == frameBytes();
  }
  f.close();
  Serial.printf(ok ? "loaded %u frames %ux%u from flash\n" : "saved GIF unreadable\n", nFrames, fw, fh);
  return ok;
}

void setup() {
  ledcAttach(TFT_BL, BL_PWM_HZ, BL_PWM_BITS);
  ledcWrite(TFT_BL, BL_MAX);           // backlight 100% until the sweep task takes over
  gpio_set_drive_capability((gpio_num_t)TFT_BL, GPIO_DRIVE_CAP_3);  // ~40 mA, in case BL feeds the LEDs directly
  xTaskCreatePinnedToCore(blSweepTask, "bl", 2048, nullptr, 1, nullptr, 0);

  Serial.setRxBufferSize(32768);       // must precede begin()
  Serial.begin(BAUD);
  delay(100);

  lcdInit();
  fillRect(0, 0, TFT_W, TFT_H, 0x0000);   // panel RAM powers up as noise

  fatOk = FFat.begin(true);            // formats on first use
  Serial.printf("panel %dx%d raw SPI %d Hz, PSRAM free %u, fat %s\n", TFT_W, TFT_H, SPI_HZ,
                ESP.getFreePsram(), fatOk ? "ok" : "FAILED");
  Serial.println("READY");

  uint32_t t0 = millis();              // short window for a host upload ...
  while (!Serial.available() && millis() - t0 < 1500) delay(5);
  if (!Serial.available() && loadGif()) {   // ... else play the saved one
    Serial.println("PLAY");
    return;
  }
  do { waitMagic(); } while (!receiveGif());
  Serial.println("PLAY");
}

void loop() {
  static uint16_t idx = 0;
  static uint32_t shown = 0, pushAcc = 0, winStart = millis();

  if (Serial.available() && Serial.peek() != 'G') Serial.read();   // line noise: drop it
  if (Serial.available()) {            // new upload while playing
    do { waitMagic(); } while (!receiveGif());
    Serial.println("PLAY");
    idx = shown = pushAcc = 0;
    winStart = millis();
  }

  static uint32_t due = millis();
  uint32_t t0 = micros();
  pushFrame(frames[idx]);
  waitTx(0);
  pushAcc += micros() - t0;
  shown++;
  due += durMs[idx];                   // absolute schedule: no drift from rounding
  int32_t wait = (int32_t)(due - millis());
  if (wait > 0) delay(wait);
  else if (wait < -100) due = millis();   // fell far behind (upload, etc.): resync
  idx = (idx + 1) % nFrames;

  uint32_t now = millis();
  if (now - winStart >= 1000) {
    Serial.printf("FPS:%.1f push_ms:%.2f\n", shown * 1000.0f / (now - winStart), pushAcc / 1000.0f / shown);
    shown = pushAcc = 0;
    winStart = now;
  }
}
