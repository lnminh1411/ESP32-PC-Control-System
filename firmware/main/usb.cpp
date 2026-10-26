#include "usb.h"
#include "sys.h"

#include <esp_check.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_rom_gpio.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>
#include <stdio.h>
#include <string.h>

#include "class/cdc/cdc_device.h"
#include "class/hid/hid.h"
#include "class/hid/hid_device.h"
#include "class/vendor/vendor_device.h"
#include "device/usbd.h"
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
  TUD_HID_REPORT_DESC_GENERIC_INOUT_FEATURE(63, HID_REPORT_ID(6)),
  TUD_HID_REPORT_DESC_SYSTEM_CONTROL(HID_REPORT_ID(1))
};

enum {
  ITF_NUM_HID = 0,
  ITF_NUM_CDC,
  ITF_NUM_CDC_DATA,
  ITF_NUM_VENDOR,    // WinUSB bulk pipe for the PC service; last, so HID and CDC keep their numbers
  ITF_NUM_TOTAL
};

enum {
  EPNUM_HID = 1,
  EPNUM_CDC_NOTIF = 2,
  EPNUM_CDC_DATA = 3,
  EPNUM_VENDOR = 4,   // the S3 has 5 IN endpoints with EP0: this is the last one
};

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN + TUD_CDC_DESC_LEN + TUD_VENDOR_DESC_LEN)

static const tusb_desc_device_t s_desc_device = {
  .bLength = sizeof(tusb_desc_device_t),
  .bDescriptorType = TUSB_DESC_DEVICE,
  .bcdUSB = 0x0210,   // 2.1: Windows asks for the BOS descriptor (MS OS 2.0 -> WinUSB)
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
  TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 500),

  // Interface 0: HID IN+OUT
  TUD_HID_INOUT_DESCRIPTOR(ITF_NUM_HID, 4, HID_ITF_PROTOCOL_NONE, sizeof(s_hid_report_descriptor),
                           EPNUM_HID, (uint8_t)(0x80 | EPNUM_HID), 64, 1),

  // Interfaces 1-2: CDC
  TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 5, (uint8_t)(0x80 | EPNUM_CDC_NOTIF), 64,
                     EPNUM_CDC_DATA, (uint8_t)(0x80 | EPNUM_CDC_DATA), 64),

  // Interface 3: vendor bulk, the PC service's fast pipe (~1 MB/s against
  // HID's 63 bytes per 1 ms frame)
  TUD_VENDOR_DESCRIPTOR(ITF_NUM_VENDOR, 6, EPNUM_VENDOR, (uint8_t)(0x80 | EPNUM_VENDOR), 64),
};

// MS OS 2.0: Windows binds WinUSB to the vendor interface by itself (no INF)
// and publishes it under this interface GUID, which the PC service looks for.
// The GUID is also in host/pc_stats/PcStats.cs.
#define VENDOR_REQUEST_MICROSOFT 1
#define MS_OS_20_DESC_LEN 0xB2

static const uint8_t s_desc_bos[] = {
  TUD_BOS_DESCRIPTOR(TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN, 1),
  TUD_BOS_MS_OS_20_DESCRIPTOR(MS_OS_20_DESC_LEN, VENDOR_REQUEST_MICROSOFT),
};

static const uint8_t s_desc_ms_os_20[] = {
  // set header: length, type, Windows version, total length
  U16_TO_U8S_LE(0x000A), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR), U32_TO_U8S_LE(0x06030000),
  U16_TO_U8S_LE(MS_OS_20_DESC_LEN),
  // configuration subset header: length, type, configuration index, reserved, subset length
  U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION), 0, 0,
  U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A),
  // function subset header: length, type, first interface, reserved, subset length
  U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION), ITF_NUM_VENDOR, 0,
  U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A - 0x08),
  // compatible ID: WINUSB
  U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID), 'W', 'I', 'N', 'U', 'S', 'B', 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0,
  // registry property DeviceInterfaceGUIDs (REG_MULTI_SZ)
  U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A - 0x08 - 0x08 - 0x14), U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
  U16_TO_U8S_LE(0x0007), U16_TO_U8S_LE(0x002A),
  'D', 0, 'e', 0, 'v', 0, 'i', 0, 'c', 0, 'e', 0, 'I', 0, 'n', 0, 't', 0, 'e', 0, 'r', 0, 'f', 0, 'a', 0, 'c', 0, 'e', 0, 'G', 0, 'U', 0, 'I', 0, 'D', 0, 's', 0, 0, 0,
  U16_TO_U8S_LE(0x0050),
  '{', 0, '3', 0, '9', 0, '5', 0, '3', 0, '1', 0, 'A', 0, 'D', 0, '1', 0, '-', 0, '5', 0, '5', 0, 'E', 0, '5', 0, '-', 0, '4', 0, '9', 0, '3', 0, '3', 0, '-', 0, '8', 0, '6', 0, '9', 0, 'D', 0, '-', 0, '1', 0, '2', 0, '3', 0, 'B', 0, '4', 0, 'E', 0, '7', 0, 'D', 0, '1', 0, '7', 0, '2', 0, 'C', 0, '}', 0, 0, 0, 0, 0,
};
static_assert(sizeof(s_desc_ms_os_20) == MS_OS_20_DESC_LEN, "MS OS 2.0 descriptor set length");

static char s_serial_str[13];

static const char *s_desc_str[] = {
  (const char[]){0x09, 0x04}, // 0: Supported language: English (0x0409)
  "Espressif Systems",        // 1: Manufacturer
  "ESP32 PC Controller",     // 2: Product
  s_serial_str,               // 3: Device serial string
  "ESP32 HID",                // 4: HID Interface
  "ESP32 CDC",                // 5: CDC Interface
  "ESP32 Bulk",               // 6: Vendor (WinUSB) Interface
};

static StreamBufferHandle_t s_hid_stream = nullptr;

// A receive path with backpressure: bytes move from TinyUSB's FIFO into the
// stream buffer only as far as they fit. What stays in the FIFO keeps the OUT
// endpoint un-armed, so the PC waits (NAK) instead of the board dropping
// bytes. The RX callback (TinyUSB task) and the reader both pump, so a mutex
// keeps the stream buffer single-writer.
struct RxPipe {
  StreamBufferHandle_t stream;
  SemaphoreHandle_t lock;
  uint32_t (*available)();
  uint32_t (*read)(void *buf, uint32_t len);
};

static uint32_t cdcAvail() { return tud_cdc_n_available(0); }
static uint32_t cdcRead(void *buf, uint32_t len) { return tud_cdc_n_read(0, buf, len); }
static uint32_t vendorAvail() { return tud_vendor_n_available(0); }
static uint32_t vendorRead(void *buf, uint32_t len) { return tud_vendor_n_read(0, buf, len); }

static RxPipe s_cdc_rx = {nullptr, nullptr, cdcAvail, cdcRead};
static RxPipe s_vendor_rx = {nullptr, nullptr, vendorAvail, vendorRead};

static void rxPump(RxPipe &p) {
  if (!p.stream || !p.lock || !p.available()) return;
  if (xSemaphoreTake(p.lock, 0) != pdTRUE) return;   // the other side is pumping
  uint8_t buf[64];
  for (;;) {
    size_t room = xStreamBufferSpacesAvailable(p.stream);
    if (!room || !p.available()) break;
    uint32_t n = p.read(buf, room < sizeof buf ? (uint32_t)room : (uint32_t)sizeof buf);
    if (!n) break;
    xStreamBufferSend(p.stream, buf, n, 0);
  }
  xSemaphoreGive(p.lock);
}

static int rxAvailable(RxPipe &p) {
  if (!p.stream) return -1;
  size_t n = xStreamBufferBytesAvailable(p.stream);
  if (!n) {   // drained: take what waited in TinyUSB's FIFO while the stream was full
    rxPump(p);
    n = xStreamBufferBytesAvailable(p.stream);
  }
  return (int)n;
}

static int rxRead(RxPipe &p) {
  if (!p.stream) return -1;
  uint8_t b;
  return xStreamBufferReceive(p.stream, &b, 1, 0) == 1 ? (int)b : -1;
}

// The vendor pipe carries board -> PC lines while the service is sending on
// it (it sends a frame every 500 ms), HID otherwise
static volatile uint32_t s_vendor_rx_ms;
static SemaphoreHandle_t s_vendor_tx_lock;

static void (*s_event_cb)(UsbEvent) = nullptr;

static volatile bool s_cdc_connected = false;
static volatile bool s_dtr = false;
static volatile bool s_rts = false;
static volatile uint8_t s_lineState = 0;
static SemaphoreHandle_t s_cdc_tx_lock;   // logMsg writes from several tasks
static SemaphoreHandle_t s_hid_tx_lock;

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
  (void)itf;
  (void)event;
  rxPump(s_cdc_rx);
}

extern "C" {

void tud_vendor_rx_cb(uint8_t idx, const uint8_t *buffer, uint16_t bufsize) {
  (void)idx;
  (void)buffer;
  (void)bufsize;
  s_vendor_rx_ms = nowMs();
  rxPump(s_vendor_rx);
}

uint8_t const *tud_descriptor_bos_cb(void) {
  return s_desc_bos;
}

// MS OS 2.0 descriptor set request (wIndex 7)
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request) {
  if (stage != CONTROL_STAGE_SETUP) return true;
  if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR && request->bRequest == VENDOR_REQUEST_MICROSOFT &&
      request->wIndex == 7) {
    return tud_control_xfer(rhport, request, (void *)(uintptr_t)s_desc_ms_os_20, sizeof s_desc_ms_os_20);
  }
  return false;
}

} // extern "C"

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

  // a terminal is listening while DTR is up; gating on lineState lost every other
  // pyserial open (RTS-then-DTR walks the reset sequence to step 2 and parks there)
  s_cdc_connected = dtr;
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
  s_hid_tx_lock = xSemaphoreCreateMutex();
  s_vendor_tx_lock = xSemaphoreCreateMutex();
  s_cdc_rx.lock = xSemaphoreCreateMutex();
  s_vendor_rx.lock = xSemaphoreCreateMutex();
  // Byte storage in PSRAM (28 KB of internal RAM back), control blocks internal:
  // a corrupted byte costs one line, a corrupted pointer would cost the board.
  // A static stream buffer holds size - 1 bytes, hence the + 1.
  static StaticStreamBuffer_t hidCtl, cdcCtl, vendorCtl;
  const size_t hidSize = 24576;   // the PC bursts up to ~14 KB per tick (cover art); overflow drops bytes
  const size_t cdcSize = 4096;    // CDC and vendor wait for room instead (rxPump)
  const size_t vendorSize = 8192;
  uint8_t *hidMem = (uint8_t *)heap_caps_malloc(hidSize + 1, MALLOC_CAP_SPIRAM);
  uint8_t *cdcMem = (uint8_t *)heap_caps_malloc(cdcSize + 1, MALLOC_CAP_SPIRAM);
  uint8_t *vendorMem = (uint8_t *)heap_caps_malloc(vendorSize + 1, MALLOC_CAP_SPIRAM);
  if (hidMem) s_hid_stream = xStreamBufferCreateStatic(hidSize + 1, 1, hidMem, &hidCtl);
  else s_hid_stream = xStreamBufferCreate(hidSize, 1);
  if (cdcMem) s_cdc_rx.stream = xStreamBufferCreateStatic(cdcSize + 1, 1, cdcMem, &cdcCtl);
  else s_cdc_rx.stream = xStreamBufferCreate(cdcSize, 1);
  if (vendorMem) s_vendor_rx.stream = xStreamBufferCreateStatic(vendorSize + 1, 1, vendorMem, &vendorCtl);
  else s_vendor_rx.stream = xStreamBufferCreate(vendorSize, 1);

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

int usbVendorAvailable() { return rxAvailable(s_vendor_rx); }
int usbVendorRead() { return rxRead(s_vendor_rx); }

static bool vendorActive() {
  uint32_t t = s_vendor_rx_ms;
  return t && nowMs() - t < 3000 && tud_vendor_n_mounted(0);
}

static bool vendorSendLine(const char *line, size_t len) {
  if (xSemaphoreTake(s_vendor_tx_lock, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  uint32_t t0 = nowMs();
  while (len && tud_vendor_n_mounted(0) && nowMs() - t0 < 200) {
    uint32_t n = tud_vendor_n_write(0, line, (uint32_t)len);
    line += n;
    len -= n;
    tud_vendor_n_write_flush(0);
    if (len) vTaskDelay(pdMS_TO_TICKS(2));
  }
  xSemaphoreGive(s_vendor_tx_lock);
  return len == 0;
}

static bool hidSendLine(const char *line);

bool usbPcSendLine(const char *line) {
  if (!line) return false;
  if (vendorActive()) return vendorSendLine(line, strlen(line));
  // nobody reads the vendor pipe: drop anything left in it, so a later
  // service start does not act on an old request
  if (tud_vendor_n_mounted(0) && xSemaphoreTake(s_vendor_tx_lock, 0) == pdTRUE) {
    tud_vendor_n_write_clear(0);
    xSemaphoreGive(s_vendor_tx_lock);
  }
  return hidSendLine(line);
}

static bool hidSendLine(const char *line) {
  if (!line || !s_hid_tx_lock) return false;
  if (!tud_mounted()) return false;
  if (xSemaphoreTake(s_hid_tx_lock, pdMS_TO_TICKS(100)) != pdTRUE) return false;

  size_t len = strlen(line);
  if (len == 0) {
    xSemaphoreGive(s_hid_tx_lock);
    return true;
  }

  size_t offset = 0;
  while (offset < len) {
    uint8_t report[63];
    memset(report, 0, sizeof(report));
    size_t chunk = len - offset;
    if (chunk > sizeof(report)) chunk = sizeof(report);
    memcpy(report, line + offset, chunk);
    offset += chunk;

    TickType_t start = xTaskGetTickCount();
    while (!tud_hid_ready()) {
      if (!tud_mounted()) {
        xSemaphoreGive(s_hid_tx_lock);
        return false;
      }
      if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(50)) {
        xSemaphoreGive(s_hid_tx_lock);
        return false;
      }
      vTaskDelay(pdMS_TO_TICKS(2));
    }

    if (!tud_hid_report(6, report, sizeof(report))) {
      xSemaphoreGive(s_hid_tx_lock);
      return false;
    }
  }

  xSemaphoreGive(s_hid_tx_lock);
  return true;
}

bool usbSystemControl(uint8_t usage) {
  if (!s_hid_tx_lock) return false;
  if (!tud_mounted() || !tud_hid_ready()) return false;
  if (xSemaphoreTake(s_hid_tx_lock, pdMS_TO_TICKS(100)) != pdTRUE) return false;

  if (!tud_mounted() || !tud_hid_ready()) {
    xSemaphoreGive(s_hid_tx_lock);
    return false;
  }

  uint8_t val = usage;
  if (!tud_hid_report(1, &val, 1)) {
    xSemaphoreGive(s_hid_tx_lock);
    return false;
  }

  vTaskDelay(pdMS_TO_TICKS(50));

  TickType_t start = xTaskGetTickCount();
  while (!tud_hid_ready()) {
    if (!tud_mounted() || (xTaskGetTickCount() - start) > pdMS_TO_TICKS(50)) {
      xSemaphoreGive(s_hid_tx_lock);
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }

  val = 0;
  bool ok = tud_hid_report(1, &val, 1);
  xSemaphoreGive(s_hid_tx_lock);
  return ok;
}

bool usbSuspended() {
  return tud_suspended();
}

bool usbRemoteWakeup() {
  if (!tud_suspended()) return false;
  return tud_remote_wakeup();
}

int usbCdcAvailable() { return rxAvailable(s_cdc_rx); }
int usbCdcRead() { return rxRead(s_cdc_rx); }

bool usbCdcConnected() {
  return s_cdc_connected;
}

// Blocking: waits for the FIFO instead of dropping (bulk dumps like `shot`)
bool usbCdcWriteAll(const char *buf, size_t len) {
  if (!s_cdc_connected || !buf || !s_cdc_tx_lock) return false;
  if (xSemaphoreTake(s_cdc_tx_lock, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  uint32_t t0 = nowMs();
  while (len && nowMs() - t0 < 2000) {
    size_t q = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (const uint8_t *)buf, len);
    buf += q;
    len -= q;
    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(20));
  }
  xSemaphoreGive(s_cdc_tx_lock);
  return len == 0;
}

void usbCdcWrite(const char *buf, size_t len) {
  if (!s_cdc_connected || !buf || !len || !s_cdc_tx_lock) return;
  // a log line from inside a locked write (TinyUSB "Flush failed") would
  // re-take this non-recursive mutex and trip a FreeRTOS assert: drop it
  if (xSemaphoreGetMutexHolder(s_cdc_tx_lock) == xTaskGetCurrentTaskHandle()) return;
  if (xSemaphoreTake(s_cdc_tx_lock, pdMS_TO_TICKS(2)) != pdTRUE) return;
  size_t queued = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (const uint8_t *)buf, len);
  if (queued > 0) tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
  xSemaphoreGive(s_cdc_tx_lock);
}
