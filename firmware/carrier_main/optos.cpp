#include "optos.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_timer.h>

#include "config.h"

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

void optosInit() {
  for (int p : PIN_DOOR) optoPinInit(p);
  optoPinInit(PIN_MOBO_RST);
  if (MOBO_RST_HOLD_TEST) gpio_set_level((gpio_num_t)PIN_MOBO_RST, 1);

  esp_timer_create_args_t ta = {};
  ta.callback = doorRelease; ta.name = "door";
  esp_timer_create(&ta, &doorTimer);
  ta.callback = rstRelease; ta.name = "mbrst";
  esp_timer_create(&ta, &rstTimer);
}

const char *doorPress(int n) {
  if (n < 1 || n > 4) return "button must be 1-4";
  if (doorActive >= 0) return "another button is being pressed";
  if (esp_timer_get_time() < doorFreeAtUs) return "too soon after the last press (500 ms gap)";
  doorActive = n - 1;
  gpio_set_level((gpio_num_t)PIN_DOOR[n - 1], 1);
  esp_timer_start_once(doorTimer, DOOR_PRESS_MS * 1000ULL);
  return nullptr;
}

const char *moboReset() {
  if (MOBO_RST_HOLD_TEST) return "RST is held HIGH (MOBO_RST_HOLD_TEST build)";
  if (rstActive) return "reset pulse already running";
  rstActive = true;
  gpio_set_level((gpio_num_t)PIN_MOBO_RST, 1);
  esp_timer_start_once(rstTimer, MOBO_RST_MS * 1000ULL);
  return nullptr;
}

int doorPressed() { return doorActive + 1; }

bool moboResetActive() { return rstActive; }
