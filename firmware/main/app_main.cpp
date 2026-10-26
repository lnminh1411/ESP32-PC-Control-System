// ============================================================
// Carrier firmware -- ESP32 PC Control System, rev 3.1 board.
// ============================================================

#include "config.h"
#include "console.h"
#include "discord.h"
#include "display.h"
#include "display_config.h"
#include "gif_store.h"
#include "host_uart.h"
#include "layout.h"
#include "log.h"
#include "net.h"
#include "optos.h"
#include "pc_state.h"
#include "pc_going.h"
#include "sys.h"
#include "usb.h"
#include "views.h"
#include "power.h"
#include "aictl.h"
#include "hwctl.h"
#include "weather.h"
#include "wol.h"

#include <esp_event.h>
#include <esp_system.h>
#include <nvs_flash.h>

// Why the board last restarted: in the log, and in #pc-announcement when it
// was not a normal power-on or a requested restart
const char *resetReasonText() {
  esp_reset_reason_t r = esp_reset_reason();
  return r == ESP_RST_POWERON ? "power-on" : r == ESP_RST_SW ? "restart requested" :
         r == ESP_RST_PANIC ? "crash" : r == ESP_RST_INT_WDT ? "interrupt watchdog" :
         r == ESP_RST_TASK_WDT ? "task watchdog" : r == ESP_RST_WDT ? "watchdog" :
         r == ESP_RST_BROWNOUT ? "brownout (supply voltage dropped)" :
         r == ESP_RST_USB ? "USB reset" : r == ESP_RST_EXT ? "reset pin" :
         r == ESP_RST_DEEPSLEEP ? "deep sleep" : "unknown";
}

static void logResetReason() {
  esp_reset_reason_t r = esp_reset_reason();
  const char *why = resetReasonText();
  logMsg("boot: last reset: %s (%d)", why, (int)r);
  if (r != ESP_RST_POWERON && r != ESP_RST_SW && r != ESP_RST_USB) {
    char msg[80];
    snprintf(msg, sizeof msg, "Board restarted after %s", why);
    discordNotify(msg);
  }
}

extern "C" void app_main() {
  // 1. opto rows first: LOW before anything else can glitch them
  optosInit();
  statusLedInit();

  // 2. serial ports + USB power-state events
  hostUartBegin();

  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  // 3. backlights: restored from NVS (else 100%)
  backlightInit();

  wolInit();

  ESP_ERROR_CHECK(esp_event_loop_create_default());

  usbInit();
  pcStateInit();

  // 4. displays: restore stored view else defaultView
  displayInit();
  for (int i = 0; i < N_PANELS; i++) {
    viewEnter(i, displayStoredView(i));
  }
  for (int i = 0; i < N_PANELS; i++) {
    displaySubmit(i);
    displayWait(i);
  }

  bool fatOk = gifFsInit();
  layoutInit();
  logMsg("carrier rev 3.1 | %d panels, dirty-rect RGB444, SPI %d Hz%s | PSRAM free %u | fat %s",
         N_PANELS, SPI_HZ, MOBO_RST_HOLD_TEST ? " | TEST: mobo RST held HIGH" : "",
         (unsigned)freePsram(), fatOk ? "ok" : "FAILED");
  hostUartPrintln("READY");

  // 5. GIF: 1.5 s window for a host upload, else the saved one
  consoleUploadWindow(1500);
  if (!gif.n && gifLoad()) logMsg("gif: %u frames %ux%u from flash", gif.n, gif.w, gif.h);
  if (!gif.n) logMsg("gif: none stored -- run scripts/gif_stream.py");
  for (int i = 0; i < N_PANELS; i++) viewEnter(i, panels[i].view);   // GIF offsets now that frames exist

  // Wi-Fi only after the GIF is read: a WPA3 handshake running alongside the
  // 140 KB flash read deadlocked the two cores at boot (interrupt watchdog)
  netInit();                           // joins Wi-Fi in the background
  discordInit();
  weatherInit();
  logResetReason();
  hostUartPrintln("PLAY");
  statusLedReady();

  for (;;) {
    consolePoll();
    wolPoll();
    powerPoll();
    hwPoll();
    aiPoll();
    pcGoingPoll();
    netPoll();
    discordPoll();
    weatherPoll();
    viewsService();
    sleepMs(1);
  }
}
