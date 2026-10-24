#include "views.h"

#include <Arduino.h>
#include <stdarg.h>

#include "gif_store.h"
#include "optos.h"
#include "pc_state.h"

#define C_BG    0x0000
#define C_TEXT  0xFFFF
#define C_DIM   0x8410
#define C_GOOD  0x07E0
#define C_WARN  0xFD20
#define C_BAD   0xF800
#define C_ACCENT 0x5D1F

static char lastAction[40] = "none";
static uint32_t lastActionMs;

const char *viewName(View v) { return v == VIEW_GIF ? "gif" : v == VIEW_STATUS ? "status" : "stats"; }

bool viewParse(const char *name, View &v) {
  for (View c : {VIEW_GIF, VIEW_STATUS, VIEW_STATS})
    if (!strcmp(name, viewName(c))) { v = c; return true; }
  return false;
}

void viewsNoteAction(const char *what) {
  snprintf(lastAction, sizeof lastAction, "%s", what);
  lastActionMs = millis();
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

static void field(GFXcanvas16 &c, int y, int size, uint16_t col, const char *fmt, ...) {
  char buf[48];
  va_list a;
  va_start(a, fmt);
  vsnprintf(buf, sizeof buf, fmt, a);
  va_end(a);
  c.fillRect(0, y, TFT_W, 8 * size, C_BG);       // clear the line, then draw it
  c.setTextSize(size);
  c.setTextColor(col);
  c.setCursor(8, y);
  c.print(buf);
}

static void drawStatus(Panel &p) {
  GFXcanvas16 &c = *p.canvas;
  uint32_t up = millis() / 1000;
  PcState pc = pcState();
  field(c, 10, 2, C_ACCENT, "PC STATUS");
  c.drawFastHLine(8, 30, TFT_W - 16, C_DIM);
  uint16_t pcCol = pc == PC_ON ? C_GOOD : pc == PC_OFF ? C_WARN : C_DIM;
  field(c, 44, 3, pcCol, "%s", pc == PC_ON ? "ON" : pc == PC_OFF ? "ASLEEP" : "NO HOST");
  uint32_t guardLeft = bootGuardLeftMs();
  field(c, 76, 1, C_DIM, guardLeft ? "boot guard: %lus left" : "", (unsigned long)(guardLeft / 1000));
  field(c, 100, 1, C_DIM, "UPTIME");
  field(c, 112, 3, C_TEXT, "%02lu:%02lu:%02lu", (unsigned long)(up / 3600), (unsigned long)(up / 60 % 60), (unsigned long)(up % 60));
  field(c, 150, 1, C_DIM, "LAST ACTION");
  field(c, 162, 2, C_TEXT, "%s", lastAction);
  field(c, 182, 1, C_DIM, lastActionMs ? "%lus ago" : "", (unsigned long)((millis() - lastActionMs) / 1000));
  field(c, 206, 1, C_DIM, "DOOR REMOTE");
  int door = doorPressed();
  field(c, 218, 2, door ? C_WARN : C_GOOD, door ? "B%d PRESSED" : "idle", door);
  field(c, 246, 1, C_DIM, "MOTHERBOARD RST");
  field(c, 258, 2, moboResetActive() ? C_BAD : C_GOOD, moboResetActive() ? "PULSING" : "idle");
}

static void bar(GFXcanvas16 &c, int y, float frac, uint16_t col) {
  int w = TFT_W - 16, f = (int)(constrain(frac, 0.0f, 1.0f) * w);
  c.fillRect(8, y, f, 8, col);
  c.fillRect(8 + f, y, w - f, 8, 0x2104);
}

static void drawStats(Panel &p) {
  GFXcanvas16 &c = *p.canvas;
  field(c, 10, 2, C_ACCENT, "SYSTEM");
  c.drawFastHLine(8, 30, TFT_W - 16, C_DIM);
  float busPct = busLoadPct();
  field(c, 42, 1, C_DIM, "SPI BUS (%d MHz, RGB444)", SPI_HZ / 1000000);
  field(c, 54, 2, busPct > 85 ? C_WARN : C_TEXT, "%.0f%% busy", busPct);
  bar(c, 74, busPct / 100, busPct > 85 ? C_WARN : C_GOOD);
  int y = 94;
  for (int i = 0; i < N_PANELS; i++, y += 34) {
    field(c, y, 1, C_DIM, "PANEL %d  %s", i + 1, viewName(panels[i].view));
    field(c, y + 12, 2, C_TEXT, "%4.1ffps %4.0fKB/s", panels[i].fps, panels[i].kbps);
  }
  field(c, 200, 1, C_DIM, "MEMORY");
  field(c, 212, 2, C_TEXT, "PSRAM %4uK", (unsigned)(ESP.getFreePsram() / 1024));
  field(c, 232, 2, C_TEXT, "heap  %4uK", (unsigned)(ESP.getFreeHeap() / 1024));
  field(c, 258, 1, C_DIM, "CHIP");
  field(c, 270, 2, C_TEXT, "%.1f C", temperatureRead());
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
  p.dueMs = millis();
}

void viewsService() {
  uint32_t now = millis();
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
      else drawStats(p);
      displaySubmit(best);
      p.dueMs = now + (p.view == VIEW_STATUS ? 250 : 500);
    }
  }
}
