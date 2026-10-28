// Small platform helpers on ESP-IDF: time, memory, chip temperature, and
// the LEDC plan shared by the backlight and status LED drivers.
#pragma once
#include <driver/ledc.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdint.h>

static inline uint32_t nowMs() { return (uint32_t)(esp_timer_get_time() / 1000); }
static inline uint32_t nowUs() { return (uint32_t)esp_timer_get_time(); }
static inline void sleepMs(uint32_t ms) { vTaskDelay(ms ? pdMS_TO_TICKS(ms) : 1); }

uint32_t freePsram();
uint32_t freeHeap();       // all heaps (internal + PSRAM)
uint32_t freeInternal();
float chipTempC();         // internal temperature sensor, NAN if unavailable
void psramEccPoll();       // main loop: log PSRAM ECC corrections (see sys.cpp)
extern uint32_t psramEccTotal;   // corrections since boot
const char *resetReasonText();   // why the board last restarted ("crash", "power-on", ...)

// LEDC: one low-speed timer at 20 kHz / 10 bit for every PWM output.
//   channels 0-2 = panel backlights, channel 3 = status LED
#define PWM_HZ   20000
#define PWM_BITS 10
#define PWM_MAX  ((1u << PWM_BITS) - 1)
#define PWM_CH_BL(i) ((ledc_channel_t)(LEDC_CHANNEL_0 + (i)))
#define PWM_CH_LED   LEDC_CHANNEL_3
void pwmAttach(int pin, ledc_channel_t ch);   // starts at duty 0; timer set up on first call
void pwmWrite(ledc_channel_t ch, uint32_t duty);
