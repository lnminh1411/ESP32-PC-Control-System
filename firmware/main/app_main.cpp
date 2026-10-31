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
#include "pcwarn.h"
#include "aictl.h"
#include "monctl.h"
#include "night.h"
#include "hwctl.h"
#include "weather.h"
#include "wol.h"

#include <esp_attr.h>
#include <esp_core_dump.h>
#include <esp_event.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <nvs_flash.h>

// A brownout boot restarts once more 3 s later (settleAfterBrownout); this
// RTC mark makes that second boot still report the brownout
#define BROWNOUT_MARK 0x42524f57u
RTC_NOINIT_ATTR static uint32_t brownoutMark;
static esp_reset_reason_t bootReason;

// The PC powering on sags the board's supply (brownout). PSRAM gave single-bit
// errors in the boot that followed (2026-10-28 14:43: a garbled post, then
// corrupt PSRAM heap and a crash), likely because the MSPI timing tuning ran
// on the sagging supply. So: nothing started, wait for the supply to settle,
// restart; the next boot tunes PSRAM on a steady supply.
static void settleAfterBrownout() {
  esp_reset_reason_t r = esp_reset_reason();
  bootReason = r;
  if (r == ESP_RST_BROWNOUT) {
    brownoutMark = BROWNOUT_MARK;
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
  }
  if (r == ESP_RST_SW && brownoutMark == BROWNOUT_MARK) bootReason = ESP_RST_BROWNOUT;
  brownoutMark = 0;
}

// Why the board last restarted: in the log, and in #pc-announcement when it
// was not a normal power-on or a requested restart
const char *resetReasonText() {
  esp_reset_reason_t r = bootReason;
  return r == ESP_RST_POWERON ? "power-on" : r == ESP_RST_SW ? "restart requested" :
         r == ESP_RST_PANIC ? "crash" : r == ESP_RST_INT_WDT ? "interrupt watchdog" :
         r == ESP_RST_TASK_WDT ? "task watchdog" : r == ESP_RST_WDT ? "watchdog" :
         r == ESP_RST_BROWNOUT ? "brownout (supply voltage dropped; restarted again once it settled)" :
         r == ESP_RST_USB ? "USB reset" : r == ESP_RST_EXT ? "reset pin" :
         r == ESP_RST_DEEPSLEEP ? "deep sleep" : "unknown";
}

static void logResetReason() {
  esp_reset_reason_t r = bootReason;
  const char *why = resetReasonText();
  logMsg("boot: last reset: %s (%d)", why, (int)r);
  // The core dump is overwritten by the next crash: its summary goes to the
  // log now (addr2line the PCs against the build's ELF)
  esp_core_dump_summary_t *cd = (esp_core_dump_summary_t *)malloc(sizeof *cd);
  if (cd && (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT) &&
      esp_core_dump_get_summary(cd) == ESP_OK) {
    char reason[120] = "", bt[16 * 11 + 1] = "";
    esp_core_dump_get_panic_reason(reason, sizeof reason);
    for (uint32_t i = 0, n = 0; i < cd->exc_bt_info.depth && i < 16; i++)
      n += snprintf(bt + n, sizeof bt - n, " 0x%08lx", (unsigned long)cd->exc_bt_info.bt[i]);
    logMsg("crash: task %s, PC 0x%08lx, cause %lu, vaddr 0x%08lx, reason: %s", cd->exc_task, (unsigned long)cd->exc_pc,
           (unsigned long)cd->ex_info.exc_cause, (unsigned long)cd->ex_info.exc_vaddr, reason[0] ? reason : "-");
    logMsg("crash: backtrace%s%s", bt, cd->exc_bt_info.corrupted ? " (corrupted)" : "");
  }
  free(cd);
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
  settleAfterBrownout();   // a brownout boot: wait 3 s, restart (PSRAM starts again on a steady supply)

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
  ledSettingsLoad();
  nightInit();

  ESP_ERROR_CHECK(esp_event_loop_create_default());

  pcStateInit();   // first: an attach during usbInit is seen
  usbInit();

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

  // 5. GIF: the pack built into the firmware, frames inflate as they play
  if (gifLoad())
    logMsg("gif: %u GIFs, %u frames %ux%u, %u KB pack in flash, %u KB PSRAM", gif.nSeg, gif.n, gif.w, gif.h,
           (unsigned)(gif.packBytes / 1024), (unsigned)(gif.bytes / 1024));
  else
    logMsg("gif: pack failed to load");
  for (int i = 0; i < N_PANELS; i++) viewEnter(i, panels[i].view);   // GIF offsets now that frames exist

  // Wi-Fi only after the GIF is read: a WPA3 handshake running alongside a
  // long flash read deadlocked the two cores at boot (interrupt watchdog)
  netInit();                           // joins Wi-Fi in the background
  discordInit();
  weatherInit();
  logResetReason();
  statusLedReady();

  esp_task_wdt_add(nullptr);           // a stalled main loop panics after 20 s (core dump + reboot)
  for (;;) {
    esp_task_wdt_reset();
    consolePoll();
    pcStatePoll();
    psramEccPoll();
    wolPoll();
    powerPoll();
    pcWarnPoll();
    hwPoll();
    aiPoll();
    monPoll();
    nightPoll();
    pcGoingPoll();
    netPoll();
    discordPoll();
    weatherPoll();
    viewsService();
    sleepMs(1);
  }
}
