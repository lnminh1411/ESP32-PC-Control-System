#pragma once

// Night mode (PC Control tab toggle, console `night`). While on, between
// 22:00 and 06:00, once the PC has had no user input for 15 min (pc.idle_s,
// GetLastInputInfo in the helper's user half), it engages:
//   PC RGB LEDs to 10 %, the board's panels to 10 % showing the PC-off faces,
//   status LED capped at 20 %, the PC's displays off (Windows, `!mon power`).
// Input on the PC disengages it and puts everything back; 15 more idle minutes
// engage it again. At 06:00 the board side goes back at once; the LEDs go
// back as soon as PcHwCtl answers (the PC is usually hibernating by then).
// Start/End (by hand) engages it now, at any hour, until input or End.
// Input only ends it if the next 10 s show a real return (> 50 px of mouse
// travel or 3 presses, pc.in_px / pc.in_press); a bumped mouse turns the
// displays off again. No automatic start while a process keeps the display on
// (pc.disp_req: a video playing). Engaged/ended also goes to the PC service as
// `!night 1|0` (every 30 s too): Process Lasso's E-core night config.

#include <stdint.h>

void nightInit();                  // NVS: the toggle, a pending LED restore
void nightPoll();                  // main loop
void nightSetEnabled(bool on);     // saved: the automatic 22:00-06:00 engage
void nightManual(bool on);         // start now (any hour, no idle wait) / end now; input on the PC ends it too
bool nightEnabled();
bool nightEngaged();
void nightStatus(char *buf, uint32_t size);   // one line for the panel and the console
void nightConsole(const char *args);          // `night [on|off|start|end|status]`
