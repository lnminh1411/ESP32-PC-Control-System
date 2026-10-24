// ============================================================
// Carrier firmware -- ESP32 PC Control System, rev 3.1 board.
// ============================================================

#include "config.h"
#include "console.h"
#include "discord.h"
#include "display.h"
#include "gif_store.h"
#include "ha.h"
#include "host_uart.h"
#include "log.h"
#include "net.h"
#include "optos.h"
#include "pc_state.h"
#include "sys.h"
#include "usb.h"
#include "views.h"

#include <esp_event.h>
#include <nvs_flash.h>

extern "C" void app_main() {
  // 1. opto rows first: LOW before anything else can glitch them
  optosInit();
  statusLedInit();

  // 2. backlights 100%
  backlightInit();

  // 3. serial ports + USB power-state events
  hostUartBegin();

  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  ESP_ERROR_CHECK(esp_event_loop_create_default());

  usbInit();
  pcStateInit();

  netInit();                           // joins Wi-Fi in the background
  haInit();
  discordInit();

  // 4. displays: GIF / status / stats (testing request: panels 1-3 all VIEW_METRICS)
  displayInit();
  viewEnter(0, VIEW_METRICS);
  viewEnter(1, VIEW_METRICS);
  viewEnter(2, VIEW_METRICS);
  for (int i = 0; i < N_PANELS; i++) {
    displaySubmit(i);
    displayWait(i);
  }

  bool fatOk = gifFsInit();
  logMsg("carrier rev 3.1 | %d panels, dirty-rect RGB444, SPI %d Hz%s | PSRAM free %u | fat %s",
         N_PANELS, SPI_HZ, MOBO_RST_HOLD_TEST ? " | TEST: mobo RST held HIGH" : "",
         (unsigned)freePsram(), fatOk ? "ok" : "FAILED");
  hostUartPrintln("READY");

  // 5. GIF: 1.5 s window for a host upload, else the saved one
  consoleUploadWindow(1500);
  if (!gif.n && gifLoad()) logMsg("gif: %u frames %ux%u from flash", gif.n, gif.w, gif.h);
  if (!gif.n) logMsg("gif: none stored -- run scripts/gif_stream.py");
  for (int i = 0; i < N_PANELS; i++) viewEnter(i, panels[i].view);   // GIF offsets now that frames exist
  hostUartPrintln("PLAY");

  for (;;) {
    consolePoll();
    netPoll();
    haPoll();
    discordPoll();
    viewsService();
    sleepMs(1);
  }
}
