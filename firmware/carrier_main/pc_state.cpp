#include "pc_state.h"

#include <Arduino.h>
#include "USB.h"

#include "config.h"
#include "display.h"
#include "log.h"

static volatile PcState state = PC_UNKNOWN;
static volatile uint32_t guardStartMs;
static volatile bool guardActive;
static volatile LedMode mode = LED_AUTO;

PcState pcState() { return state; }

const char *pcStateName(PcState s) {
  return s == PC_ON ? "on" : s == PC_OFF ? "asleep/off" : "unknown (no USB host yet)";
}

uint32_t bootGuardLeftMs() {
  uint32_t t = millis() - guardStartMs;
  return guardActive && t < USB_GUARD_MS ? USB_GUARD_MS - t : 0;
}

static void pcSet(PcState s) {
  state = s;
  backlightIdle(s == PC_OFF);
}

static void usbEvent(void *, esp_event_base_t base, int32_t id, void *) {
  if (base != ARDUINO_USB_EVENTS) return;
  switch (id) {
    case ARDUINO_USB_STARTED_EVENT:
    case ARDUINO_USB_RESUME_EVENT:
      guardStartMs = millis();
      guardActive = true;
      pcSet(PC_ON);
      logMsg("usb: %s -> PC on (30 s guard armed)", id == ARDUINO_USB_STARTED_EVENT ? "started" : "resume");
      break;
    case ARDUINO_USB_SUSPEND_EVENT:
      if (bootGuardLeftMs()) {
        logMsg("usb: suspend inside guard (%lu ms) -> ignored", (unsigned long)(millis() - guardStartMs));
      } else {
        pcSet(PC_OFF);
        logMsg("usb: suspend -> PC asleep/off, screens idle");
      }
      break;
    case ARDUINO_USB_STOPPED_EVENT:
      logMsg("usb: stopped (logged only)");
      break;
  }
}

void pcStateInit() { USB.onEvent(usbEvent); }

static void ledTask(void *) {
  for (;;) {
    uint32_t t = millis();
    bool on;
    if (mode != LED_AUTO) on = (mode == LED_ON);
    else if (state == PC_ON) on = true;
    else if (state == PC_OFF) on = (t % 2000) < 1000;     // slow blink
    else on = (t % 2000) < 80;                            // short blip
    digitalWrite(PIN_LED, on);
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void statusLedInit() {
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);
  xTaskCreatePinnedToCore(ledTask, "led", 2048, nullptr, 1, nullptr, 0);
}

void ledModeSet(LedMode m) { mode = m; }

LedMode ledMode() { return mode; }
