#include "night.h"

#include <math.h>
#include <nvs.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "board_time.h"
#include "discord.h"
#include "display.h"
#include "hwctl.h"
#include "log.h"
#include "monctl.h"
#include "pc_state.h"
#include "pcstats.h"
#include "power.h"
#include "screens.h"
#include "settings_or_example.h"
#include "sys.h"
#include "usb.h"
#include "views.h"

static const int IDLE_ENGAGE_S = NIGHT_IDLE_ENGAGE_S;
static const int START_HOUR = NIGHT_START_HOUR, END_HOUR = NIGHT_END_HOUR;
static const uint8_t PANEL_PCT = NIGHT_PANEL_PCT, STATUS_LED_PCT = NIGHT_STATUS_LED_PCT, RGB_PCT = NIGHT_RGB_PCT;

static bool enabled;
static bool engaged;
static bool manual;                      // started by hand: no window, no idle needed
static uint32_t engagedAtMin;            // minutes since midnight, for the status line
static uint32_t engagedMs;               // input newer than this ends it
// Wake guard: input during the night only ends it if the next 10 s show a
// real return (mouse moved > 50 px, or 3 key / button presses); otherwise
// the displays go off again and the night carries on
static const uint32_t GUARD_MS = NIGHT_GUARD_MS;
static const double GUARD_PX = NIGHT_GUARD_PX, GUARD_PRESSES = NIGHT_GUARD_PRESSES;
static uint32_t guardUntilMs;            // 0 = no wake being checked
static double guardPx0, guardPress0;     // pc.in_px / pc.in_press when the wake was seen
static uint32_t falseWakes;              // this night, for the status line

// Sleep and hibernate (in the night window, while engaged): NIGHT_SLEEP_S
// after the last real input on the PC, NIGHT_HIBERNATE_S for hibernate.
// False wakes don't move the clock. Held while an exe is heavy (pc.heavy),
// a power request keeps the PC up (pc.disp_req / sys_req / exec_req, minus
// NIGHT_REQ_IGNORE), or the network is busy (net_down / net_up over
// NIGHT_NET_HOLD_BPS, ~1 min average); the displays stay off meanwhile. A PC
// night mode put to sleep is woken and hibernated (NIGHT_HIBERNATE_S -
// NIGHT_SLEEP_S) later, after 06:00 too, unless someone powers it on first.
static const uint32_t SLEEP_MS = NIGHT_SLEEP_S * 1000UL, HIBERNATE_MS = NIGHT_HIBERNATE_S * 1000UL;
static const uint32_t POWER_WAIT_MS = 3 * 60 * 1000;   // for the PC to go down, or to come up when woken
static uint32_t inputAtMs;               // last real input on the PC
static uint32_t sleepReqMs, hibReqMs;    // asked for, PC not down yet
static uint32_t sleptAtMs;               // asleep for night mode: hibernate due later
static uint32_t wokeMs;                  // woken by us to hibernate
static bool offByNight;                  // asleep or hibernating because of night mode
static uint32_t powerTryMs;              // last sleep / hibernate request
static float netAvg;                     // bytes/s, the busier of net_down / net_up, ~1 min average
static char holdWhy[64];                 // why sleep waits, "" = it doesn't

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

void nightWindowText(char *buf, uint32_t size) {
  snprintf(buf, size, "%02d:00-%02d:00 after %d min", START_HOUR, END_HOUR, IDLE_ENGAGE_S / 60);
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

static void disengage(const char *why);

// "claude.exe" in NIGHT_REQ_IGNORE
static bool reqIgnored(const char *by) {
  if (!by || !*by) return false;
  const char *list = NIGHT_REQ_IGNORE;
  size_t n = strlen(by);
  for (const char *p = list; *p;) {
    while (*p == ' ' || *p == ',') p++;
    const char *e = p;
    while (*e && *e != ',') e++;
    const char *t = e;
    while (t > p && t[-1] == ' ') t--;
    if ((size_t)(t - p) == n && !strncasecmp(p, by, n)) return true;
    p = e;
  }
  return false;
}

// a power request that holds sleep: one from an ignored holder doesn't (the
// helper only names the first holder, so two or more always hold)
static bool reqHolds(const char *key, const char *byKey) {
  float n = pcStat(key);
  if (isnan(n) || n < 1) return false;
  return !(n < 2 && reqIgnored(pcText(byKey)));
}

// "" when sleep and hibernate may go ahead, else why not
static const char *sleepHold() {
  if (pcStat("pc.heavy") >= 1) snprintf(holdWhy, sizeof holdWhy, "%.40s busy", pcText("pc.heavy_by"));
  else if (pcStat("pc.disp_req") >= 1) snprintf(holdWhy, sizeof holdWhy, "%.40s keeps the display on", pcText("pc.disp_req_by"));
  else if (reqHolds("pc.sys_req", "pc.sys_req_by")) snprintf(holdWhy, sizeof holdWhy, "%.40s keeps the PC up", pcText("pc.sys_req_by"));
  else if (reqHolds("pc.exec_req", "pc.exec_req_by")) snprintf(holdWhy, sizeof holdWhy, "%.40s keeps running", pcText("pc.exec_req_by"));
  else if (netAvg > NIGHT_NET_HOLD_BPS) snprintf(holdWhy, sizeof holdWhy, "network %.1f MB/s", netAvg / 1e6f);
  else holdWhy[0] = 0;
  return holdWhy;
}

static void netAverage() {
  if (!pcStatsFresh()) return;
  float d = pcStat("net_down"), u = pcStat("net_up");
  float v = isnan(d) ? 0 : d;
  if (!isnan(u) && u > v) v = u;
  netAvg += (v - netAvg) / 60;   // polled every second
}

// While engaged, in the window, no wake being checked: sleep, or hibernate
static void sleepCheck(uint32_t now) {
  static char lastHold[64];
  if (sleepReqMs || hibReqMs || sleptAtMs || offByNight) return;
  if (pcState() != PC_ON || !pcStatsFresh()) return;
  uint32_t inactive = now - inputAtMs;
  if (inactive < SLEEP_MS) { lastHold[0] = 0; return; }
  const char *why = sleepHold();
  if (strcmp(why, lastHold)) {
    if (*why) logMsg("night: sleep held, %s", why);
    else if (*lastHold) logMsg("night: sleep no longer held");
    strncpy(lastHold, why, sizeof lastHold - 1);
  }
  if (*why || now - powerTryMs < 60000) return;
  powerTryMs = now;
  bool hib = inactive >= HIBERNATE_MS;
  const char *err = powerRequest(hib ? "hibernate" : "sleep");
  if (err) { logMsg("night: %s not sent (%s)", hib ? "hibernate" : "sleep", err); return; }
  if (hib) hibReqMs = now;
  else sleepReqMs = now;
  logMsg("night: %lu min without input, %s", (unsigned long)(inactive / 60000), hib ? "hibernating the PC" : "putting the PC to sleep");
}

// Every second, engaged or not: did the PC go down, is a hibernate due, did
// someone power it on
static void powerFollow(uint32_t now) {
  bool on = pcState() == PC_ON;
  if (sleepReqMs) {
    if (!on) {
      sleepReqMs = 0;
      sleptAtMs = now;
      offByNight = true;
      char what[96];
      snprintf(what, sizeof what, "PC asleep; it is woken and hibernated in %d min unless someone powers it on",
               (int)((HIBERNATE_MS - SLEEP_MS) / 60000));
      logMsg("night: %s", what);
      announce(what, 0x2C3E50);
    } else if (now - sleepReqMs > POWER_WAIT_MS) {
      sleepReqMs = 0;
      logMsg("night: the PC did not go to sleep");
    }
  }
  if (hibReqMs) {
    if (!on) {
      hibReqMs = 0;
      sleptAtMs = 0;
      wokeMs = 0;
      offByNight = true;
      logMsg("night: PC hibernating");
      announce("PC hibernating, until someone powers it on", 0x2C3E50);
    } else if (now - hibReqMs > POWER_WAIT_MS) {
      hibReqMs = 0;
      logMsg("night: the PC did not hibernate");
    }
  }
  if (wokeMs) {
    if (on && pcStatsFresh()) {
      if (now - powerTryMs >= 5000) {
        powerTryMs = now;
        const char *err = powerRequest("hibernate");
        if (!err) { hibReqMs = now; wokeMs = 0; logMsg("night: woken, hibernating"); }
      }
    } else if (now - wokeMs > POWER_WAIT_MS) {
      wokeMs = 0;
      sleptAtMs = 0;
      logMsg("night: the PC %s after the wake, hibernate dropped", on ? "came up without the PC service" : "did not come up");
    }
    return;
  }
  if (on && offByNight && !sleepReqMs && !hibReqMs) {
    // up again without us: someone powered it on
    offByNight = false;
    sleptAtMs = 0;
    logMsg("night: PC powered on");
    if (engaged) disengage("PC powered on");
    return;
  }
  if (sleptAtMs && !on && now - sleptAtMs >= HIBERNATE_MS - SLEEP_MS) {
    sleptAtMs = 0;
    wokeMs = now;
    logMsg("night: asleep for %d min, waking the PC to hibernate it", (int)((HIBERNATE_MS - SLEEP_MS) / 60000));
    const char *err = powerWake();
    if (err) logMsg("night: wake: %s", err);
  }
}

static void engage(bool byHand) {
  engaged = true;
  manual = byHand;
  engagedMs = nowMs();
  guardUntilMs = 0;
  falseWakes = 0;
  float idle = pcStat("pc.idle_s");
  inputAtMs = engagedMs - (!isnan(idle) && idle > 0 && idle < 86400 ? (uint32_t)(idle * 1000) : 0);
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
  char rgbPct[8];
  snprintf(rgbPct, sizeof rgbPct, "%d %%", RGB_PCT);
  logMsg("night: engaged%s (PC LEDs %s, displays %s)", byHand ? " by hand" : "",
         rgbLowered ? rgbPct : "left alone", err ? err : "off");
  char what[160];
  snprintf(what, sizeof what, "Engaged%s: PC LEDs %s, board panels %d %% on the PC-off screens, status LED %d %%, PC displays %s",
           byHand ? " by hand" : "", rgbLowered ? rgbPct : "unchanged", PANEL_PCT, STATUS_LED_PCT, err ? "not reached" : "off");
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

  // the PC (back) on: the night state at once, so `!night 0` gives a PC
  // that slept or hibernated through the morning its day config back
  static uint32_t lassoMs;
  static bool wasFresh;
  bool fresh = pcStatsFresh();
  if ((fresh && !wasFresh) || now - lassoMs >= NIGHT_LASSO_REPEAT_MS) { lassoMs = now; lassoSend(); }
  wasFresh = fresh;
  netAverage();
  powerFollow(now);
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
    } else if (idleKnown && idle + 3 < since && !wokeMs && !hibReqMs) {
      guardPx0 = pcStat("pc.in_px");
      guardPress0 = pcStat("pc.in_press");
      if (isnan(guardPx0) || isnan(guardPress0)) {
        disengage("input on the PC");   // a helper without the counters: no guard
      } else {
        guardUntilMs = now + GUARD_MS;
        logMsg("night: input on the PC, checking for 10 s");
      }
    } else if (inWindow) {
      sleepCheck(now);
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
  char win[40], what[96];
  nightWindowText(win, sizeof win);
  snprintf(what, sizeof what, "Turned on: engages %s without input on the PC", win);
  announce(on ? what : "Turned off", 0x95A5A6);
  if (!on && engaged) disengage("turned off");
}

void nightManual(bool on) {
  if (on && !engaged) engage(true);
  else if (!on && engaged) disengage("ended by hand");
}

bool nightEnabled() { return enabled; }
bool nightEngaged() { return engaged; }

void nightStatus(char *buf, uint32_t size) {
  uint32_t now = nowMs();
  if (sleptAtMs || wokeMs)
    snprintf(buf, size, "PC asleep, hibernates %s", wokeMs ? "now" : "later");
  else if (engaged) {
    char sleep[48] = "";
    uint32_t inactive = now - inputAtMs;
    if (*sleepHold() && inactive >= SLEEP_MS) snprintf(sleep, sizeof sleep, ", sleep held: %.28s", holdWhy);
    else if (inactive < SLEEP_MS) snprintf(sleep, sizeof sleep, ", sleep in %lu min", (unsigned long)((SLEEP_MS - inactive) / 60000 + 1));
    snprintf(buf, size, "engaged since %02lu:%02lu%s%s%s", (unsigned long)(engagedAtMin / 60),
             (unsigned long)(engagedAtMin % 60), manual ? ", by hand" : "",
             guardUntilMs ? ", checking a wake" : falseWakes ? ", false wakes ignored" : "", sleep);
  }
  else if (enabled && pcStat("pc.disp_req") >= 1)
    snprintf(buf, size, "on, held off: %.20s keeps the display on", pcText("pc.disp_req_by"));
  else if (enabled) {
    char win[40];
    nightWindowText(win, sizeof win);
    snprintf(buf, size, "on: %s idle%s", win, rgbLowered ? ", LED restore owed" : "");
  }
  else
    snprintf(buf, size, "off%s", rgbLowered ? ", LED restore owed" : "");
}

void nightConsole(const char *args) {
  while (args && *args == ' ') args++;
  char st[128];
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
  logMsg("night: %s | PC idle %s%.0f s, input %.0f px / %.0f presses, requests display %.0f system %.0f execution %.0f, heavy %.0f, net %.2f MB/s | %s",
         st, pcStat("idle.ok") >= 1 ? "" : "(stale) ", isnan(idle) ? -1.0f : idle, pcStat("pc.in_px"),
         pcStat("pc.in_press"), pcStat("pc.disp_req"), pcStat("pc.sys_req"), pcStat("pc.exec_req"), pcStat("pc.heavy"),
         netAvg / 1e6f, *sleepHold() ? holdWhy : "sleep not held");
}
