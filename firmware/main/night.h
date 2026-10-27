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
// `!night 1|0` (every 30 s too, and at once when the PC comes back): Process
// Lasso's E-core night config with efficiency mode, heavy exes left alone.
// In the window, NIGHT_SLEEP_S (1 h) after the last real input (false wakes
// don't count) it sleeps the PC, NIGHT_HIBERNATE_S (2 h) hibernates it; both
// wait while an exe is heavy (pc.heavy), a power request holds the PC
// (pc.disp_req / sys_req / exec_req, minus NIGHT_REQ_IGNORE) or the network is
// busy (NIGHT_NET_HOLD_BPS). A PC it put to sleep is woken and hibernated an
// hour later, after 06:00 too, unless someone powers it on first; powering on
// a PC night mode sent down ends the night.

#include <stdint.h>

void nightInit();                  // NVS: the toggle, a pending LED restore
void nightPoll();                  // main loop
void nightSetEnabled(bool on);     // saved: the automatic 22:00-06:00 engage
void nightManual(bool on);         // start now (any hour, no idle wait) / end now; input on the PC ends it too
bool nightEnabled();
bool nightEngaged();
void nightStatus(char *buf, uint32_t size);   // one line for the panel and the console
void nightWindowText(char *buf, uint32_t size);   // "22:00-06:00 after 15 min" (from settings.h)
void nightConsole(const char *args);          // `night [on|off|start|end|status]`
