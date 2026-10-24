#include "views.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <LovyanGFX.hpp>

#include "display_config.h"
#include "fonts.h"
#include "gif_store.h"
#include "optos.h"
#include "pc_state.h"
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

const char *viewName(View v) {
  switch (v) {
    case VIEW_GIF:     return "gif";
    case VIEW_STATUS:  return "status";
    case VIEW_STATS:   return "stats";
    case VIEW_METRICS: return "metrics";
    default:           return "?";
  }
}

bool viewParse(const char *name, View &v) {
  for (View c : {VIEW_GIF, VIEW_STATUS, VIEW_STATS, VIEW_METRICS}) {
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
}

void viewsShowGifFrame(int idx) {
  for (int i = 0; i < N_PANELS; i++) {
    Panel &p = panels[i];
    if (p.view == VIEW_GIF) {
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
  uint16_t pcCol = pc == PC_ON ? C_GOOD : pc == PC_OFF ? C_WARN : C_DIM;
  field(c, 38, F_BIG, pcCol, "%s", pc == PC_ON ? "ON" : pc == PC_OFF ? "ASLEEP" : "NO HOST");
  uint32_t guardLeft = bootGuardLeftMs();
  field(c, 74, F_SMALL, C_DIM, guardLeft ? "boot guard: %lus left" : "", (unsigned long)(guardLeft / 1000));
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

void viewEnter(int i, View v) {
  displayWait(i);
  Panel &p = panels[i];
  if (v == VIEW_GIF) {
    p.view = VIEW_STATUS;   // normal canvas flush
    p.canvas->fillScreen(C_BG);
    displaySubmit(i);
    displayWait(i);
    p.gifShown = -1;
  }
  p.view = v;
  p.canvas->fillScreen(C_BG);
  memset(p.sent, 0xA5, (size_t)TFT_W * TFT_H * 2);
  p.gifIdx = gif.n ? (uint16_t)(i * gif.n / N_PANELS) : 0;
  p.dueMs = nowMs();
}

void viewsService() {
  uint32_t now = nowMs();
  for (;;) {
    int best = -1;
    int32_t maxOverdue = -1;
    for (int i = 0; i < N_PANELS; i++) {
      Panel &p = panels[i];
      if (p.busy) continue;
      int32_t overdue = (int32_t)(now - p.dueMs);
      if (overdue >= 0 && overdue > maxOverdue) {
        maxOverdue = overdue;
        best = i;
      }
    }
    if (best < 0) break;

    Panel &p = panels[best];
    if (p.view == VIEW_GIF) {
      if (!gif.n) { p.dueMs = now + 200; continue; }
      uint16_t idx = p.gifIdx;
      if (idx >= gif.n) idx = 0;
      p.gifFrame = idx;
      displaySubmit(best);
      p.dueMs += gif.durMs[idx];
      if ((int32_t)(now - p.dueMs) > (int32_t)gif.durMs[idx]) p.dueMs = now;
      p.gifIdx = (idx + 1) % gif.n;
    } else {
      if (p.view == VIEW_STATUS) drawStatus(p);
      else if (p.view == VIEW_STATS) drawStats(p);
      else if (p.view == VIEW_METRICS) drawMetrics(p, best);
      displaySubmit(best);
      p.dueMs = now + (p.view == VIEW_STATUS ? 250 : 500);
    }
  }
}
