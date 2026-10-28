#include "aictl.h"

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

const char *aiSend(const char *command, uint32_t *id) {
  if (!command || !*command) return "command required";
  if (!pcStatsFresh()) return "PC service not connected";
  if (strlen(command) > 96) return "command too long";

  // seeded from uptime so a reboot doesn't replay an id the service still repeats
  if (!s_nextId) s_nextId = nowMs() & 0xffff;
  uint32_t n = ++s_nextId;
  char line[128];
  snprintf(line, sizeof line, "!ai %lx %s\n", (unsigned long)n, command);
  if (!usbPcSendLine(line)) return "USB send failed";
  if (id) *id = n;
  return nullptr;
}

// Copies aictl.reply's text into out when it answers request id
static bool replyFor(uint32_t id, char *out, size_t outSize) {
  char buf[121];
  pcTextCopy("aictl.reply", buf, sizeof buf);   // the bot task reads it too: copied under the table's lock
  char *end;
  unsigned long got = strtoul(buf, &end, 16);
  if (end == buf || *end != ' ' || got != id) return false;
  snprintf(out, outSize, "%s", end + 1);
  return true;
}

bool aiWaitReply(uint32_t id, uint32_t timeoutMs, char *out, size_t outSize) {
  uint32_t t0 = nowMs();
  while ((uint32_t)(nowMs() - t0) < timeoutMs) {
    if (replyFor(id, out, outSize)) return true;
    discordWaitTick();   // the bot task keeps acking clicks and heartbeating
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  snprintf(out, outSize, "err no answer from the PC");
  return false;
}

bool aiRun(const char *command, uint32_t timeoutMs, char *out, size_t outSize) {
  uint32_t id;
  const char *err = aiSend(command, &id);
  if (err) { snprintf(out, outSize, "err %s", err); return false; }
  return aiWaitReply(id, timeoutMs, out, outSize) && !strncmp(out, "ok", 2);
}

void aiPoll() {
  const char *r = pcText("aictl.reply");
  if (!*r || !strcmp(r, s_lastLogged)) return;
  strncpy(s_lastLogged, r, sizeof s_lastLogged - 1);
  s_lastLogged[sizeof s_lastLogged - 1] = 0;
  logMsg("ai: #%s", s_lastLogged);
}

void aiConsole(const char *command) {
  while (*command == ' ' || *command == '\t') command++;
  if (!*command) { logMsg("ai: usage ai <status | model load|unload|... | restart <name> | ...>"); return; }
  uint32_t id;
  const char *err = aiSend(command, &id);
  if (err) logMsg("ai: %s", err);
  else logMsg("ai: sent #%lx %s", (unsigned long)id, command);
}
