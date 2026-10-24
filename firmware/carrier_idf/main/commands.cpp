#include "commands.h"

#include <ctype.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "discord.h"
#include "display.h"
#include "gif_store.h"
#include "log.h"
#include "net.h"
#include "optos.h"
#include "pc_state.h"
#include "pcstats.h"
#include "sys.h"
#include "views.h"

typedef void (*Handler)(const char *a1, const char *a2);

struct Command {
  const char *name;
  const char *usage;   // arguments, for help and usage errors
  Handler run;
};

static void cmdHelp(const char *, const char *);

// perf: CPU per core + busiest tasks since the previous `perf` (run it twice).
// Diffs FreeRTOS run-time counters, so it never blocks the main loop.
static void cmdPerf(const char *, const char *) {
  static TaskStatus_t prev[40];
  static UBaseType_t nPrev;
  static uint32_t prevUs, prevSpin;
  extern uint32_t displaySpinUs;
  uint32_t spin = displaySpinUs - prevSpin;
  prevSpin = displaySpinUs;
  TaskStatus_t cur[40];
  uint32_t total;
  UBaseType_t n = uxTaskGetSystemState(cur, 40, &total);
  uint32_t nowUsVal = nowUs(), dt = nowUsVal - prevUs;
  if (!prevUs) { logMsg("perf: baseline taken, run again"); }
  else {
    uint32_t idle[2] = {0, 0};
    for (UBaseType_t i = 0; i < n; i++) {
      uint32_t d = cur[i].ulRunTimeCounter;
      for (UBaseType_t j = 0; j < nPrev; j++)
        if (prev[j].xHandle == cur[i].xHandle) { d -= prev[j].ulRunTimeCounter; break; }
      if (!strncmp(cur[i].pcTaskName, "IDLE", 4)) idle[cur[i].pcTaskName[4] == '1'] = d;
      else if (d * 200ull > dt)   // >0.5% of one core
        logMsg("  %-16s core %d  %5.1f%%", cur[i].pcTaskName, (int)xTaskGetCoreID(cur[i].xHandle), d * 100.0f / dt);
    }
    logMsg("perf %.1fs: core0 %.1f%%  core1 %.1f%%  spi %.0f%%", dt / 1e6f,
           100 - idle[0] * 100.0f / dt, 100 - idle[1] * 100.0f / dt, busLoadPct());
    logMsg("  flush DMA wait %.1f%%  fps %.1f / %.1f / %.1f", spin * 100.0f / dt,
           panels[0].fps, panels[1].fps, panels[2].fps);
    logMsg("  canvas internal %d/%d/%d  internal heap free %uK", panels[0].canvasInternal,
           panels[1].canvasInternal, panels[2].canvasInternal,
           (unsigned)(freeInternal() / 1024));
    float kb = panels[0].kbps + panels[1].kbps + panels[2].kbps;
    logMsg("  wire %.0f KB/s = %.1f%% of %d MHz", kb, kb * 1024 * 8 * 100 / SPI_HZ, SPI_HZ / 1000000);
  }
  memcpy(prev, cur, n * sizeof *cur);
  nPrev = n;
  prevUs = nowUsVal;
}

static void cmdStatus(const char *, const char *) {
  LedMode led = ledMode();
  logMsg("pc %s%s | bus %.0f%% | bl %u/%u/%u%% %s | door %s | led %s",
         pcStateName(pcState()), bootGuardLeftMs() ? " (guard)" : "", busLoadPct(),
         backlightPct(0), backlightPct(1), backlightPct(2), backlightIsIdle() ? "(idle: off)" : "",
         doorPressed() ? "pressing" : "idle", led == LED_AUTO ? "auto" : led == LED_ON ? "on" : led == LED_SWEEP ? "sweep" : "off");
  for (int i = 0; i < N_PANELS; i++)
    logMsg("  panel %d: %-6s %5.1f fps %6.1f KB/s  bus %5.1f ms/s", i + 1, viewName(panels[i].view),
           panels[i].fps, panels[i].kbps, panels[i].busMs);
  logMsg("  gif: %u frames %ux%u", gif.n, gif.w, gif.h);
  logMsg("  wifi %s %d dBm | discord %s", netUp() ? netIp() : "down", netRssi(),
         discordUp() ? "online" : "offline");
  logMsg("  pc stats: %d numbers, %d texts, %s", pcStatsCount(), pcTextCount(), pcStatsFresh() ? "live" : "no data");
}

// pcstats [text]: stats whose key contains text (case-insensitive)
static void cmdPcStats(const char *a1, const char *) {
  int n = pcStatsCount();
  for (int i = 0; i < n; i++) {
    const char *k = pcStatKey(i);
    if (!k || (a1 && !strcasestr(k, a1))) continue;
    double v = pcStatAt(i);
    if (v == floor(v) && fabs(v) <= 9007199254740992.0) logMsg("  %-23s  %16.0f", k, v);
    else logMsg("  %-23s  %16.3f", k, v);
  }
  int tn = pcTextCount();
  for (int i = 0; i < tn; i++) {
    const char *k = pcTextKey(i);
    if (!k || (a1 && !strcasestr(k, a1))) continue;
    logMsg("  %-23s  \"%s\"", k, pcTextAt(i));
  }
  logMsg("pcstats: %d numbers, %d texts, %s", n, tn, pcStatsFresh() ? "live" : "no data");
}

static void cmdView(const char *a1, const char *a2) {
  int n = a1 ? atoi(a1) : 0;
  View v;
  if (n < 1 || n > N_PANELS || !a2 || !viewParse(a2, v)) { logMsg("view: usage view <1|2|3> <gif|status|stats|metrics>"); return; }
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
  discordNotify(what);
}

static void cmdReset(const char *, const char *) {
  const char *err = moboReset();
  if (err) { logMsg("reset: refused, %s", err); return; }
  logMsg("reset: motherboard RST pulsed for %d ms", MOBO_RST_MS);
  viewsNoteAction("PC reset");
  discordNotify("PC reset");
}

// bl <n|all> <0-100|on|off>: "on" brings back the last non-zero level
static void cmdBacklight(const char *a1, const char *a2) {
  static uint8_t lastOn[N_PANELS] = {100, 100, 100};
  bool on = a2 && !strcmp(a2, "on"), off = a2 && !strcmp(a2, "off");
  int pct = on || off ? 0 : a2 && isdigit((unsigned char)*a2) ? atoi(a2) : -1;
  if (!a1 || (!on && !off && (pct < 0 || pct > 100))) { logMsg("bl: usage bl <1|2|3|all> <0-100|on|off>"); return; }
  for (int i = 0; i < N_PANELS; i++) {
    if (strcmp(a1, "all") && atoi(a1) != i + 1) continue;
    if (backlightPct(i)) lastOn[i] = backlightPct(i);
    int v = on ? lastOn[i] : pct;
    backlightSet(i, v);
    if (v) lastOn[i] = v;
  }
  logMsg("bl: %u/%u/%u%%", backlightPct(0), backlightPct(1), backlightPct(2));
}

// led <auto|on|off|sweep|0-100>: a number sets the brightness of every "on"
static void cmdLed(const char *a1, const char *) {
  if (a1 && !strcmp(a1, "on")) ledModeSet(LED_ON);
  else if (a1 && !strcmp(a1, "off")) ledModeSet(LED_OFF);
  else if (a1 && !strcmp(a1, "auto")) ledModeSet(LED_AUTO);
  else if (a1 && !strcmp(a1, "sweep")) ledModeSet(LED_SWEEP);
  else if (a1 && isdigit((unsigned char)*a1) && atoi(a1) <= 100) ledBrightnessSet(atoi(a1));
  else { logMsg("led: usage led <auto|on|off|sweep|0-100>"); return; }
  logMsg("led: %s (brightness %u%%)", a1, ledBrightness());
}

static const Command COMMANDS[] = {
  {"help",    "",                                    cmdHelp},
  {"status",  "",                                    cmdStatus},
  {"view",    "<1|2|3> <gif|status|stats|metrics>", cmdView},
  {"door",    "<1-4>",                               cmdDoor},
  {"reset",   "",                                    cmdReset},
  {"bl",      "<1|2|3|all> <0-100|on|off>",          cmdBacklight},
  {"led",     "<auto|on|off|sweep|0-100>",           cmdLed},
  {"perf",    "",                                    cmdPerf},
  {"pcstats", "[text]",                              cmdPcStats},
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
