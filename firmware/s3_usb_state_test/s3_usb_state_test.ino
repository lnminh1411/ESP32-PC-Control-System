// ============================================================
// S3 USB PHY state probe — test whether Active/Suspended events
// from the native USB-OTG (TinyUSB) peripheral can be used to
// guess the host PC's power state (on / asleep / off).
// ============================================================
//
// WHY THIS EXISTS
// PC's front-panel Power LED header and ATX PWR_OK are the reliable
// way to sense PC power state, but both need a new physical tap not
// yet on the board. Testing whether the S3's own USB link state
// already tells us enough before adding hardware for that.
//
// RESULT SO FAR (2026-09-30 bench test): confirmed usable —
//   USB Active   -> PC ON
//   USB Suspend  -> PC asleep/off
//   STOPPED never actually observed in practice; not used for any
//   decision, only logged for diagnostics.
//
// PROBLEM FOUND: cold boot causes a brief USB on/off blip (BIOS
// POST re-enumerating the controller) before Windows is actually
// up, which looks identical to a real suspend if taken at face
// value. FIX: a 30s guard window, armed every time the link goes
// ACTIVE (fresh boot or resume). Suspend events inside the guard
// window are logged but ignored — state stays ACTIVE. Once armed
// (30s past the last ACTIVE transition), a suspend is honored
// immediately, no further debounce:
//
//   PC turns on:  USB Active -> monitoring screens on now,
//                 guard starts (30s) -> after 30s, suspend is
//                 trusted again.
//   PC turns off: USB Active, guard already elapsed -> Suspend
//                 honored immediately -> screens go idle
//                 (weather/etc. placeholder on this bench rig,
//                 real product target is 3 LCDs).
//
// REQUIRED BOARD CONFIG — this will NOT compile/behave right with
// the same options used for s3_display_fps_test:
//   USB Mode:      "USB-OTG (TinyUSB)"   (Arduino IDE: Tools > USB Mode)
//   USB CDC On Boot: "Enabled"           (Tools > USB CDC On Boot)
// arduino-cli equivalent (board options on the FQBN):
//   --fqbn esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc
// Native "Hardware CDC and JTAG" mode (the default, used by the FPS
// test sketch) has no TinyUSB device stack underneath it and never
// fires these events.
//
// Wiring: same display, same pins as s3_display_fps_test.ino. Kept
// so the display keeps logging even while the USB link itself is
// suspended and Serial can't be read.

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include "USB.h"

#define TFT_CS   3
#define TFT_DC   4
#define TFT_RST  5
#define TFT_MOSI 6
#define TFT_SCK  7

#define TFT_W 240
#define TFT_H 320

SPIClass tftSPI(FSPI);
Adafruit_ST7789 tft(&tftSPI, TFT_CS, TFT_DC, TFT_RST);

// ---- guarded state machine ----
const uint32_t GUARD_MS = 30000;

enum PowerGuess { GUESS_UNKNOWN, GUESS_ACTIVE, GUESS_SUSPENDED };
enum ScreenMode { MODE_BOOT, MODE_MONITOR, MODE_IDLE };

volatile PowerGuess currentGuess = GUESS_UNKNOWN;
volatile ScreenMode screenMode = MODE_BOOT;
volatile bool guardActive = false;
volatile uint32_t guardStartMs = 0;
volatile bool stateDirty = true;

#define LOG_LINES 12
struct LogEntry {
  uint32_t t;
  char msg[30];
  uint16_t color;
};
LogEntry logBuf[LOG_LINES];
uint8_t logCount = 0;
volatile bool logDirty = true;

void fmtTime(uint32_t ms, char *out) {
  uint32_t s = ms / 1000;
  uint32_t m = s / 60;
  s %= 60;
  uint32_t frac = ms % 1000;
  snprintf(out, 12, "%02lu:%02lu.%03lu", (unsigned long)m, (unsigned long)s, (unsigned long)frac);
}

void addLog(const char *label, uint16_t color) {
  if (logCount == LOG_LINES) {
    memmove(&logBuf[0], &logBuf[1], sizeof(LogEntry) * (LOG_LINES - 1));
    logCount--;
  }
  LogEntry &e = logBuf[logCount++];
  e.t = millis();
  char ts[12];
  fmtTime(e.t, ts);
  snprintf(e.msg, sizeof(e.msg), "%s %s", ts, label);
  e.color = color;
  logDirty = true;
  Serial.printf("[%s] %s\n", ts, label);
}

// Entering ACTIVE (fresh boot or resume): screens on immediately,
// (re)arm the 30s guard so the boot-time USB blip can't be
// mistaken for a real suspend.
void enterActive(const char *why) {
  currentGuess = GUESS_ACTIVE;
  screenMode = MODE_MONITOR;
  guardActive = true;
  guardStartMs = millis();
  char buf[30];
  snprintf(buf, sizeof(buf), "%s -> ACTIVE, guard 30s", why);
  addLog(buf, ST77XX_GREEN);
  stateDirty = true;
}

void usbEventCallback(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_base != ARDUINO_USB_EVENTS) return;
  switch (event_id) {
    case ARDUINO_USB_STARTED_EVENT:
      enterActive("STARTED");
      break;
    case ARDUINO_USB_RESUME_EVENT:
      enterActive("RESUME");
      break;
    case ARDUINO_USB_STOPPED_EVENT:
      // Never actually observed on the bench; logged only, not used
      // for any state decision.
      addLog("STOPPED (ignored, info only)", ST77XX_ORANGE);
      break;
    case ARDUINO_USB_SUSPEND_EVENT:
      if (guardActive) {
        uint32_t elapsed = millis() - guardStartMs;
        uint32_t leftS = (elapsed >= GUARD_MS) ? 0 : (GUARD_MS - elapsed) / 1000;
        char buf[30];
        snprintf(buf, sizeof(buf), "SUSPEND ignored (guard %lus)", (unsigned long)leftS);
        addLog(buf, ST77XX_ORANGE);
        // state/screen unchanged — still counted as ACTIVE
      } else {
        currentGuess = GUESS_SUSPENDED;
        screenMode = MODE_IDLE;
        addLog("SUSPEND -> IDLE mode", ST77XX_YELLOW);
        stateDirty = true;
      }
      break;
    default:
      break;
  }
}

const char *guessLabel(PowerGuess g) {
  switch (g) {
    case GUESS_ACTIVE:    return "PC: ON";
    case GUESS_SUSPENDED: return "PC: ASLEEP/OFF";
    default:              return "waiting for first USB event...";
  }
}

uint16_t guessColor(PowerGuess g) {
  switch (g) {
    case GUESS_ACTIVE:    return ST77XX_GREEN;
    case GUESS_SUSPENDED: return ST77XX_YELLOW;
    default:              return ST77XX_WHITE;
  }
}

void drawBanner() {
  uint16_t bg = (screenMode == MODE_IDLE) ? tft.color565(5, 10, 25) : tft.color565(15, 15, 15);
  tft.fillRect(0, 0, tft.width(), 50, bg);
  tft.setTextSize(2);
  tft.setTextColor(guessColor(currentGuess), bg);
  tft.setCursor(6, 6);
  tft.print(guessLabel(currentGuess));

  tft.setTextSize(1);
  tft.setTextColor(ST77XX_CYAN, bg);
  tft.setCursor(6, 30);
  if (screenMode == MODE_IDLE) {
    tft.print("MODE: IDLE (weather placeholder, 3 LCD target)");
  } else if (guardActive) {
    uint32_t elapsed = millis() - guardStartMs;
    uint32_t leftS = (elapsed >= GUARD_MS) ? 0 : (GUARD_MS - elapsed) / 1000;
    tft.print("MODE: MONITOR, boot guard ");
    tft.print(leftS);
    tft.print("s left");
  } else {
    tft.print("MODE: MONITOR, suspend armed");
  }
}

void drawLog() {
  int y0 = 54;
  int lineH = 14;
  tft.fillRect(0, y0, tft.width(), tft.height() - y0, ST77XX_BLACK);
  tft.setTextSize(1);
  for (uint8_t i = 0; i < logCount; i++) {
    tft.setTextColor(logBuf[i].color, ST77XX_BLACK);
    tft.setCursor(4, y0 + i * lineH);
    tft.print(logBuf[i].msg);
  }
}

void setup() {
  Serial.begin(115200);

  tftSPI.begin(TFT_SCK, -1 /* no MISO */, TFT_MOSI, TFT_CS);
  tft.init(TFT_W, TFT_H);
  tft.setSPISpeed(40000000);
  tft.setRotation(1);
  tft.fillScreen(ST77XX_BLACK);

  USB.onEvent(usbEventCallback);
  USB.begin();

  addLog("BOOT", ST77XX_WHITE);
  drawBanner();
}

void loop() {
  static uint32_t lastHeartbeat = 0;
  static uint32_t lastGuardRedraw = 0;
  static bool guardWasActive = false;
  uint32_t now = millis();

  // Clear the guard once 30s have passed since the last ACTIVE
  // transition — suspend events are trusted (and acted on
  // immediately, no further debounce) from this point on.
  if (guardActive && (now - guardStartMs >= GUARD_MS)) {
    guardActive = false;
    addLog("guard cleared, suspend armed", ST77XX_CYAN);
    stateDirty = true;
  }

  // Redraw the banner every second while the guard countdown is
  // live, so the on-screen "Ns left" stays current.
  if (guardActive && now - lastGuardRedraw >= 1000) {
    lastGuardRedraw = now;
    stateDirty = true;
  }
  bool nowGuardActive = guardActive;
  if (nowGuardActive != guardWasActive) {
    guardWasActive = nowGuardActive;
    stateDirty = true;
  }

  if (now - lastHeartbeat >= 5000) {
    lastHeartbeat = now;
    char hb[30];
    snprintf(hb, sizeof(hb), "heartbeat, heap %lu", (unsigned long)ESP.getFreeHeap());
    addLog(hb, tft.color565(80, 80, 80));
  }

  if (stateDirty) {
    stateDirty = false;
    drawBanner();
  }
  if (logDirty) {
    logDirty = false;
    drawLog();
  }

  delay(20);
}
