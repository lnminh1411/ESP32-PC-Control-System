#include "pc_state.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>
#include <stdint.h>

#include "config.h"
#include "display.h"
#include "log.h"
#include "pc_going.h"
#include "sys.h"
#include "usb.h"

static volatile PcState state = PC_UNKNOWN;
static volatile uint32_t guardStartMs;
static volatile bool guardActive;
static volatile LedMode mode = LED_AUTO;
static volatile uint8_t ledPct = 100;

PcState pcState() { return state; }

const char *pcStateName(PcState s) {
  return s == PC_ON ? "on" : s == PC_OFF ? "asleep/off" : "unknown (no USB host yet)";
}

uint32_t bootGuardLeftMs() {
  uint32_t t = nowMs() - guardStartMs;
  return guardActive && t < USB_GUARD_MS ? USB_GUARD_MS - t : 0;
}

static void pcSet(PcState s) {
  state = s;
  backlightIdle(s == PC_OFF && !pcGoingHoldsScreens());
}

static void usbEventHandler(UsbEvent ev) {
  switch (ev) {
    case USB_EV_STARTED:
    case USB_EV_RESUME:
      guardStartMs = nowMs();
      guardActive = true;
      pcSet(PC_ON);
      logMsg("usb: %s -> PC on (30 s guard armed)", ev == USB_EV_STARTED ? "started" : "resume");
      break;
    case USB_EV_SUSPEND:
      if (bootGuardLeftMs()) {
        logMsg("usb: suspend inside guard (%lu ms) -> ignored", (unsigned long)(nowMs() - guardStartMs));
      } else {
        pcSet(PC_OFF);
        logMsg("usb: suspend -> PC asleep/off, screens idle");
      }
      break;
    case USB_EV_STOPPED:
      logMsg("usb: stopped (logged only)");
      break;
  }
}

void pcStateInit() { usbOnEvent(usbEventHandler); }

// perceived level 0..1 -> duty: gamma curve, capped at LED_DUTY_CEIL
static uint32_t ledDuty(float level) {
  return (uint32_t)(PWM_MAX * LED_DUTY_CEIL * powf(level, LED_GAMMA) + 0.5f);
}

static void ledTask(void *) {
  for (;;) {
    uint32_t t = nowMs();
    bool on;
    float lvl = ledPct / 100.0f;
    if (mode == LED_SWEEP) {                              // test ramp: 2 s down, 1 s up
      uint32_t p = t % 3000;
      pwmWrite(PWM_CH_LED, ledDuty(lvl * (p < 2000 ? 1.0f - p / 2000.0f : (p - 2000) / 1000.0f)));
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    if (mode != LED_AUTO) on = (mode == LED_ON);
    else if (state == PC_ON) on = true;
    else if (state == PC_OFF) on = (t % 2000) < 1000;     // slow blink
    else on = (t % 2000) < 80;                            // short blip
    pwmWrite(PWM_CH_LED, on ? ledDuty(lvl) : 0);
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void statusLedInit() {
  pwmAttach(PIN_LED, PWM_CH_LED);
  pwmWrite(PWM_CH_LED, 0);
  xTaskCreatePinnedToCore(ledTask, "led", 2048, nullptr, 1, nullptr, 0);
}

void ledModeSet(LedMode m) { mode = m; }

LedMode ledMode() { return mode; }

void ledBrightnessSet(uint8_t pct) { ledPct = pct > 100 ? 100 : pct; }

uint8_t ledBrightness() { return ledPct; }
