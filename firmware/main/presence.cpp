#include "presence.h"

#include <math.h>
#include <nvs.h>
#include <stdio.h>
#include <string.h>

#include "discord.h"
#include "log.h"
#include "monctl.h"
#include "night.h"
#include "owntracks.h"
#include "pc_state.h"
#include "pcstats.h"
#include "power.h"
#include "settings_or_example.h"
#include "sys.h"
#include "usb.h"
#include "weather.h"

// The last check. The bot task reads them for the panel: words, set whole
static volatile int seen = PRES_UNKNOWN, confPct;
static volatile uint32_t checks, checkedMs;
static char why[12] = "-";
static int lastN = -1;                   // pc.present's count
static int nobodyRun;                    // checks in a row that saw nobody

static volatile bool away;               // phone beyond PRESENCE_AWAY_KM
static int simAway = -1;                 // test: 1 away, 0 home, -1 the phone decides
static bool watching;                    // night mode asked for fast checks
static bool dry;                         // test: log instead of acting (RAM only)
static int sentEvery = -1;               // cadence the helper has, -1 = none
static uint32_t sentMs;

static bool deskOff;                     // nobody at the desk by day: Windows' display off
static uint32_t nobodySinceMs;           // by day: the first of the checks that saw nobody, 0 = none
static bool nativeOff;                   // our Windows display off went out
static uint32_t nativeTryMs;             // the last one failed: again 30 s later
static bool lowByDesk;                   // low power mode is on because nobody was at the desk
static bool awayInput;                   // input while away: displays back until it stops
static bool offSent;                     // our displays off (DDC, away) went out
static uint32_t offAtMs, offTryMs;       // input newer than offAtMs gives them back

static volatile bool enabled = true;     // the camera checks and what follows from them (saved, PC Control button)
static bool enabledLoaded;

static int yesRun;                       // someone seen while away, in a row (unknown doesn't end it)
static uint32_t alertedMs, alertPendingMs;

static bool idleKnown(float *idle) {
  *idle = pcStat("pc.idle_s");
  return pcStatsFresh() && pcStat("idle.ok") >= 1 && !isnan(*idle);
}

// Away: two checks in a row before anything happens, the second right away.
// The lock goes first and costs nothing; the alert waits for a recent fix.
static void intruderCheck(uint32_t now) {
  if (!away) { yesRun = 0; return; }
  if (seen == PRES_NOBODY) yesRun = 0;
  if (seen != PRES_SOMEONE) return;
  if (!yesRun) {
    yesRun = 1;
    presenceCheckNow();
    return;
  }
  yesRun = 2;
  if (dry) logMsg("presence: dry, would lock the PC");
  else {
    const char *err = powerLockQuiet();
    if (err) logMsg("presence: lock not sent (%s)", err);
  }
  if (alertPendingMs || (alertedMs && now - alertedMs < PRESENCE_ALERT_REPEAT_S * 1000UL)) return;
  alertPendingMs = now | 1;
  logMsg("presence: someone at the desk while away (%d %%), PC locked", (int)confPct);
  if (simAway < 0 && owntracksAgeSec() > PRESENCE_FIX_FRESH_S) owntracksAskNow();
}

static void alertFollow(uint32_t now) {
  if (!alertPendingMs) return;
  uint32_t age = simAway < 0 ? owntracksAgeSec() : 0;
  if (age > PRESENCE_FIX_FRESH_S && now - alertPendingMs < PRESENCE_ASK_WAIT_S * 1000UL) return;
  alertPendingMs = 0;
  alertedMs = now | 1;
  char what[140], stale[40] = "";
  if (age > PRESENCE_FIX_FRESH_S) snprintf(stale, sizeof stale, " Last phone fix %lu min old.", (unsigned long)(age / 60));
  snprintf(what, sizeof what, "Someone is at the desk while you are away (%d %% sure). PC locked.%s", (int)confPct, stale);
  if (dry) logMsg("presence: dry, would alert: %s", what);
  else discordPost(DC_PC, "Someone at your PC", what, 0xE74C3C, 2);
}

// The desk in use again (or the PC off, or you home): Windows' display off ends
// (displaysFollow), and so does the low power mode it turned on
static void deskBack(const char *why) {
  nobodySinceMs = 0;
  if (deskOff) logMsg("presence: %s, displays on", why);
  deskOff = false;
  if (!lowByDesk) return;
  lowByDesk = false;
  if (dry) logMsg("presence: dry, would end low power mode");
  else nightLowPower(false, why);
}

// Low power mode with the displays, unless something holds sleep; tried again
// at each check that still sees nobody
static void deskLowPower() {
  static char lastHold[64];
  if (!deskOff || lowByDesk || nightLowPowerOn()) return;   // on by hand: stays the user's
  char hold[64];
  nightSleepHold(hold, sizeof hold);
  if (*hold) {
    if (strcmp(hold, lastHold)) logMsg("presence: low power mode held, %s", hold);
    snprintf(lastHold, sizeof lastHold, "%s", hold);
    return;
  }
  lastHold[0] = 0;
  lowByDesk = true;
  if (dry) logMsg("presence: dry, would turn low power mode on");
  else nightLowPower(true, "with nobody at the desk");
}

// By day at home: nobody seen and no input for PRESENCE_DAY_OFF_S, nothing
// keeping the display on: Windows' display off (no DDC/CI: the monitors keep
// their power and wake with any input) and low power mode
static void deskCheck(uint32_t now) {
  if (seen == PRES_SOMEONE) { deskBack("with someone at the desk"); return; }
  if (seen != PRES_NOBODY || away || nightEngaged() || nightCovers()) return;
  if (!nobodySinceMs) nobodySinceMs = now | 1;
  if (deskOff) { deskLowPower(); return; }
  if (now - nobodySinceMs + 5000 < PRESENCE_DAY_OFF_S * 1000UL) return;   // 5 s: check timing
  float idle;
  if (!idleKnown(&idle) || idle < PRESENCE_DAY_OFF_S || pcStat("pc.disp_req") >= 1) return;
  deskOff = true;
  offAtMs = now;
  logMsg("presence: nobody at the desk for %d min, displays off", PRESENCE_DAY_OFF_S / 60);
  deskLowPower();
}

static void onCheck(int answer, int conf, const char *whyText) {
  uint32_t now = nowMs();
  seen = answer;
  confPct = conf;
  snprintf(why, sizeof why, "%s", whyText);
  checkedMs = now ? now : 1;
  checks = checks + 1;
  nobodyRun = answer == PRES_NOBODY ? nobodyRun + 1 : 0;
  intruderCheck(now);
  deskCheck(now);
}

static void readCheck() {
  char text[40], word[8], whyText[12];
  int n, conf;
  pcTextCopy("pc.present", text, sizeof text);
  if (sscanf(text, "%d %7s %11s %d", &n, word, whyText, &conf) != 4 || n == lastN) return;
  bool first = lastN < 0;   // an answer from before a board restart: shown, not acted on
  lastN = n;
  int answer = !strcmp(word, "yes") ? PRES_SOMEONE : !strcmp(word, "no") ? PRES_NOBODY : PRES_UNKNOWN;
  if (!first) { onCheck(answer, conf, whyText); return; }
  seen = answer;
  confPct = conf;
  snprintf(why, sizeof why, "%s", whyText);
  uint32_t now = nowMs();
  checkedMs = now ? now : 1;
}

// Home under PRESENCE_HOME_KM, away over PRESENCE_AWAY_KM, the last answer
// between; a vague fix switches neither way. No valid fix: not away.
static void phoneFollow() {
  bool was = away, is = was;
  double lat, lon;
  if (simAway >= 0) is = simAway;
  else if (!owntracksEnabled() || !owntracksValid() || !weatherHomeLocation(&lat, &lon)) is = false;
  else {
    double d = owntracksDistanceTo(lat, lon);
    uint32_t acc = 0;
    owntracksGetLocation(nullptr, nullptr, nullptr, &acc);
    if (d >= 0 && acc <= PRESENCE_ACC_M) {
      if (d > PRESENCE_AWAY_KM) is = true;
      else if (d < PRESENCE_HOME_KM) is = false;
    }
  }
  if (is == was) return;
  away = is;
  logMsg("presence: phone %s", is ? "away from home" : "back home");
  if (is) {   // the away rule takes the displays; low power mode stays
    deskOff = false;
    nobodySinceMs = 0;
  } else {
    deskBack("with you back home");
  }
  awayInput = false;
  yesRun = 0;
  alertPendingMs = alertedMs = 0;
}

static void displaysFollow(uint32_t now) {
  float idle;
  bool known = idleKnown(&idle);
  if (offAtMs && known && idle + 3 < (now - offAtMs) / 1000.0f) {   // 3 s for the feed's lag
    offAtMs = 0;
    deskBack("with input on the PC");
    if (away) {
      awayInput = true;
      logMsg("presence: input on the PC while away, displays on until %d min without input", PRESENCE_AWAY_IDLE_S / 60);
    }
  }
  if (awayInput && known && idle >= PRESENCE_AWAY_IDLE_S) awayInput = false;
  if (deskOff != nativeOff && pcReachable() && (!nativeTryMs || now - nativeTryMs >= 30000)) {
    const char *cmd = deskOff ? "power all sleep" : "power all wake";
    const char *err = nullptr;
    uint32_t id;
    if (dry) logMsg("presence: dry, would send %s", cmd);
    else err = monSend(cmd, &id);
    if (err) {
      nativeTryMs = now | 1;
      logMsg("presence: %s: %s, again in 30 s", cmd, err);
    } else {
      nativeOff = deskOff;
      nativeTryMs = 0;
    }
  }
  if (!presenceDisplaysOff()) {
    if (offSent) {
      offSent = false;
      offAtMs = 0;
      if (dry) logMsg("presence: dry, would turn the displays on");
      else nightDisplaysOnNow();
    }
    return;
  }
  if (!pcReachable()) {   // comes back on with the PC: off again then
    offSent = false;
    return;
  }
  if (offSent || (offTryMs && now - offTryMs < 30000)) return;
  offTryMs = now | 1;
  const char *err = nullptr;
  if (dry) logMsg("presence: dry, would turn the displays off");
  else err = nightDisplaysOff();
  if (err) { logMsg("presence: displays off: %s, again in 30 s", err); return; }
  offSent = true;
  if (!offAtMs) offAtMs = now;
}

static void loadEnabled() {
  enabledLoaded = true;
  nvs_handle_t h;
  uint8_t v;
  if (nvs_open("presence", NVS_READONLY, &h) != ESP_OK) return;
  if (nvs_get_u8(h, "on", &v) == ESP_OK) enabled = v != 0;
  nvs_close(h);
}

void presenceSetEnabled(bool on) {
  if (!enabledLoaded) loadEnabled();
  if (on == enabled) return;
  enabled = on;
  nvs_handle_t h;
  if (nvs_open("presence", NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_u8(h, "on", on ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
  }
  // off: no answer stands; the desk rule gives the displays back
  seen = PRES_UNKNOWN;
  nobodyRun = yesRun = 0;
  alertPendingMs = 0;
  if (!on) deskBack("with presence detection off");
  sentEvery = -1;   // the new cadence (or none) goes to the helper now
  logMsg("presence: detection %s", on ? "on" : "off");
}

bool presenceEnabled() {
  if (!enabledLoaded) loadEnabled();
  return enabled;
}

void presencePoll() {
  static uint32_t lastMs;
  uint32_t now = nowMs();
  if (now - lastMs < 1000) return;
  lastMs = now;
  if (!enabledLoaded) loadEnabled();

  phoneFollow();
  alertFollow(now);
  if (pcState() != PC_ON || !pcStatsFresh()) {
    sentEvery = -1;
    nobodyRun = yesRun = 0;
    if (pcState() != PC_ON) deskBack("with the PC off");
    displaysFollow(now);
    return;
  }
  if (enabled) readCheck();
  displaysFollow(now);
  // off: `!pres 0`, the helper stops asking the camera
  int every = !enabled ? 0 : away || watching ? PRESENCE_FAST_S : PRESENCE_DAY_S;
  if (every == sentEvery && now - sentMs < 30000) return;
  char line[24];
  snprintf(line, sizeof line, "!pres %d\n", every);
  if (usbPcSendLine(line)) {
    sentEvery = every;
    sentMs = now;
  }
}

void presenceWatch(bool on) { watching = on; }
void presenceCheckNow() { if (enabled) usbPcSendLine("!pres now\n"); }
uint32_t presenceChecks() { return checks; }
int presenceSeen() { return enabled ? seen : PRES_UNKNOWN; }
bool presenceNobody() { return enabled && nobodyRun >= 2 && nowMs() - checkedMs < 30000; }
bool presenceAway() { return away; }
bool presenceDisplaysOff() { return away && !awayInput; }
bool presenceDry() { return dry; }

void presenceStatus(char *buf, uint32_t size) {
  if (!enabled) { snprintf(buf, size, "off"); return; }
  if (pcState() != PC_ON) { snprintf(buf, size, "PC not on"); return; }
  if (!checkedMs) { snprintf(buf, size, "no check yet"); return; }
  char what[40], ago[16];
  if (seen == PRES_SOMEONE) snprintf(what, sizeof what, "someone at the desk (%d %%)", (int)confPct);
  else if (seen == PRES_NOBODY) snprintf(what, sizeof what, "nobody at the desk");
  else snprintf(what, sizeof what, "can't tell (%s)", why);
  uint32_t age = (nowMs() - checkedMs) / 1000;
  if (age < 120) snprintf(ago, sizeof ago, "%lu s ago", (unsigned long)age);
  else snprintf(ago, sizeof ago, "%lu min ago", (unsigned long)(age / 60));
  snprintf(buf, size, "%s | %s%s", what, ago, away ? " | you are away" : "");
}

void presenceConsole(const char *a1, const char *a2) {
  if (a1 && !a2 && (!strcmp(a1, "on") || !strcmp(a1, "off"))) {
    presenceSetEnabled(!strcmp(a1, "on"));
    return;
  }
  if (a1 && a2 && !strcmp(a1, "away")) {
    simAway = !strcmp(a2, "on") ? 1 : !strcmp(a2, "off") ? 0 : -1;
  } else if (a1 && a2 && !strcmp(a1, "seen")) {
    onCheck(!strcmp(a2, "yes") ? PRES_SOMEONE : !strcmp(a2, "no") ? PRES_NOBODY : PRES_UNKNOWN, 50, "test");
  } else if (a1 && a2 && !strcmp(a1, "dry")) {
    dry = !strcmp(a2, "on");
  } else if (a1) {
    logMsg("presence: usage presence [on | off | away on|off|auto | seen yes|no|unknown | dry on|off]");
    return;
  }
  char st[96];
  presenceStatus(st, sizeof st);
  logMsg("presence: %s | checks %lu, every %d s%s%s, displays off %s", st, (unsigned long)checks, sentEvery,
         simAway >= 0 ? ", phone simulated" : "", dry ? ", dry" : "", presenceDisplaysOff() ? "wanted" : "no");
}
