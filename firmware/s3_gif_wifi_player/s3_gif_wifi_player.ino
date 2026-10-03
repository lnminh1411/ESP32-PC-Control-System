// ============================================================
// S3 GIF player, WiFi-streamed -- diagnostic build.
// ============================================================
//
// s3_gif_player.ino (flash-embedded frames + on-device palette decode)
// stayed stuck around half the source GIF's natural frame rate even
// after moving the decode buffer to internal SRAM and switching to a
// bulk memcpy_P instead of per-byte flash reads. This build removes
// the flash-decode step entirely to isolate where the remaining time
// actually goes: a PC-side Python server
// (gif_stream_server.py, session scratchpad) pre-decodes every frame
// to raw RGB565 once and streams the fixed-size frames over WiFi as
// a continuous byte stream (no protocol framing -- frame size is
// constant and known, so the client just reads FRAME_BYTES per
// iteration). The S3 does nothing but socket read + SPI push.
//
//   - If this runs at full/near-source speed: flash-decode (DIO mode
//     flash + palette lookup CPU cost) was the real bottleneck in
//     s3_gif_player.ino, confirming that diagnosis.
//   - If this is STILL slow: the bottleneck is actually the SPI push
//     / panel throughput itself, not decode -- a different problem.
//
// Same display wiring/pins as the other sketches. No flash-embedded
// frame data, so no PartitionScheme override needed -- default
// partition scheme is fine.
//
// WiFi credentials live in secrets.h (gitignored): copy
// secrets.example.h to secrets.h and fill in your network.

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <stdarg.h>

#include "secrets.h"  // WIFI_SSID / WIFI_PASS

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

#define FRAME_W 240
#define FRAME_H 291
#define FRAME_BYTES (FRAME_W * FRAME_H * 2)

const char *SERVER_IP = "192.168.1.11";
const uint16_t SERVER_PORT = 5000;

SPIClass tftSPI(FSPI);
Adafruit_ST7789 tft(&tftSPI, TFT_CS, TFT_DC, TFT_RST);
WiFiClient client;
WiFiUDP udpLog;
const uint16_t UDP_LOG_PORT = 5001;

static uint16_t frameBuf[FRAME_W * FRAME_H];

uint32_t frameCount = 0;
uint32_t fpsWindowStart = 0;
uint32_t readMsAccum = 0, pushMsAccum = 0;

void logf(const char *fmt, ...) {
  char buf[160];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  udpLog.beginPacket(SERVER_IP, UDP_LOG_PORT);
  udpLog.write((const uint8_t *)buf, strlen(buf));
  udpLog.endPacket();
  Serial.println(buf);
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("connecting to WiFi %s", WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.printf("\nconnected, IP: %s\n", WiFi.localIP().toString().c_str());
}

void connectServer() {
  Serial.printf("connecting to %s:%u...\n", SERVER_IP, SERVER_PORT);
  while (!client.connect(SERVER_IP, SERVER_PORT)) {
    Serial.println("connect failed, retrying...");
    delay(500);
  }
  client.setNoDelay(true);
  Serial.println("connected to frame server");
}

bool readFullFrame() {
  uint8_t *p = (uint8_t *)frameBuf;
  size_t got = 0;
  while (got < FRAME_BYTES) {
    if (!client.connected()) return false;
    int n = client.read(p + got, FRAME_BYTES - got);
    if (n > 0) {
      got += n;
    } else {
      delay(1);
    }
  }
  return true;
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

  tftSPI.begin(TFT_SCK, -1 /* no MISO */, TFT_MOSI, TFT_CS);
  tft.init(TFT_W, TFT_H);
  tft.setSPISpeed(40000000);
  tft.setRotation(0);
  tft.fillScreen(ST77XX_BLACK);

  connectWiFi();
  udpLog.begin(UDP_LOG_PORT);
  connectServer();

  fpsWindowStart = millis();
}

void loop() {
  if (!client.connected()) {
    logf("server disconnected, reconnecting...");
    client.stop();
    connectServer();
    return;
  }

  uint32_t t0 = millis();
  if (!readFullFrame()) return;
  uint32_t t1 = millis();

  int16_t yOffset = (TFT_H - FRAME_H) / 2;
  tft.startWrite();
  tft.setAddrWindow(0, yOffset, FRAME_W, FRAME_H);
  tft.writePixels(frameBuf, FRAME_W * FRAME_H);
  tft.endWrite();
  uint32_t t2 = millis();

  readMsAccum += (t1 - t0);
  pushMsAccum += (t2 - t1);
  frameCount++;
  uint32_t now = millis();
  if (now - fpsWindowStart >= 1000) {
    logf("FPS:%lu avg_read_ms:%lu avg_push_ms:%lu",
         (unsigned long)(frameCount * 1000 / (now - fpsWindowStart)),
         (unsigned long)(readMsAccum / frameCount),
         (unsigned long)(pushMsAccum / frameCount));
    frameCount = 0;
    readMsAccum = 0;
    pushMsAccum = 0;
    fpsWindowStart = now;
  }
}
