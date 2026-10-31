#include "sys.h"

#include "log.h"
#include "pc_state.h"

#include <driver/temperature_sensor.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <sdkconfig.h>
#include <soc/soc.h>
#include <soc/spi_mem_reg.h>

uint32_t psramEccTotal;

// PSRAM ECC (sdkconfig SPIRAM_ECC_ENABLE) corrects single-bit read errors;
// the MSPI counts them once SMEM_ECC_ERR_INT_EN is set. Every 10 s: any
// corrections go to the log with the PC state, then the count is cleared.
// Tells whether PSRAM bits still flip, and when (PC power-on, PC load).
void psramEccPoll() {
#if CONFIG_SPIRAM_ECC_ENABLE
  static bool counting;
  static uint32_t lastMs;
  if (!counting) {
    SET_PERI_REG_MASK(SPI_MEM_SPI_SMEM_AC_REG(0), SPI_MEM_SPI_SMEM_ECC_ERR_INT_EN);
    WRITE_PERI_REG(SPI_MEM_INT_CLR_REG(0), SPI_MEM_ECC_ERR_INT_CLR);
    counting = true;
    lastMs = nowMs();
    return;
  }
  if (nowMs() - lastMs < 10000) return;
  lastMs = nowMs();
  uint32_t bits = READ_PERI_REG(SPI_MEM_ECC_ERR_BIT_REG(0));
  uint32_t n = (bits >> SPI_MEM_ECC_ERR_CNT_S) & SPI_MEM_ECC_ERR_CNT_V;
  if (!n) return;
  uint32_t addr = READ_PERI_REG(SPI_MEM_ECC_ERR_ADDR_REG(0));
  WRITE_PERI_REG(SPI_MEM_INT_CLR_REG(0), SPI_MEM_ECC_ERR_INT_CLR);
  psramEccTotal += n;
  logMsg("psram: ECC corrected %lu read error(s) in 10 s%s, first at PSRAM 0x%06lx %s bit %lu (PC %s, %lu since boot)",
         (unsigned long)n, n >= 255 ? " (count saturated)" : "", (unsigned long)addr,
         bits & SPI_MEM_ECC_BYTE_ERR ? "check byte" : "data",
         (unsigned long)(bits & SPI_MEM_ECC_BYTE_ERR ? (bits >> SPI_MEM_ECC_CHK_ERR_BIT_S) & SPI_MEM_ECC_CHK_ERR_BIT_V
                                                       : (bits >> SPI_MEM_ECC_DATA_ERR_BIT_S) & SPI_MEM_ECC_DATA_ERR_BIT_V),
         pcStateName(pcState()), (unsigned long)psramEccTotal);
#endif
}

uint32_t freePsram() { return heap_caps_get_free_size(MALLOC_CAP_SPIRAM); }
uint32_t freeHeap() { return heap_caps_get_free_size(MALLOC_CAP_DEFAULT); }
uint32_t freeInternal() { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL); }

void *psramCalloc(size_t size) {
  void *p = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : calloc(1, size);
}

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
