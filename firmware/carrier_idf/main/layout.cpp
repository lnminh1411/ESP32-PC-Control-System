// Dashboard layout engine implementation.
// Parses JSON layouts, manages page/widget structs in PSRAM, and executes differential redraws.
#include "layout.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <math.h>
#include <mbedtls/base64.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <LovyanGFX.hpp>

#include "config.h"
#include "fonts.h"
#include "log.h"
#include "pcstats.h"
#include "sys.h"
#include "views.h"

#define MAX_PAGES 6
#define MAX_WIDGETS_PER_PAGE 32
#define UPLOAD_BUF_SIZE 24576

enum WidgetType : uint8_t {
  WIDGET_LABEL,
  WIDGET_VALUE,
  WIDGET_TEXT,
  WIDGET_BAR,
  WIDGET_SPARK,
  WIDGET_LINE,
};

enum WidgetFont : uint8_t {
  FONT_SMALL,
  FONT_BODY,
  FONT_HEAD,
  FONT_BIG,
};

enum WidgetAlign : uint8_t {
  ALIGN_LEFT,
  ALIGN_CENTER,
  ALIGN_RIGHT,
};

struct Widget {
  WidgetType type;
  WidgetFont font;
  WidgetAlign align;
  int16_t x, y;
  int16_t w, h;
  uint16_t color;
  uint16_t bg;
  bool has_bg;
  uint16_t warn_color;
  uint16_t bad_color;
  double min_val, max_val;
  double warn_val, bad_val;
  double scale;
  char key[24];
  char text[44];
  char fmt[24];
  char unit[12];

  // Sparkline history ring buffer
  uint16_t spark_samples;
  uint16_t spark_count;
  uint16_t spark_head;
  float *spark_ring;
  uint32_t spark_last_seq;

  // Diff cache
  char last_text[64];
  int16_t last_fill_w;
  uint16_t last_color;
  uint32_t last_drawn_seq;
};

struct Page {
  uint16_t bg;
  uint8_t widget_count;
  Widget widgets[MAX_WIDGETS_PER_PAGE];
};

struct Layout {
  uint8_t page_count;
  bool is_default;
  bool page_drawn[MAX_PAGES];
  Page pages[MAX_PAGES];
};

static void freeLayout(struct Layout *l);
static Layout *activeLayout = nullptr;
// PUT /layout runs in the HTTP task: the swap must not free a layout core 1 is drawing
static SemaphoreHandle_t layoutLock;

static void lock() {
  if (!layoutLock) layoutLock = xSemaphoreCreateMutex();
  xSemaphoreTake(layoutLock, portMAX_DELAY);
}
static void unlock() { xSemaphoreGive(layoutLock); }

static void swapLayout(Layout *l) {
  lock();
  Layout *old = activeLayout;
  activeLayout = l;
  unlock();
  freeLayout(old);
  viewsMarkDirty(-1);
}

// fmt is handed a double: allow exactly one floating conversion (plus %%), nothing else
static bool fmtValid(const char *f) {
  int conv = 0;
  for (; *f; f++) {
    if (*f != '%') continue;
    if (*++f == '%') continue;
    while (*f && strchr("-+ #0", *f)) f++;
    while (*f >= '0' && *f <= '9') f++;
    if (*f == '.') for (f++; *f >= '0' && *f <= '9'; f++) {}
    if (!*f || !strchr("fFeEgG", *f)) return false;
    conv++;
  }
  return conv == 1;
}
static char *uploadBuf = nullptr;
static size_t uploadLen = 0;

static const char BUILTIN_LAYOUT_JSON[] = R"json({
  "pages": [
    {
      "bg": "#000000",
      "widgets": [
        {"type":"label", "x":8, "y":6, "font":"head", "color":"#58A6FF", "text":"SYSTEM"},
        {"type":"line",  "x":8, "y":30, "w":224, "color":"#333333"},
        {"type":"label", "x":8, "y":36, "font":"small", "color":"#888888", "text":"CPU"},
        {"type":"value", "x":8, "y":50, "font":"big", "key":"cpu_temp", "fmt":"%.0f\u00b0C", "color":"#FFFFFF", "warn":75, "bad":90, "warn_color":"#FFA500", "bad_color":"#FF3030", "w":100, "align":"left"},
        {"type":"value", "x":120, "y":50, "font":"big", "key":"cpu_load", "fmt":"%.0f%%", "color":"#FFFFFF", "warn":80, "bad":95, "warn_color":"#FFA500", "bad_color":"#FF3030", "w":112, "align":"right"},
        {"type":"bar",   "x":8, "y":82, "w":224, "h":8, "key":"cpu_load", "min":0, "max":100, "color":"#3FB950", "bg":"#222222", "warn":80, "bad":95},
        {"type":"spark", "x":8, "y":94, "w":224, "h":30, "key":"cpu_load", "min":0, "max":100, "samples":60, "color":"#58A6FF"},
        {"type":"label", "x":8, "y":128, "font":"small", "color":"#888888", "text":"Clock"},
        {"type":"value", "x":50, "y":128, "font":"small", "key":"cpu_clock", "fmt":"%.0f MHz", "color":"#CCCCCC", "w":70, "align":"left"},
        {"type":"label", "x":130, "y":128, "font":"small", "color":"#888888", "text":"Fan"},
        {"type":"value", "x":160, "y":128, "font":"small", "key":"cpu_fan", "fmt":"%.0f RPM", "color":"#CCCCCC", "w":72, "align":"right"},
        {"type":"line",  "x":8, "y":144, "w":224, "color":"#333333"},
        {"type":"label", "x":8, "y":150, "font":"small", "color":"#888888", "text":"RAM"},
        {"type":"value", "x":8, "y":164, "font":"body", "key":"ram_used", "fmt":"%.1f GB", "color":"#FFFFFF", "w":100, "align":"left"},
        {"type":"value", "x":120, "y":164, "font":"body", "key":"ram_load", "fmt":"%.0f%%", "color":"#FFFFFF", "warn":80, "bad":90, "warn_color":"#FFA500", "bad_color":"#FF3030", "w":112, "align":"right"},
        {"type":"bar",   "x":8, "y":184, "w":224, "h":8, "key":"ram_load", "min":0, "max":100, "color":"#3FB950", "bg":"#222222", "warn":80, "bad":90},
        {"type":"line",  "x":8, "y":198, "w":224, "color":"#333333"},
        {"type":"label", "x":8, "y":204, "font":"small", "color":"#888888", "text":"DISK C:"},
        {"type":"value", "x":8, "y":218, "font":"body", "key":"c_used", "fmt":"%.0f GB", "color":"#FFFFFF", "w":100, "align":"left"},
        {"type":"label", "x":112, "y":218, "font":"body", "color":"#666666", "text":"/ "},
        {"type":"value", "x":124, "y":218, "font":"body", "key":"c_total", "fmt":"%.0f GB", "color":"#888888", "w":108, "align":"right"},
        {"type":"line",  "x":8, "y":240, "w":224, "color":"#333333"},
        {"type":"label", "x":8, "y":246, "font":"small", "color":"#888888", "text":"UPTIME"},
        {"type":"value", "x":60, "y":246, "font":"small", "key":"uptime", "fmt":"%.0fs", "color":"#CCCCCC", "w":172, "align":"right"},
        {"type":"label", "x":8, "y":266, "font":"small", "color":"#888888", "text":"PLAN"},
        {"type":"text",  "x":8, "y":282, "font":"small", "key":"claude.plan", "w":224, "color":"#58A6FF"}
      ]
    },
    {
      "bg": "#000000",
      "widgets": [
        {"type":"label", "x":8, "y":6, "font":"head", "color":"#58A6FF", "text":"GRAPHICS & NET"},
        {"type":"line",  "x":8, "y":30, "w":224, "color":"#333333"},
        {"type":"label", "x":8, "y":36, "font":"small", "color":"#888888", "text":"GPU 1"},
        {"type":"value", "x":8, "y":48, "font":"big", "key":"gpu1_temp", "fmt":"%.0f\u00b0C", "color":"#FFFFFF", "warn":75, "bad":85, "warn_color":"#FFA500", "bad_color":"#FF3030", "w":100, "align":"left"},
        {"type":"value", "x":120, "y":48, "font":"big", "key":"gpu1_load", "fmt":"%.0f%%", "color":"#FFFFFF", "warn":85, "bad":95, "warn_color":"#FFA500", "bad_color":"#FF3030", "w":112, "align":"right"},
        {"type":"bar",   "x":8, "y":78, "w":224, "h":8, "key":"gpu1_load", "min":0, "max":100, "color":"#3FB950", "bg":"#222222", "warn":85, "bad":95},
        {"type":"spark", "x":8, "y":90, "w":224, "h":28, "key":"gpu1_load", "min":0, "max":100, "samples":60, "color":"#58A6FF"},
        {"type":"label", "x":8, "y":122, "font":"small", "color":"#888888", "text":"VRAM"},
        {"type":"value", "x":48, "y":122, "font":"small", "key":"gpu1_vram", "fmt":"%.0f MB", "color":"#CCCCCC", "w":70, "align":"left"},
        {"type":"label", "x":128, "y":122, "font":"small", "color":"#888888", "text":"Clk"},
        {"type":"value", "x":156, "y":122, "font":"small", "key":"gpu1_clock", "fmt":"%.0f MHz", "color":"#CCCCCC", "w":76, "align":"right"},
        {"type":"line",  "x":8, "y":138, "w":224, "color":"#333333"},
        {"type":"label", "x":8, "y":144, "font":"small", "color":"#888888", "text":"GPU 2"},
        {"type":"value", "x":50, "y":144, "font":"small", "key":"gpu2_temp", "fmt":"%.0f\u00b0C", "color":"#FFFFFF", "w":50, "align":"left"},
        {"type":"value", "x":104, "y":144, "font":"small", "key":"gpu2_load", "fmt":"%.0f%%", "color":"#FFFFFF", "w":50, "align":"left"},
        {"type":"value", "x":160, "y":144, "font":"small", "key":"gpu2_vram", "fmt":"%.0fMB", "color":"#CCCCCC", "w":72, "align":"right"},
        {"type":"bar",   "x":8, "y":158, "w":224, "h":5, "key":"gpu2_load", "min":0, "max":100, "color":"#3FB950", "bg":"#222222"},
        {"type":"label", "x":8, "y":168, "font":"small", "color":"#888888", "text":"iGPU Load"},
        {"type":"value", "x":140, "y":168, "font":"small", "key":"igpu_load", "fmt":"%.0f%%", "color":"#FFFFFF", "w":92, "align":"right"},
        {"type":"bar",   "x":8, "y":182, "w":224, "h":5, "key":"igpu_load", "min":0, "max":100, "color":"#3FB950", "bg":"#222222"},
        {"type":"line",  "x":8, "y":192, "w":224, "color":"#333333"},
        {"type":"label", "x":8, "y":198, "font":"small", "color":"#888888", "text":"NET DOWN"},
        {"type":"value", "x":90, "y":198, "font":"small", "key":"net_down", "fmt":"%.1f KB/s", "scale":0.0009765625, "color":"#3FB950", "w":142, "align":"right"},
        {"type":"spark", "x":8, "y":212, "w":224, "h":36, "key":"net_down", "scale":0.0009765625, "min":0, "max":5000, "samples":60, "color":"#3FB950"},
        {"type":"label", "x":8, "y":254, "font":"small", "color":"#888888", "text":"NET UP"},
        {"type":"value", "x":90, "y":254, "font":"small", "key":"net_up", "fmt":"%.1f KB/s", "scale":0.0009765625, "color":"#58A6FF", "w":142, "align":"right"},
        {"type":"spark", "x":8, "y":268, "w":224, "h":36, "key":"net_up", "scale":0.0009765625, "min":0, "max":2000, "samples":60, "color":"#58A6FF"}
      ]
    },
    {
      "bg": "#000000",
      "widgets": [
        {"type":"label", "x":8, "y":6, "font":"head", "color":"#D29922", "text":"AI STATUS"},
        {"type":"line",  "x":8, "y":30, "w":224, "color":"#333333"},
        {"type":"label", "x":8, "y":36, "font":"small", "color":"#888888", "text":"Claude Session"},
        {"type":"value", "x":140, "y":36, "font":"body", "key":"claude.s_pct", "fmt":"%.0f%%", "color":"#FFFFFF", "warn":75, "bad":90, "warn_color":"#FFA500", "bad_color":"#FF3030", "w":92, "align":"right"},
        {"type":"bar",   "x":8, "y":54, "w":224, "h":8, "key":"claude.s_pct", "min":0, "max":100, "color":"#58A6FF", "bg":"#222222", "warn":75, "bad":90},
        {"type":"label", "x":8, "y":66, "font":"small", "color":"#666666", "text":"Reset:"},
        {"type":"text",  "x":52, "y":66, "font":"small", "key":"claude.s_reset_at", "w":180, "color":"#AAAAAA"},
        {"type":"label", "x":8, "y":84, "font":"small", "color":"#888888", "text":"Claude Weekly"},
        {"type":"value", "x":140, "y":84, "font":"body", "key":"claude.w_pct", "fmt":"%.0f%%", "color":"#FFFFFF", "warn":80, "bad":95, "warn_color":"#FFA500", "bad_color":"#FF3030", "w":92, "align":"right"},
        {"type":"bar",   "x":8, "y":102, "w":224, "h":8, "key":"claude.w_pct", "min":0, "max":100, "color":"#3FB950", "bg":"#222222", "warn":80, "bad":95},
        {"type":"label", "x":8, "y":114, "font":"small", "color":"#666666", "text":"Reset:"},
        {"type":"text",  "x":52, "y":114, "font":"small", "key":"claude.w_reset_at", "w":180, "color":"#AAAAAA"},
        {"type":"line",  "x":8, "y":132, "w":224, "color":"#333333"},
        {"type":"label", "x":8, "y":138, "font":"small", "color":"#888888", "text":"AGY Flash Quota"},
        {"type":"value", "x":140, "y":138, "font":"body", "key":"agy2.flash_pct", "fmt":"%.0f%%", "color":"#FFFFFF", "warn":80, "bad":95, "warn_color":"#FFA500", "bad_color":"#FF3030", "w":92, "align":"right"},
        {"type":"bar",   "x":8, "y":156, "w":224, "h":8, "key":"agy2.flash_pct", "min":0, "max":100, "color":"#BC8CFF", "bg":"#222222", "warn":80, "bad":95},
        {"type":"label", "x":8, "y":168, "font":"small", "color":"#666666", "text":"Reset:"},
        {"type":"text",  "x":52, "y":168, "font":"small", "key":"agy2.flash_reset_at", "w":180, "color":"#AAAAAA"},
        {"type":"line",  "x":8, "y":186, "w":224, "color":"#333333"},
        {"type":"label", "x":8, "y":192, "font":"small", "color":"#888888", "text":"ACTIVE AGENT"},
        {"type":"text",  "x":8, "y":208, "font":"body", "key":"agy.active", "w":224, "color":"#58A6FF"},
        {"type":"label", "x":8, "y":230, "font":"small", "color":"#888888", "text":"CLAUDE PLAN"},
        {"type":"text",  "x":8, "y":246, "font":"small", "key":"claude.plan", "w":224, "color":"#E6EDF3"},
        {"type":"label", "x":8, "y":264, "font":"small", "color":"#888888", "text":"STATUS / GUARD"},
        {"type":"text",  "x":8, "y":280, "font":"small", "key":"ai.status", "w":224, "color":"#8B949E"},
        {"type":"text",  "x":8, "y":296, "font":"small", "key":"ai.guard", "w":224, "color":"#FFA500"}
      ]
    }
  ]
})json";

static uint16_t parseColor(const char *str, uint16_t def) {
  if (!str) return def;
  if (*str == '#') str++;
  if (strlen(str) != 6) return def;
  unsigned int r = 0, g = 0, b = 0;
  if (sscanf(str, "%02x%02x%02x", &r, &g, &b) == 3) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
  }
  return def;
}

static const lgfx::IFont *getFont(WidgetFont wf) {
  switch (wf) {
    case FONT_SMALL: return fontSmall();
    case FONT_BODY:  return fontBody();
    case FONT_HEAD:  return fontHead();
    case FONT_BIG:   return fontBig();
    default:         return fontSmall();
  }
}

static void freeLayout(Layout *l) {
  if (!l) return;
  for (uint8_t p = 0; p < l->page_count; p++) {
    for (uint8_t w = 0; w < l->pages[p].widget_count; w++) {
      if (l->pages[p].widgets[w].spark_ring) {
        free(l->pages[p].widgets[w].spark_ring);
        l->pages[p].widgets[w].spark_ring = nullptr;
      }
    }
  }
  free(l);
}

static Layout *parseLayout(const char *json, size_t len) {
  if (!json || len == 0) return nullptr;
  cJSON *root = cJSON_ParseWithLength(json, len);
  if (!root) {
    logMsg("layout: JSON parse error");
    return nullptr;
  }

  cJSON *pages = cJSON_GetObjectItem(root, "pages");
  if (!cJSON_IsArray(pages)) {
    logMsg("layout: root 'pages' array missing");
    cJSON_Delete(root);
    return nullptr;
  }

  int numPages = cJSON_GetArraySize(pages);
  if (numPages <= 0) {
    logMsg("layout: 'pages' array empty");
    cJSON_Delete(root);
    return nullptr;
  }
  if (numPages > MAX_PAGES) {
    logMsg("layout: ignoring pages beyond limit of %d (found %d)", MAX_PAGES, numPages);
    numPages = MAX_PAGES;
  }

  Layout *l = (Layout *)heap_caps_malloc(sizeof(Layout), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!l) l = (Layout *)malloc(sizeof(Layout));
  if (!l) {
    logMsg("layout: out of memory for Layout struct");
    cJSON_Delete(root);
    return nullptr;
  }
  memset(l, 0, sizeof(Layout));
  l->page_count = (uint8_t)numPages;

  for (int pIdx = 0; pIdx < numPages; pIdx++) {
    cJSON *pageObj = cJSON_GetArrayItem(pages, pIdx);
    if (!cJSON_IsObject(pageObj)) {
      logMsg("layout: page %d is not an object, skipping", pIdx);
      continue;
    }
    Page &page = l->pages[pIdx];
    cJSON *bgItem = cJSON_GetObjectItem(pageObj, "bg");
    page.bg = (bgItem && cJSON_IsString(bgItem)) ? parseColor(bgItem->valuestring, 0x0000) : 0x0000;

    cJSON *widgets = cJSON_GetObjectItem(pageObj, "widgets");
    if (!cJSON_IsArray(widgets)) continue;

    int numWidgets = cJSON_GetArraySize(widgets);
    for (int wIdx = 0; wIdx < numWidgets; wIdx++) {
      if (page.widget_count >= MAX_WIDGETS_PER_PAGE) {
        logMsg("layout: page %d reached %d widget limit, skipping remaining", pIdx, MAX_WIDGETS_PER_PAGE);
        break;
      }
      cJSON *wObj = cJSON_GetArrayItem(widgets, wIdx);
      if (!cJSON_IsObject(wObj)) {
        logMsg("layout: page %d widget %d is not an object", pIdx, wIdx);
        continue;
      }

      cJSON *typeItem = cJSON_GetObjectItem(wObj, "type");
      if (!typeItem || !cJSON_IsString(typeItem)) {
        logMsg("layout: page %d widget %d missing 'type'", pIdx, wIdx);
        continue;
      }
      const char *typeStr = typeItem->valuestring;

      cJSON *xItem = cJSON_GetObjectItem(wObj, "x");
      cJSON *yItem = cJSON_GetObjectItem(wObj, "y");
      if (!xItem || !cJSON_IsNumber(xItem) || !yItem || !cJSON_IsNumber(yItem)) {
        logMsg("layout: page %d widget %d missing numeric 'x' or 'y'", pIdx, wIdx);
        continue;
      }

      Widget &w = page.widgets[page.widget_count];
      memset(&w, 0, sizeof(Widget));
      w.x = (int16_t)xItem->valueint;
      w.y = (int16_t)yItem->valueint;
      w.warn_val = NAN;
      w.bad_val = NAN;
      w.scale = 1.0;
      w.color = 0xFFFF;
      w.bg = 0x0000;
      w.has_bg = false;
      w.warn_color = 0xFD20; // #FFA500
      w.bad_color = 0xF800;  // #FF3030

      cJSON *wItem = cJSON_GetObjectItem(wObj, "w");
      if (wItem && cJSON_IsNumber(wItem)) w.w = (int16_t)wItem->valueint;

      cJSON *hItem = cJSON_GetObjectItem(wObj, "h");
      if (hItem && cJSON_IsNumber(hItem)) w.h = (int16_t)hItem->valueint;

      cJSON *fontItem = cJSON_GetObjectItem(wObj, "font");
      if (fontItem && cJSON_IsString(fontItem)) {
        if (!strcmp(fontItem->valuestring, "small")) w.font = FONT_SMALL;
        else if (!strcmp(fontItem->valuestring, "body")) w.font = FONT_BODY;
        else if (!strcmp(fontItem->valuestring, "head")) w.font = FONT_HEAD;
        else if (!strcmp(fontItem->valuestring, "big")) w.font = FONT_BIG;
      }

      cJSON *alignItem = cJSON_GetObjectItem(wObj, "align");
      if (alignItem && cJSON_IsString(alignItem)) {
        if (!strcmp(alignItem->valuestring, "right")) w.align = ALIGN_RIGHT;
        else if (!strcmp(alignItem->valuestring, "center")) w.align = ALIGN_CENTER;
        else w.align = ALIGN_LEFT;
      }

      cJSON *colItem = cJSON_GetObjectItem(wObj, "color");
      if (colItem && cJSON_IsString(colItem)) w.color = parseColor(colItem->valuestring, 0xFFFF);

      cJSON *bgValItem = cJSON_GetObjectItem(wObj, "bg");
      if (bgValItem && cJSON_IsString(bgValItem)) {
        w.bg = parseColor(bgValItem->valuestring, 0x0000);
        w.has_bg = true;
      }

      cJSON *scaleItem = cJSON_GetObjectItem(wObj, "scale");
      if (scaleItem && cJSON_IsNumber(scaleItem)) w.scale = scaleItem->valuedouble;

      cJSON *warnItem = cJSON_GetObjectItem(wObj, "warn");
      if (warnItem && cJSON_IsNumber(warnItem)) w.warn_val = warnItem->valuedouble;

      cJSON *badItem = cJSON_GetObjectItem(wObj, "bad");
      if (badItem && cJSON_IsNumber(badItem)) w.bad_val = badItem->valuedouble;

      cJSON *warnColItem = cJSON_GetObjectItem(wObj, "warn_color");
      if (warnColItem && cJSON_IsString(warnColItem)) w.warn_color = parseColor(warnColItem->valuestring, 0xFD20);

      cJSON *badColItem = cJSON_GetObjectItem(wObj, "bad_color");
      if (badColItem && cJSON_IsString(badColItem)) w.bad_color = parseColor(badColItem->valuestring, 0xF800);

      cJSON *fmtItem = cJSON_GetObjectItem(wObj, "fmt");
      if (fmtItem && cJSON_IsString(fmtItem)) {
        if (strlen(fmtItem->valuestring) >= sizeof(w.fmt) || !fmtValid(fmtItem->valuestring)) {
          logMsg("layout: page %d widget %d bad 'fmt' (one %%f/%%e/%%g only)", pIdx, wIdx);
          continue;
        }
        strcpy(w.fmt, fmtItem->valuestring);
      }

      cJSON *unitItem = cJSON_GetObjectItem(wObj, "unit");
      if (unitItem && cJSON_IsString(unitItem)) {
        strncpy(w.unit, unitItem->valuestring, sizeof(w.unit) - 1);
        w.unit[sizeof(w.unit) - 1] = '\0';
      }

      cJSON *keyItem = cJSON_GetObjectItem(wObj, "key");
      if (keyItem && cJSON_IsString(keyItem)) {
        strncpy(w.key, keyItem->valuestring, sizeof(w.key) - 1);
        w.key[sizeof(w.key) - 1] = '\0';
      }

      if (!strcmp(typeStr, "label")) {
        cJSON *textItem = cJSON_GetObjectItem(wObj, "text");
        if (!textItem || !cJSON_IsString(textItem)) {
          logMsg("layout: page %d widget %d label missing 'text'", pIdx, wIdx);
          continue;
        }
        w.type = WIDGET_LABEL;
        strncpy(w.text, textItem->valuestring, sizeof(w.text) - 1);
        w.text[sizeof(w.text) - 1] = '\0';
      } else if (!strcmp(typeStr, "value")) {
        if (!w.key[0]) {
          logMsg("layout: page %d widget %d value missing 'key'", pIdx, wIdx);
          continue;
        }
        w.type = WIDGET_VALUE;
      } else if (!strcmp(typeStr, "text")) {
        if (!w.key[0]) {
          logMsg("layout: page %d widget %d text missing 'key'", pIdx, wIdx);
          continue;
        }
        w.type = WIDGET_TEXT;
      } else if (!strcmp(typeStr, "bar")) {
        if (!w.key[0]) {
          logMsg("layout: page %d widget %d bar missing 'key'", pIdx, wIdx);
          continue;
        }
        if (w.w <= 0 || w.h <= 0) {
          logMsg("layout: page %d widget %d bar has invalid dimensions (%d x %d)", pIdx, wIdx, w.w, w.h);
          continue;
        }
        w.type = WIDGET_BAR;
        cJSON *minItem = cJSON_GetObjectItem(wObj, "min");
        cJSON *maxItem = cJSON_GetObjectItem(wObj, "max");
        w.min_val = (minItem && cJSON_IsNumber(minItem)) ? minItem->valuedouble : 0.0;
        w.max_val = (maxItem && cJSON_IsNumber(maxItem)) ? maxItem->valuedouble : 100.0;
        if (!w.has_bg) { w.bg = 0x2104; w.has_bg = true; } // #222222
        if (!colItem) w.color = 0x3DC6; // #3FB950
      } else if (!strcmp(typeStr, "spark")) {
        if (!w.key[0]) {
          logMsg("layout: page %d widget %d spark missing 'key'", pIdx, wIdx);
          continue;
        }
        if (w.w <= 0 || w.h <= 0) {
          logMsg("layout: page %d widget %d spark has invalid dimensions (%d x %d)", pIdx, wIdx, w.w, w.h);
          continue;
        }
        w.type = WIDGET_SPARK;
        cJSON *minItem = cJSON_GetObjectItem(wObj, "min");
        cJSON *maxItem = cJSON_GetObjectItem(wObj, "max");
        w.min_val = (minItem && cJSON_IsNumber(minItem)) ? minItem->valuedouble : 0.0;
        w.max_val = (maxItem && cJSON_IsNumber(maxItem)) ? maxItem->valuedouble : 100.0;
        cJSON *sampItem = cJSON_GetObjectItem(wObj, "samples");
        int samples = (sampItem && cJSON_IsNumber(sampItem)) ? sampItem->valueint : 60;
        if (samples < 2) samples = 2;
        if (samples > 240) samples = 240;
        w.spark_samples = (uint16_t)samples;
        w.spark_ring = (float *)heap_caps_malloc(samples * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!w.spark_ring) w.spark_ring = (float *)malloc(samples * sizeof(float));
        if (w.spark_ring) {
          for (int s = 0; s < samples; s++) w.spark_ring[s] = NAN;
        }
        w.spark_count = 0;
        w.spark_head = 0;
        w.spark_last_seq = 0xFFFFFFFF;
        w.last_drawn_seq = 0xFFFFFFFF;
      } else if (!strcmp(typeStr, "line")) {
        w.type = WIDGET_LINE;
        if (w.h <= 0) w.h = 1;
        if (w.w <= 0) w.w = TFT_W - w.x;
      } else {
        logMsg("layout: page %d widget %d unknown type '%s'", pIdx, wIdx, typeStr);
        continue;
      }

      page.widget_count++;
    }
  }

  cJSON_Delete(root);
  return l;
}

static void drawSpark(LGFX_Sprite &c, const Page &page, Widget &w, bool full, uint32_t curSeq) {
  // Update spark ring buffer once per new pcStats sample
  if (curSeq != w.spark_last_seq) {
    w.spark_last_seq = curSeq;
    double val = pcStat(w.key);
    if (!isnan(val) && w.scale != 0.0 && w.scale != 1.0) val *= w.scale;
    if (w.spark_ring && w.spark_samples > 0) {
      w.spark_ring[w.spark_head] = (float)val;
      w.spark_head = (w.spark_head + 1) % w.spark_samples;
      if (w.spark_count < w.spark_samples) w.spark_count++;
    }
  }

  // If not full redraw and no new sample arrived, skip
  if (!full && w.last_drawn_seq == curSeq) return;

  uint16_t bg = w.has_bg ? w.bg : page.bg;
  c.fillRect(w.x, w.y, w.w, w.h, bg);

  if (w.spark_count > 0 && w.spark_ring) {
    float minV = (float)w.min_val;
    float maxV = (float)w.max_val;
    if (maxV <= minV) maxV = minV + 1.0f;

    int prevX = 0, prevY = 0;
    bool hasPrev = false;

    for (uint16_t i = 0; i < w.spark_count; i++) {
      int ringIdx = (w.spark_count < w.spark_samples)
                        ? (int)i
                        : (int)((w.spark_head + i) % w.spark_samples);
      float val = w.spark_ring[ringIdx];

      int px = w.x;
      if (w.spark_samples > 1) {
        px = w.x + (int)round((double)i * (w.w - 1) / (w.spark_samples - 1));
      }

      if (!isnan(val)) {
        if (val < minV) val = minV;
        if (val > maxV) val = maxV;
        float norm = (val - minV) / (maxV - minV);
        int py = w.y + w.h - 1 - (int)round(norm * (w.h - 1));

        if (hasPrev) {
          c.drawLine(prevX, prevY, px, py, w.color);
        } else if (w.spark_count == 1) {
          c.drawPixel(px, py, w.color);
        }
        prevX = px;
        prevY = py;
        hasPrev = true;
      } else {
        hasPrev = false;
      }
    }
  }
  w.last_drawn_seq = curSeq;
}

static void drawBar(LGFX_Sprite &c, Widget &w, bool full) {
  double val = pcStat(w.key);
  if (!isnan(val) && w.scale != 0.0 && w.scale != 1.0) val *= w.scale;

  int fillW = 0;
  uint16_t col = w.color;

  if (!isnan(val)) {
    double minV = w.min_val;
    double maxV = w.max_val;
    if (maxV <= minV) maxV = minV + 1.0;
    double norm = (val - minV) / (maxV - minV);
    if (norm < 0.0) norm = 0.0;
    if (norm > 1.0) norm = 1.0;
    fillW = (int)round(norm * w.w);

    bool hasWarn = !isnan(w.warn_val);
    bool hasBad = !isnan(w.bad_val);
    if (hasWarn && hasBad) {
      if (w.bad_val >= w.warn_val) {
        if (val >= w.bad_val) col = w.bad_color;
        else if (val >= w.warn_val) col = w.warn_color;
      } else {
        if (val <= w.bad_val) col = w.bad_color;
        else if (val <= w.warn_val) col = w.warn_color;
      }
    } else if (hasBad) {
      if (val >= w.bad_val) col = w.bad_color;
    } else if (hasWarn) {
      if (val >= w.warn_val) col = w.warn_color;
    }
  }

  if (!full && fillW == w.last_fill_w && col == w.last_color) return;

  uint16_t bg = w.has_bg ? w.bg : 0x2104; // default dim bg
  c.fillRect(w.x, w.y, w.w, w.h, bg);
  if (fillW > 0) {
    c.fillRect(w.x, w.y, fillW, w.h, col);
  }
  w.last_fill_w = fillW;
  w.last_color = col;
}

static void drawValue(LGFX_Sprite &c, const Page &page, Widget &w, bool full) {
  int bw = (w.w > 0) ? w.w : (TFT_W - w.x);
  const lgfx::IFont *f = getFont(w.font);
  c.setFont(f);
  int bh = c.fontHeight();

  double val = pcStat(w.key);
  if (!isnan(val) && w.scale != 0.0 && w.scale != 1.0) val *= w.scale;

  char valBuf[64];
  uint16_t col = w.color;

  if (isnan(val)) {
    snprintf(valBuf, sizeof(valBuf), "--");
  } else {
    bool hasWarn = !isnan(w.warn_val);
    bool hasBad = !isnan(w.bad_val);
    if (hasWarn && hasBad) {
      if (w.bad_val >= w.warn_val) {
        if (val >= w.bad_val) col = w.bad_color;
        else if (val >= w.warn_val) col = w.warn_color;
      } else {
        if (val <= w.bad_val) col = w.bad_color;
        else if (val <= w.warn_val) col = w.warn_color;
      }
    } else if (hasBad) {
      if (val >= w.bad_val) col = w.bad_color;
    } else if (hasWarn) {
      if (val >= w.warn_val) col = w.warn_color;
    }

    if (w.fmt[0] != '\0') {
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#pragma GCC diagnostic ignored "-Wformat-security"
#endif
      snprintf(valBuf, sizeof(valBuf), w.fmt, val);
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    } else {
      if (val == floor(val) && fabs(val) <= 9007199254740992.0) {
        snprintf(valBuf, sizeof(valBuf), "%.0f", val);
      } else {
        snprintf(valBuf, sizeof(valBuf), "%.1f", val);
      }
    }

    if (w.unit[0] != '\0') {
      size_t len = strlen(valBuf);
      snprintf(valBuf + len, sizeof(valBuf) - len, "%s", w.unit);
    }
  }

  if (!full && col == w.last_color && !strcmp(valBuf, w.last_text)) return;

  c.fillRect(w.x, w.y, bw, bh, page.bg);
  c.setTextColor(col, page.bg);
  c.setTextSize(1);

  if (w.align == ALIGN_RIGHT) {
    c.setTextDatum(lgfx::textdatum_t::top_right);
    c.drawString(valBuf, w.x + bw, w.y);
  } else if (w.align == ALIGN_CENTER) {
    c.setTextDatum(lgfx::textdatum_t::top_center);
    c.drawString(valBuf, w.x + bw / 2, w.y);
  } else {
    c.setTextDatum(lgfx::textdatum_t::top_left);
    c.drawString(valBuf, w.x, w.y);
  }

  memcpy(w.last_text, valBuf, sizeof(w.last_text));   // same 64-byte size, NUL-terminated
  w.last_text[sizeof(w.last_text) - 1] = '\0';
  w.last_color = col;
}

static void drawText(LGFX_Sprite &c, const Page &page, Widget &w, bool full) {
  int bw = (w.w > 0) ? w.w : (TFT_W - w.x);
  const lgfx::IFont *f = getFont(w.font);
  c.setFont(f);
  int bh = c.fontHeight();

  const char *rawText = pcText(w.key);
  if (!rawText) rawText = "";

  char textBuf[64];
  strncpy(textBuf, rawText, sizeof(textBuf) - 1);
  textBuf[sizeof(textBuf) - 1] = '\0';
  while (textBuf[0] != '\0' && c.textWidth(textBuf) > bw) {
    textBuf[strlen(textBuf) - 1] = '\0';
  }

  if (!full && !strcmp(textBuf, w.last_text)) return;

  c.fillRect(w.x, w.y, bw, bh, page.bg);
  c.setTextColor(w.color, page.bg);
  c.setTextSize(1);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString(textBuf, w.x, w.y);

  memcpy(w.last_text, textBuf, sizeof(w.last_text));   // same 64-byte size, NUL-terminated
  w.last_text[sizeof(w.last_text) - 1] = '\0';
}

static void drawLabel(LGFX_Sprite &c, const Page &page, Widget &w) {
  const lgfx::IFont *f = getFont(w.font);
  c.setFont(f);
  c.setTextColor(w.color, page.bg);
  c.setTextSize(1);

  if (w.align == ALIGN_RIGHT && w.w > 0) {
    c.setTextDatum(lgfx::textdatum_t::top_right);
    c.drawString(w.text, w.x + w.w, w.y);
  } else if (w.align == ALIGN_CENTER && w.w > 0) {
    c.setTextDatum(lgfx::textdatum_t::top_center);
    c.drawString(w.text, w.x + w.w / 2, w.y);
  } else {
    c.setTextDatum(lgfx::textdatum_t::top_left);
    c.drawString(w.text, w.x, w.y);
  }
}

static void drawLine(LGFX_Sprite &c, Widget &w) {
  int lw = (w.w > 0) ? w.w : (TFT_W - w.x);
  int lh = (w.h > 0) ? w.h : 1;
  c.fillRect(w.x, w.y, lw, lh, w.color);
}

static void layoutDrawLocked(int dashIdx, LGFX_Sprite &c, bool full);

void layoutDraw(int dashIdx, LGFX_Sprite &c, bool full) {
  lock();
  layoutDrawLocked(dashIdx, c, full);
  unlock();
}

static void layoutDrawLocked(int dashIdx, LGFX_Sprite &c, bool full) {
  if (!activeLayout || dashIdx < 0 || dashIdx >= activeLayout->page_count) {
    c.fillScreen(0x0000);
    c.setFont(fontBody());
    c.setTextColor(0xFFFF, 0x0000);
    c.setTextDatum(lgfx::textdatum_t::middle_center);
    char buf[32];
    snprintf(buf, sizeof(buf), "no page %d", dashIdx + 1);
    c.drawString(buf, TFT_W / 2, TFT_H / 2);
    return;
  }

  Page &page = activeLayout->pages[dashIdx];

  if (!activeLayout->page_drawn[dashIdx]) {
    full = true;
  }

  if (full) {
    c.fillScreen(page.bg);
    activeLayout->page_drawn[dashIdx] = true;
  }

  uint32_t curSeq = pcStatsSeq();

  for (uint8_t i = 0; i < page.widget_count; i++) {
    Widget &w = page.widgets[i];
    switch (w.type) {
      case WIDGET_LABEL:
        if (full) drawLabel(c, page, w);
        break;
      case WIDGET_LINE:
        if (full) drawLine(c, w);
        break;
      case WIDGET_VALUE:
        drawValue(c, page, w, full);
        break;
      case WIDGET_TEXT:
        drawText(c, page, w, full);
        break;
      case WIDGET_BAR:
        drawBar(c, w, full);
        break;
      case WIDGET_SPARK:
        drawSpark(c, page, w, full, curSeq);
        break;
    }
  }
}

uint32_t layoutPeriodMs(int dashIdx) {
  uint32_t ms = 0;
  lock();
  if (activeLayout && dashIdx >= 0 && dashIdx < activeLayout->page_count) {
    const Page &page = activeLayout->pages[dashIdx];
    for (uint8_t i = 0; i < page.widget_count; i++) {
      const Widget &w = page.widgets[i];
      if ((w.type == WIDGET_VALUE || w.type == WIDGET_TEXT) && !strcmp(w.key, "uptime")) ms = 1000;
    }
  }
  unlock();
  return ms;
}

const char *layoutApply(const char *json, size_t len) {
  if (!json || len == 0) return "empty JSON";
  if (len > 16384) return "JSON too large (>16KB)";

  Layout *l = parseLayout(json, len);
  if (!l) return "JSON parse failed";

  FILE *f = fopen("/ffat/layout.tmp", "wb");
  if (!f) {
    freeLayout(l);
    return "failed to open /ffat/layout.tmp";
  }
  size_t written = fwrite(json, 1, len, f);
  fclose(f);
  if (written != len) {
    remove("/ffat/layout.tmp");
    freeLayout(l);
    return "write to /ffat/layout.tmp failed";
  }

  remove("/ffat/layout.json");
  if (rename("/ffat/layout.tmp", "/ffat/layout.json") != 0) {
    remove("/ffat/layout.tmp");
    freeLayout(l);
    return "rename to /ffat/layout.json failed";
  }

  l->is_default = false;
  uint8_t pages = l->page_count;
  swapLayout(l);
  logMsg("layout: %u pages loaded", pages);
  return nullptr;
}

void layoutInit() {
  FILE *f = fopen("/ffat/layout.json", "rb");
  if (f) {
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz > 0 && sz <= 16384) {
      char *buf = (char *)heap_caps_malloc(sz + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!buf) buf = (char *)malloc(sz + 1);
      if (buf) {
        size_t rd = fread(buf, 1, sz, f);
        buf[rd] = '\0';
        fclose(f);
        f = nullptr;
        Layout *l = parseLayout(buf, rd);
        free(buf);
        if (l) {
          l->is_default = false;
          activeLayout = l;
          logMsg("layout: loaded /ffat/layout.json (%u pages)", activeLayout->page_count);
          return;
        }
      }
    }
    if (f) fclose(f);
    logMsg("layout: /ffat/layout.json invalid, loading built-in default");
  } else {
    logMsg("layout: /ffat/layout.json not found, loading built-in default");
  }

  Layout *l = parseLayout(BUILTIN_LAYOUT_JSON, strlen(BUILTIN_LAYOUT_JSON));
  if (l) {
    l->is_default = true;
    activeLayout = l;
    logMsg("layout: loaded built-in default (%u pages)", activeLayout->page_count);
  } else {
    logMsg("layout: critical error parsing built-in default");
  }
}

void layoutCommand(const char *a1, const char *a2) {
  if (!a1 || !strcmp(a1, "show")) {
    if (!activeLayout) {
      logMsg("layout: none loaded");
      return;
    }
    logMsg("layout: %u pages (%s)", activeLayout->page_count,
           activeLayout->is_default ? "built-in default" : "/ffat/layout.json");
    for (uint8_t i = 0; i < activeLayout->page_count; i++) {
      logMsg("layout: page %u: %u widgets", i, activeLayout->pages[i].widget_count);
    }
  } else if (!strcmp(a1, "reload")) {
    FILE *f = fopen("/ffat/layout.json", "rb");
    if (!f) {
      logMsg("layout: /ffat/layout.json not found");
      return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 16384) {
      fclose(f);
      logMsg("layout: invalid file size (%ld bytes)", sz);
      return;
    }
    char *buf = (char *)heap_caps_malloc(sz + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = (char *)malloc(sz + 1);
    if (!buf) {
      fclose(f);
      logMsg("layout: out of memory");
      return;
    }
    size_t rd = fread(buf, 1, sz, f);
    fclose(f);
    buf[rd] = '\0';
    Layout *l = parseLayout(buf, rd);
    free(buf);
    if (!l) {
      logMsg("layout: parse failed, keeping current layout");
      return;
    }
    l->is_default = false;
    uint8_t pages = l->page_count;
    swapLayout(l);
    logMsg("layout: %u pages loaded", pages);
  } else if (!strcmp(a1, "default")) {
    remove("/ffat/layout.json");
    Layout *l = parseLayout(BUILTIN_LAYOUT_JSON, strlen(BUILTIN_LAYOUT_JSON));
    if (!l) {
      logMsg("layout: error parsing built-in default");
      return;
    }
    l->is_default = true;
    uint8_t pages = l->page_count;
    swapLayout(l);
    logMsg("layout: %u pages loaded", pages);
  } else if (!strcmp(a1, "begin")) {
    if (uploadBuf) free(uploadBuf);
    uploadBuf = (char *)heap_caps_malloc(UPLOAD_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!uploadBuf) uploadBuf = (char *)malloc(UPLOAD_BUF_SIZE);
    uploadLen = 0;
    if (!uploadBuf) {
      logMsg("layout: out of memory for upload buffer");
      return;
    }
    logMsg("layout: upload started");
  } else if (!strcmp(a1, "data")) {
    if (!uploadBuf) {
      logMsg("layout: no upload in progress (run layout begin)");
      return;
    }
    if (!a2 || !*a2) return;
    size_t chunkLen = strlen(a2);
    if (uploadLen + chunkLen >= UPLOAD_BUF_SIZE) {
      logMsg("layout: upload exceeded size limit");
      free(uploadBuf);
      uploadBuf = nullptr;
      uploadLen = 0;
      return;
    }
    memcpy(uploadBuf + uploadLen, a2, chunkLen);
    uploadLen += chunkLen;
    uploadBuf[uploadLen] = '\0';
  } else if (!strcmp(a1, "end")) {
    if (!uploadBuf) {
      logMsg("layout: no upload in progress");
      return;
    }
    size_t maxDecoded = 16384;
    char *decoded = (char *)heap_caps_malloc(maxDecoded + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!decoded) decoded = (char *)malloc(maxDecoded + 1);
    if (!decoded) {
      logMsg("layout: out of memory for decode");
      free(uploadBuf);
      uploadBuf = nullptr;
      uploadLen = 0;
      return;
    }
    size_t outLen = 0;
    int ret = mbedtls_base64_decode((unsigned char *)decoded, maxDecoded, &outLen,
                                    (const unsigned char *)uploadBuf, uploadLen);
    free(uploadBuf);
    uploadBuf = nullptr;
    uploadLen = 0;
    if (ret != 0) {
      free(decoded);
      logMsg("layout: base64 decode failed (%d)", ret);
      return;
    }
    decoded[outLen] = '\0';
    const char *err = layoutApply(decoded, outLen);
    free(decoded);
    if (err) {
      logMsg("layout: %s", err);
    }
  } else {
    logMsg("layout: usage layout <show|reload|default|begin|data <b64>|end>");
  }
}
