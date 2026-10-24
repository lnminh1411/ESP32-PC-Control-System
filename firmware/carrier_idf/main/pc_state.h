// PC power state from the native USB link (TinyUSB events), and the status
// LED that shows it.
//
// USB started/resumed = PC on; suspended = asleep/off. Every time the link
// goes active a 30 s guard starts, and suspends inside it are ignored: a cold
// boot blips the link during POST (docs/pc-power-detection.md). PC asleep/off
// turns the backlights off; awake restores them.
//
// Status LED (GPIO2): solid = PC on, slow blink = asleep/off, short blip
// every 2 s = no USB host seen yet. `led on|off` overrides it. The LED is
// PWM-driven (LEDC): ledBrightnessSet() scales every "on", and LED_SWEEP is
// a test ramp, 100% -> 0% over 2 s and back over 1 s.
#pragma once
#include <stdint.h>

enum PcState { PC_UNKNOWN, PC_ON, PC_OFF };
enum LedMode { LED_AUTO, LED_ON, LED_OFF, LED_SWEEP };

void pcStateInit();                // register the USB event handler
void statusLedInit();              // start the LED task
PcState pcState();
const char *pcStateName(PcState s);
uint32_t bootGuardLeftMs();        // 0 = guard not running
void ledModeSet(LedMode m);
LedMode ledMode();
void ledBrightnessSet(uint8_t pct);   // 0-100, applies to every "on"
uint8_t ledBrightness();
