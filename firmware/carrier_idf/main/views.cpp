#include "views.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <LovyanGFX.hpp>

#include "display_config.h"
#include "fonts.h"
#include "gif_store.h"
#include "layout.h"
#include "screens.h"
#include "optos.h"
#include "pc_state.h"
#include "pc_going.h"
#include "pcstats.h"
#include "sys.h"

#define C_BG     0x0000
#define C_TEXT   0xFFFF
#define C_DIM    0x8410
#define C_GOOD   0x07E0
#define C_WARN   0xFD20
#define C_BAD    0xF800
#define C_ACCENT 0x5D1F

static char lastAction[40] = "none";
static uint32_t lastActionMs = 0;

// Dynamic FPS engine state
static uint8_t busBudgetPct = 85;
static uint8_t panelCapFps[N_PANELS] = {0, 0, 0};   // 0 = off (default 20 cap)
static uint32_t boostUntilMs[N_PANELS] = {0, 0, 0};
static bool panelDirty[N_PANELS] = {true, true, true};
static bool panelFirstFrame[N_PANELS] = {true, true, true};
static uint32_t lastFrameMs[N_PANELS] = {0, 0, 0};

static uint32_t deferredCount[N_PANELS] = {0, 0, 0};
static bool wasDeferred[N_PANELS] = {false, false, false};
static uint32_t gifNominalMs[N_PANELS] = {0, 0, 0};
static uint32_t statsWinStart = 0;

static double tokenBucket = 0.0;
static uint32_t lastRefillUs = 0;

enum PanelPriority { PRIO_LOW = 0, PRIO_NORMAL = 1, PRIO_HIGH = 2 };

struct StatusSnapshot {
  PcState pc;
  uint32_t guardSec;
  int door;
  bool moboReset;
  uint32_t lastActionMs;
  GoingKind going;
  uint32_t goingLeftSec;
};
static StatusSnapshot lastStatusSnap;
static uint32_t lastStatsSeq = 0;
static uint32_t lastUptimeSec = 0;
static uint32_t lastStatsHeartbeatMs = 0;
static uint32_t lastMetricsHeartbeatMs = 0;
static uint32_t lastDashHeartbeatMs[N_PANELS] = {0, 0, 0};
static bool wasIdle = false;

// Auto views (pc / media / ai): the face on glass and when it was last drawn
static Face panelFace[N_PANELS];
static uint32_t lastFaceDrawMs[N_PANELS];

static double getBucketCapacity() {
  double ratePerUs = ((double)displayBus.hz / 8.0 * (double)busBudgetPct / 100.0) / 1000000.0;
  return ratePerUs * 100000.0;   // ~100 ms worth
}

static void refillBucket() {
  uint32_t now = nowUs();
  double ratePerUs = ((double)displayBus.hz / 8.0 * (double)busBudgetPct / 100.0) / 1000000.0;
  double cap = ratePerUs * 100000.0;
  if (lastRefillUs == 0) {
    lastRefillUs = now;
    tokenBucket = cap;
  } else {
    uint32_t dt = now - lastRefillUs;
    lastRefillUs = now;
    tokenBucket += (double)dt * ratePerUs;
    if (tokenBucket > cap) tokenBucket = cap;
  }
}

uint8_t viewsBusBudget() { return busBudgetPct; }
void viewsSetBusBudget(uint8_t pct) {
  if (pct >= 20 && pct <= 95) busBudgetPct = pct;
}

uint8_t viewsPanelCap(int panel) {
  if (panel >= 0 && panel < N_PANELS) return panelCapFps[panel];
  return 0;
}

void viewsSetPanelCap(int panel, uint8_t capFps) {
  if (panel >= 0 && panel < N_PANELS) {
    panelCapFps[panel] = (capFps <= 60) ? capFps : 60;
  }
}

static PanelPriority getPriority(int panel, uint32_t now) {
  if ((int32_t)(boostUntilMs[panel] - now) > 0) return PRIO_HIGH;
  if (panels[panel].gifMode) return PRIO_LOW;
  return PRIO_NORMAL;
}

const char *viewsPanelPriorityName(int panel) {
  if (panel < 0 || panel >= N_PANELS) return "?";
  PanelPriority p = getPriority(panel, nowMs());
  switch (p) {
    case PRIO_HIGH:   return "HIGH";
    case PRIO_NORMAL: return "NORMAL";
    case PRIO_LOW:    return "LOW";
    default:          return "?";
  }
}

double viewsBucketLevel() { return tokenBucket; }
double viewsBucketCapacity() { return getBucketCapacity(); }

void viewsBoost(int panel) {
  uint32_t t = nowMs() + 2000;
  if (panel < 0) {
    for (int i = 0; i < N_PANELS; i++) boostUntilMs[i] = t;
  } else if (panel < N_PANELS) {
    boostUntilMs[panel] = t;
  }
}

void viewsMarkDirty(int panel) {
  if (panel < 0) {
    for (int i = 0; i < N_PANELS; i++) panelDirty[i] = true;
  } else if (panel < N_PANELS) {
    panelDirty[panel] = true;
  }
}

static uint32_t getMinFrameMs(int panel) {
  uint8_t cap = panelCapFps[panel];
  if (cap >= 1 && cap <= 60) return 1000 / cap;
  return 50;   // default 20 fps cap (MIN_FRAME_MS)
}

static float getExpectedBytes(int panel) {
  Panel &p = panels[panel];
  if (panelFirstFrame[panel] || p.gifShown == -1) {
    return (float)(TFT_W * TFT_H * 3 / 2);
  }
  if (p.emaBytes <= 1000.0f) return 1000.0f;
  return p.emaBytes;
}

const char *viewName(View v) {
  switch (v) {
    case VIEW_GIF:     return "gif";
    case VIEW_STATUS:  return "status";
    case VIEW_STATS:   return "stats";
    case VIEW_METRICS: return "metrics";
    case VIEW_DASH:    return "dash";
    case VIEW_PC:      return "pc";
    case VIEW_MEDIA:   return "media";
    case VIEW_AI:      return "ai";
    default:           return "?";
  }
}

bool viewParse(const char *name, View &v) {
  for (View c : {VIEW_GIF, VIEW_STATUS, VIEW_STATS, VIEW_METRICS, VIEW_DASH, VIEW_PC, VIEW_MEDIA, VIEW_AI}) {
    if (!strcmp(name, viewName(c))) {
      v = c;
      return true;
    }
  }
  return false;
}

void viewsNoteAction(const char *what) {
  snprintf(lastAction, sizeof lastAction, "%s", what);
  lastActionMs = nowMs();
  for (int i = 0; i < N_PANELS; i++) {
    if (panels[i].view == VIEW_STATUS) {
      panelDirty[i] = true;
      viewsBoost(i);
    }
  }
}

void viewsShowGifFrame(int idx) {
  for (int i = 0; i < N_PANELS; i++) {
    Panel &p = panels[i];
    if (p.gifMode) {
      p.gifFrame = idx;
      displaySubmit(i);
      displayWait(i);
    }
  }
}

enum FieldFont { F_SMALL, F_BODY, F_BIG, F_HEAD };

static void field(LGFX_Sprite &c, int y, FieldFont ff, uint16_t col, const char *fmt, ...) {
  char buf[64];
  va_list a;
  va_start(a, fmt);
  vsnprintf(buf, sizeof buf, fmt, a);
  va_end(a);

  c.setFont(ff == F_SMALL ? fontSmall() : ff == F_BODY ? fontBody() : ff == F_BIG ? fontBig() : fontHead());
  c.setTextSize(1);
  c.fillRect(0, y, TFT_W, c.fontHeight(), C_BG);   // clear the line, then draw it
  c.setTextColor(col, C_BG);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString(buf, 8, y);
}

static void drawStatus(Panel &p) {
  LGFX_Sprite &c = *p.canvas;
  uint32_t up = nowMs() / 1000;
  PcState pc = pcState();
  field(c, 6, F_HEAD, C_ACCENT, "PC STATUS");
  c.drawFastHLine(8, 32, TFT_W - 16, C_DIM);
  GoingKind going = pcGoing();
  if (going != GO_NONE) {
    field(c, 38, F_BIG, C_WARN, "%s", pcGoingLabel());
    char sub[64] = "";
    const char *by = pcGoingBy();
    if (going == GO_RESTART || going == GO_UPDATE_RESTART || going == GO_UPDATE_SHUTDOWN) {
      uint32_t leftSec = (pcGoingLeftMs() + 999) / 1000;
      if (by && by[0]) {
        snprintf(sub, sizeof sub, "by %s  %02lu:%02lu left", by,
                 (unsigned long)(leftSec / 60), (unsigned long)(leftSec % 60));
      } else {
        snprintf(sub, sizeof sub, "%02lu:%02lu left",
                 (unsigned long)(leftSec / 60), (unsigned long)(leftSec % 60));
      }
    } else {
      uint32_t ageSec = pcGoingAgeMs() / 1000;
      if (by && by[0]) {
        snprintf(sub, sizeof sub, "by %s  %02lu:%02lu", by,
                 (unsigned long)(ageSec / 60), (unsigned long)(ageSec % 60));
      } else {
        snprintf(sub, sizeof sub, "%02lu:%02lu",
                 (unsigned long)(ageSec / 60), (unsigned long)(ageSec % 60));
      }
    }
    field(c, 74, F_SMALL, C_DIM, "%s", sub);
  } else {
    uint16_t pcCol = pc == PC_ON ? C_GOOD : pc == PC_OFF ? C_WARN : C_DIM;
    field(c, 38, F_BIG, pcCol, "%s", pc == PC_ON ? "ON" : pc == PC_OFF ? "ASLEEP" : "NO HOST");
    uint32_t guardLeft = bootGuardLeftMs();
    if (guardLeft) {
      field(c, 74, F_SMALL, C_DIM, "boot guard: %lus left", (unsigned long)(guardLeft / 1000));
    } else {
      const char *ld = pcLastDown();
      const char *lda = pcLastDownAt();
      if (ld && ld[0] && strcmp(ld, "-") != 0 && strcmp(ld, "unknown") != 0) {
        if (lda && lda[0] && strcmp(lda, "-") != 0) {
          field(c, 74, F_SMALL, C_DIM, "last down: %s %s", ld, lda);
        } else {
          field(c, 74, F_SMALL, C_DIM, "last down: %s", ld);
        }
      } else {
        field(c, 74, F_SMALL, C_DIM, "");
      }
    }
  }
  field(c, 96, F_SMALL, C_DIM, "UPTIME");
  field(c, 112, F_BIG, C_TEXT, "%02lu:%02lu:%02lu", (unsigned long)(up / 3600), (unsigned long)(up / 60 % 60), (unsigned long)(up % 60));
  field(c, 152, F_SMALL, C_DIM, "LAST ACTION");
  field(c, 168, F_BODY, C_TEXT, "%s", lastAction);
  field(c, 190, F_SMALL, C_DIM, lastActionMs ? "%lus ago" : "", (unsigned long)((nowMs() - lastActionMs) / 1000));
  field(c, 212, F_SMALL, C_DIM, "DOOR REMOTE");
  int door = doorPressed();
  field(c, 228, F_BODY, door ? C_WARN : C_GOOD, door ? "B%d PRESSED" : "idle", door);
  field(c, 256, F_SMALL, C_DIM, "MOTHERBOARD RST");
  field(c, 272, F_BODY, moboResetActive() ? C_BAD : C_GOOD, moboResetActive() ? "PULSING" : "idle");
}

static void bar(LGFX_Sprite &c, int y, float frac, uint16_t col) {
  if (frac < 0.0f) frac = 0.0f;
  if (frac > 1.0f) frac = 1.0f;
  int w = TFT_W - 16;
  int f = (int)(frac * w);
  c.fillRect(8, y, f, 8, col);
  c.fillRect(8 + f, y, w - f, 8, 0x2104);
}

static void drawStats(Panel &p) {
  LGFX_Sprite &c = *p.canvas;
  field(c, 6, F_HEAD, C_ACCENT, "SYSTEM");
  c.drawFastHLine(8, 32, TFT_W - 16, C_DIM);
  float busPct = busLoadPct();
  field(c, 38, F_SMALL, C_DIM, "SPI BUS (%d MHz, RGB444)", SPI_HZ / 1000000);
  field(c, 54, F_BODY, busPct > 85 ? C_WARN : C_TEXT, "%.0f%% busy", busPct);
  bar(c, 78, busPct / 100.0f, busPct > 85 ? C_WARN : C_GOOD);
  int y = 92;
  for (int i = 0; i < N_PANELS; i++, y += 36) {
    field(c, y, F_SMALL, C_DIM, "PANEL %d  %s", i + 1, viewName(panels[i].view));
    field(c, y + 16, F_BODY, C_TEXT, "%4.1f fps %4.0f KB/s", panels[i].fps, panels[i].kbps);
  }
  field(c, 204, F_SMALL, C_DIM, "MEMORY");
  field(c, 220, F_BODY, C_TEXT, "PSRAM %4u K", (unsigned)(freePsram() / 1024));
  field(c, 240, F_BODY, C_TEXT, "heap  %4u K", (unsigned)(freeHeap() / 1024));
  field(c, 264, F_SMALL, C_DIM, "CHIP");
  field(c, 280, F_BODY, C_TEXT, "%.1f\xC2\xB0" "C", chipTempC());   // UTF-8 degree sign
}

static void drawMetrics(Panel &p, int panelIdx) {
  LGFX_Sprite &c = *p.canvas;

  // Rank k (0-based) and total n among panels currently in VIEW_METRICS
  int n = 0;
  int k = -1;
  for (int i = 0; i < N_PANELS; i++) {
    if (panels[i].view == VIEW_METRICS) {
      if (i == panelIdx) k = n;
      n++;
    }
  }
  if (n <= 0 || k < 0) return;

  int numCount = pcStatsCount();
  int textCount = pcTextCount();
  int totalItems = numCount + textCount;

  // Split combined list evenly across n panels
  int base = totalItems / n;
  int rem = totalItems % n;
  int startIdx = k * base + (k < rem ? k : rem);
  int count = base + (k < rem ? 1 : 0);

  // Clear header area
  c.fillRect(0, 0, TFT_W, 26, C_BG);

  // Header line: "PC METRICS k/n" plus "live" / "no data"
  char hdr[48];
  snprintf(hdr, sizeof(hdr), "PC METRICS %d/%d", k + 1, n);

  c.setFont(fontBody());
  c.setTextSize(1);
  c.setTextColor(C_ACCENT, C_BG);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString(hdr, 8, 4);

  bool fresh = pcStatsFresh();
  c.setTextColor(fresh ? C_GOOD : C_WARN, C_BG);
  c.setTextDatum(lgfx::textdatum_t::top_right);
  c.drawString(fresh ? "live" : "no data", TFT_W - 8, 4);

  // Divider line
  c.drawFastHLine(8, 26, TFT_W - 16, C_DIM);

  // Rows in the small font from y = 30, paging every 5 s when they overflow
  c.setFont(fontSmall());
  const int rowH = c.fontHeight();
  const int ROWS_PER_PAGE = (TFT_H - 30) / rowH;
  int numPages = count > 0 ? ((count + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE) : 1;
  uint32_t page = (nowMs() / 5000) % (uint32_t)numPages;
  int pageStart = (int)page * ROWS_PER_PAGE;
  int pageItems = count - pageStart;
  if (pageItems > ROWS_PER_PAGE) pageItems = ROWS_PER_PAGE;
  if (pageItems < 0) pageItems = 0;

  for (int r = 0; r < ROWS_PER_PAGE; r++) {
    int y = 30 + r * rowH;
    c.fillRect(0, y, TFT_W, rowH, C_BG);

    if (totalItems == 0 && r == 0) {
      c.setTextColor(C_DIM, C_BG);
      c.setTextDatum(lgfx::textdatum_t::top_left);
      c.drawString("no metrics", 8, y);
      continue;
    }

    if (r >= pageItems) continue;

    int globalIdx = startIdx + pageStart + r;
    const char *rawKey = nullptr;
    char valBuf[64];

    if (globalIdx < numCount) {
      rawKey = pcStatKey(globalIdx);
      double val = pcStatAt(globalIdx);
      if (isnan(val)) {
        snprintf(valBuf, sizeof(valBuf), "--");
      } else if (val == floor(val) && fabs(val) <= 9007199254740992.0) {
        snprintf(valBuf, sizeof(valBuf), "%.0f", val);   // whole numbers in full (uptime)
      } else {
        snprintf(valBuf, sizeof(valBuf), "%.6g", val);
      }
    } else {
      int textIdx = globalIdx - numCount;
      rawKey = pcTextKey(textIdx);
      const char *rawVal = pcTextAt(textIdx);
      if (!rawVal) rawVal = "";

      // Truncate text value to fit line without overlapping key (monospace font)
      int charW = c.textWidth("M");
      int keyLen = rawKey ? (int)strlen(rawKey) : 0;
      if (keyLen > 20) keyLen = 20;
      int keyPx = keyLen * charW;
      int availValPx = (TFT_W - 16) - keyPx - charW;
      int maxChars = availValPx > 0 ? (availValPx / charW) : 0;
      if (maxChars > (int)sizeof(valBuf) - 1) maxChars = sizeof(valBuf) - 1;
      if (maxChars > 0) {
        strncpy(valBuf, rawVal, maxChars);
        valBuf[maxChars] = '\0';
      } else {
        valBuf[0] = '\0';
      }
    }

    char keyBuf[24];
    if (rawKey && (int)strlen(rawKey) > 20) {
      strncpy(keyBuf, rawKey, 20);
      keyBuf[20] = '\0';
    } else {
      snprintf(keyBuf, sizeof(keyBuf), "%s", rawKey ? rawKey : "?");
    }

    // Key left (dim)
    c.setTextColor(C_DIM, C_BG);
    c.setTextDatum(lgfx::textdatum_t::top_left);
    c.drawString(keyBuf, 8, y);

    // Value right-aligned (white)
    c.setTextColor(C_TEXT, C_BG);
    c.setTextDatum(lgfx::textdatum_t::top_right);
    c.drawString(valBuf, TFT_W - 8, y);
  }

  // Clear bottom margin
  int bottom = 30 + ROWS_PER_PAGE * rowH;
  c.fillRect(0, bottom, TFT_W, TFT_H - bottom, C_BG);
}

static void drawDash(Panel &p, int panelIdx, bool full) {
  int dashIdx = 0;
  for (int i = 0; i < panelIdx; i++) {
    if (panels[i].view == VIEW_DASH) dashIdx++;
  }
  layoutDraw(dashIdx, *p.canvas, full);
}

void viewEnter(int i, View v) {
  displayWait(i);
  Panel &p = panels[i];
  p.gifMode = false;        // normal canvas flush until a GIF face starts
  p.bandY0 = p.bandY1 = 0;
  if (v == VIEW_GIF) {
    p.view = VIEW_STATUS;
    p.canvas->fillScreen(C_BG);
    displaySubmit(i);
    displayWait(i);
    p.gifShown = -1;
    p.gifMode = true;
  }
  p.view = v;
  panelFace[i] = FACE_NONE;   // auto views pick their face on the next service pass
  p.canvas->fillScreen(C_BG);
  memset(p.sent, 0xA5, (size_t)TFT_W * TFT_H * 2);
  p.gifIdx = gif.n ? (uint16_t)(i * gif.n / N_PANELS) : 0;
  p.dueMs = nowMs();
  panelDirty[i] = true;
  panelFirstFrame[i] = true;
  lastFrameMs[i] = 0;
  viewsRefreshIdle();
}

bool viewIsAuto(View v) { return v == VIEW_PC || v == VIEW_MEDIA || v == VIEW_AI; }

void viewsRefreshIdle() {
  bool anyAuto = false;
  for (int i = 0; i < N_PANELS; i++) anyAuto |= viewIsAuto(panels[i].view);
  backlightIdle(pcState() == PC_OFF && !pcGoingHoldsScreens() && !anyAuto);
}

// Put face f on panel i. GIF faces clear the glass once and let the flush
// task push GIF frames (with the overlay band from the canvas); canvas faces
// force a full resend.
static void enterFace(int i, Face f) {
  Panel &p = panels[i];
  bool gifNow = screensIsGif(f);
  if (gifNow && !p.gifMode) {
    p.canvas->fillScreen(C_BG);
    displaySubmit(i);
    displayWait(i);
  }
  p.gifMode = gifNow;
  p.gifShown = -1;                               // full GIF frame: repaints an old band too
  if (gifNow && f != FACE_GIF) {
    p.bandY0 = OVERLAY_Y0;
    p.bandY1 = OVERLAY_Y1;
    screensDraw(f, *p.canvas);
  } else {
    p.bandY0 = p.bandY1 = 0;
  }
  if (!gifNow) memset(p.sent, 0xA5, (size_t)TFT_W * TFT_H * 2);
  panelFace[i] = f;
  lastFaceDrawMs[i] = nowMs();
  panelDirty[i] = true;
  panelFirstFrame[i] = true;
}

void viewsService() {
  uint32_t now = nowMs();

  // NVS debounced writes
  displayNvsPoll();

  // Idle check: PC asleep/off -> submit nothing at all
  if (backlightIsIdle()) {
    wasIdle = true;
    return;
  }
  if (wasIdle) {
    wasIdle = false;
    for (int i = 0; i < N_PANELS; i++) {
      memset(panels[i].sent, 0xA5, (size_t)TFT_W * TFT_H * 2);
      panels[i].gifShown = -1;
      panelDirty[i] = true;
      panelFirstFrame[i] = true;
      panels[i].dueMs = now;
      viewsBoost(i);
    }
  }

  // Refill SPI wire byte bucket
  refillBucket();

  // 1-second rolling metrics for deferred frames & gifStretch
  if (now - statsWinStart >= 1000) {
    float dt = (now - statsWinStart) / 1000.0f;
    for (int i = 0; i < N_PANELS; i++) {
      panels[i].deferred = deferredCount[i];
      deferredCount[i] = 0;
      if (panels[i].gifMode && gifNominalMs[i] > 0) {
        panels[i].gifStretch = (dt * 1000.0f) / (float)gifNominalMs[i];
      } else {
        panels[i].gifStretch = 1.0f;
      }
      gifNominalMs[i] = 0;
    }
    statsWinStart = now;
  }

  // Event: pcStatsSeq() changed -> metrics, dash, stats dirty
  uint32_t curSeq = pcStatsSeq();
  if (curSeq != lastStatsSeq) {
    lastStatsSeq = curSeq;
    for (int i = 0; i < N_PANELS; i++) {
      if (panels[i].view == VIEW_METRICS || panels[i].view == VIEW_DASH || panels[i].view == VIEW_STATS) {
        panelDirty[i] = true;
      }
      if (viewIsAuto(panels[i].view) && !panels[i].gifMode) panelDirty[i] = true;
    }
  }

  // Event: status snapshot changed -> status panels dirty
  uint32_t gLeft = bootGuardLeftMs();
  uint32_t gGoingLeft = pcGoingLeftMs();
  uint32_t goingSec = gGoingLeft ? ((gGoingLeft + 999) / 1000) : (pcGoing() != GO_NONE ? (pcGoingAgeMs() / 1000) : 0);
  StatusSnapshot curSnap = {
    pcState(),
    gLeft ? ((gLeft + 999) / 1000) : 0,
    doorPressed(),
    moboResetActive(),
    lastActionMs,
    pcGoing(),
    goingSec
  };
  // field compare: memcmp would see the struct's uninitialised padding
  if (curSnap.pc != lastStatusSnap.pc || curSnap.guardSec != lastStatusSnap.guardSec ||
      curSnap.door != lastStatusSnap.door || curSnap.moboReset != lastStatusSnap.moboReset ||
      curSnap.lastActionMs != lastStatusSnap.lastActionMs ||
      curSnap.going != lastStatusSnap.going || curSnap.goingLeftSec != lastStatusSnap.goingLeftSec) {
    lastStatusSnap = curSnap;
    for (int i = 0; i < N_PANELS; i++) {
      if (panels[i].view == VIEW_STATUS) panelDirty[i] = true;
    }
  }

  // Heartbeats:
  // Status: 1000 ms aligned to uptime second
  uint32_t curUptimeSec = now / 1000;
  if (curUptimeSec != lastUptimeSec) {
    lastUptimeSec = curUptimeSec;
    for (int i = 0; i < N_PANELS; i++) {
      if (panels[i].view == VIEW_STATUS) panelDirty[i] = true;
    }
  }

  // Stats: 1000 ms
  if (now - lastStatsHeartbeatMs >= 1000) {
    lastStatsHeartbeatMs = now;
    for (int i = 0; i < N_PANELS; i++) {
      if (panels[i].view == VIEW_STATS) panelDirty[i] = true;
    }
  }

  // Metrics: 1000 ms (paging)
  if (now - lastMetricsHeartbeatMs >= 1000) {
    lastMetricsHeartbeatMs = now;
    for (int i = 0; i < N_PANELS; i++) {
      if (panels[i].view == VIEW_METRICS) panelDirty[i] = true;
    }
  }

  // Dash: layoutPeriodMs(dashIdx) if non-zero else 1000 ms
  for (int i = 0; i < N_PANELS; i++) {
    if (panels[i].view == VIEW_DASH) {
      int dashIdx = 0;
      for (int j = 0; j < i; j++) {
        if (panels[j].view == VIEW_DASH) dashIdx++;
      }
      uint32_t pMs = layoutPeriodMs(dashIdx);
      uint32_t hb = pMs ? pMs : 1000;
      if (now - lastDashHeartbeatMs[i] >= hb) {
        lastDashHeartbeatMs[i] = now;
        panelDirty[i] = true;
      }
    }
  }

  // Auto views: follow the PC state, redraw on their own period
  for (int i = 0; i < N_PANELS; i++) {
    Panel &p = panels[i];
    if (!viewIsAuto(p.view) || p.busy) continue;
    Face f = screensForce[i] != FACE_NONE ? screensForce[i] : screensFace(p.view);
    if (f != panelFace[i]) {
      enterFace(i, f);
      continue;
    }
    uint32_t period = screensPeriodMs(f);
    if (screensIsGif(f)) {
      // the band's text can change (update minutes); the next GIF frame sends it
      if (f != FACE_GIF && now - lastFaceDrawMs[i] >= 1000) {
        screensDraw(f, *p.canvas);
        lastFaceDrawMs[i] = now;
      }
    } else if (period && now - lastFaceDrawMs[i] >= period) {
      panelDirty[i] = true;
    }
  }

  // Schedule due panels
  for (;;) {
    int best = -1;
    PanelPriority bestPrio = PRIO_LOW;
    int32_t maxOverdue = -1;

    for (int i = 0; i < N_PANELS; i++) {
      Panel &p = panels[i];
      if (p.busy) continue;

      uint32_t minMs = getMinFrameMs(i);
      bool isDue = false;
      int32_t overdue = 0;

      if (p.gifMode) {
        if (!gif.n) {
          p.dueMs = now + 200;
          continue;
        }
        if ((now - lastFrameMs[i] >= minMs) && ((int32_t)(now - p.dueMs) >= 0)) {
          isDue = true;
          overdue = (int32_t)(now - p.dueMs);
        }
      } else {
        if (panelDirty[i] && (now - lastFrameMs[i] >= minMs)) {
          isDue = true;
          overdue = (int32_t)(now - p.dueMs);
        }
      }

      if (!isDue) continue;

      PanelPriority prio = getPriority(i, now);
      if (best < 0 || prio > bestPrio || (prio == bestPrio && overdue > maxOverdue)) {
        best = i;
        bestPrio = prio;
        maxOverdue = overdue;
      }
    }

    if (best < 0) break;

    Panel &p = panels[best];
    float expected = getExpectedBytes(best);
    // a full bucket always admits one frame, else a frame bigger than the bucket never goes
    double need = (double)expected < getBucketCapacity() ? (double)expected : getBucketCapacity();
    bool canSubmit = (bestPrio == PRIO_HIGH) || (tokenBucket >= need);

    if (!canSubmit) {
      if (!wasDeferred[best]) {
        deferredCount[best]++;
        wasDeferred[best] = true;
      }
      break;
    }

    tokenBucket -= (double)expected;
    wasDeferred[best] = false;
    lastFrameMs[best] = now;

    if (p.gifMode) {
      uint16_t idx = p.gifIdx;
      if (idx >= gif.n) idx = 0;
      p.gifFrame = idx;
      displaySubmit(best);
      gifNominalMs[best] += gif.durMs[idx];
      // keep the GIF's own cadence; once a whole frame behind, slip instead of bursting
      p.dueMs += gif.durMs[idx];
      if ((int32_t)(now - p.dueMs) > (int32_t)gif.durMs[idx]) p.dueMs = now;
      p.gifIdx = (idx + 1) % gif.n;
      panelFirstFrame[best] = false;
    } else {
      if (p.view == VIEW_STATUS) drawStatus(p);
      else if (p.view == VIEW_STATS) drawStats(p);
      else if (p.view == VIEW_METRICS) drawMetrics(p, best);
      else if (p.view == VIEW_DASH) drawDash(p, best, panelFirstFrame[best]);
      else if (viewIsAuto(p.view)) {
        screensDraw(panelFace[best], *p.canvas);
        lastFaceDrawMs[best] = now;
      }

      panelFirstFrame[best] = false;
      panelDirty[best] = false;
      displaySubmit(best);
      p.dueMs = now + getMinFrameMs(best);
    }
  }
}
