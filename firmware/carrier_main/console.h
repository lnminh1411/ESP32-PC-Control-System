// Serial console: command lines from BOTH the native USB CDC port (to the
// PC's internal header) and the UART (CH343 USB-C, 3 Mbaud), plus GIF
// uploads on the UART (protocol in gif_store.h). Lines starting with '@' are
// PC stats from the host service (pcstats.h) over vendor HID or CDC fallback.
#pragma once
#include <stdint.h>

void consoleBegin();                      // call from setup() right after Serial.begin()
void consolePoll();                       // call from loop()
void consoleUploadWindow(uint32_t ms);    // at boot: wait up to ms for a host upload
