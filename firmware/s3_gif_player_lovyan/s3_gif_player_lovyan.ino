// ============================================================
// S3 GIF player -- LovyanGFX driver variant.
// ============================================================
//
// Same RLE frame data, same pins, same decode logic as
// s3_gif_player.ino (Adafruit_GFX/Adafruit_ST7789) -- only the display
// driver differs, to compare push throughput. LovyanGFX talks to the
// panel over the ESP-IDF SPI driver directly (DMA-capable) rather than
// through Arduino's SPIClass wrapper, which Adafruit_ST7789 sits on top
// of -- the expected win here is lower per-call overhead on
// writePixels()/pushImage(), not a different pixel format or decode
// path (both still just push a pre-decoded RGB565 buffer).
//
// Pin/panel assumptions carried over unverified from the Adafruit build:
// CS=3, DC=4, RST=5, MOSI=6, SCK=7, 80MHz, no MISO, no BLK line (panel's
// backlight is tied on internally). `invert=true` below is the common
// default for ST7789 panels (matches what Adafruit_ST7789's own init()
// sends) -- not confirmed against this exact panel's datasheet, so if
// colors come out inverted on first boot, flip that one flag.
//
// Board config (arduino-cli):
//   --fqbn esp32:esp32:esp32s3:PartitionScheme=huge_app

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <stdarg.h>
#include "gif_frames_rle.h"

// Pins: rev 3 carrier board (pcb/main.ato), display 1 socket (J3). SCK/MOSI/
// DC are shared by all three display sockets; each has its own CS/RST/BL.
// (Old bench rig -- SuperMini, panel wired to GPIO3-7: CS 3, DC 4, RST 5,
// MOSI 6, SCK 7.)
#define TFT_CS   8    // CS1
#define TFT_DC   9    // shared DC
#define TFT_RST  10   // RST1
#define TFT_MOSI 11   // shared, via 33R (FSPI IO_MUX pin, 80 MHz-capable)
#define TFT_SCK  12   // shared, via 33R (FSPI IO_MUX pin)
#define TFT_BL   3    // BL1 -> panel's backlight transistor (HIGH = on)
// displays 2/3 sit on the same bus: keep them deselected and dark
#define TFT2_CS  18
#define TFT3_CS  17
#define TFT2_BL  6
#define TFT3_BL  5

#define TFT_W 240
#define TFT_H 320

#define SPI_HZ 80000000

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel_instance;
  lgfx::Bus_SPI _bus_instance;

public:
  LGFX(void) {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host = SPI2_HOST;  // FSPI on ESP32-S3
      cfg.spi_mode = 0;
      cfg.freq_write = SPI_HZ;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = TFT_SCK;
      cfg.pin_mosi = TFT_MOSI;
      cfg.pin_miso = -1;
      cfg.pin_dc = TFT_DC;
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }
    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs = TFT_CS;
      cfg.pin_rst = TFT_RST;
      cfg.pin_busy = -1;
      cfg.panel_width = TFT_W;
      cfg.panel_height = TFT_H;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.readable = false;
      cfg.invert = true;
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel_instance.config(cfg);
    }
    setPanel(&_panel_instance);
  }
};

LGFX tft;

static uint16_t frameBuf[GIF_FRAME_W * GIF_FRAME_H];

void logf(const char *fmt, ...) {
  char buf[160];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.println(buf);
}

void decodeFrame(uint8_t frameIdx) {
  const uint8_t *runLens = (const uint8_t *)pgm_read_ptr(&gifRunLens[frameIdx]);
  const uint16_t *runPixs = (const uint16_t *)pgm_read_ptr(&gifRunPixs[frameIdx]);
  uint32_t entries = gifFrameEntryCount[frameIdx];

  uint32_t out = 0;
  for (uint32_t i = 0; i < entries; i++) {
    uint8_t len = pgm_read_byte(&runLens[i]);
    uint16_t val = pgm_read_word(&runPixs[i]);
    for (uint8_t k = 0; k < len; k++) {
      frameBuf[out++] = val;
    }
  }
}

// Call first in setup(), before the display library touches the bus.
static void boardPinsInit() {
  pinMode(TFT2_CS, OUTPUT); digitalWrite(TFT2_CS, HIGH);
  pinMode(TFT3_CS, OUTPUT); digitalWrite(TFT3_CS, HIGH);
  pinMode(TFT2_BL, OUTPUT); digitalWrite(TFT2_BL, LOW);
  pinMode(TFT3_BL, OUTPUT); digitalWrite(TFT3_BL, LOW);
  pinMode(TFT_BL, OUTPUT);  digitalWrite(TFT_BL, HIGH);
}

void setup() {
  boardPinsInit();
  Serial.begin(115200);
  delay(200);

  tft.init();
  tft.setRotation(2);     // was upside-down at rotation 0 on this panel
  tft.setSwapBytes(true); // frameBuf is plain native-uint16 RGB565 (same
                           // bytes Adafruit_ST7789's writePixels() took and
                           // byte-swapped internally); LovyanGFX's pushImage
                           // needs that same swap told explicitly, or colors
                           // come out as structure-correct but color-wrong
                           // speckle -- the classic byte-order symptom.
  tft.fillScreen(TFT_BLACK);

  logf("LovyanGFX: playing %d frames at %dx%d, SPI %d Hz", GIF_FRAME_COUNT, GIF_FRAME_W, GIF_FRAME_H, SPI_HZ);
}

void loop() {
  static uint8_t frameIdx = 0;
  static int16_t yOffset = (TFT_H - GIF_FRAME_H) / 2;
  static uint32_t frameCount = 0, fpsWindowStart = 0, decodeMsAccum = 0, pushMsAccum = 0;
  if (fpsWindowStart == 0) fpsWindowStart = millis();

  uint32_t t0 = millis();
  decodeFrame(frameIdx);
  uint32_t t1 = millis();

  tft.startWrite();
  tft.pushImage(0, yOffset, GIF_FRAME_W, GIF_FRAME_H, frameBuf);
  tft.endWrite();
  uint32_t t2 = millis();

  decodeMsAccum += (t1 - t0);
  pushMsAccum += (t2 - t1);
  frameCount++;

  uint16_t wantDelay = gifFrameDurationMs[frameIdx];
  frameIdx = (frameIdx + 1) % GIF_FRAME_COUNT;

  uint32_t now = millis();
  if (now - fpsWindowStart >= 1000) {
    logf("FPS:%lu avg_decode_ms:%lu avg_push_ms:%lu",
         (unsigned long)(frameCount * 1000 / (now - fpsWindowStart)),
         (unsigned long)(decodeMsAccum / frameCount),
         (unsigned long)(pushMsAccum / frameCount));
    frameCount = 0;
    decodeMsAccum = 0;
    pushMsAccum = 0;
    fpsWindowStart = now;
  }

  uint32_t spent = t2 - t0;
  if (wantDelay > spent) delay(wantDelay - spent);
}
