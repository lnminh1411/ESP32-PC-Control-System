#include "pc_state.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>
#include <nvs.h>
#include <stdint.h>

#include "config.h"
#include "display.h"
#include "log.h"
#include "pc_going.h"
#include "pcstats.h"
#include "views.h"
#include "sys.h"
#include "usb.h"
#include "settings_or_example.h"

static volatile PcState state = PC_UNKNOWN;
static volatile uint32_t guardStartMs;
static volatile bool guardActive;
static volatile LedMode mode = LED_AUTO;
static volatile uint8_t ledPct = 100;
static volatile uint8_t ledCapPct = 100;   // night mode
static volatile uint32_t readyMs;   // 0 = still booting
static volatile int simState = -1;  // `pcsim`: -1 = follow USB

PcState pcState() { return simState >= 0 ? (PcState)simState : state; }

void pcStateSimulate(int s) {
  simState = s;
  viewsRefreshIdle();
}

const char *pcStateName(PcState s) {
  return s == PC_ON ? "on" : s == PC_OFF ? "asleep/off" : "unknown (no USB host yet)";
}

uint32_t bootGuardLeftMs() {
  uint32_t t = nowMs() - guardStartMs;
  return guardActive && t < USB_GUARD_MS ? USB_GUARD_MS - t : 0;
}

static void pcSet(PcState s) {
  state = s;
  viewsRefreshIdle();
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
    float lvl = (ledPct < ledCapPct ? ledPct : ledCapPct) / 100.0f;
    if (mode == LED_SWEEP) {                              // test ramp: 2 s down, 1 s up
      uint32_t p = t % 3000;
      pwmWrite(PWM_CH_LED, ledDuty(lvl * (p < 2000 ? 1.0f - p / 2000.0f : (p - 2000) / 1000.0f)));
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    GoingKind going = pcGoing();
    PcState st = pcState();
    bool restartish = going == GO_RESTART || going == GO_UPDATE_RESTART || going == GO_UPDATE_SHUTDOWN;
    bool loading = !readyMs || (st == PC_ON && !pcStatsFresh()) || (st == PC_UNKNOWN && t - readyMs < 5000);
    float level;   // 0..1 before brightness
    if (mode != LED_AUTO) level = (mode == LED_ON) ? 1.0f : 0.0f;
    else if (restartish) level = (t % LED_RESTART_BLINK_MS) < LED_RESTART_BLINK_MS / 2 ? 1.0f : 0.0f;
    else if (loading) level = (t % LED_LOADING_BLINK_MS) < LED_LOADING_BLINK_MS / 2 ? 1.0f : 0.0f;
    else if (st == PC_ON) level = 1.0f;
    else level = 0.5f - 0.5f * cosf((t % LED_BREATHE_MS) * (2.0f * (float)M_PI / LED_BREATHE_MS));   // breathing
    pwmWrite(PWM_CH_LED, ledDuty(lvl * level));
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void statusLedInit() {
  pwmAttach(PIN_LED, PWM_CH_LED);
  pwmWrite(PWM_CH_LED, 0);
  xTaskCreatePinnedToCore(ledTask, "led", 2048, nullptr, 1, nullptr, 0);
}

void statusLedReady() { readyMs = nowMs() ? nowMs() : 1; }

void ledModeSet(LedMode m) { mode = m; }

LedMode ledMode() { return mode; }

void ledBrightnessSet(uint8_t pct) { ledPct = pct > 100 ? 100 : pct; }

uint8_t ledBrightness() { return ledPct; }

void ledSettingsLoad() {
  nvs_handle_t h;
  if (nvs_open("led", NVS_READONLY, &h) != ESP_OK) return;
  uint8_t v;
  if (nvs_get_u8(h, "pct", &v) == ESP_OK) ledBrightnessSet(v);
  if (nvs_get_u8(h, "off", &v) == ESP_OK && v) mode = LED_OFF;
  nvs_close(h);
}

void ledSettingsSave() {
  nvs_handle_t h;
  if (nvs_open("led", NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_u8(h, "pct", ledPct);
  nvs_set_u8(h, "off", mode == LED_OFF ? 1 : 0);
  nvs_commit(h);
  nvs_close(h);
}

void ledCapSet(uint8_t pct) { ledCapPct = pct > 100 ? 100 : pct; }
