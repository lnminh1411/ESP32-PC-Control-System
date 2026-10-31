#include "commands.h"

#include <ctype.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <mbedtls/base64.h>
#include <LovyanGFX.hpp>
#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bench.h"
#include "config.h"
#include "discord.h"
#include "display.h"
#include "layout.h"
#include "weather.h"
#include "gif_store.h"
#include "log.h"
#include "net.h"
#include "optos.h"
#include "pc_going.h"
#include "pc_state.h"
#include "pcstats.h"
#include "pcwarn.h"
#include "screens.h"
#include "power.h"
#include "aictl.h"
#include "monctl.h"
#include "hwctl.h"
#include "usb.h"
#include "wol.h"
#include "sys.h"
#include "views.h"
#include "night.h"

typedef void (*Handler)(const char *a1, const char *a2);

struct Command {
  const char *name;
  const char *usage;   // arguments, for help and usage errors
  Handler run;
};

static void cmdHelp(const char *, const char *);

// socks: every open lwIP socket with its local and remote address
static void cmdWifi(const char *a1, const char *) {
  if (a1 && !strcmp(a1, "scan")) netScan();
  else logMsg("wifi: usage wifi scan");
}

static void cmdSocks(const char *, const char *) {
  int n = 0;
  for (int fd = LWIP_SOCKET_OFFSET; fd < LWIP_SOCKET_OFFSET + CONFIG_LWIP_MAX_SOCKETS; fd++) {
    int type = 0;
    socklen_t tl = sizeof type;
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tl) != 0) continue;
    struct sockaddr_in me = {}, peer = {};
    socklen_t ml = sizeof me, pl = sizeof peer;
    getsockname(fd, (struct sockaddr *)&me, &ml);
    bool hasPeer = getpeername(fd, (struct sockaddr *)&peer, &pl) == 0;
    char a[16], b[16] = "-";
    inet_ntoa_r(me.sin_addr, a, sizeof a);
    if (hasPeer) inet_ntoa_r(peer.sin_addr, b, sizeof b);
    logMsg("sock %d %s %s:%u -> %s:%u", fd, type == SOCK_STREAM ? "tcp" : type == SOCK_DGRAM ? "udp" : "raw",
           a, ntohs(me.sin_port), b, hasPeer ? ntohs(peer.sin_port) : 0);
    n++;
  }
  logMsg("socks: %d of %d open | internal RAM free %u, largest %u", n, CONFIG_LWIP_MAX_SOCKETS,
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

// perf: CPU per core + busiest tasks since the previous `perf` (run it twice).
// Diffs FreeRTOS run-time counters, so it never blocks the main loop.
static void cmdPerf(const char *, const char *) {
  static TaskStatus_t prev[40];
  static UBaseType_t nPrev;
  static uint32_t prevUs, prevSpin, prevDec, prevDecN;
  extern uint32_t displaySpinUs;
  uint32_t spin = displaySpinUs - prevSpin;
  prevSpin = displaySpinUs;
  uint32_t dec = gifDecodeUs - prevDec, decN = gifDecodeN - prevDecN;
  prevDec = gifDecodeUs;
  prevDecN = gifDecodeN;
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
    if (decN) logMsg("  gif inflate %.1f%% of core 1, %.1f ms a frame", dec * 100.0f / dt, dec / 1000.0f / decN);
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
  char goingBuf[48] = "";
  if (pcGoing() != GO_NONE) {
    snprintf(goingBuf, sizeof goingBuf, " | going %s %lus", pcGoingLabel(), (unsigned long)(pcGoingAgeMs() / 1000));
  }
  logMsg("pc %s%s | bus %.0f%% | bl %u/%u/%u%% %s | door %s | led %s%s",
         pcStateName(pcState()), bootGuardLeftMs() ? " (guard)" : "", busLoadPct(),
         backlightPct(0), backlightPct(1), backlightPct(2), backlightIsIdle() ? "(idle: off)" : "",
         doorPressed() ? "pressing" : "idle", led == LED_AUTO ? "auto" : led == LED_ON ? "on" : led == LED_SWEEP ? "sweep" : "off",
         goingBuf);
  for (int i = 0; i < N_PANELS; i++)
    logMsg("  panel %d: %-6s %5.1f fps %6.1f KB/s  bus %5.1f ms/s", i + 1, viewName(panels[i].view),
           panels[i].fps, panels[i].kbps, panels[i].busMs);
  logMsg("  gif: %u GIFs, %u frames %ux%u, %u KB pack in flash, %u KB PSRAM", gif.nSeg, gif.n, gif.w, gif.h,
         (unsigned)(gif.packBytes / 1024), (unsigned)(gif.bytes / 1024));
  logMsg("  wifi %s %d dBm | discord %s", netUp() ? netIp() : "down", netRssi(),
         discordUp() ? "online" : "offline");
  logMsg("  pc stats: %d numbers, %d texts, %s", pcStatsCount(), pcTextCount(), pcStatsFresh() ? "live" : "no data");
  const char *mac = wolMacStr();
  logMsg("  wol mac %s | usb %s", (mac && *mac) ? mac : "unknown", usbSuspended() ? "suspended" : "active");
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
  if (n < 1 || n > N_PANELS || !a2 || !viewParse(a2, v)) { logMsg("view: usage view <1|2|3> <pc|media|ai|gif|status|stats|metrics|dash>"); return; }
  viewEnter(n - 1, v);
  displaySaveView(n - 1, v);
  viewsBoost(n - 1);
  logMsg("view: panel %d -> %s", n, a2);
}

static void cmdDoor(const char *a1, const char *a2) {
  int ms = a2 ? atoi(a2) : DOOR_PRESS_MS;   // some keyfobs need a longer hold
  if (ms < 50 || ms > 5000) { logMsg("door: usage door <1-4> [50-5000 ms]"); return; }
  const char *err = doorPress(a1 ? atoi(a1) : 0, (uint32_t)ms);
  if (err) { logMsg("door: refused, %s", err); return; }
  logMsg("door: B%s pressed for %d ms", a1, ms);
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

static void cmdWake(const char *a1, const char *a2) {
  if (a1 && !strcmp(a1, "mac")) {
    if (!a2 || !wolSetMac(a2)) {
      logMsg("wake: usage wake mac <AA:BB:CC:DD:EE:FF>");
      return;
    }
    return;
  }
  bool force = (a1 && !strcmp(a1, "force"));
  if (a1 && !force) {
    logMsg("wake: usage wake [force | mac <AA:BB:CC:DD:EE:FF>]");
    return;
  }
  if (!force) {
    const char *block = pcGoingWakeBlock();
    if (block) {
      logMsg("wake: refused, %s", block);
      return;
    }
  }
  const char *err = powerWake();
  if (err) {
    logMsg("wake: refused, %s", err);
    return;
  }
  logMsg("wake: wake initiated");
}

static void cmdGoing(const char *a1, const char *) {
  if (!a1 || (strcmp(a1, "restart") &&
              strcmp(a1, "shutdown") &&
              strcmp(a1, "update_restart") &&
              strcmp(a1, "update_shutdown") &&
              strcmp(a1, "sleep") &&
              strcmp(a1, "clear"))) {
    logMsg("going: usage going <restart|shutdown|update_restart|update_shutdown|sleep|clear>");
    return;
  }
  pcGoingInject(a1);
}

static void cmdPower(const char *a1, const char *) {
  if (!a1) {
    logMsg("power: usage power <sleep|hibernate|restart|shutdown|lock>");
    return;
  }
  const char *err = powerRequest(a1);
  if (err) {
    logMsg("power: refused, %s", err);
    return;
  }
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
    displaySaveBacklight(i, v);
    viewsBoost(i);
    if (v) lastOn[i] = v;
  }
  logMsg("bl: %u/%u/%u%%", backlightPct(0), backlightPct(1), backlightPct(2));
}

// pcsim <on|off|auto>: pretend the PC is on or off (LED, faces, backlights)
static void cmdPcSim(const char *a1, const char *) {
  if (a1 && !strcmp(a1, "on")) pcStateSimulate(PC_ON);
  else if (a1 && !strcmp(a1, "off")) pcStateSimulate(PC_OFF);
  else if (a1 && !strcmp(a1, "auto")) pcStateSimulate(-1);
  else { logMsg("pcsim: usage pcsim <on|off|auto>"); return; }
  logMsg("pcsim: PC %s", pcStateName(pcState()));
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

static void cmdFps(const char *a1, const char *a2) {
  if (!a1) {
    for (int i = 0; i < N_PANELS; i++) {
      Panel &p = panels[i];
      uint8_t cap = viewsPanelCap(i);
      char capBuf[16];
      if (cap >= 1 && cap <= 60) snprintf(capBuf, sizeof(capBuf), "%u", cap);
      else snprintf(capBuf, sizeof(capBuf), "20");

      if (p.gifMode) {
        logMsg("panel %d: %-7s %-6s target %-3s %4.1f fps %5.0f KB/s  deferred %lu  stretch %.2f",
               i + 1, viewName(p.view), viewsPanelPriorityName(i), capBuf,
               p.fps, p.kbps, (unsigned long)p.deferred, p.gifStretch);
      } else {
        logMsg("panel %d: %-7s %-6s target %-3s %4.1f fps %5.0f KB/s  deferred %lu",
               i + 1, viewName(p.view), viewsPanelPriorityName(i), capBuf,
               p.fps, p.kbps, (unsigned long)p.deferred);
      }
    }
    logMsg("budget %u%% | bus load %.0f%% | bucket %d B",
           viewsBusBudget(), busLoadPct(), (int)viewsBucketLevel());
    return;
  }
  if (!strcmp(a1, "budget")) {
    if (!a2 || !isdigit((unsigned char)*a2)) {
      logMsg("fps: usage fps budget <20-95>");
      return;
    }
    int b = atoi(a2);
    if (b < 20 || b > 95) {
      logMsg("fps: budget must be 20-95");
      return;
    }
    viewsSetBusBudget((uint8_t)b);
    logMsg("fps: budget set to %d%%", b);
    return;
  }
  if (!strcmp(a1, "cap")) {
    int p = a2 ? atoi(a2) : 0;
    char *a3 = strtok(nullptr, " \t");
    if (p < 1 || p > N_PANELS || !a3) {
      logMsg("fps: usage fps cap <1|2|3> <1-60|off>");
      return;
    }
    if (!strcmp(a3, "off")) {
      viewsSetPanelCap(p - 1, 0);
      logMsg("fps: panel %d cap off", p);
      return;
    }
    int cap = atoi(a3);
    if (cap < 1 || cap > 60) {
      logMsg("fps: cap must be 1-60 or off");
      return;
    }
    viewsSetPanelCap(p - 1, (uint8_t)cap);
    logMsg("fps: panel %d cap %d fps", p, cap);
    return;
  }
  logMsg("fps: usage fps [budget <20-95> | cap <1|2|3> <1-60|off>]");
}

// Panel screenshot for docs: the canvas (RGB565, sprite byte order) as
// base64 lines on the CDC console. scripts/panel_shot.py turns it into a PNG.
static void cmdShot(const char *a1, const char *) {
  int n = a1 ? atoi(a1) : 0;
  if (n < 1 || n > N_PANELS || !panels[n - 1].canvas) { logMsg("shot: usage shot <1|2|3>"); return; }
  const size_t bytes = (size_t)TFT_W * TFT_H * 2;
  uint8_t *copy = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  if (!copy) { logMsg("shot: no memory"); return; }
  Panel &p = panels[n - 1];
  memcpy(copy, p.canvas->getBuffer(), bytes);   // snapshot: the canvas keeps changing
  displayWait(n - 1);   // the flush task writes the GIF frame while it pushes
  if (p.gifMode && gif.n && p.gifCur && p.gifShown >= 0) {
    // GIF faces bypass the canvas: the panel's current frame through its
    // palette, and only the overlay band from the canvas
    const uint16_t *pal = gif.pal[gifSegOf(p.gifShown)];
    int x0 = ((TFT_W - gif.w) / 2) & ~1, y0 = (TFT_H - gif.h) / 2;
    for (int y = 0; y < TFT_H; y++) {
      bool inBand = p.bandY1 > p.bandY0 && y >= p.bandY0 && y < p.bandY1;
      for (int x = 0; x < TFT_W; x++) {
        uint8_t *cp = copy + ((size_t)y * TFT_W + x) * 2;
        if (inBand && !(cp[0] == (C_GIF_KEY >> 8) && cp[1] == (C_GIF_KEY & 0xFF))) continue;   // overlay pixel
        int gx = x - x0, gy = y - y0;
        uint16_t c = 0;
        if (gx >= 0 && gx < gif.w && gy >= 0 && gy < gif.h) {
          uint16_t q = pal[p.gifCur[(size_t)gy * gif.w + gx]];
          int r = q >> 8, g = (q >> 4) & 15, b = q & 15;
          c = (uint16_t)((r << 12) | (r >> 3 << 11) | (g << 7) | (g >> 2 << 5) | (b << 1) | (b >> 3));
        }
        copy[((size_t)y * TFT_W + x) * 2] = c >> 8;
        copy[((size_t)y * TFT_W + x) * 2 + 1] = c & 0xFF;
      }
    }
  }
  char line[1100];
  int len = snprintf(line, sizeof line, "SHOT begin %d %d %d\n", n, TFT_W, TFT_H);
  usbCdcWriteAll(line, len);
  for (size_t off = 0; off < bytes; off += 768) {
    size_t out = 0;
    memcpy(line, "SHOT d ", 7);
    mbedtls_base64_encode((unsigned char *)line + 7, sizeof line - 9, &out, copy + off,
                          bytes - off < 768 ? bytes - off : 768);
    line[7 + out] = '\n';
    if (!usbCdcWriteAll(line, 8 + out)) break;   // PC stopped reading
  }
  usbCdcWriteAll("SHOT end\n", 9);
  free(copy);
}

// Pin a face on a panel for screenshots; `face <n> auto` returns to the PC state
static void cmdFace(const char *a1, const char *a2) {
  static const struct { const char *name; Face f; } names[] = {
    {"auto", FACE_NONE}, {"pc", FACE_PC_STATS}, {"gif", FACE_GIF}, {"restart", FACE_GIF_RESTART},
    {"update", FACE_GIF_UPDATE}, {"media", FACE_MEDIA}, {"weather", FACE_WEATHER},
    {"ai", FACE_AI_USAGE}, {"localai", FACE_LOCAL_AI}, {"clock", FACE_CLOCK}};
  int n = a1 ? atoi(a1) : 0;
  for (auto &e : names) {
    if (n >= 1 && n <= N_PANELS && a2 && !strcmp(a2, e.name)) {
      screensForce[n - 1] = e.f;
      viewsMarkDirty(n - 1);
      logMsg("face: panel %d -> %s", n, e.name);
      return;
    }
  }
  logMsg("face: usage face <1|2|3> <auto|pc|gif|restart|update|media|weather|ai|localai|clock>");
}

// The board itself, not the PC (`reset` is the PC's reset line)
static void cmdReboot(const char *, const char *) {
  logMsg("reboot: restarting the board");
  sleepMs(300);   // let the line reach the console and the log
  esp_restart();
}

// display <on|off>: all panels dark until turned on again; not saved
static void cmdDisplay(const char *a1, const char *) {
  bool on = a1 && !strcmp(a1, "on"), off = a1 && !strcmp(a1, "off");
  if (!on && !off) { logMsg("display: usage display <on|off> (now %s)", backlightForcedOff() ? "off" : "on"); return; }
  backlightForceOff(off);
  logMsg("display: panels %s", off ? "off" : "on");
}

// screenshot: ask the PC service to post a one-off screenshot to #logging
// (auto-deletes after 5 min). Logs and does nothing if the PC is offline.
static void cmdScreenshot(const char *, const char *) {
  const char *err = screenshotRequest();
  if (err) logMsg("screenshot: %s", err);
  else logMsg("screenshot: requested (posts to #logging for 5 min)");
}

static void cmdWeather(const char *a1, const char *a2) {
  weatherCommand(a1, a2);
}

static void cmdLayout(const char *a1, const char *a2) {
  layoutCommand(a1, a2);
}

// `hw` and `ai` take the whole rest of the line; commandRun hands it over
// before strtok splits it, so these only run for help's sake
static void cmdHw(const char *, const char *) { hwConsole(""); }
static void cmdAi(const char *, const char *) { aiConsole(""); }
static void cmdMon(const char *, const char *) { monConsole(""); }
static void cmdNight(const char *a1, const char *a2) {
  char args[24];
  snprintf(args, sizeof args, "%s%s%s", a1 ? a1 : "", a1 && a2 ? " " : "", a1 && a2 ? a2 : "");
  nightConsole(args);
}
static void cmdPcWarn(const char *, const char *) { pcWarnCommand(""); }

static const Command COMMANDS[] = {
  {"help",    "",                                    cmdHelp},
  {"status",  "",                                    cmdStatus},
  {"view",    "<1|2|3> <pc|media|ai|gif|status|stats|metrics|dash>", cmdView},
  {"door",    "<1-4> [ms]",                          cmdDoor},
  {"reset",   "",                                    cmdReset},
  {"wake",    "[force | mac <AA:BB:..>]",            cmdWake},
  {"going",   "<restart|shutdown|update_restart|update_shutdown|sleep|clear>", cmdGoing},
  {"power",   "<sleep|hibernate|restart|shutdown|lock>", cmdPower},
  {"bl",      "<1|2|3|all> <0-100|on|off>",          cmdBacklight},
  {"display", "<on|off>  (all panels, not saved)",   cmdDisplay},
  {"reboot",  "  (restart the board)",               cmdReboot},
  {"hw",      "<ping | rgb ...>  (PcHwCtl via the PC)",       cmdHw},
  {"ai",      "<status | model ... | restart <name> | ...>  (LocalAIHost via the PC)", cmdAi},
  {"mon",     "<bright <n|all> <0-100> | power all on|off>  (PC monitors: DDC/CI brightness, Windows display off)", cmdMon},
  {"night",   "[on|off|start|end|low on|low off|status]  (night mode: on/off = the automatic one; start/end = now; low = low power mode)", cmdNight},
  {"screenshot", "  (secret PC screenshot to #logging, 5 min)", cmdScreenshot},
  {"led",     "<auto|on|off|sweep|0-100>",           cmdLed},
  {"pcsim",   "<on|off|auto>  (test: pretend the PC is on/off)", cmdPcSim},
  {"perf",    "",                                             cmdPerf},
  {"socks",   "",                                             cmdSocks},
  {"wifi",    "scan  (the networks the board hears)",         cmdWifi},
  {"bench",   "[all|usage|cpu|mem|flash|spi|usb <KB>]",             benchCommand},
  {"pcstats", "[text]",                                       cmdPcStats},
  {"pcwarn",  "[set <metric> <high|crit> <v|off|default> | avg <ms> | reset | demo [one] | off]", cmdPcWarn},
  {"fps",     "[budget <20-95> | cap <1|2|3> <1-60|off>]",   cmdFps},
  {"layout",  "<show|reload|default|begin|data <b64>|end>",  cmdLayout},
  {"face",    "<1|2|3> <auto|pc|gif|restart|update|media|weather|ai|localai|clock>", cmdFace},
  {"shot",    "<1|2|3>  (panel screenshot, base64 on CDC)",   cmdShot},
  {"weather", "<show|refresh|demo [rain|typhoon]|clear|storms>",                    cmdWeather},
};

static void cmdHelp(const char *, const char *) {
  // the list runs past one log line (logMsg cuts at 255): a full line goes
  // out before the next entry, so n never runs past buf
  char buf[240], item[200];
  int n = snprintf(buf, sizeof buf, "commands:");
  for (const Command &c : COMMANDS) {
    int k = snprintf(item, sizeof item, " %s%s%s%s", &c == COMMANDS ? "" : "| ", c.name, *c.usage ? " " : "", c.usage);
    if (n + k >= (int)sizeof buf) { logMsg("%s", buf); n = 0; }
    n += snprintf(buf + n, sizeof buf - n, "%s", item);
  }
  logMsg("%s", buf);
}

void commandRun(char *line) {
  while (*line == ' ' || *line == '\t') line++;
  if (!strncmp(line, "hw", 2) && (!line[2] || line[2] == ' ' || line[2] == '\t')) { hwConsole(line + 2); return; }
  if (!strncmp(line, "ai", 2) && (!line[2] || line[2] == ' ' || line[2] == '\t')) { aiConsole(line + 2); return; }
  if (!strncmp(line, "mon", 3) && (!line[3] || line[3] == ' ' || line[3] == '\t')) { monConsole(line + 3); return; }
  if (!strncmp(line, "pcwarn", 6) && (!line[6] || line[6] == ' ' || line[6] == '\t')) { pcWarnCommand(line + 6); return; }
  char *cmd = strtok(line, " \t");
  if (!cmd) return;
  char *a1 = strtok(nullptr, " \t"), *a2 = strtok(nullptr, " \t");
  for (const Command &c : COMMANDS)
    if (!strcmp(cmd, c.name)) { c.run(a1, a2); return; }
  logMsg("unknown command '%s' (try help)", cmd);
}
