#include "usb.h"

#include <esp_check.h>
#include <esp_err.h>
#include <esp_mac.h>
#include <esp_rom_gpio.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>
#include <stdio.h>
#include <string.h>

#include "class/hid/hid.h"
#include "class/hid/hid_device.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"

#include <driver/gpio.h>
#include <esp_intr_alloc.h>
#include <freertos/semphr.h>
#include <initializer_list>

#include "hal/clk_gate_ll.h"
#include "hal/usb_serial_jtag_ll.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"
#include "soc/usb_serial_jtag_reg.h"

#define TUD_HID_REPORT_DESC_GENERIC_INOUT_FEATURE(report_size, ...) \
  HID_USAGE_PAGE_N(HID_USAGE_PAGE_VENDOR, 2), HID_USAGE(0x01), HID_COLLECTION(HID_COLLECTION_APPLICATION), \
    __VA_ARGS__ \
    HID_USAGE(0x02), \
    HID_LOGICAL_MIN(0x00), HID_LOGICAL_MAX(0xff), HID_REPORT_SIZE(8), HID_REPORT_COUNT(report_size), \
    HID_INPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE), \
    HID_USAGE(0x03), HID_LOGICAL_MIN(0x00), HID_LOGICAL_MAX(0xff), HID_REPORT_SIZE(8), HID_REPORT_COUNT(report_size), \
    HID_OUTPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE), \
    HID_USAGE(0x04), HID_LOGICAL_MIN(0x00), HID_LOGICAL_MAX(0xff), HID_REPORT_SIZE(8), HID_REPORT_COUNT(report_size), \
    HID_FEATURE(HID_DATA | HID_VARIABLE | HID_ABSOLUTE), HID_COLLECTION_END

static const uint8_t s_hid_report_descriptor[] = {
  TUD_HID_REPORT_DESC_GENERIC_INOUT_FEATURE(63, HID_REPORT_ID(6))
};

enum {
  ITF_NUM_HID = 0,
  ITF_NUM_CDC,
  ITF_NUM_CDC_DATA,
  ITF_NUM_TOTAL
};

enum {
  EPNUM_HID = 1,
  EPNUM_CDC_NOTIF = 2,
  EPNUM_CDC_DATA = 3,
};

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN + TUD_CDC_DESC_LEN)

static const tusb_desc_device_t s_desc_device = {
  .bLength = sizeof(tusb_desc_device_t),
  .bDescriptorType = TUSB_DESC_DEVICE,
  .bcdUSB = 0x0200,
  .bDeviceClass = TUSB_CLASS_MISC,
  .bDeviceSubClass = MISC_SUBCLASS_COMMON,
  .bDeviceProtocol = MISC_PROTOCOL_IAD,
  .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
  .idVendor = 0x303A,
  .idProduct = 0x1001,
  .bcdDevice = 0x0100,
  .iManufacturer = 1,
  .iProduct = 2,
  .iSerialNumber = 3,
  .bNumConfigurations = 1,
};

static const uint8_t s_desc_cfg[] = {
  TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0, 500),

  // Interface 0: HID IN+OUT
  TUD_HID_INOUT_DESCRIPTOR(ITF_NUM_HID, 4, HID_ITF_PROTOCOL_NONE, sizeof(s_hid_report_descriptor),
                           EPNUM_HID, (uint8_t)(0x80 | EPNUM_HID), 64, 1),

  // Interfaces 1-2: CDC
  TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 5, (uint8_t)(0x80 | EPNUM_CDC_NOTIF), 64,
                     EPNUM_CDC_DATA, (uint8_t)(0x80 | EPNUM_CDC_DATA), 64),
};

static char s_serial_str[13];

static const char *s_desc_str[] = {
  (const char[]){0x09, 0x04}, // 0: Supported language: English (0x0409)
  "Espressif Systems",        // 1: Manufacturer
  "ESP32 PC Controller",     // 2: Product
  s_serial_str,               // 3: Device serial string
  "ESP32 HID",                // 4: HID Interface
  "ESP32 CDC",                // 5: CDC Interface
};

static StreamBufferHandle_t s_hid_stream = nullptr;
static StreamBufferHandle_t s_cdc_stream = nullptr;

static void (*s_event_cb)(UsbEvent) = nullptr;

static volatile bool s_cdc_connected = false;
static volatile bool s_dtr = false;
static volatile bool s_rts = false;
static volatile uint8_t s_lineState = 0;
static SemaphoreHandle_t s_cdc_tx_lock;   // logMsg writes from several tasks

static void IRAM_ATTR busResetIsr(void *arg) {
  BaseType_t woken = pdFALSE;
  uint32_t st = usb_serial_jtag_ll_get_intsts_mask();
  usb_serial_jtag_ll_clr_intsts_mask(st);
  if (st & USB_SERIAL_JTAG_INTR_BUS_RESET) xSemaphoreGiveFromISR((SemaphoreHandle_t)arg, &woken);
  if (woken) portYIELD_FROM_ISR();
}

// Hand the USB pads back to the USB-Serial-JTAG peripheral before rebooting
// into the ROM downloader: the PHY select lives in the RTC domain and
// survives esp_restart(), and the ROM only downloads over USB-Serial-JTAG.
// Port of usb_switch_to_cdc_jtag() in Arduino's esp32-hal-tinyusb.c.
static void switchToUsbSerialJtag() {
  periph_ll_reset(PERIPH_USB_MODULE);               // stop USB-OTG
  periph_ll_disable_clk_set_rst(PERIPH_USB_MODULE);
  CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG, RTC_CNTL_SW_HW_USB_PHY_SEL | RTC_CNTL_SW_USB_PHY_SEL | RTC_CNTL_USB_PAD_ENABLE);
  CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_PHY_SEL);         // internal PHY
  CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);  // pads off
  // D- (GPIO19) and D+ (GPIO20) low: the host sees a disconnect and re-enumerates
  for (gpio_num_t pin : {GPIO_NUM_19, GPIO_NUM_20}) {
    gpio_set_direction(pin, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(pin, 0);
  }
  const usb_serial_jtag_pull_override_vals_t pulls = {.dp_pu = 1, .dm_pu = 0, .dp_pd = 0, .dm_pd = 0};
  usb_serial_jtag_ll_phy_enable_pull_override(&pulls);
  usb_serial_jtag_ll_phy_disable_pull_override();
  usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_LL_INTR_MASK);
  usb_serial_jtag_ll_clr_intsts_mask(USB_SERIAL_JTAG_LL_INTR_MASK);
  usb_serial_jtag_ll_ena_intr_mask(USB_SERIAL_JTAG_INTR_BUS_RESET);
  intr_handle_t intr = nullptr;
  SemaphoreHandle_t resetSeen = xSemaphoreCreateBinary();
  if (resetSeen && esp_intr_alloc(ETS_USB_SERIAL_JTAG_INTR_SOURCE, 0, busResetIsr, resetSeen, &intr) != ESP_OK) {
    vSemaphoreDelete(resetSeen);
    resetSeen = nullptr;
  }
  SET_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);    // pads to USB-Serial-JTAG
  if (resetSeen) {                                  // host's bus reset = it saw the new device
    xSemaphoreTake(resetSeen, pdMS_TO_TICKS(1000));
    usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_LL_INTR_MASK);
    esp_intr_free(intr);
    vSemaphoreDelete(resetSeen);
  }
}

static void usbResetToBootloader() {
  switchToUsbSerialJtag();
  REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
  esp_restart();
}

extern "C" {

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
  (void)instance;
  return s_hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen) {
  (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
  return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize) {
  (void)instance;
  const uint8_t *data = buffer;
  uint16_t len = bufsize;
  if (!report_id && (!report_type || report_type == HID_REPORT_TYPE_OUTPUT)) {
    if (bufsize == 0) return;
    report_id = buffer[0];
    data = buffer + 1;
    len = bufsize - 1;
  }
  if (report_id != 6) return;
  if (!s_hid_stream) return;
  for (uint16_t i = 0; i < len; i++) {
    if (data[i] != 0) {
      xStreamBufferSend(s_hid_stream, &data[i], 1, 0);
    }
  }
}

} // extern "C"

static void on_cdc_rx(int itf, cdcacm_event_t *event) {
  (void)event;
  uint8_t buf[64];
  size_t rx_len = 0;
  while (tinyusb_cdcacm_read((tinyusb_cdcacm_itf_t)itf, buf, sizeof(buf), &rx_len) == ESP_OK && rx_len > 0) {
    if (s_cdc_stream) {
      xStreamBufferSend(s_cdc_stream, buf, rx_len, 0);
    }
  }
}

static void on_cdc_line_state(int itf, cdcacm_event_t *event) {
  (void)itf;
  bool dtr = event->line_state_changed_data.dtr;
  bool rts = event->line_state_changed_data.rts;
  if (s_dtr == dtr && s_rts == rts) return;
  s_dtr = dtr;
  s_rts = rts;

  // DTR/RTS bootloader state machine ported from USBCDC.cpp
  if (!dtr && rts) {
    if (s_lineState == 0) {
      s_lineState = s_lineState + 1;
      s_cdc_connected = false;
    } else {
      s_lineState = 0;
    }
  } else if (dtr && rts) {
    if (s_lineState == 1) {
      s_lineState = s_lineState + 1;
    } else {
      s_lineState = 0;
    }
  } else if (dtr && !rts) {
    if (s_lineState == 2) {
      s_lineState = s_lineState + 1;
    } else {
      s_lineState = 0;
    }
  } else if (!dtr && !rts) {
    if (s_lineState == 3) {
      usbResetToBootloader();
    } else {
      s_lineState = 0;
    }
  }

  if (s_lineState == 0) {
    if (dtr && rts && !s_cdc_connected) {
      s_cdc_connected = true;
    } else if (!dtr && s_cdc_connected) {
      s_cdc_connected = false;
    }
  }
}

static void on_cdc_line_coding(int itf, cdcacm_event_t *event) {
  (void)itf;
  if (event->line_coding_changed_data.p_line_coding->bit_rate == 1200) {
    usbResetToBootloader();
  }
}

static void on_tinyusb_event(tinyusb_event_t *event, void *arg) {
  (void)arg;
  if (!event) return;
  UsbEvent ev;
  switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
      ev = USB_EV_STARTED;
      break;
    case TINYUSB_EVENT_RESUMED:
      ev = USB_EV_RESUME;
      break;
    case TINYUSB_EVENT_SUSPENDED:
      ev = USB_EV_SUSPEND;
      break;
    case TINYUSB_EVENT_DETACHED:
      ev = USB_EV_STOPPED;
      s_cdc_connected = false;
      s_dtr = false;
      s_rts = false;
      s_lineState = 0;
      break;
    default:
      return;
  }
  if (s_event_cb) {
    s_event_cb(ev);
  }
}

void usbOnEvent(void (*cb)(UsbEvent)) {
  s_event_cb = cb;
}

void usbInit() {
  uint8_t mac[6];
  if (esp_efuse_mac_get_default(mac) == ESP_OK) {
    snprintf(s_serial_str, sizeof(s_serial_str), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  } else {
    snprintf(s_serial_str, sizeof(s_serial_str), "000000000000");
  }

  s_cdc_tx_lock = xSemaphoreCreateMutex();
  s_hid_stream = xStreamBufferCreate(4096, 1);
  s_cdc_stream = xStreamBufferCreate(4096, 1);

  const tinyusb_config_t tusb_cfg = {
    .port = TINYUSB_PORT_FULL_SPEED_0,
    .phy = {
      .skip_setup = false,
      .self_powered = false,
      .vbus_monitor_io = -1,
    },
    .task = {
      .size = 4096,
      .priority = 5,
      .xCoreID = 0,
    },
    .descriptor = {
      .device = &s_desc_device,
      .qualifier = nullptr,
      .string = s_desc_str,
      .string_count = sizeof(s_desc_str) / sizeof(s_desc_str[0]),
      .full_speed_config = s_desc_cfg,
      .high_speed_config = nullptr,
    },
    .pm_lock_enable = false,
    .event_cb = on_tinyusb_event,
    .event_arg = nullptr,
  };

  ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

  const tinyusb_config_cdcacm_t cdc_cfg = {
    .cdc_port = TINYUSB_CDC_ACM_0,
    .callback_rx = on_cdc_rx,
    .callback_rx_wanted_char = nullptr,
    .callback_line_state_changed = on_cdc_line_state,
    .callback_line_coding_changed = on_cdc_line_coding,
  };
  ESP_ERROR_CHECK(tinyusb_cdcacm_init(&cdc_cfg));
}

int usbHidAvailable() {
  if (!s_hid_stream) return -1;
  return (int)xStreamBufferBytesAvailable(s_hid_stream);
}

int usbHidRead() {
  if (!s_hid_stream) return -1;
  uint8_t b;
  size_t n = xStreamBufferReceive(s_hid_stream, &b, 1, 0);
  return (n == 1) ? (int)b : -1;
}

int usbCdcAvailable() {
  if (!s_cdc_stream) return -1;
  return (int)xStreamBufferBytesAvailable(s_cdc_stream);
}

int usbCdcRead() {
  if (!s_cdc_stream) return -1;
  uint8_t b;
  size_t n = xStreamBufferReceive(s_cdc_stream, &b, 1, 0);
  return (n == 1) ? (int)b : -1;
}

bool usbCdcConnected() {
  return s_cdc_connected;
}

void usbCdcWrite(const char *buf, size_t len) {
  if (!s_cdc_connected || !buf || !len || !s_cdc_tx_lock) return;
  if (xSemaphoreTake(s_cdc_tx_lock, pdMS_TO_TICKS(2)) != pdTRUE) return;
  size_t queued = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (const uint8_t *)buf, len);
  if (queued > 0) tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
  xSemaphoreGive(s_cdc_tx_lock);
}
