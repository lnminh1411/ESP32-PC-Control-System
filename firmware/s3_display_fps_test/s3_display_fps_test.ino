// ============================================================
// S3 + ST7789 240x320 SPI panel (rev 3 board: GMT024-08-SPI8P) — Hello World + FPS test
// ============================================================
// Pins: the rev 3 carrier board's display 1 socket (J3) -- see the #defines
// below. The panel plugs straight into the board: VCC comes from the module's
// 5Vin (the GMT024 panel has its own 3.3V regulator), GND from the board.
//
// History: these sketches were first brought up on a bench rig (SuperMini +
// GMT020-02 panel wired CS/DC/RST/SDA/SCL -> GPIO3/4/5/6/7, VCC on 3V3 --
// that bare GMT020 module had no regulator and must NOT see 5V). To run on
// that rig again, put the old pin numbers back and drop boardPinsInit().
//
// Libraries needed (Library Manager):
//   "Adafruit GFX Library"
//   "Adafruit ST7789"
//
// Board: ESP32S3 Dev Module (or "ESP32S3 SuperMini" if your board list has it)

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>

// Pins: rev 3 carrier board (pcb/main.ato), display 1 socket (J3). SCK/MOSI/
// DC are shared by all three display sockets; each has its own CS/RST/BL.
// (Old bench rig -- SuperMini, panel wired to GPIO3-7: CS 3, DC 4, RST 5,
// MOSI 6, SCK 7.)
#define TFT_CS   8    // CS1
#define TFT_DC   9    // shared DC
#define TFT_RST  10   // RST1
#define TFT_MOSI 11   // shared, via 33R (FSPI IO_MUX pin, 80 MHz-capable)
#define TFT_SCK  12   // shared, via 33R (FSPI IO_MUX pin)
#define TFT_BL   13   // BL1 -> panel's backlight transistor (HIGH = on)
// displays 2/3 sit on the same bus: keep them deselected and dark
#define TFT2_CS  18
#define TFT3_CS  17
#define TFT2_BL  6
#define TFT3_BL  5

#define TFT_W 240
#define TFT_H 320

SPIClass tftSPI(FSPI);
Adafruit_ST7789 tft(&tftSPI, TFT_CS, TFT_DC, TFT_RST);

uint32_t frameCount = 0;
uint32_t fpsWindowStart = 0;
uint16_t fps = 0;

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
