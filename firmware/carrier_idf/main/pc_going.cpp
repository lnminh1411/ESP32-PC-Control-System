#include "pc_going.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "discord.h"
#include "display.h"
#include "log.h"
#include "pc_state.h"
#include "pcstats.h"
#include "sys.h"
#include "views.h"

static GoingKind latchedKind = GO_NONE;
static char latchedBy[64] = "";
static char latchedWhy[80] = "";
static uint32_t announcementMs = 0;
static uint64_t lastLatchedId = 0;
static bool pcWentOff = false;
static bool fromHost = false;               // injected latches ignore the host's "none"
static uint32_t offStartMs = 0;

static char latchedLastDown[32] = "";
static char latchedLastDownAt[32] = "";

static bool holdingScreens = false;
static View savedViews[N_PANELS];
static uint64_t injectIdCounter = 900000;

static GoingKind parseKind(const char *s) {
  if (!s || !*s) return GO_NONE;
  if (!strcmp(s, "restart")) return GO_RESTART;
  if (!strcmp(s, "shutdown")) return GO_SHUTDOWN;
  if (!strcmp(s, "update_restart")) return GO_UPDATE_RESTART;
  if (!strcmp(s, "update_shutdown")) return GO_UPDATE_SHUTDOWN;
  if (!strcmp(s, "sleep")) return GO_SLEEP;
  return GO_NONE;
}

// Discord messages are capped at NOTIFY_MSG_LEN; cutting a long reason is fine
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
static void buildNotifyMsg(char *buf, size_t size, GoingKind k, const char *by, const char *why) {
  const char *action;
  switch (k) {
    case GO_RESTART: action = "PC restarting"; break;
    case GO_UPDATE_RESTART: action = "PC updating and restarting"; break;
    case GO_UPDATE_SHUTDOWN: action = "PC updating and shutting down"; break;
    case GO_SHUTDOWN: action = "PC shutting down"; break;
    case GO_SLEEP: action = "PC going to sleep"; break;
    default: action = "PC going"; break;
  }
  bool hasBy = by && by[0];
  bool hasWhy = why && why[0];
  if (hasBy && hasWhy) {
    snprintf(buf, size, "%s -- %s: %s", action, by, why);
  } else if (hasBy) {
    snprintf(buf, size, "%s -- %s", action, by);
  } else if (hasWhy) {
    snprintf(buf, size, "%s -- %s", action, why);
  } else {
    snprintf(buf, size, "%s", action);
  }
}
#pragma GCC diagnostic pop

static void syncScreenHold() {
  bool holdNow = pcGoingHoldsScreens();
  if (holdNow && !holdingScreens) {
    holdingScreens = true;
    // auto views show restart/update themselves; only the old views switch to status
    for (int i = 0; i < N_PANELS; i++) {
      savedViews[i] = panels[i].view;
      if (!viewIsAuto(savedViews[i])) viewEnter(i, VIEW_STATUS);
    }
    viewsRefreshIdle();
  } else if (!holdNow && holdingScreens) {
    holdingScreens = false;
    for (int i = 0; i < N_PANELS; i++) {
      if (!viewIsAuto(savedViews[i])) viewEnter(i, savedViews[i]);
    }
    viewsRefreshIdle();
  }
}

static void clearLatch() {
  latchedKind = GO_NONE;
  latchedBy[0] = '\0';
  latchedWhy[0] = '\0';
  announcementMs = 0;
  pcWentOff = false;
  offStartMs = 0;
  syncScreenHold();
}

GoingKind pcGoing() {
  return latchedKind;
}

const char *pcGoingLabel() {
  switch (latchedKind) {
    case GO_RESTART: return "RESTARTING";
    case GO_UPDATE_RESTART:
    case GO_UPDATE_SHUTDOWN: return "UPDATING";
    case GO_SHUTDOWN: return "SHUTTING DOWN";
    case GO_SLEEP: return "GOING TO SLEEP";
    default: return "";
  }
}

const char *pcGoingBy() {
  return latchedBy;
}

const char *pcGoingWhy() {
  return latchedWhy;
}

uint32_t pcGoingAgeMs() {
  if (latchedKind == GO_NONE || !announcementMs) return 0;
  return nowMs() - announcementMs;
}

uint32_t pcGoingLeftMs() {
  if (latchedKind != GO_RESTART &&
      latchedKind != GO_UPDATE_RESTART &&
      latchedKind != GO_UPDATE_SHUTDOWN) {
    return 0;
  }
  uint32_t windowMs = (latchedKind == GO_RESTART) ? (180 * 1000) : (45 * 60 * 1000);
  if (pcWentOff) {
    uint32_t elapsed = nowMs() - offStartMs;
    return elapsed < windowMs ? (windowMs - elapsed) : 0;
  }
  return windowMs;
}

bool pcGoingHoldsScreens() {
  if (pcState() != PC_OFF) return false;
  return latchedKind == GO_RESTART ||
         latchedKind == GO_UPDATE_RESTART ||
         latchedKind == GO_UPDATE_SHUTDOWN;
}

const char *pcGoingWakeBlock() {
  if (latchedKind != GO_RESTART &&
      latchedKind != GO_UPDATE_RESTART &&
      latchedKind != GO_UPDATE_SHUTDOWN) {
    return nullptr;
  }
  static char blockBuf[64];
  uint32_t leftMs = pcGoingLeftMs();
  uint32_t mins = (leftMs + 59999) / 60000;
  if (mins == 0) mins = 1;
  if (latchedKind == GO_RESTART) {
    snprintf(blockBuf, sizeof blockBuf, "PC is restarting (back in ~%lu min)", (unsigned long)mins);
  } else {
    snprintf(blockBuf, sizeof blockBuf, "PC is installing updates (%lu min)", (unsigned long)mins);
  }
  return blockBuf;
}

const char *pcLastDown() {
  return latchedLastDown;
}

const char *pcLastDownAt() {
  return latchedLastDownAt;
}

void pcGoingOnSample() {
  const char *ld = pcText("pc.last_down");
  if (ld && *ld && strcmp(ld, "-") != 0) {
    snprintf(latchedLastDown, sizeof latchedLastDown, "%s", ld);
  }
  const char *lda = pcText("pc.last_down_at");
  if (lda && *lda && strcmp(lda, "-") != 0) {
    snprintf(latchedLastDownAt, sizeof latchedLastDownAt, "%s", lda);
  }

  double gid = pcStat("pc.going_id");

  if (latchedKind != GO_NONE && fromHost && !pcWentOff && !isnan(gid) && gid == 0.0) {
    // id back to 0 without USB ever dropping: either the PC rebooted fast
    // (uptime younger than the announcement) or the shutdown was aborted
    double up = pcStat("uptime");
    bool rebooted = !isnan(up) && up * 1000.0 < (double)pcGoingAgeMs();
    bool backKind = latchedKind == GO_RESTART || latchedKind == GO_UPDATE_RESTART;
    if (rebooted && backKind) {
      char buf[64];
      snprintf(buf, sizeof buf, "PC back after restart (%lu s)", (unsigned long)(pcGoingAgeMs() / 1000));
      logMsg("pc: %s", buf);
      discordNotify(buf);
    } else if (!rebooted) {
      logMsg("pc: shutdown cancelled");
    }
    clearLatch();
    lastLatchedId = 0;
    return;
  }

  if (!isnan(gid) && gid != 0.0) {
    uint64_t idVal = (uint64_t)gid;
    if (idVal != lastLatchedId) {
      const char *gText = pcText("pc.going");
      GoingKind k = parseKind(gText);
      if (k != GO_NONE) {
        lastLatchedId = idVal;
        latchedKind = k;
        fromHost = true;
        const char *by = pcText("pc.going_by");
        if (by && *by && strcmp(by, "-") != 0) {
          snprintf(latchedBy, sizeof latchedBy, "%s", by);
        } else {
          latchedBy[0] = '\0';
        }
        const char *why = pcText("pc.going_why");
        if (why && *why && strcmp(why, "-") != 0) {
          snprintf(latchedWhy, sizeof latchedWhy, "%s", why);
        } else {
          latchedWhy[0] = '\0';
        }
        announcementMs = nowMs();
        pcWentOff = false;
        offStartMs = 0;

        char notifyBuf[128];
        buildNotifyMsg(notifyBuf, sizeof notifyBuf, latchedKind, latchedBy, latchedWhy);
        logMsg("pc: %s", notifyBuf);
        discordNotify(notifyBuf);
        syncScreenHold();
      }
    }
  }
}

void pcGoingPoll() {
  syncScreenHold();

  if (latchedKind == GO_NONE) {
    return;
  }

  PcState st = pcState();
  if (!pcWentOff) {
    if (st == PC_OFF) {
      pcWentOff = true;
      offStartMs = nowMs();
      syncScreenHold();
    } else {
      if (announcementMs && (uint32_t)(nowMs() - announcementMs) >= 120000) {
        logMsg("pc: announcement timed out (PC still on)");
        clearLatch();
        return;
      }
    }
  } else {
    if (latchedKind == GO_RESTART || latchedKind == GO_UPDATE_RESTART) {
      uint32_t windowMs = (latchedKind == GO_RESTART) ? (180 * 1000) : (45 * 60 * 1000);
      if (st == PC_ON) {
        uint32_t offSec = (nowMs() - offStartMs) / 1000;
        char buf[64];
        snprintf(buf, sizeof buf, "PC back after restart (%lu s)", (unsigned long)offSec);
        logMsg("pc: %s", buf);
        discordNotify(buf);
        clearLatch();
      } else if ((uint32_t)(nowMs() - offStartMs) >= windowMs) {
        logMsg("pc: did not come back");
        clearLatch();
      }
    } else if (latchedKind == GO_UPDATE_SHUTDOWN) {
      uint32_t windowMs = 45 * 60 * 1000;
      if (st == PC_ON) {
        clearLatch();
      } else if ((uint32_t)(nowMs() - offStartMs) >= windowMs) {
        clearLatch();
      }
    } else if (latchedKind == GO_SHUTDOWN || latchedKind == GO_SLEEP) {
      if ((uint32_t)(nowMs() - offStartMs) >= 5000) {
        clearLatch();
      }
    }
  }
}

void pcGoingInject(const char *kind) {
  if (!kind || !strcmp(kind, "clear")) {
    clearLatch();
    return;
  }
  GoingKind k = parseKind(kind);
  if (k == GO_NONE) return;

  latchedKind = k;
  snprintf(latchedBy, sizeof latchedBy, "test");
  snprintf(latchedWhy, sizeof latchedWhy, "injected");
  announcementMs = nowMs();
  pcWentOff = false;
  offStartMs = 0;
  lastLatchedId = ++injectIdCounter;
  fromHost = false;

  char notifyBuf[128];
  buildNotifyMsg(notifyBuf, sizeof notifyBuf, latchedKind, latchedBy, latchedWhy);
  logMsg("pc: %s", notifyBuf);
  discordNotify(notifyBuf);
  syncScreenHold();
}
