#include "night.h"

#include <math.h>
#include <nvs.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "board_time.h"
#include "discord.h"
#include "display.h"
#include "hwctl.h"
#include "log.h"
#include "monctl.h"
#include "pc_state.h"
#include "pcstats.h"
#include "screens.h"
#include "sys.h"
#include "usb.h"
#include "views.h"

static const int IDLE_ENGAGE_S = 15 * 60;
static const int START_HOUR = 22, END_HOUR = 6;
static const uint8_t PANEL_PCT = 10, STATUS_LED_PCT = 20, RGB_PCT = 10;

static bool enabled;
static bool engaged;
static bool manual;                      // started by hand: no window, no idle needed
static uint32_t engagedAtMin;            // minutes since midnight, for the status line
static uint32_t engagedMs;               // input newer than this ends it
// Wake guard: input during the night only ends it if the next 10 s show a
// real return (mouse moved > 50 px, or 3 key / button presses); otherwise
// the displays go off again and the night carries on
static const uint32_t GUARD_MS = 10000;
static const double GUARD_PX = 50, GUARD_PRESSES = 3;
static uint32_t guardUntilMs;            // 0 = no wake being checked
static double guardPx0, guardPress0;     // pc.in_px / pc.in_press when the wake was seen
static uint32_t falseWakes;              // this night, for the status line

// Process Lasso's night config on the PC (Night.cs), repeated every 30 s
static void lassoSend() { usbPcSendLine(engaged ? "!night 1\n" : "!night 0\n"); }

// counters that wrap at 1e6
static double counted(const char *key, double from) {
  double d = pcStat(key) - from;
  return d < 0 ? d + 1000000.0 : d;
}
static int8_t savedRgb = -1;             // PC LED brightness to put back, -1 = nothing owed (NVS)
static bool rgbLowered;                  // our 10 % is on the LEDs right now (NVS with savedRgb)

// #pc-announcement
static void announce(const char *what, uint32_t color) { discordPost(DC_PC, "Night mode", what, color); }

static void saveNvs() {
  nvs_handle_t h;
  if (nvs_open("night", NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_u8(h, "on", enabled ? 1 : 0);
  nvs_set_i8(h, "rgb", savedRgb);
  nvs_commit(h);
  nvs_close(h);
}

void nightInit() {
  nvs_handle_t h;
  if (nvs_open("night", NVS_READONLY, &h) != ESP_OK) return;
  uint8_t on = 0;
  if (nvs_get_u8(h, "on", &on) == ESP_OK) enabled = on;
  nvs_get_i8(h, "rgb", &savedRgb);
  nvs_close(h);
  rgbLowered = savedRgb >= 0;   // a restart while engaged: the LEDs are still at 10 %
}

static bool pcReachable() { return pcState() == PC_ON && pcStatsFresh() && pcStat("hw.ok") >= 1; }

static void boardSide(bool night) {
  screensNight = night;
  backlightCap(night ? PANEL_PCT : 100);
  ledCapSet(night ? STATUS_LED_PCT : 100);
  viewsMarkDirty(-1);
}

// PC LEDs back to what they were, now if PcHwCtl is there, else when it is
static void rgbRestoreTry(bool post) {
  if (!rgbLowered || savedRgb < 0 || !pcReachable()) return;
  char cmd[32];
  snprintf(cmd, sizeof cmd, "rgb brightness %d", savedRgb);
  uint32_t id;
  const char *err = hwSend(cmd, &id);
  if (err) return;   // next poll tries again
  logMsg("night: PC LEDs back to %d %%", savedRgb);
  if (post) {
    char what[64];
    snprintf(what, sizeof what, "PC LEDs back to %d %%", savedRgb);
    announce(what, 0xF1C40F);
  }
  rgbLowered = false;
  savedRgb = -1;
  saveNvs();
}

static void engage(bool byHand) {
  engaged = true;
  manual = byHand;
  engagedMs = nowMs();
  guardUntilMs = 0;
  falseWakes = 0;
  struct tm t;
  engagedAtMin = boardLocalTime(&t) ? t.tm_hour * 60 + t.tm_min : 0;
  boardSide(true);
  // PC LEDs: remember the level once (a re-engage keeps the first one)
  float b = pcStat("hw.rgb.bright");
  if (!rgbLowered && !isnan(b) && b > RGB_PCT && pcReachable()) {
    uint32_t id;
    if (!hwSend("rgb brightness 10", &id)) {
      savedRgb = (int8_t)lroundf(b);
      rgbLowered = true;
      saveNvs();
    }
  }
  uint32_t id;
  const char *err = monSend("power all off", &id);
  logMsg("night: engaged%s (PC LEDs %s, displays %s)", byHand ? " by hand" : "",
         rgbLowered ? "10 %" : "left alone", err ? err : "off");
  char what[160];
  snprintf(what, sizeof what, "Engaged%s: PC LEDs %s, board panels 10 %% on the PC-off screens, status LED 20 %%, PC displays %s",
           byHand ? " by hand" : "", rgbLowered ? "10 %" : "unchanged", err ? "not reached" : "off");
  announce(what, 0x2C3E50);
  lassoSend();
}

static void disengage(const char *why) {
  engaged = false;
  manual = false;
  guardUntilMs = 0;
  boardSide(false);
  lassoSend();
  rgbRestoreTry(false);   // the "back to normal" post covers it
  logMsg("night: off again (%s)%s", why, rgbLowered ? ", PC LEDs go back when PcHwCtl answers" : "");
  char what[160];
  snprintf(what, sizeof what, "Back to normal (%s)%s", why, rgbLowered ? "; PC LEDs go back once the PC is on" : "");
  announce(what, 0xF1C40F);
}

void nightPoll() {
  static uint32_t lastMs;
  uint32_t now = nowMs();
  if (now - lastMs < 1000) return;
  lastMs = now;

  static uint32_t lassoMs;
  if (now - lassoMs >= 30000) { lassoMs = now; lassoSend(); }
  if (!engaged) rgbRestoreTry(true);   // owed from a morning or a restart
  if (!enabled && !engaged) return;
  struct tm t;
  bool haveTime = boardLocalTime(&t);
  bool inWindow = haveTime && (t.tm_hour >= START_HOUR || t.tm_hour < END_HOUR);
  float idle = pcStat("pc.idle_s");
  bool idleKnown = pcStatsFresh() && pcStat("idle.ok") >= 1 && !isnan(idle);

  if (engaged) {
    // input after engaging: idle shorter than the time since (3 s for the
    // feed's lag), so a start by hand right after using the PC still holds
    float since = (now - engagedMs) / 1000.0f;
    if (!enabled && !manual) disengage("turned off");
    else if (!inWindow && !manual) disengage("06:00");
    else if (guardUntilMs) {
      double px = counted("pc.in_px", guardPx0), presses = counted("pc.in_press", guardPress0);
      if (px > GUARD_PX || presses >= GUARD_PRESSES) {
        disengage("input on the PC");
      } else if ((int32_t)(now - guardUntilMs) >= 0) {
        guardUntilMs = 0;
        engagedMs = now;   // the next input is a new wake
        falseWakes++;
        uint32_t id;
        monSend("power all off", &id);
        logMsg("night: false wake (%.0f px, %.0f presses in 10 s), displays off again", px, presses);
      }
    } else if (idleKnown && idle + 3 < since) {
      guardPx0 = pcStat("pc.in_px");
      guardPress0 = pcStat("pc.in_press");
      if (isnan(guardPx0) || isnan(guardPress0)) {
        disengage("input on the PC");   // a helper without the counters: no guard
      } else {
        guardUntilMs = now + GUARD_MS;
        logMsg("night: input on the PC, checking for 10 s");
      }
    }
    return;
  }
  // video guard: no automatic start while something keeps the display on
  bool videoHold = pcStat("pc.disp_req") >= 1;
  if (inWindow && pcState() == PC_ON && idleKnown && idle >= IDLE_ENGAGE_S && !videoHold) engage(false);
}

void nightSetEnabled(bool on) {
  enabled = on;
  saveNvs();
  logMsg("night: mode %s", on ? "on" : "off");
  announce(on ? "Turned on: engages 22:00-06:00 after 15 min without input on the PC" : "Turned off", 0x95A5A6);
  if (!on && engaged) disengage("turned off");
}

void nightManual(bool on) {
  if (on && !engaged) engage(true);
  else if (!on && engaged) disengage("ended by hand");
}

bool nightEnabled() { return enabled; }
bool nightEngaged() { return engaged; }

void nightStatus(char *buf, uint32_t size) {
  if (engaged)
    snprintf(buf, size, "engaged since %02lu:%02lu%s%s", (unsigned long)(engagedAtMin / 60),
             (unsigned long)(engagedAtMin % 60), manual ? ", by hand" : "",
             guardUntilMs ? ", checking a wake" : falseWakes ? ", false wakes ignored" : "");
  else if (enabled && pcStat("pc.disp_req") >= 1)
    snprintf(buf, size, "on, held off: %.20s keeps the display on", pcText("pc.disp_req_by"));
  else if (enabled)
    snprintf(buf, size, "on: 22:00-06:00 after 15 min idle%s", rgbLowered ? ", LED restore owed" : "");
  else
    snprintf(buf, size, "off%s", rgbLowered ? ", LED restore owed" : "");
}

void nightConsole(const char *args) {
  while (args && *args == ' ') args++;
  char st[64];
  if (args && !strcmp(args, "on")) nightSetEnabled(true);
  else if (args && !strcmp(args, "off")) nightSetEnabled(false);
  else if (args && (!strcmp(args, "start") || !strcmp(args, "test"))) nightManual(true);
  else if (args && !strcmp(args, "end")) nightManual(false);
  else if (args && *args && strcmp(args, "status")) {
    logMsg("night: usage night [on|off|start|end|status]");
    return;
  }
  nightStatus(st, sizeof st);
  float idle = pcStat("pc.idle_s");
  logMsg("night: %s | PC idle %s%.0f s, input %.0f px / %.0f presses, display request %.0f", st,
         pcStat("idle.ok") >= 1 ? "" : "(stale) ", isnan(idle) ? -1.0f : idle, pcStat("pc.in_px"),
         pcStat("pc.in_press"), pcStat("pc.disp_req"));
}
