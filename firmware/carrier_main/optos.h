// Optocoupler outputs: door keyfob buttons B1-B4 (rows 1-4) and the
// motherboard reset line (row 5).
//
// Pins: latch pre-staged LOW with gpio_set_level, then pinMode OUTPUT
// (Hi-Z -> LOW, never a HIGH glitch); never INPUT_PULLUP (pcb/README.md).
// Pulses are released by one-shot esp_timers, on time whatever the main
// loop is doing. The board has no hardware interlock between door buttons:
// one button at a time and the gap between presses are enforced here.
#pragma once

void optosInit();                  // call first thing in setup()
const char *doorPress(int n);      // n = 1..4; nullptr = pressed, else why not
const char *moboReset();           // nullptr = pulse started, else why not
int doorPressed();                 // button 1..4 being pressed, 0 = none
bool moboResetActive();
