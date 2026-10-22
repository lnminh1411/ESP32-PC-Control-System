// ============================================================
// Carrier firmware -- ESP32 PC Control System, rev 3.1 board.
// ============================================================
//
// Everything the carrier PCB does (pins: pcb/main.ato, pcb/README.md):
//   - 3x GMT024 ST7789 panels (240x320) on the shared SPI bus at 40 MHz
//     (80 MHz: blank panels on the carrier; the S3 can't do 60), each on its
//     own CS, each showing its own view (gif | status | stats). Dirty-
//     rectangle engine: only changed pixels are sent, as RGB444 (12 bpp).
//   - Per-panel backlight PWM (BL1-3), 100% at boot.
//   - Door remote: opto rows 1-4 (GPIO39/40/41/42) press keyfob buttons
//     B1-B4. A press is a 500 ms pulse; only one button at a time, and a
//     new press is refused until 500 ms after the last one ended (no
//     hardware interlock on the board -- the rule lives here).
//   - Motherboard reset: opto row 5 (GPIO1), 200 ms pulse, only on an
//     explicit command, never from boot. MOBO_RST_HOLD_TEST=1 (bench, LED on
//     J6): held HIGH from boot instead -- build with 0 for a real PC.
//   - PC power state from the native USB link (TinyUSB events) with the
//     30 s guard against the cold-boot blip (docs/pc-power-detection.md).
//     PC asleep/off -> backlights off; awake -> restored.
//   - Status LED (GPIO2): solid = PC on, slow blink = PC asleep/off,
//     short blip every 2 s = no USB host seen yet.
//
// Opto pins: latch pre-staged LOW with gpio_set_level, then pinMode OUTPUT
// (Hi-Z -> LOW, never a HIGH glitch); never INPUT_PULLUP (pcb/README.md).
//
// Control: text commands, one per line, on BOTH the native USB CDC port
// (to the PC's internal header) and the UART (CH343 USB-C, 3 Mbaud):
//   help | status | view <1|2|3> <gif|status|stats> | door <1-4> | reset
//   bl <1|2|3|all> <0-100> | led <auto|on|off>
// GIF upload: Scripts/gif_stream.py on the UART port (same protocol as
// s3_gif_stream_test; saved to /gif.bin in the FAT partition, so the GIF
// that bench sketch stored keeps playing here).
//
// Board config (arduino-cli):
//   --fqbn esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,USBMode=default,CDCOnBoot=cdc
// (USBMode=default = USB-OTG/TinyUSB, needed for the suspend/resume events;
// with CDCOnBoot=cdc, `Serial` is the native USB port, `Serial0` the UART.)

#include <Adafruit_GFX.h>
#include <FFat.h>
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_st7789.h>
#include <esp_timer.h>
#include "USB.h"
#include "carrier_types.h"

// ---- pins (rev 3.1) ----
#define PIN_SCK   12   // shared, via 33R (FSPI IO_MUX pin)
#define PIN_MOSI  11   // shared, via 33R (FSPI IO_MUX pin)
#define PIN_DC    9    // shared
static const int PIN_CS[3]  = {8, 18, 17};
static const int PIN_RST[3] = {10, 16, 15};
static const int PIN_BL[3]  = {3, 6, 5};
static const int PIN_DOOR[4] = {39, 40, 41, 42};   // keyfob B1-B4 (opto rows 1-4)
#define PIN_MOBO_RST 1                              // opto row 5
#define PIN_LED      2

// ---- display ----
#define TFT_W 240
#define TFT_H 320
#ifndef SPI_HZ
#define SPI_HZ 40000000   // 80 MHz gave blank panels on the carrier bus
#endif
#define CHUNK_LINES 32
#define CHUNK_BYTES (TFT_W * CHUNK_LINES * 2)

#define BL_PWM_HZ   20000
#define BL_PWM_BITS 10
#define BL_MAX      ((1u << BL_PWM_BITS) - 1)

// ---- optos ----
#define DOOR_PRESS_MS 500
#define DOOR_GAP_MS   500
#define MOBO_RST_MS   200
// TEST ONLY (LED on J6 instead of the motherboard): hold RST HIGH all the time.
// Never on a real motherboard -- held HIGH keeps the PC in reset.
#ifndef MOBO_RST_HOLD_TEST
#define MOBO_RST_HOLD_TEST 0
#endif

// ---- PC power state ----
#define USB_GUARD_MS 30000

// ---- GIF storage / upload ----
#define UART       Serial0
#define UART_BAUD  3000000
#define MAX_FRAMES 64
#define GIF_FILE   "/gif.bin"   // "GIF5" + w,h,n + durations + frames

// ============================================================
// Logging: UART always, native USB only while a host is listening.
// Muted during a GIF upload so the upload protocol stays clean.
// ============================================================
static volatile bool uploading;

static void logf(const char *fmt, ...) {
  char buf[200];
  va_list a;
  va_start(a, fmt);
  vsnprintf(buf, sizeof buf, fmt, a);
  va_end(a);
  if (!uploading) UART.println(buf);
  if (Serial) Serial.println(buf);
}

// ============================================================
// Displays: dirty-rectangle engine, RGB444 on the wire.
//
// Each panel has its own SPI device (own CS) and two PSRAM buffers:
//   canvas -- what the panel should show (Adafruit GFXcanvas16, RGB565),
//             views draw into it freely;
//   sent   -- what the panel shows now.
// flush() diffs the two in BAND-row bands, merges runs of dirty bands into
// one rectangle (x range = union, even-aligned), and pushes only those,
// converted to RGB444 (12 bpp, 2 px in 3 bytes: 25% less than RGB565).
// A static dashboard costs nothing; a changed number costs its glyphs.
// ============================================================
#define BAND 8

static Panel P[3];

static uint8_t *dmaBuf[2];
static volatile uint32_t txDone;
static uint32_t txIssued;

static bool IRAM_ATTR onTxDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *) {
  txDone++;
  return false;
}

static void waitTx(uint32_t inFlight) {
  while (txIssued - txDone > inFlight) {
  }
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

// Push whatever changed since the last flush. Returns bus microseconds.
static uint32_t flush(Panel &p) {
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
  return us;
}

static void lcdInit() {
  // Hardware reset of all three together; CS idles HIGH, the SPI driver
  // drives each panel's CS per transfer.
  for (int i = 0; i < 3; i++) {
    gpio_set_level((gpio_num_t)PIN_CS[i], 1);
    pinMode(PIN_CS[i], OUTPUT);
    pinMode(PIN_RST[i], OUTPUT);
    digitalWrite(PIN_RST[i], LOW);
  }
  delay(20);
  for (int i = 0; i < 3; i++) digitalWrite(PIN_RST[i], HIGH);
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

  for (int i = 0; i < 3; i++) {
    Panel &p = P[i];
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

    p.canvas = new PsCanvas(TFT_W, TFT_H);
    p.sent = (uint16_t *)ps_malloc((size_t)TFT_W * TFT_H * 2);
    memset(p.sent, 0xA5, (size_t)TFT_W * TFT_H * 2);   // != any canvas: first flush sends all
  }
  for (int i = 0; i < 2; i++) dmaBuf[i] = (uint8_t *)heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
}

// ---- backlights ----
static uint8_t blPct[3] = {100, 100, 100};   // user setting
static bool screensIdle;                     // PC asleep/off -> all dark

static void blApply() {
  for (int i = 0; i < 3; i++) ledcWrite(PIN_BL[i], screensIdle ? 0 : BL_MAX * blPct[i] / 100);
}

// ============================================================
// Optos: one-shot esp_timer releases each pulse on time, whatever
// the GIF loop is doing.
// ============================================================
static esp_timer_handle_t doorTimer, rstTimer;
static volatile int doorActive = -1;          // index of the pressed button, -1 = none
static volatile int64_t doorFreeAtUs;         // earliest start of the next press
static volatile bool rstActive;

static void optoPinInit(int pin) {
  gpio_set_level((gpio_num_t)pin, 0);         // latch LOW while still Hi-Z
  pinMode(pin, OUTPUT);                       // Hi-Z (10k pull-down) -> driven LOW
}

static void doorRelease(void *) {
  if (doorActive >= 0) gpio_set_level((gpio_num_t)PIN_DOOR[doorActive], 0);
  doorFreeAtUs = esp_timer_get_time() + DOOR_GAP_MS * 1000LL;
  doorActive = -1;
}

static void rstRelease(void *) {
  gpio_set_level((gpio_num_t)PIN_MOBO_RST, 0);
  rstActive = false;
}

static const char *doorPress(int n) {          // n = 1..4; nullptr = OK, else why not
  if (n < 1 || n > 4) return "button must be 1-4";
  if (doorActive >= 0) return "another button is being pressed";
  if (esp_timer_get_time() < doorFreeAtUs) return "too soon after the last press (500 ms gap)";
  doorActive = n - 1;
  gpio_set_level((gpio_num_t)PIN_DOOR[n - 1], 1);
  esp_timer_start_once(doorTimer, DOOR_PRESS_MS * 1000ULL);
  return nullptr;
}

static const char *moboReset() {
  if (MOBO_RST_HOLD_TEST) return "RST is held HIGH (MOBO_RST_HOLD_TEST build)";
  if (rstActive) return "reset pulse already running";
  rstActive = true;
  gpio_set_level((gpio_num_t)PIN_MOBO_RST, 1);
  esp_timer_start_once(rstTimer, MOBO_RST_MS * 1000ULL);
  return nullptr;
}

// ============================================================
// PC power state (native USB events) + status LED.
// ============================================================
static volatile PcState pcState = PC_UNKNOWN;
static volatile uint32_t guardStartMs;
static volatile bool guardActive;
static const char *pcName(PcState s) { return s == PC_ON ? "on" : s == PC_OFF ? "asleep/off" : "unknown (no USB host yet)"; }

static void pcSet(PcState s) {
  pcState = s;
  screensIdle = (s == PC_OFF);
  blApply();
}

static void usbEvent(void *, esp_event_base_t base, int32_t id, void *) {
  if (base != ARDUINO_USB_EVENTS) return;
  switch (id) {
    case ARDUINO_USB_STARTED_EVENT:
    case ARDUINO_USB_RESUME_EVENT:
      guardStartMs = millis();
      guardActive = true;
      pcSet(PC_ON);
      logf("usb: %s -> PC on (30 s guard armed)", id == ARDUINO_USB_STARTED_EVENT ? "started" : "resume");
      break;
    case ARDUINO_USB_SUSPEND_EVENT:
      if (guardActive && millis() - guardStartMs < USB_GUARD_MS) {
        logf("usb: suspend inside guard (%lu ms) -> ignored", (unsigned long)(millis() - guardStartMs));
      } else {
        pcSet(PC_OFF);
        logf("usb: suspend -> PC asleep/off, screens idle");
      }
      break;
    case ARDUINO_USB_STOPPED_EVENT:
      logf("usb: stopped (logged only)");
      break;
  }
}

static volatile LedMode ledMode = LED_AUTO;

static void ledTask(void *) {
  for (;;) {
    uint32_t t = millis();
    bool on;
    if (ledMode != LED_AUTO) on = (ledMode == LED_ON);
    else if (pcState == PC_ON) on = true;
    else if (pcState == PC_OFF) on = (t % 2000) < 1000;     // slow blink
    else on = (t % 2000) < 80;                              // short blip
    digitalWrite(PIN_LED, on);
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ============================================================
// GIF frames: PSRAM, saved in FAT, uploaded over the UART.
// ============================================================
static uint16_t fw, fh, nFrames;
static uint16_t durMs[MAX_FRAMES];
static uint16_t *frames[MAX_FRAMES];
static bool fatOk;

static size_t frameBytes() { return (size_t)fw * fh * 2; }

// GIF frame (stored big-endian, as the old raw push wanted) -> canvas, centred.
static void drawGifFrame(Panel &p, const uint16_t *f) {
  uint16_t *dst = p.canvas->getBuffer();
  int x0 = (TFT_W - fw) / 2, y0 = (TFT_H - fh) / 2;
  for (int y = 0; y < fh; y++) {
    const uint16_t *s = f + (size_t)y * fw;
    uint16_t *d = dst + (size_t)(y0 + y) * TFT_W + x0;
    for (int x = 0; x < fw; x++) d[x] = __builtin_bswap16(s[x]);
  }
}

static bool readExact(void *dst, size_t len, uint32_t timeoutMs = 5000) {
  uint8_t *p = (uint8_t *)dst;
  uint32_t last = millis();
  while (len) {
    int avail = UART.available();
    if (avail > 0) {
      size_t got = UART.readBytes(p, min((size_t)avail, len));
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
  if (fw == 0 || fw > TFT_W || fh == 0 || fh > TFT_H || nFrames == 0 || nFrames > MAX_FRAMES) {
    UART.printf("ERR bad size %ux%u x%u\n", fw, fh, nFrames);
    return false;
  }
  for (uint16_t i = 0; i < MAX_FRAMES; i++) { free(frames[i]); frames[i] = nullptr; }
  for (uint16_t i = 0; i < nFrames; i++) {
    frames[i] = (uint16_t *)ps_malloc(frameBytes());
    if (!frames[i]) { UART.printf("ERR psram alloc frame %u\n", i); return false; }
  }
  return true;
}

// After the "GIF5" magic: header, frames (drawn as they land), save.
static bool receiveGif() {
  uint16_t hdr[3];
  if (!readExact(hdr, sizeof hdr)) { UART.println("ERR header timeout"); return false; }
  fw = hdr[0]; fh = hdr[1]; nFrames = hdr[2];
  if (nFrames == 0 || nFrames > MAX_FRAMES) { UART.printf("ERR bad count %u\n", nFrames); return false; }
  if (!readExact(durMs, nFrames * 2)) { UART.println("ERR duration timeout"); return false; }
  if (!allocFrames()) return false;
  UART.println("OK");
  for (uint16_t i = 0; i < nFrames;) {
    uint32_t sum = 0, want;
    if (!readExact(frames[i], frameBytes()) || !readExact(&want, 4)) { UART.printf("ERR frame %u timeout\n", i); return false; }
    const uint8_t *p = (const uint8_t *)frames[i];
    for (size_t k = 0; k < frameBytes(); k++) sum += p[k];
    if (sum != want) { UART.printf("E%u\n", i); continue; }
    for (Panel &pn : P)
      if (pn.view == VIEW_GIF) { drawGifFrame(pn, frames[i]); flush(pn); }
    UART.printf("K%u\n", i);
    i++;
  }
  File f = fatOk ? FFat.open(GIF_FILE, FILE_WRITE) : File();
  bool ok = f;
  if (ok) {
    uint16_t h[3] = {fw, fh, nFrames};
    ok = f.write((const uint8_t *)"GIF5", 4) == 4 && f.write((uint8_t *)h, 6) == 6 &&
         f.write((uint8_t *)durMs, nFrames * 2) == nFrames * 2u;
    for (uint16_t i = 0; ok && i < nFrames; i++) ok = f.write((uint8_t *)frames[i], frameBytes()) == frameBytes();
    f.close();
  }
  UART.println(ok ? "SAVED" : "ERR save");
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
    for (uint16_t i = 0; ok && i < nFrames; i++) ok = f.read((uint8_t *)frames[i], frameBytes()) == frameBytes();
  }
  f.close();
  if (!ok) nFrames = 0;
  return ok;
}

// ============================================================
// Views: what each panel shows. GIF runs at its own frame timing; the two
// dashboards redraw every 250/500 ms, but only changed glyphs hit the bus.
// ============================================================
#define C_BG    0x0000
#define C_TEXT  0xFFFF
#define C_DIM   0x8410
#define C_GOOD  0x07E0
#define C_WARN  0xFD20
#define C_BAD   0xF800
#define C_ACCENT 0x5D1F

static const char *viewName(View v) { return v == VIEW_GIF ? "gif" : v == VIEW_STATUS ? "status" : "stats"; }
static char lastAction[40] = "none";
static uint32_t lastActionMs;
static uint32_t busUsWindow, busUsLast;          // whole-bus time, this / last 1 s

static void field(GFXcanvas16 &c, int y, int size, uint16_t col, const char *fmt, ...) {
  char buf[48];
  va_list a;
  va_start(a, fmt);
  vsnprintf(buf, sizeof buf, fmt, a);
  va_end(a);
  c.fillRect(0, y, TFT_W, 8 * size, C_BG);       // clear the line, then draw it
  c.setTextSize(size);
  c.setTextColor(col);
  c.setCursor(8, y);
  c.print(buf);
}

static void drawStatus(Panel &p) {
  GFXcanvas16 &c = *p.canvas;
  uint32_t up = millis() / 1000;
  field(c, 10, 2, C_ACCENT, "PC STATUS");
  c.drawFastHLine(8, 30, TFT_W - 16, C_DIM);
  uint16_t pcCol = pcState == PC_ON ? C_GOOD : pcState == PC_OFF ? C_WARN : C_DIM;
  field(c, 44, 3, pcCol, "%s", pcState == PC_ON ? "ON" : pcState == PC_OFF ? "ASLEEP" : "NO HOST");
  bool guard = guardActive && millis() - guardStartMs < USB_GUARD_MS;
  field(c, 76, 1, C_DIM, guard ? "boot guard: %lus left" : "", (unsigned long)((USB_GUARD_MS - (millis() - guardStartMs)) / 1000));
  field(c, 100, 1, C_DIM, "UPTIME");
  field(c, 112, 3, C_TEXT, "%02lu:%02lu:%02lu", (unsigned long)(up / 3600), (unsigned long)(up / 60 % 60), (unsigned long)(up % 60));
  field(c, 150, 1, C_DIM, "LAST ACTION");
  field(c, 162, 2, C_TEXT, "%s", lastAction);
  field(c, 182, 1, C_DIM, lastActionMs ? "%lus ago" : "", (unsigned long)((millis() - lastActionMs) / 1000));
  field(c, 206, 1, C_DIM, "DOOR REMOTE");
  field(c, 218, 2, doorActive >= 0 ? C_WARN : C_GOOD, doorActive >= 0 ? "B%d PRESSED" : "idle", doorActive + 1);
  field(c, 246, 1, C_DIM, "MOTHERBOARD RST");
  field(c, 258, 2, rstActive ? C_BAD : C_GOOD, rstActive ? "PULSING" : "idle");
}

static void bar(GFXcanvas16 &c, int y, float frac, uint16_t col) {
  int w = TFT_W - 16, f = (int)(constrain(frac, 0.0f, 1.0f) * w);
  c.fillRect(8, y, f, 8, col);
  c.fillRect(8 + f, y, w - f, 8, 0x2104);
}

static void drawStats(Panel &p) {
  GFXcanvas16 &c = *p.canvas;
  field(c, 10, 2, C_ACCENT, "SYSTEM");
  c.drawFastHLine(8, 30, TFT_W - 16, C_DIM);
  float busPct = busUsLast / 10000.0f;           // us per 1 s -> %
  field(c, 42, 1, C_DIM, "SPI BUS (40 MHz, RGB444)");
  field(c, 54, 2, busPct > 85 ? C_WARN : C_TEXT, "%.0f%% busy", busPct);
  bar(c, 74, busPct / 100, busPct > 85 ? C_WARN : C_GOOD);
  int y = 94;
  for (int i = 0; i < 3; i++, y += 34) {
    field(c, y, 1, C_DIM, "PANEL %d  %s", i + 1, viewName(P[i].view));
    field(c, y + 12, 2, C_TEXT, "%4.1ffps %4.0fKB/s", P[i].fps, P[i].kbps);
  }
  field(c, 200, 1, C_DIM, "MEMORY");
  field(c, 212, 2, C_TEXT, "PSRAM %4uK", (unsigned)(ESP.getFreePsram() / 1024));
  field(c, 232, 2, C_TEXT, "heap  %4uK", (unsigned)(ESP.getFreeHeap() / 1024));
  field(c, 258, 1, C_DIM, "CHIP");
  field(c, 270, 2, C_TEXT, "%.1f C", temperatureRead());
}

static void viewEnter(Panel &p, View v, int i) {
  p.view = v;
  p.canvas->fillScreen(C_BG);
  p.gifIdx = nFrames ? (uint16_t)(i * nFrames / 3) : 0;   // same GIF on several panels: offset starts
  p.dueMs = millis();
}

static void serviceViews() {
  uint32_t now = millis();
  for (int i = 0; i < 3; i++) {
    Panel &p = P[i];
    if ((int32_t)(now - p.dueMs) < 0) continue;
    if (p.view == VIEW_GIF) {
      if (!nFrames) { p.dueMs = now + 200; continue; }
      if (p.gifIdx >= nFrames) p.gifIdx = 0;
      drawGifFrame(p, frames[p.gifIdx]);
      p.dueMs += durMs[p.gifIdx];
      if ((int32_t)(now - p.dueMs) > 100) p.dueMs = now;   // fell far behind: resync
      p.gifIdx = (p.gifIdx + 1) % nFrames;
    } else {
      if (p.view == VIEW_STATUS) drawStatus(p); else drawStats(p);
      p.dueMs = now + (p.view == VIEW_STATUS ? 250 : 500);
    }
    busUsWindow += flush(p);
    p.frames++;
  }
  static uint32_t winStart = now;
  if (now - winStart >= 1000) {
    float dt = (now - winStart) / 1000.0f;
    for (Panel &p : P) {
      p.fps = p.frames / dt;
      p.kbps = p.bytes / 1024.0f / dt;
      p.busMs = p.pushUs / 1000.0f / dt;
      p.frames = p.pushUs = p.bytes = 0;
    }
    busUsLast = busUsWindow / dt;
    busUsWindow = 0;
    winStart = now;
  }
}

// ============================================================
// Commands
// ============================================================
static void handleCommand(char *line) {
  char *cmd = strtok(line, " \t");
  if (!cmd) return;
  char *a1 = strtok(nullptr, " \t"), *a2 = strtok(nullptr, " \t");
  if (!strcmp(cmd, "help")) {
    logf("commands: status | view <1|2|3> <gif|status|stats> | door <1-4> | reset | bl <1|2|3|all> <0-100> | led <auto|on|off>");
  } else if (!strcmp(cmd, "status")) {
    logf("pc %s%s | bus %.0f%% | bl %u/%u/%u%% %s | door %s | led %s",
         pcName(pcState), guardActive && millis() - guardStartMs < USB_GUARD_MS ? " (guard)" : "",
         busUsLast / 10000.0f, blPct[0], blPct[1], blPct[2], screensIdle ? "(idle: off)" : "",
         doorActive >= 0 ? "pressing" : "idle", ledMode == LED_AUTO ? "auto" : ledMode == LED_ON ? "on" : "off");
    for (int i = 0; i < 3; i++)
      logf("  panel %d: %-6s %5.1f fps %6.1f KB/s  bus %5.1f ms/s", i + 1, viewName(P[i].view), P[i].fps, P[i].kbps, P[i].busMs);
    logf("  gif: %u frames %ux%u", nFrames, fw, fh);
  } else if (!strcmp(cmd, "view")) {
    int n = a1 ? atoi(a1) : 0;
    View v = a2 && !strcmp(a2, "gif") ? VIEW_GIF : a2 && !strcmp(a2, "status") ? VIEW_STATUS : VIEW_STATS;
    if (n < 1 || n > 3 || !a2 || (v == VIEW_STATS && strcmp(a2, "stats"))) { logf("view: usage view <1|2|3> <gif|status|stats>"); return; }
    viewEnter(P[n - 1], v, n - 1);
    logf("view: panel %d -> %s", n, a2);
  } else if (!strcmp(cmd, "door")) {
    const char *err = doorPress(a1 ? atoi(a1) : 0);
    if (err) logf("door: refused, %s", err);
    else {
      logf("door: B%s pressed for %d ms", a1, DOOR_PRESS_MS);
      snprintf(lastAction, sizeof lastAction, "door B%s", a1);
      lastActionMs = millis();
    }
  } else if (!strcmp(cmd, "reset")) {
    const char *err = moboReset();
    if (err) logf("reset: refused, %s", err);
    else {
      logf("reset: motherboard RST pulsed for %d ms", MOBO_RST_MS);
      snprintf(lastAction, sizeof lastAction, "PC reset");
      lastActionMs = millis();
    }
  } else if (!strcmp(cmd, "bl")) {
    int pct = a2 ? atoi(a2) : -1;
    if (!a1 || pct < 0 || pct > 100) { logf("bl: usage bl <1|2|3|all> <0-100>"); return; }
    for (int i = 0; i < 3; i++)
      if (!strcmp(a1, "all") || atoi(a1) == i + 1) blPct[i] = pct;
    blApply();
    logf("bl: %u/%u/%u%%", blPct[0], blPct[1], blPct[2]);
  } else if (!strcmp(cmd, "led")) {
    if (a1 && !strcmp(a1, "on")) ledMode = LED_ON;
    else if (a1 && !strcmp(a1, "off")) ledMode = LED_OFF;
    else if (a1 && !strcmp(a1, "auto")) ledMode = LED_AUTO;
    else { logf("led: usage led <auto|on|off>"); return; }
    logf("led: %s", a1);
  } else {
    logf("unknown command '%s' (try help)", cmd);
  }
}

// Line assembly per port. On the UART an upper-case 'G' at the start of a
// line starts a GIF upload instead (commands are all lower-case).
static LineBuf uartLine, usbLine;

static bool feed(LineBuf &lb, int c) {        // true = a full line is in lb.buf
  if (c == '\r' || c == '\n') {
    if (!lb.n) return false;
    lb.buf[lb.n] = 0;
    lb.n = 0;
    return true;
  }
  if (lb.n < sizeof lb.buf - 1) lb.buf[lb.n++] = (char)c;
  return false;
}

static void waitMagicThenReceive() {
  const char magic[] = "GIF5";
  uploading = true;
  for (;;) {
    uint8_t m = 0;
    uint32_t t0 = millis();
    while (m < 4 && millis() - t0 < 3000) {
      if (UART.available()) m = (UART.read() == magic[m]) ? m + 1 : 0;
    }
    if (m < 4) break;                  // not an upload after all
    if (receiveGif()) { UART.println("PLAY"); break; }
  }
  uploading = false;
}

static void pollCommands() {
  while (UART.available()) {
    if (!uartLine.n && UART.peek() == 'G') { waitMagicThenReceive(); return; }
    if (feed(uartLine, UART.read())) handleCommand(uartLine.buf);
  }
  while (Serial.available())
    if (feed(usbLine, Serial.read())) handleCommand(usbLine.buf);
}

// ============================================================
void setup() {
  // 1. opto rows first: LOW before anything else can glitch them
  for (int p : PIN_DOOR) optoPinInit(p);
  optoPinInit(PIN_MOBO_RST);
  if (MOBO_RST_HOLD_TEST) gpio_set_level((gpio_num_t)PIN_MOBO_RST, 1);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  esp_timer_create_args_t ta = {};
  ta.callback = doorRelease; ta.name = "door";
  esp_timer_create(&ta, &doorTimer);
  ta.callback = rstRelease; ta.name = "mbrst";
  esp_timer_create(&ta, &rstTimer);

  // 2. backlights 100%
  for (int i = 0; i < 3; i++) {
    ledcAttach(PIN_BL[i], BL_PWM_HZ, BL_PWM_BITS);
    gpio_set_drive_capability((gpio_num_t)PIN_BL[i], GPIO_DRIVE_CAP_3);
  }
  blApply();

  // 3. serial ports + USB power-state events
  UART.setRxBufferSize(32768);
  UART.begin(UART_BAUD);
  USB.onEvent(usbEvent);
  Serial.begin();                      // native USB CDC (TinyUSB)
  xTaskCreatePinnedToCore(ledTask, "led", 2048, nullptr, 1, nullptr, 0);
  delay(100);

  // 4. displays
  lcdInit();
  viewEnter(P[0], VIEW_GIF, 0);
  viewEnter(P[1], VIEW_STATUS, 1);
  viewEnter(P[2], VIEW_STATS, 2);
  for (Panel &p : P) flush(p);           // clear all three to black

  fatOk = FFat.begin(true);
  logf("carrier rev 3.1 | 3 panels, dirty-rect RGB444, SPI %d Hz%s | PSRAM free %u | fat %s",
       SPI_HZ,
       MOBO_RST_HOLD_TEST ? " | TEST: mobo RST held HIGH" : "", ESP.getFreePsram(), fatOk ? "ok" : "FAILED");
  UART.println("READY");

  // 5. GIF: 1.5 s window for a host upload, else the saved one
  uint32_t t0 = millis();
  while (!UART.available() && millis() - t0 < 1500) delay(5);
  if (UART.available()) waitMagicThenReceive();
  if (!nFrames && loadGif()) logf("gif: %u frames %ux%u from flash", nFrames, fw, fh);
  if (!nFrames) logf("gif: none stored -- run Scripts/gif_stream.py");
  for (int i = 0; i < 3; i++) viewEnter(P[i], P[i].view, i);   // GIF offsets now that frames exist
  UART.println("PLAY");
}

void loop() {
  pollCommands();
  serviceViews();
  delay(1);
}
