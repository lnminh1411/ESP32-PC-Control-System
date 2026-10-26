#include "power.h"

#include <esp_random.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "config.h"
#include "discord.h"
#include "display.h"
#include "log.h"
#include "net.h"
#include "pc_state.h"
#include "pcstats.h"
#include "sys.h"
#include "usb.h"
#include "views.h"
#include "secrets_or_example.h"
#include "wol.h"

static const char *powerWakeSend();

static bool s_pending = false;
static char s_action[16] = "";
static char s_nonce[16] = "";
static uint32_t s_reqTimeMs = 0;

const char *powerRequest(const char *action) {
  if (!action || !*action) return "action required";

  bool isSleep = !strcmp(action, "sleep");
  bool isHibernate = !strcmp(action, "hibernate");
  bool isRestart = !strcmp(action, "restart");
  bool isShutdown = !strcmp(action, "shutdown");

  if (!isSleep && !isHibernate && !isRestart && !isShutdown) {
    return "unknown action (must be sleep, hibernate, restart, shutdown)";
  }

  if (pcState() != PC_ON && !pcStatsFresh()) {
    return "PC not on";
  }

  if (s_pending) {
    return "another request pending";
  }

  if (pcStatsFresh()) {
    uint32_t r = esp_random();
    snprintf(s_nonce, sizeof(s_nonce), "%lx", (unsigned long)r);
    strncpy(s_action, action, sizeof(s_action) - 1);
    s_action[sizeof(s_action) - 1] = '\0';

    char line[64];
    snprintf(line, sizeof(line), "!power %s %s\n", action, s_nonce);
    if (!usbHidSendLine(line)) {
      s_pending = false;
      s_nonce[0] = '\0';
      return "USB HID send failed";
    }

    s_pending = true;
    s_reqTimeMs = nowMs();
    logMsg("power: requesting %s (nonce %s)", action, s_nonce);
    return nullptr;
  }

  // Fallback without the service
  if (isSleep) {
    logMsg("power: sending sleep key (Windows applies power/sleep button settings)");
    if (!usbSystemControl(2)) {
      return "USB System Control sleep failed";
    }
    viewsNoteAction("PC sleep (key)");
    discordNotify("PC sleep (key)");
    return nullptr;
  } else if (isShutdown) {
    logMsg("power: sending power down key (Windows applies power/sleep button settings)");
    if (!usbSystemControl(1)) {
      return "USB System Control power down failed";
    }
    viewsNoteAction("PC shutdown (key)");
    discordNotify("PC shutdown (key)");
    return nullptr;
  } else {
    return "needs the PC stats service";
  }
}

void powerPoll() {
  if (!s_pending) return;

  const char *ack = pcText("power.ack");
  if (ack && *ack) {
    char w1[32], w2[32];
    if (sscanf(ack, "%31s %31s", w1, w2) == 2) {
      if (!strcmp(w1, s_action) && !strcasecmp(w2, s_nonce)) {
        logMsg("power: PC accepted %s", s_action);
        char note[48];
        snprintf(note, sizeof(note), "PC accepted %s", s_action);
        viewsNoteAction(note);
        discordNotify(note);
        s_pending = false;
        s_nonce[0] = '\0';
        return;
      } else if (!strcmp(w1, "error") && !strcasecmp(w2, s_nonce)) {
        const char *reason = strstr(ack, s_nonce);
        if (reason) {
          reason += strlen(s_nonce);
          while (*reason == ' ') reason++;
        }
        if (!reason || !*reason) reason = "refused";
        logMsg("power: PC refused %s (%s)", s_action, reason);
        char note[48];
        snprintf(note, sizeof(note), "PC refused %s", s_action);
        viewsNoteAction(note);
        discordNotify(note);
        s_pending = false;
        s_nonce[0] = '\0';
        return;
      }
    }
  }

  if (nowMs() - s_reqTimeMs >= POWER_ACK_TIMEOUT_MS) {
    if (!strcmp(s_action, "sleep")) {
      logMsg("power: no answer from PC service, falling back to sleep key (Windows applies power/sleep button settings)");
      if (usbSystemControl(2)) {
        viewsNoteAction("PC sleep (key)");
        discordNotify("PC sleep (key)");
      } else {
        logMsg("power: fallback sleep key failed");
        viewsNoteAction("PC sleep failed");
        discordNotify("PC sleep failed");
      }
    } else if (!strcmp(s_action, "shutdown")) {
      logMsg("power: no answer from PC service, falling back to power down key (Windows applies power/sleep button settings)");
      if (usbSystemControl(1)) {
        viewsNoteAction("PC shutdown (key)");
        discordNotify("PC shutdown (key)");
      } else {
        logMsg("power: fallback power down key failed");
        viewsNoteAction("PC shutdown failed");
        discordNotify("PC shutdown failed");
      }
    } else {
      logMsg("power: no answer from the PC service");
      char note[48];
      snprintf(note, sizeof(note), "power %s timed out", s_action);
      viewsNoteAction(note);
      discordNotify(note);
    }
    s_pending = false;
    s_nonce[0] = '\0';
  }
}

const char *powerWake() {
  if (!WAKE_DIM_BACKLIGHTS) return powerWakeSend();
  // Motherboard quirk (WAKE_DIM_BACKLIGHTS in secrets.h): the PC's power-on
  // drags the USB 5 V rail down and the board browns out. Backlights off while
  // the wake goes out, back WAKE_DIM_MS later.
  bool wasIdle = backlightIsIdle();
  backlightIdle(true);
  logMsg("wake: backlights off for the wake");
  const char *err = powerWakeSend();
  sleepMs(WAKE_DIM_MS);
  backlightIdle(wasIdle);
  logMsg("wake: backlights restored");
  return err;
}

static const char *powerWakeSend() {
  bool usbAttempted = false;
  bool usbOk = false;
  if (usbSuspended()) {
    usbAttempted = true;
    usbOk = usbRemoteWakeup();
    logMsg("wake: usb remote wakeup %s", usbOk ? "sent" : "failed");
  }

  bool wolAttempted = false;
  const char *wolErr = nullptr;
  if (netUp()) {
    wolAttempted = true;
    wolErr = wolSend();
  }

  if ((usbAttempted && usbOk) || (wolAttempted && !wolErr)) {
    viewsNoteAction("wake PC");
    discordNotify("wake PC");
    return nullptr;
  }

  static char errBuf[128];
  const char *mac = wolMacStr();
  bool hasMac = (mac && *mac);

  if (!usbAttempted && !wolAttempted) {
    if (!netUp() && !hasMac) {
      snprintf(errBuf, sizeof(errBuf), "no MAC known, network down and USB not suspended");
    } else if (!hasMac) {
      snprintf(errBuf, sizeof(errBuf), "no MAC known and USB not suspended");
    } else {
      snprintf(errBuf, sizeof(errBuf), "network down and USB not suspended");
    }
  } else if (!usbAttempted) {
    snprintf(errBuf, sizeof(errBuf), "USB not suspended, WoL: %s", wolErr ? wolErr : "failed");
  } else if (!wolAttempted) {
    if (!netUp()) {
      snprintf(errBuf, sizeof(errBuf), "USB remote wakeup %s, network down", usbOk ? "sent" : "failed");
    } else {
      snprintf(errBuf, sizeof(errBuf), "USB remote wakeup %s, no MAC known", usbOk ? "sent" : "failed");
    }
  } else {
    snprintf(errBuf, sizeof(errBuf), "USB remote wakeup %s, WoL: %s", usbOk ? "sent" : "failed", wolErr ? wolErr : "failed");
  }
  return errBuf;
}
