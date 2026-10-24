// ============================================================
// Carrier firmware -- ESP32 PC Control System, rev 3.1 board.
// ============================================================
//
// Modules (pins and build switches in config.h):
//   display   3x GMT024 ST7789 panels on one SPI bus, dirty-rectangle
//             engine, RGB444 on the wire; per-panel backlight PWM
//   views     what each panel shows: gif | status | stats
//   optos     door keyfob buttons B1-B4 and the motherboard reset pulse
//   pc_state  PC power state from the native USB link; status LED
//   gif_store GIF frames in PSRAM, saved to FAT, uploaded over the UART
//   commands  the text command set
//   console   serial transports for commands and uploads
//   net       Wi-Fi station, mDNS, over-the-air updates (credentials: secrets.h)
//   ha        Home Assistant via MQTT discovery (optional, needs an HA broker)
//   discord   Discord bot: slash commands + buttons, remote control from anywhere
//
// Board config (arduino-cli):
//   --fqbn esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,USBMode=default,CDCOnBoot=cdc
// (USBMode=default = USB-OTG/TinyUSB, needed for the suspend/resume events;
// with CDCOnBoot=cdc, `Serial` is the native USB port, `Serial0` the UART.)

#include "config.h"
#include "console.h"
#include "display.h"
#include "gif_store.h"
#include "discord.h"
#include "ha.h"
#include "net.h"
#include "log.h"
#include "optos.h"
#include "pc_state.h"
#include "views.h"
#include "USB.h"

void setup() {
  // 1. opto rows first: LOW before anything else can glitch them
  optosInit();
  statusLedInit();

  // 2. backlights 100%; the module's own RGB LED off
  backlightInit();
  rgbLedWrite(ONBOARD_RGB_GPIO, 0, 0, 0);

  // 3. serial ports + USB power-state events
  HOST_UART.setRxBufferSize(32768);
  HOST_UART.begin(UART_BAUD);
  pcStateInit();
  Serial.begin();                      // native USB CDC (TinyUSB)
  consoleBegin();
  delay(100);
  netInit();                           // joins Wi-Fi in the background
  haInit();
  discordInit();

  // 4. displays: GIF / status / stats
  displayInit();
  viewEnter(0, VIEW_GIF);
  viewEnter(1, VIEW_STATUS);
  viewEnter(2, VIEW_STATS);
  for (int i = 0; i < N_PANELS; i++) {
    displaySubmit(i);
    displayWait(i);
  }

  bool fatOk = gifFsInit();
  logMsg("carrier rev 3.1 | %d panels, dirty-rect RGB444, SPI %d Hz%s | PSRAM free %u | fat %s",
         N_PANELS, SPI_HZ, MOBO_RST_HOLD_TEST ? " | TEST: mobo RST held HIGH" : "",
         (unsigned)ESP.getFreePsram(), fatOk ? "ok" : "FAILED");
  HOST_UART.println("READY");

  // 5. GIF: 1.5 s window for a host upload, else the saved one
  consoleUploadWindow(1500);
  if (!gif.n && gifLoad()) logMsg("gif: %u frames %ux%u from flash", gif.n, gif.w, gif.h);
  if (!gif.n) logMsg("gif: none stored -- run scripts/gif_stream.py");
  for (int i = 0; i < N_PANELS; i++) viewEnter(i, panels[i].view);   // GIF offsets now that frames exist
  HOST_UART.println("PLAY");
}

void loop() {
  consolePoll();
  netPoll();
  haPoll();
  discordPoll();
  viewsService();
  delay(1);
}
