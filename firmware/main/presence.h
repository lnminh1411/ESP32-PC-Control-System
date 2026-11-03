#pragma once

// Is someone at the desk, and what follows from it (docs/power-and-night.md).
// The camera check is PcHwCtl's, about 3.5 s each; the helper runs one every
// `!pres <s>` seconds and answers in the text stat pc.present, "<n> <yes|no|unknown>
// <why> <conf %> [zone]", n counting the checks; someone seen in PcHwCtl's
// "room" zone (beyond ~1.6 m) counts as nobody at the desk everywhere. Every PRESENCE_DAY_S at home
// (PRESENCE_BACK_S while the desk rule has the displays off), PRESENCE_FAST_S
// while the phone is away or night mode asks (presenceWatch).
//
// Phone beyond PRESENCE_AWAY_KM of home: PC displays off, on again when it is
// back within PRESENCE_HOME_KM. Input on the PC while away gives them back
// until PRESENCE_AWAY_IDLE_S without input (a dead phone can't leave you in
// the dark). Someone seen while away on two checks in a row, with input on
// the PC in the last PRESENCE_USE_S (someone in the room who leaves the PC
// alone gets nothing): the PC locks (again at each further check) and
// #pc-announcement and Pushover (emergency) get an alert, repeated every
// PRESENCE_ALERT_REPEAT_S while they stay at the desk. A fix older
// than PRESENCE_FIX_FRESH_S is checked with the phone first: an alert for
// someone coming home to a stale fix helps nobody.
//
// At home outside the night window: nobody seen (at the first check,
// PRESENCE_DAY_OFF_S) and no input for PRESENCE_DAY_IDLE_S, nothing keeping
// the display on (pc.disp_req): Windows' own display off (`!mon power all
// sleep`, no DDC/CI) and the AFK faces. Nobody for PRESENCE_DAY_LOW_S: low
// power mode on, PC only (Process Lasso; PC LEDs, board panels and status LED
// unchanged), unless something holds sleep (nightSleepHold; tried again at
// each check).
// Someone seen, input, the PC going off or the phone coming home: displays
// back, and low power mode off if this turned it on. Unknown (too dark, camera
// busy) changes nothing anywhere.

#include <stdint.h>

enum { PRES_UNKNOWN = -1, PRES_NOBODY = 0, PRES_SOMEONE = 1 };

void presencePoll();                 // main loop
void presenceWatch(bool on);         // night mode: a check every PRESENCE_FAST_S
// Detection on or off (saved; PC Control button, console `presence on|off`).
// Off: no camera checks, no answer counts (presenceSeen unknown, presenceNobody
// false), so night mode falls back to input alone and the desk and intruder
// rules rest. The phone-away displays-off doesn't need the camera and stays.
void presenceSetEnabled(bool on);
bool presenceEnabled();
void presenceCheckNow();             // one check now, whatever the cadence
uint32_t presenceChecks();           // checks answered so far: a change means a new answer
int presenceSeen();                  // the last answer, PRES_*
bool presenceNobody();               // the last two checks, both recent, saw nobody
bool presenceAway();                 // the phone is away from home
bool presenceDisplaysOff();          // this module wants the PC displays off
bool presenceDry();                  // test: log lock, alert, displays and sleep instead of doing them
void presenceStatus(char *buf, uint32_t size);   // one line for the panel and the console
void presenceConsole(const char *a1, const char *a2);   // `presence [away on|off|auto | seen yes|no|unknown | dry on|off]`
