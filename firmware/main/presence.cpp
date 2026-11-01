#include "presence.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "discord.h"
#include "log.h"
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

static bool deskOff;                     // nobody at the desk by day
static bool awayInput;                   // input while away: displays back until it stops
static bool offSent;                     // our displays off went out
static uint32_t offAtMs, offTryMs;       // input newer than offAtMs gives them back

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

static void deskCheck(uint32_t now) {
  if (seen == PRES_SOMEONE) {
    if (deskOff) logMsg("presence: someone at the desk, displays on");
    deskOff = false;
    return;
  }
  if (seen != PRES_NOBODY || deskOff || away || nightEngaged() || nightCovers()) return;
  float idle;
  if (!idleKnown(&idle) || idle < PRESENCE_DAY_IDLE_S || pcStat("pc.disp_req") >= 1) return;
  deskOff = true;
  offAtMs = now;
  logMsg("presence: nobody at the desk, displays off");
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
  deskOff = false;
  awayInput = false;
  yesRun = 0;
  alertPendingMs = alertedMs = 0;
}

static void displaysFollow(uint32_t now) {
  float idle;
  bool known = idleKnown(&idle);
  if (offAtMs && known && idle + 3 < (now - offAtMs) / 1000.0f) {   // 3 s for the feed's lag
    offAtMs = 0;
    if (deskOff) logMsg("presence: input on the PC, displays on");
    deskOff = false;
    if (away) {
      awayInput = true;
      logMsg("presence: input on the PC while away, displays on until %d min without input", PRESENCE_AWAY_IDLE_S / 60);
    }
  }
  if (awayInput && known && idle >= PRESENCE_AWAY_IDLE_S) awayInput = false;
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

void presencePoll() {
  static uint32_t lastMs;
  uint32_t now = nowMs();
  if (now - lastMs < 1000) return;
  lastMs = now;

  phoneFollow();
  alertFollow(now);
  if (pcState() != PC_ON || !pcStatsFresh()) {
    sentEvery = -1;
    nobodyRun = yesRun = 0;
    displaysFollow(now);
    return;
  }
  readCheck();
  displaysFollow(now);
  int every = away || watching ? PRESENCE_FAST_S : PRESENCE_DAY_S;
  if (every == sentEvery && now - sentMs < 30000) return;
  char line[24];
  snprintf(line, sizeof line, "!pres %d\n", every);
  if (usbPcSendLine(line)) {
    sentEvery = every;
    sentMs = now;
  }
}

void presenceWatch(bool on) { watching = on; }
void presenceCheckNow() { usbPcSendLine("!pres now\n"); }
uint32_t presenceChecks() { return checks; }
int presenceSeen() { return seen; }
bool presenceNobody() { return nobodyRun >= 2 && nowMs() - checkedMs < 30000; }
bool presenceAway() { return away; }
bool presenceDisplaysOff() { return deskOff || (away && !awayInput); }
bool presenceDry() { return dry; }

void presenceStatus(char *buf, uint32_t size) {
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
  if (a1 && a2 && !strcmp(a1, "away")) {
    simAway = !strcmp(a2, "on") ? 1 : !strcmp(a2, "off") ? 0 : -1;
  } else if (a1 && a2 && !strcmp(a1, "seen")) {
    onCheck(!strcmp(a2, "yes") ? PRES_SOMEONE : !strcmp(a2, "no") ? PRES_NOBODY : PRES_UNKNOWN, 50, "test");
  } else if (a1 && a2 && !strcmp(a1, "dry")) {
    dry = !strcmp(a2, "on");
  } else if (a1) {
    logMsg("presence: usage presence [away on|off|auto | seen yes|no|unknown | dry on|off]");
    return;
  }
  char st[96];
  presenceStatus(st, sizeof st);
  logMsg("presence: %s | checks %lu, every %d s%s%s, displays off %s", st, (unsigned long)checks, sentEvery,
         simAway >= 0 ? ", phone simulated" : "", dry ? ", dry" : "", presenceDisplaysOff() ? "wanted" : "no");
}
