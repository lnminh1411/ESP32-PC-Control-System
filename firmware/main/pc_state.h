// PC power state from the native USB link (TinyUSB events), and the status
// LED that shows it.
//
// USB started/resumed = PC on; suspended = asleep/off. Every time the link
// goes active a 30 s guard starts, and suspends inside it are ignored: a cold
// boot blips the link during POST (docs/pc-power-detection.md). PC asleep/off
// turns the backlights off; awake restores them.
//
// Status LED (GPIO2):
//   loading                  quick pulses, 100 ms on / 100 ms off
//   PC on                    solid
//   PC restarting / updating quick pulses, 200 ms on / 200 ms off
//   PC asleep / off          slow breathing, fading in and out over 3 s
// "Loading" is the board booting (until statusLedReady()), or the PC on
// without its stats arriving yet (also after a restart), or, for up to 5 s
// after boot, no USB host seen yet. `led on|off` overrides it. The LED is
// PWM-driven (LEDC): ledBrightnessSet() scales every level, and LED_SWEEP is
// a test ramp, 100% -> 0% over 2 s and back over 1 s.
#pragma once
#include <stdint.h>

enum PcState { PC_UNKNOWN, PC_ON, PC_OFF };
enum LedMode { LED_AUTO, LED_ON, LED_OFF, LED_SWEEP };

void pcStateInit();                // register the USB event handler (before usbInit)
void pcStatePoll();                // main loop: USB event log lines and the backlights
void statusLedInit();              // start the LED task
void statusLedReady();             // boot finished: leave the boot pattern
PcState pcState();
void pcStateSimulate(int s);       // test hook: PC_ON / PC_OFF overrides the USB state, -1 = real again
const char *pcStateName(PcState s);
uint32_t bootGuardLeftMs();        // 0 = guard not running
void ledModeSet(LedMode m);
LedMode ledMode();
void ledBrightnessSet(uint8_t pct);   // 0-100, applies to every "on"
uint8_t ledBrightness();
void ledSettingsLoad();               // after nvs_flash_init: saved brightness and on/off
void ledSettingsSave();               // Discord: keep brightness and on/off (LED_OFF = off, else auto)
void ledCapSet(uint8_t pct);          // night mode: brightness at most pct, 100 = no cap (not saved)
