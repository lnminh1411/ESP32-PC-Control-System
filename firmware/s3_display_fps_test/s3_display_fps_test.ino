// ============================================================
// S3 + GMT020-02-7P (ST7789, 240x320, 7-pin SPI) — Hello World + FPS test
// ============================================================
// Wiring per diagrams/schematic screenshot (2026-09-29 test wiring),
// traced from wire color at the display header to the SuperMini's
// numbered GPIO pins:
//
//   Display pin (left->right on header)  ->  S3 pin
//   CS                                   ->  GPIO3
//   DC                                   ->  GPIO4
//   RST                                  ->  GPIO5
//   SDA (MOSI)                           ->  GPIO6
//   SCL (SCK)                            ->  GPIO7
//   VCC                                  ->  3V3  (MUST be 3.3V — no level
//                                              shifter/regulator on this
//                                              module, do NOT wire to 5V/BAT)
//   GND                                  ->  GND
//
// CS/DC/RST/SDA/SCL order above is the module's *typical* silkscreen order
// (confirmed for GMT020-02 VER:1.1 boards via web search) applied to the
// wire colors read off the schematic screenshot. The screenshot resolution
// was too low to OCR the actual printed labels on your VER:1.3 board and
// revisions can differ — if the screen stays blank/garbled after upload,
// the wiring is still fine electrically (all 5 are just logic GPIOs, no
// damage risk), it just means two of CS/DC/RST/SCL/SDA are swapped: check
// the real silkscreen on your board and fix the #defines below.
//
// VCC->3V3 is the one wire worth actually verifying before power-on: if it
// landed on the S3's 5V or BAT pin instead, that overvolts the panel.
//
// Libraries needed (Library Manager):
//   "Adafruit GFX Library"
//   "Adafruit ST7789"
//
// Board: ESP32S3 Dev Module (or "ESP32S3 SuperMini" if your board list has it)

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>

#define TFT_CS   3
#define TFT_DC   4
#define TFT_RST  5
#define TFT_MOSI 6
#define TFT_SCK  7

#define TFT_W 240
#define TFT_H 320

SPIClass tftSPI(FSPI);
Adafruit_ST7789 tft(&tftSPI, TFT_CS, TFT_DC, TFT_RST);

uint32_t frameCount = 0;
uint32_t fpsWindowStart = 0;
uint16_t fps = 0;

void setup() {
  Serial.begin(115200);

  tftSPI.begin(TFT_SCK, -1 /* no MISO */, TFT_MOSI, TFT_CS);
  tft.init(TFT_W, TFT_H);
  tft.setSPISpeed(40000000);   // 40MHz — push the module as hard as it'll take
  tft.setRotation(1);          // landscape
  tft.fillScreen(ST77XX_BLACK);

  fpsWindowStart = millis();
}

void loop() {
  // Partial redraw only (text box + FPS readout) — a full 240x320 fill at
  // 40MHz SPI caps out around ~30fps just from raw byte-push time, so a
  // real 60fps+ test has to avoid repainting the whole panel every frame,
  // same as any real UI would.
  static uint16_t bg = ST77XX_BLACK;
  bg = (bg == ST77XX_BLACK) ? tft.color565(10, 10, 30) : ST77XX_BLACK;

  tft.fillRect(0, 0, TFT_W, 60, bg);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(3);
  tft.setCursor(10, 10);
  tft.print("Hello, world!");

  tft.setTextSize(2);
  tft.setTextColor(ST77XX_GREEN, bg);
  tft.setCursor(10, 40);
  tft.print(fps);
  tft.print(" fps");

  frameCount++;
  uint32_t now = millis();
  if (now - fpsWindowStart >= 1000) {
    fps = frameCount * 1000 / (now - fpsWindowStart);
    Serial.printf("FPS: %u\n", fps);
    frameCount = 0;
    fpsWindowStart = now;
  }
}
