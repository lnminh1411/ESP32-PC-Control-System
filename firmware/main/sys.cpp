#include "sys.h"

#include <driver/temperature_sensor.h>
#include <esp_heap_caps.h>
#include <math.h>

uint32_t freePsram() { return heap_caps_get_free_size(MALLOC_CAP_SPIRAM); }
uint32_t freeHeap() { return heap_caps_get_free_size(MALLOC_CAP_DEFAULT); }
uint32_t freeInternal() { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL); }

float chipTempC() {
  static temperature_sensor_handle_t ts;
  static bool tried;
  if (!tried) {
    tried = true;
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&cfg, &ts) != ESP_OK || temperature_sensor_enable(ts) != ESP_OK) ts = nullptr;
  }
  float c;
  return ts && temperature_sensor_get_celsius(ts, &c) == ESP_OK ? c : NAN;
}

void pwmAttach(int pin, ledc_channel_t ch) {
  static bool timerUp;
  if (!timerUp) {
    ledc_timer_config_t t = {};
    t.speed_mode = LEDC_LOW_SPEED_MODE;
    t.duty_resolution = (ledc_timer_bit_t)PWM_BITS;
    t.timer_num = LEDC_TIMER_0;
    t.freq_hz = PWM_HZ;
    t.clk_cfg = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&t));
    timerUp = true;
  }
  ledc_channel_config_t c = {};
  c.gpio_num = pin;
  c.speed_mode = LEDC_LOW_SPEED_MODE;
  c.channel = ch;
  c.timer_sel = LEDC_TIMER_0;
  c.duty = 0;
  ESP_ERROR_CHECK(ledc_channel_config(&c));
}

void pwmWrite(ledc_channel_t ch, uint32_t duty) {
  ledc_set_duty(LEDC_LOW_SPEED_MODE, ch, duty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, ch);
}
