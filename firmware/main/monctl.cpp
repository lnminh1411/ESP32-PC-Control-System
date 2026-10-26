#include "monctl.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "discord.h"
#include "log.h"
#include "pcstats.h"
#include "sys.h"
#include "usb.h"

static uint32_t s_nextId = 0;   // Discord task and console; a rare clash only reuses an id
static char s_lastLogged[121] = "";

const char *monSend(const char *command, uint32_t *id) {
  if (!command || !*command) return "command required";
  if (!pcStatsFresh()) return "PC service not connected";
  if (strlen(command) > 64) return "command too long";

  // seeded from uptime so a reboot doesn't replay an id the service still repeats
  if (!s_nextId) s_nextId = nowMs() & 0xffff;
  uint32_t n = ++s_nextId;
  char line[96];
  snprintf(line, sizeof line, "!mon %lx %s\n", (unsigned long)n, command);
  if (!usbPcSendLine(line)) return "USB send failed";
  if (id) *id = n;
  return nullptr;
}

// Copies monctl.reply's text into out when it answers request id
static bool replyFor(uint32_t id, char *out, size_t outSize) {
  char buf[121];
  strncpy(buf, pcText("monctl.reply"), sizeof buf - 1);   // copy: the table changes under us
  buf[sizeof buf - 1] = 0;
  char *end;
  unsigned long got = strtoul(buf, &end, 16);
  if (end == buf || *end != ' ' || got != id) return false;
  snprintf(out, outSize, "%s", end + 1);
  return true;
}

bool monRun(const char *command, uint32_t timeoutMs, char *out, size_t outSize) {
  uint32_t id;
  const char *err = monSend(command, &id);
  if (err) { snprintf(out, outSize, "err %s", err); return false; }
  uint32_t t0 = nowMs();
  while ((uint32_t)(nowMs() - t0) < timeoutMs) {
    if (replyFor(id, out, outSize)) return !strncmp(out, "ok", 2);
    discordWaitTick();   // the bot task keeps acking clicks and heartbeating
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  snprintf(out, outSize, "err no answer from the PC");
  return false;
}

void monPoll() {
  const char *r = pcText("monctl.reply");
  if (!*r || !strcmp(r, s_lastLogged)) return;
  strncpy(s_lastLogged, r, sizeof s_lastLogged - 1);
  s_lastLogged[sizeof s_lastLogged - 1] = 0;
  logMsg("mon: #%s", s_lastLogged);
}

void monConsole(const char *command) {
  while (*command == ' ' || *command == '\t') command++;
  if (!*command) { logMsg("mon: usage mon <bright <n|all> <0-100> | power <n|all> on|off>"); return; }
  uint32_t id;
  const char *err = monSend(command, &id);
  if (err) logMsg("mon: %s", err);
  else logMsg("mon: sent #%lx %s", (unsigned long)id, command);
}
