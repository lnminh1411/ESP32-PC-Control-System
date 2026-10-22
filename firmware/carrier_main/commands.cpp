#include "commands.h"

#include <Arduino.h>

#include "config.h"
#include "display.h"
#include "gif_store.h"
#include "log.h"
#include "optos.h"
#include "pc_state.h"
#include "views.h"

typedef void (*Handler)(const char *a1, const char *a2);

struct Command {
  const char *name;
  const char *usage;   // arguments, for help and usage errors
  Handler run;
};

static void cmdHelp(const char *, const char *);

static void cmdStatus(const char *, const char *) {
  LedMode led = ledMode();
  logMsg("pc %s%s | bus %.0f%% | bl %u/%u/%u%% %s | door %s | led %s",
         pcStateName(pcState()), bootGuardLeftMs() ? " (guard)" : "", busLoadPct(),
         backlightPct(0), backlightPct(1), backlightPct(2), backlightIsIdle() ? "(idle: off)" : "",
         doorPressed() ? "pressing" : "idle", led == LED_AUTO ? "auto" : led == LED_ON ? "on" : "off");
  for (int i = 0; i < N_PANELS; i++)
    logMsg("  panel %d: %-6s %5.1f fps %6.1f KB/s  bus %5.1f ms/s", i + 1, viewName(panels[i].view),
           panels[i].fps, panels[i].kbps, panels[i].busMs);
  logMsg("  gif: %u frames %ux%u", gif.n, gif.w, gif.h);
}

static void cmdView(const char *a1, const char *a2) {
  int n = a1 ? atoi(a1) : 0;
  View v;
  if (n < 1 || n > N_PANELS || !a2 || !viewParse(a2, v)) { logMsg("view: usage view <1|2|3> <gif|status|stats>"); return; }
  viewEnter(n - 1, v);
  logMsg("view: panel %d -> %s", n, a2);
}

static void cmdDoor(const char *a1, const char *) {
  const char *err = doorPress(a1 ? atoi(a1) : 0);
  if (err) { logMsg("door: refused, %s", err); return; }
  logMsg("door: B%s pressed for %d ms", a1, DOOR_PRESS_MS);
  char what[16];
  snprintf(what, sizeof what, "door B%s", a1);
  viewsNoteAction(what);
}

static void cmdReset(const char *, const char *) {
  const char *err = moboReset();
  if (err) { logMsg("reset: refused, %s", err); return; }
  logMsg("reset: motherboard RST pulsed for %d ms", MOBO_RST_MS);
  viewsNoteAction("PC reset");
}

static void cmdBacklight(const char *a1, const char *a2) {
  int pct = a2 ? atoi(a2) : -1;
  if (!a1 || pct < 0 || pct > 100) { logMsg("bl: usage bl <1|2|3|all> <0-100>"); return; }
  for (int i = 0; i < N_PANELS; i++)
    if (!strcmp(a1, "all") || atoi(a1) == i + 1) backlightSet(i, pct);
  logMsg("bl: %u/%u/%u%%", backlightPct(0), backlightPct(1), backlightPct(2));
}

static void cmdLed(const char *a1, const char *) {
  if (a1 && !strcmp(a1, "on")) ledModeSet(LED_ON);
  else if (a1 && !strcmp(a1, "off")) ledModeSet(LED_OFF);
  else if (a1 && !strcmp(a1, "auto")) ledModeSet(LED_AUTO);
  else { logMsg("led: usage led <auto|on|off>"); return; }
  logMsg("led: %s", a1);
}

static const Command COMMANDS[] = {
  {"help",   "",                               cmdHelp},
  {"status", "",                               cmdStatus},
  {"view",   "<1|2|3> <gif|status|stats>",     cmdView},
  {"door",   "<1-4>",                          cmdDoor},
  {"reset",  "",                               cmdReset},
  {"bl",     "<1|2|3|all> <0-100>",            cmdBacklight},
  {"led",    "<auto|on|off>",                  cmdLed},
};

static void cmdHelp(const char *, const char *) {
  char buf[200];
  int n = snprintf(buf, sizeof buf, "commands:");
  for (const Command &c : COMMANDS)
    n += snprintf(buf + n, sizeof buf - n, " %s%s%s%s", &c == COMMANDS ? "" : "| ", c.name, *c.usage ? " " : "", c.usage);
  logMsg("%s", buf);
}

void commandRun(char *line) {
  char *cmd = strtok(line, " \t");
  if (!cmd) return;
  char *a1 = strtok(nullptr, " \t"), *a2 = strtok(nullptr, " \t");
  for (const Command &c : COMMANDS)
    if (!strcmp(cmd, c.name)) { c.run(a1, a2); return; }
  logMsg("unknown command '%s' (try help)", cmd);
}
