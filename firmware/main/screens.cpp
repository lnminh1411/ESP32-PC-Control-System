// Desk screens (Screen 1: PC stats/GIF, Screen 2: Media/Weather, Screen 3: AI/Clock).
// Ported from .mock/screens_mockup.html to pure ESP-IDF 5.5 C++ with LovyanGFX.
#include "screens.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <LovyanGFX.hpp>

#include "art.h"
#include "board_time.h"
#include "config.h"
#include "display.h"
#include "fonts.h"
#include "pc_going.h"
#include "pc_state.h"
#include "pcstats.h"
#include "pcwarn.h"
#include "sys.h"
#include "textimg.h"
#include "weather.h"

// 16-bit RGB565 palette matching the mockup (survives 4 bits per channel wire format).
constexpr uint16_t C_BG = 0x0000;            // #000000
constexpr uint16_t C_CARD = 0x18C5;          // #1a1a2a
constexpr uint16_t C_TRACK = 0x3188;         // #333344
constexpr uint16_t C_TEXT = 0xEF7D;          // #eeeeee
constexpr uint16_t C_DIM = 0x8CD5;           // #8899aa
constexpr uint16_t C_FAINT = 0x532E;         // #556677
constexpr uint16_t C_CPU = 0x455F;           // #44aaff
constexpr uint16_t C_RAM = 0xBC5F;           // #bb88ff
constexpr uint16_t C_AMD = 0xFAA8;           // #ff5544
constexpr uint16_t C_NV = 0x8EE6;            // #88dd33
constexpr uint16_t C_DISK = 0xFD46;          // #ffaa33
constexpr uint16_t C_FAN = 0x46F9;           // #44ddcc
constexpr uint16_t C_GOOD = 0x46EC;          // #44dd66
constexpr uint16_t C_WARN = 0xFE64;          // #ffcc22
constexpr uint16_t C_BAD = 0xFA26;           // #ff4433
constexpr uint16_t C_PURPLE = 0xAABB;        // #aa55dd
constexpr uint16_t C_CLAUDE = 0xEBAA;        // #ee7755
constexpr uint16_t C_AGY = 0x64DF;           // #6699ff

// Weather screen palette
constexpr uint16_t C_WX_BG = 0x1108;         // #112244
constexpr uint16_t C_WX_CLOUD = 0xDF7F;      // #ddeeff
constexpr uint16_t C_WX_RAIN = 0x8E7F;       // #88ccff
constexpr uint16_t C_WX_TEXT_DIM = 0x74D7;   // #7799bb
constexpr uint16_t C_WX_TEXT_MID = 0xCEFD;   // #ccddee
constexpr uint16_t C_WX_BOX_BG = 0x218A;     // #223355
constexpr uint16_t C_WX_DIV = 0x322C;        // #334466

// Local AI screen palette
constexpr uint16_t C_AI_BG = 0x1043;         // #110a1a
constexpr uint16_t C_AI_CELL = 0x20C6;       // #221833
constexpr uint16_t C_AI_PULSE_DIM = 0x510E;  // #552277

// Clock palette
constexpr uint16_t C_CLOCK_HOURS = 0x9E7D;   // #99ccee

// Threshold severity color picker: good if < warn, warn if < bad, else bad.
static inline uint16_t sev(double v, double w, double b) {
  if (isnan(v)) return C_DIM;
  if (v >= b) return C_BAD;
  if (v >= w) return C_WARN;
  return C_GOOD;
}

// Rounded card background container.
static inline void drawCard(LGFX_Sprite &c, int x, int y, int w, int h) {
  c.fillRoundRect(x, y, w, h, 6, C_CARD);
}

// PC face cards under a usage warning (pcwarn.h): the card turns dark amber
// (high) or dark red (critical) and its temperature takes the warning colour
constexpr uint16_t C_CARD_WARN = 0x3982;     // #3d3210
constexpr uint16_t C_CARD_BAD = 0x40C3;      // #401818

static PcWarnLevel worst(PcWarnLevel a, PcWarnLevel b) { return a > b ? a : b; }

static void drawWarnCard(LGFX_Sprite &c, int x, int y, int w, int h, PcWarnLevel level) {
  c.fillRoundRect(x, y, w, h, 6, level == PCWARN_CRITICAL ? C_CARD_BAD : level == PCWARN_HIGH ? C_CARD_WARN : C_CARD);
}

static uint16_t warnTempColor(double temp, PcWarnLevel level) {
  if (isnan(temp)) return C_DIM;
  return level == PCWARN_CRITICAL ? C_BAD : level == PCWARN_HIGH ? C_WARN : C_GOOD;
}

// Rounded horizontal progress bar with track and thumb.
static void drawBar(LGFX_Sprite &c, int x, int y, int w, int h, float frac, uint16_t hex) {
  int r = h >> 1;
  c.fillRoundRect(x, y, w, h, r, C_TRACK);
  if (isnan(frac) || frac <= 0.0f) return;
  if (frac > 1.0f) frac = 1.0f;
  int fw = (int)roundf((float)w * frac);
  if (fw > 1) {
    int fillW = fw < h ? h : fw;
    if (fillW > w) fillW = w;
    c.fillRoundRect(x, y, fillW, h, r, hex);
  }
}

// Small rounded status badge/chip.
static void drawChip(LGFX_Sprite &c, int x, int y, const char *label, uint16_t bgHex, uint16_t fgHex, bool rightAlign = true) {
  c.setFont(fontSmall());
  int w = c.textWidth(label) + 12;
  int x0 = rightAlign ? (x - w) : x;
  c.fillRoundRect(x0, y, w, 18, 9, bgHex);
  c.setTextColor(fgHex);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString(label, x0 + 6, y + 1);
}

// Format byte rate into KB/s or MB/s.
static void formatRate(double b, char *buf, size_t len) {
  if (isnan(b)) {
    snprintf(buf, len, "--");
  } else {
    // no decimal from 100 up, so the rate stays clear of a 2-digit-day uptime
    bool mb = b >= 1048576.0;
    double v = mb ? b / 1048576.0 : b / 1024.0;
    snprintf(buf, len, v >= 100.0 ? "%.0f %s" : "%.1f %s", v, mb ? "MB/s" : "KB/s");
  }
}

// Reset line from a Unix reset time (tsKey): countdown, then local time.
//   session: "Resets in 02:01:25 | 13:30"
//   weekly:  "Resets in 02:16:31:25 | Oct 28" (dd:hh:mm:ss)
// The "Resets in " prefix goes when the line would not fit the card.
static void drawReset(LGFX_Sprite &c, const char *tsKey, bool weekly, int x, int y, int maxW) {
  static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  char val[40], buf[56];
  double ts = pcStat(tsKey);
  struct tm at;
  if (isnan(ts) || ts <= 0 || !boardLocalTimeAt((time_t)ts, &at)) {
    snprintf(val, sizeof(val), "--");
  } else {
    long left = (long)(ts - (double)time(nullptr));
    if (left < 0) left = 0;
    long d = left / 86400, h = (left / 3600) % 24, m = (left / 60) % 60, sec = left % 60;
    if (weekly)
      snprintf(val, sizeof(val), "%02ld:%02ld:%02ld:%02ld | %s %d", d, h, m, sec, months[at.tm_mon % 12], at.tm_mday);
    else
      snprintf(val, sizeof(val), "%02ld:%02ld:%02ld | %02d:%02d", d * 24 + h, m, sec, at.tm_hour, at.tm_min);
  }
  snprintf(buf, sizeof(buf), "Resets in %s", val);
  c.setFont(fontSmall());
  c.setTextColor(C_FAINT);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString(c.textWidth(buf) <= maxW ? buf : val, x, y);
}

// GPU load averaged over ~2 s: at idle clocks the cards report bursty
// utilization (a wallpaper frame keeps a 210 MHz core busy), so the raw
// value swings 0-50% between samples.
static double smoothedStat(int slot, const char *key) {
  static double val[2];
  static uint32_t lastMs[2];
  double v = pcStat(key);
  if (isnan(v)) return v;
  uint32_t now = nowMs();
  if (isnan(val[slot]) || !lastMs[slot] || now - lastMs[slot] > 10000) {
    val[slot] = v;
  } else {
    val[slot] += (v - val[slot]) * (1.0 - exp(-(double)(now - lastMs[slot]) / 2000.0));
  }
  lastMs[slot] = now;
  return val[slot];
}

// Format uptime seconds into "1d 20:44:16" or "20:44:16".
static void formatUptime(double uptimeSec, char *buf, size_t len) {
  if (isnan(uptimeSec) || uptimeSec < 0.0) {
    snprintf(buf, len, "--");
    return;
  }
  uint32_t s = (uint32_t)uptimeSec;
  uint32_t d = s / 86400;
  uint32_t h = (s / 3600) % 24;
  uint32_t m = (s / 60) % 60;
  uint32_t sec = s % 60;
  if (d >= 100) {
    // 3-digit days: seconds dropped so it stays clear of the net rates
    snprintf(buf, len, "%lud %02lu:%02lu", (unsigned long)d, (unsigned long)h, (unsigned long)m);
  } else if (d > 0) {
    snprintf(buf, len, "%lud %02lu:%02lu:%02lu", (unsigned long)d, (unsigned long)h, (unsigned long)m,
             (unsigned long)sec);
  } else {
    snprintf(buf, len, "%02lu:%02lu:%02lu", (unsigned long)h, (unsigned long)m, (unsigned long)sec);
  }
}

// 56-sample ring buffer for net_down sparkline, sampled on pcStatsSeq() changes.
static float netDownHist[56] = {0};
static int netHistCount = 0;
static int netHistHead = 0;
static uint32_t lastNetSeq = 0;

static void updateNetHistory() {
  uint32_t seq = pcStatsSeq();
  if (seq != lastNetSeq) {
    lastNetSeq = seq;
    double down = pcStat("net_down");
    float v = (isnan(down) || down < 0.0) ? 0.0f : (float)down;
    netDownHist[netHistHead] = v;
    netHistHead = (netHistHead + 1) % 56;
    if (netHistCount < 56) netHistCount++;
  }
}

static void drawSparkline(LGFX_Sprite &c) {
  updateNetHistory();
  float samples[56];
  if (netHistCount < 56) {
    int empty = 56 - netHistCount;
    for (int i = 0; i < empty; i++) samples[i] = 0.0f;
    for (int i = 0; i < netHistCount; i++) samples[empty + i] = netDownHist[i];
  } else {
    for (int i = 0; i < 56; i++) {
      samples[i] = netDownHist[(netHistHead + i) % 56];
    }
  }

  float maxVal = 0.0f;
  for (int i = 0; i < 56; i++) {
    if (samples[i] > maxVal) maxVal = samples[i];
  }

  if (maxVal < 1.0f) {
    c.drawLine(12, 308, 232, 308, C_CPU);
    return;
  }

  for (int i = 1; i < 56; i++) {
    int x0 = 12 + (i - 1) * 4;
    int y0 = 308 - (int)roundf((samples[i - 1] / maxVal) * 18.0f);
    int x1 = 12 + i * 4;
    int y1 = 308 - (int)roundf((samples[i] / maxVal) * 18.0f);
    c.drawLine(x0, y0, x1, y1, C_CPU);
  }
}

// Back-and-forth marquee: 30 px/s, 1.5 s pause at both ends, clip to 224 px window.
static char lastTitle[128] = "";
static uint32_t marqueeStartMs = 0;

static void drawMarquee(LGFX_Sprite &c, const char *key, const char *s, int y, const lgfx::IFont *f, uint16_t hex) {
  c.setFont(f);
  int w = textImgWidth(c, key, s);
  const int room = 224;
  if (w <= room) {
    c.setTextColor(hex);
    c.setTextDatum(lgfx::textdatum_t::top_center);
    textImgDraw(c, key, s, 120, y);
    return;
  }

  // Restart marquee cycle when title text changes
  if (strcmp(s, lastTitle) != 0) {
    strncpy(lastTitle, s, sizeof(lastTitle) - 1);
    lastTitle[sizeof(lastTitle) - 1] = '\0';
    marqueeStartMs = nowMs();
  }

  float travel = (float)(w - room);
  const float speed = 30.0f;
  const float pause = 1.5f;
  float leg = travel / speed;
  float cyc = 2.0f * (leg + pause);
  if (cyc <= 0.0f) cyc = 1.0f;

  float elapsedSec = (float)(nowMs() - marqueeStartMs) / 1000.0f;
  float t = fmodf(elapsedSec, cyc);
  if (t < 0.0f) t += cyc;

  float off = 0.0f;
  if (t < pause) {
    off = 0.0f;
  } else if (t < pause + leg) {
    off = (t - pause) * speed;
  } else if (t < 2.0f * pause + leg) {
    off = travel;
  } else {
    off = travel - (t - 2.0f * pause - leg) * speed;
  }

  c.setClipRect(8, y, room, c.fontHeight());
  c.setTextColor(hex);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  textImgDraw(c, key, s, 8 - (int)roundf(off), y);
  c.clearClipRect();
}

static int measureTextWidth(const char *key, const char *s, const lgfx::IFont *f) {
  if (!s || !s[0]) return 0;
  for (int i = 0; i < N_PANELS; i++) {
    if (panels[i].canvas) {
      panels[i].canvas->setFont(f);
      return textImgWidth(*panels[i].canvas, key, s);
    }
  }
  return (int)strlen(s) * 12;
}

static bool mediaMarqueeMoving() {
  if (pcStat("media.ok") != 1.0) return false;
  const char *title = pcText("media.title");
  if (!title || !title[0]) return false;
  // the scheduler asks on every pass: measure once per title, except while
  // a non-ASCII title's rendered width may still change (its image arrives)
  static char measured[128];
  static bool wide, final;
  if (!final || strcmp(title, measured)) {
    snprintf(measured, sizeof measured, "%s", title);
    wide = measureTextWidth("media.title", title, fontHead()) > 224;
    final = true;
    for (const char *p = title; *p; p++) if ((uint8_t)*p >= 0x80) final = false;
  }
  return wide;
}

// Map WxIcon enum to UTF-8 Nerd Font weather glyph.
static const char *getWxIconUtf8(WxIcon icon, bool isDay) {
  switch (icon) {
    case WX_CLEAR:   return isDay ? "\uE30D" : "\uE32B";
    case WX_PARTLY:  return isDay ? "\uE302" : "\uE37E";
    case WX_CLOUDY:  return "\uE312";
    case WX_FOG:     return "\uE313";
    case WX_DRIZZLE: return "\uE31B";
    case WX_RAIN:    return isDay ? "\uE318" : "\uE325";
    case WX_SHOWERS: return "\uE319";
    case WX_SNOW:    return "\uE31A";
    case WX_STORM:   return "\uE31D";
    default:         return isDay ? "\uE30D" : "\uE32B";
  }
}

// Media player source brand icon and color.
static const char *getMediaSourceIcon(const char *src, uint16_t &iconCol) {
  if (strstr(src, "Spotify") || strstr(src, "spotify")) {
    iconCol = C_GOOD;
    return "\uF1BC";
  } else if (strstr(src, "Chrome") || strstr(src, "chrome")) {
    iconCol = C_WARN;
    return "\uE743";
  } else if (strstr(src, "Firefox") || strstr(src, "firefox")) {
    iconCol = C_CLAUDE;
    return "\uE745";
  } else if (strstr(src, "Edge") || strstr(src, "edge")) {
    iconCol = C_CPU;
    return "\uF282";
  } else {
    iconCol = C_DIM;
    return "\uF001";
  }
}

// Small directional network arrow (down/up).
static inline void drawNetArrow(LGFX_Sprite &c, int x, int y, bool down, uint16_t hex) {
  if (down) {
    c.fillTriangle(x, y, x + 8, y, x + 4, y + 6, hex);
  } else {
    c.fillTriangle(x, y + 6, x + 8, y + 6, x + 4, y, hex);
  }
}

// Warning overlay box for restarting / updating on GIF faces.
static void drawWarnBox(LGFX_Sprite &c, const char *title, const char *sub) {
  c.fillRect(0, OVERLAY_Y0, TFT_W, OVERLAY_Y1 - OVERLAY_Y0, C_GIF_KEY);

  int y = 248;
  c.fillRoundRect(12, y, 216, 56, 8, C_WARN);
  c.fillRoundRect(15, y + 3, 210, 50, 6, C_BG);

  c.setFont(fontIconsSm());
  c.setTextColor(C_WARN);
  c.setTextDatum(lgfx::textdatum_t::middle_center);
  c.drawString("\uF071", 40, y + 28);

  if (sub && sub[0]) {
    c.setFont(fontBody());
    c.setTextColor(C_WARN);
    c.setTextDatum(lgfx::textdatum_t::top_left);
    c.drawString(title, 60, y + 9);

    c.setFont(fontSmall());
    c.setTextColor(C_DIM);
    c.setTextDatum(lgfx::textdatum_t::top_left);
    c.drawString(sub, 60, y + 31);
  } else {
    c.setFont(fontBody());
    c.setTextColor(C_WARN);
    c.setTextDatum(lgfx::textdatum_t::top_left);
    c.drawString(title, 60, y + 18);
  }
}

// A typhoon in its last 6 h before arrival gets the box too
static bool typhoonSoon(const Cyclone &cy) {
  return cy.tracked && !cy.active && cy.arriveUnix && (long)cy.arriveUnix - (long)time(nullptr) <= 6 * 3600;
}

// Warning box: typhoon here or close, else rain on its way (not while it
// rains: then there is nothing left to warn about)
static bool warnBoxShown() {
  const Cyclone &cy = weatherCyclone();
  return cy.active || typhoonSoon(cy) || rainAlertHome().active;
}

// First of the candidates that fits maxW in the current font
static const char *fitText(LGFX_Sprite &c, int maxW, const char *a, const char *b, const char *d = nullptr) {
  if (c.textWidth(a) <= maxW) return a;
  if (!d || c.textWidth(b) <= maxW) return b;
  return d;
}

static void fmtMinutes(char *out, size_t cap, long min) {
  if (min < 60) snprintf(out, cap, "%ld min", min);
  else if (min % 60 == 0 || min >= 600) snprintf(out, cap, "%ld h", (min + 30) / 60);
  else snprintf(out, cap, "%ld h %ld min", min / 60, min % 60);
}

// Warning banner: red for typhoons and rain within 20 min, yellow before
static void drawWarnBox(LGFX_Sprite &c, int y) {
  const Cyclone &cy = weatherCyclone();
  const RainAlert &rain = rainAlertHome();
  bool typhoon = cy.active || typhoonSoon(cy);
  int eta = rainEtaMin(rain);
  uint16_t col = (typhoon || eta <= 20) ? C_BAD : C_WARN;
  c.fillRoundRect(8, y, 224, 44, 6, col);
  c.fillRoundRect(10, y + 2, 220, 40, 5, C_BG);

  char t1[72], t2[72], t3[48], sub1[64], sub2[64];
  const char *kind = cy.superTyphoon ? "SUPER TYPHOON" : "TYPHOON";
  if (cy.active) {
    snprintf(t1, sizeof t1, "%s %s ACTIVE", kind, cy.name);
    snprintf(t2, sizeof t2, "%s %s", kind, cy.name);
    snprintf(t3, sizeof t3, "%s ACTIVE", kind);
    snprintf(sub1, sizeof sub1, "active, centre ~%u km, %u km/h", cy.distKm, cy.windKmh);
    snprintf(sub2, sizeof sub2, "centre ~%u km, %u km/h", cy.distKm, cy.windKmh);
  } else if (typhoon) {
    long left = (long)cy.arriveUnix - (long)time(nullptr);
    char dur[24];
    fmtMinutes(dur, sizeof dur, left > 0 ? left / 60 : 0);
    snprintf(t1, sizeof t1, "%s %s IN ~%s", kind, cy.name, dur);
    snprintf(t2, sizeof t2, "%s %s ~%s", kind, cy.name, dur);
    snprintf(t3, sizeof t3, "%s ~%s", kind, dur);
    struct tm at;
    if (boardLocalTimeAt((time_t)cy.arriveUnix, &at))
      snprintf(sub1, sizeof sub1, "arrives ~%02d:%02d, closest ~%u km", at.tm_hour, at.tm_min, cy.closestKm);
    else
      snprintf(sub1, sizeof sub1, "closest ~%u km, %u km/h", cy.closestKm, cy.windKmh);
    snprintf(sub2, sizeof sub2, "closest ~%u km", cy.closestKm);
  } else {
    char tier[24];
    snprintf(tier, sizeof tier, "%s", wxTierName(rain.tier));
    for (char *p = tier; *p; p++) *p = (char)toupper((unsigned char)*p);
    snprintf(t1, sizeof t1, "%s IN ~%d MIN", tier, eta);
    snprintf(t2, sizeof t2, "%s ~%d MIN", tier, eta);
    snprintf(t3, sizeof t3, "RAIN ~%d MIN", eta);
    if (rain.startUnix && rain.endUnix) {
      char dur[24];
      fmtMinutes(dur, sizeof dur, (long)(rain.endUnix - rain.startUnix) / 60);
      snprintf(sub1, sizeof sub1, "lasts ~%s, %.1f mm/h%s%s", dur, rain.mmh, rain.from[0] ? " from " : "", rain.from);
      snprintf(sub2, sizeof sub2, "lasts ~%s, %.1f mm/h", dur, rain.mmh);
    } else {
      snprintf(sub1, sizeof sub1, "%.1f mm/h, moving in from %s", rain.mmh, rain.from[0] ? rain.from : "?");
      snprintf(sub2, sizeof sub2, "%.1f mm/h", rain.mmh);
    }
  }
  c.setFont(fontBody());
  c.setTextColor(col);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString(fitText(c, 206, t1, t2, t3), 18, y + 6);
  c.setFont(fontSmall());
  c.setTextColor(C_DIM);
  c.drawString(fitText(c, 206, sub1, sub2), 18, y + 25);
}

// Joins the warnings of one kind into "RAM 93%, CPU 96%, GPU 97/95%" (style
// 0), "RAM 93 CPU 96 GPU 97/95%" (1) or "RAM93 CPU96 GPU97/95%" (2); both
// GPUs share one entry. Temperatures end in a degree sign instead of %.
static void joinWarns(char *out, size_t cap, const PcWarn *w, int n, bool temp, int style) {
  const char *unit = temp ? "\u00B0" "C" : "%";
  const char *sep = style == 0 ? ", " : " ";
  const char *gap = style == 2 ? "" : " ";
  int g1 = -1, g2 = -1;
  for (int i = 0; i < n; i++) {
    if (w[i].temp != temp) continue;
    if (!strcmp(w[i].label, "GPU1")) g1 = i;
    if (!strcmp(w[i].label, "GPU2")) g2 = i;
  }
  size_t len = 0;
  out[0] = 0;
  for (int i = 0; i < n && len < cap; i++) {
    if (w[i].temp != temp || (i == g2 && g1 >= 0)) continue;
    bool last = true;   // the unit only after the last value in compact styles
    for (int j = i + 1; j < n; j++)
      if (w[j].temp == temp && !(j == g2 && g1 >= 0)) last = false;
    const char *u = style == 0 || last ? unit : "";
    if (i == g1 && g2 >= 0)
      len += snprintf(out + len, cap - len, "%s%s%s%.0f/%.0f%s", len ? sep : "", "GPU", gap, w[g1].value, w[g2].value, u);
    else
      len += snprintf(out + len, cap - len, "%s%s%s%.0f%s", len ? sep : "", w[i].label, gap, w[i].value, u);
  }
}

// PC usage warnings in place of the weather box: one line for a single
// warning, else a count over a load line and a heat line. Red when any is
// critical (RAM over 90 %, anything over 75 C), else yellow. Returns the
// box height.
static int drawPcWarnBox(LGFX_Sprite &c, int y, const PcWarn *w, int n) {
  bool critical = false, anyLoad = false, anyHeat = false, ramCritical = false;
  for (int i = 0; i < n; i++) {
    critical |= w[i].level == PCWARN_CRITICAL;
    ramCritical |= !w[i].temp && w[i].level == PCWARN_CRITICAL;
    (w[i].temp ? anyHeat : anyLoad) = true;
  }
  int lines = n == 1 ? 1 : (int)anyLoad + (int)anyHeat;
  int h = lines == 2 ? 58 : 44;
  uint16_t col = critical ? C_BAD : C_WARN;
  c.fillRoundRect(8, y, 224, h, 6, col);
  c.fillRoundRect(10, y + 2, 220, h - 4, 5, C_BG);

  char t1[48], t2[48], sub[3][72];
  if (n == 1) {
    const PcWarn &a = w[0];
    float secs = pcWarnAvgMs() / 1000.0f;
    if (a.temp) {
      snprintf(t1, sizeof t1, "%s HOT %.0f\u00B0" "C", a.label, a.value);
      snprintf(sub[0], sizeof sub[0], "%gs average over the %g\u00B0" "C limit", secs, a.limit);
    } else {
      if (!strcmp(a.label, "RAM"))
        snprintf(t1, sizeof t1, "%s RAM %.0f%%", a.level == PCWARN_CRITICAL ? "CRITICAL" : "HIGH", a.value);
      else
        snprintf(t1, sizeof t1, "HIGH %s %s %.0f%%", a.label, strcmp(a.label, "VRAM") ? "LOAD" : "USE", a.value);
      snprintf(sub[0], sizeof sub[0], "%gs average over the %g%% limit", secs, a.limit);
    }
    snprintf(t2, sizeof t2, "%s %.0f%s", a.label, a.value, a.temp ? "\u00B0" "C" : "%");
  } else {
    snprintf(t1, sizeof t1, "%d PC WARNINGS", n);
    snprintf(t2, sizeof t2, "%d WARNINGS", n);
  }
  c.setFont(fontBody());
  c.setTextColor(col);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString(fitText(c, 206, t1, t2), 18, y + 6);

  c.setFont(fontSmall());
  if (n == 1) {
    c.setTextColor(C_DIM);
    c.drawString(sub[0], 18, y + 25);
    return h;
  }
  int ly = y + 25;
  for (int kind = 0; kind < 2; kind++) {   // loads, then heat
    bool temp = kind == 1;
    if (temp ? !anyHeat : !anyLoad) continue;
    for (int style = 0; style < 3; style++) joinWarns(sub[style], sizeof sub[style], w, n, temp, style);
    char hot[3][80];
    const char *a = sub[0], *b = sub[1], *d = sub[2];
    if (temp) {   // "Hot: CPU 82..." so the heat line reads on its own
      snprintf(hot[0], sizeof hot[0], "Hot: %s", sub[0]);
      snprintf(hot[1], sizeof hot[1], "Hot %s", sub[1]);
      snprintf(hot[2], sizeof hot[2], "%s", sub[2]);
      a = hot[0], b = hot[1], d = hot[2];
    }
    c.setTextColor(temp || ramCritical ? C_BAD : C_WARN);
    c.drawString(fitText(c, 206, a, b, d), 18, ly);
    ly += 14;
  }
  return h;
}

// GPU stat card (RX 6600 or RTX 3050).
static void drawGpuCard(LGFX_Sprite &c, int x, int y, const char *name, uint16_t acc,
                        double temp, double load, double mhz, double vram, double vramT, PcWarnLevel level) {
  drawWarnCard(c, x, y, 114, 98, level);
  c.setFont(fontSmall());
  c.setTextColor(acc);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString(name, x + 8, y + 6);

  char buf[32];
  if (isnan(temp)) {
    snprintf(buf, sizeof(buf), "--");
  } else {
    snprintf(buf, sizeof(buf), "%.0f\u00B0" "C", temp);
  }
  c.setTextColor(warnTempColor(temp, level));
  c.setTextDatum(lgfx::textdatum_t::top_right);
  c.drawString(buf, x + 106, y + 6);

  if (isnan(load)) {
    snprintf(buf, sizeof(buf), "--");
  } else {
    snprintf(buf, sizeof(buf), "%.0f%%", load);
  }
  c.setFont(fontHead());
  c.setTextColor(C_TEXT);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString(buf, x + 8, y + 23);

  if (isnan(mhz)) {
    snprintf(buf, sizeof(buf), "--");
  } else {
    snprintf(buf, sizeof(buf), "%.0fMHz", mhz);
  }
  c.setFont(fontSmall());
  c.setTextColor(C_DIM);
  c.setTextDatum(lgfx::textdatum_t::top_right);
  c.drawString(buf, x + 106, y + 28);

  drawBar(c, x + 8, y + 52, 98, 6, isnan(load) ? 0.0f : (float)(load / 100.0), acc);

  c.setTextColor(C_FAINT);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString("VRAM", x + 8, y + 62);

  if (isnan(vram) || isnan(vramT)) {
    snprintf(buf, sizeof(buf), "--");
  } else {
    if (fabs(vramT - round(vramT)) < 0.05) {
      snprintf(buf, sizeof(buf), "%.1f/%.0fG", vram, vramT);
    } else {
      snprintf(buf, sizeof(buf), "%.1f/%.1fG", vram, vramT);
    }
  }
  c.setTextColor(C_DIM);
  c.setTextDatum(lgfx::textdatum_t::top_right);
  c.drawString(buf, x + 106, y + 62);

  float vramFrac = (isnan(vram) || isnan(vramT) || vramT <= 0.0) ? 0.0f : (float)(vram / vramT);
  drawBar(c, x + 8, y + 82, 98, 6, vramFrac, acc);
}

// Screen 1: PC stats face.
static void drawPcStats(LGFX_Sprite &c) {
  c.fillScreen(C_BG);

  // Cards tint with their 2 s usage/temperature warnings (RAM: usage only)
  PcWarnLevel cpuLevel = worst(pcWarnLevel("cpu_load"), pcWarnLevel("cpu_temp"));
  PcWarnLevel ramLevel = pcWarnLevel("ram_load");
  PcWarnLevel rxLevel = worst(worst(pcWarnLevel("gpu1_load"), pcWarnLevel("gpu1_vram_pct")), pcWarnLevel("gpu1_temp"));
  PcWarnLevel rtxLevel = worst(pcWarnLevel("gpu2_load"), pcWarnLevel("gpu2_temp"));

  // CPU card
  drawWarnCard(c, 4, 4, 232, 66, cpuLevel);
  c.setFont(fontSmall());
  c.setTextColor(C_CPU);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString("CPU", 12, 9);

  double cpuLoad = pcStat("cpu_load");
  char buf[48];
  if (isnan(cpuLoad)) snprintf(buf, sizeof(buf), "--");
  else snprintf(buf, sizeof(buf), "%.0f%%", cpuLoad);
  c.setFont(fontBig());
  c.setTextColor(C_TEXT);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString(buf, 12, 24);

  double cpuClock = pcStat("cpu_clock");
  if (isnan(cpuClock)) snprintf(buf, sizeof(buf), "--");
  else snprintf(buf, sizeof(buf), "%.2f GHz", cpuClock / 1000.0);
  c.setFont(fontBody());
  c.setTextColor(C_TEXT);
  c.setTextDatum(lgfx::textdatum_t::top_right);
  c.drawString(buf, 228, 24);

  double cpuTemp = pcStat("cpu_temp");
  if (isnan(cpuTemp)) snprintf(buf, sizeof(buf), "--");
  else snprintf(buf, sizeof(buf), "%.1f\u00B0" "C", cpuTemp);
  c.setTextColor(warnTempColor(cpuTemp, cpuLevel));
  c.setTextDatum(lgfx::textdatum_t::top_right);
  c.drawString(buf, 228, 44);

  drawBar(c, 12, 61, 216, 4, isnan(cpuLoad) ? 0.0f : (float)(cpuLoad / 100.0), C_CPU);

  // RAM card
  drawWarnCard(c, 4, 76, 232, 52, ramLevel);
  c.setFont(fontSmall());
  c.setTextColor(C_RAM);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString("RAM", 12, 81);

  double ramUsed = pcStat("ram_used");
  double ramFree = pcStat("ram_free");
  if (isnan(ramUsed) || isnan(ramFree)) {
    snprintf(buf, sizeof(buf), "--");
  } else {
    snprintf(buf, sizeof(buf), "%.1f / %.1f GB", ramUsed, ramUsed + ramFree);
  }
  c.setTextColor(C_DIM);
  c.setTextDatum(lgfx::textdatum_t::top_right);
  c.drawString(buf, 228, 81);

  double ramLoad = pcStat("ram_load");
  if (isnan(ramLoad)) snprintf(buf, sizeof(buf), "--");
  else snprintf(buf, sizeof(buf), "%.0f%%", ramLoad);
  c.setFont(fontHead());
  c.setTextColor(C_TEXT);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString(buf, 12, 98);

  drawBar(c, 72, 107, 156, 8, isnan(ramLoad) ? 0.0f : (float)(ramLoad / 100.0), C_RAM);

  // GPU cards (RX 6600 & RTX 3050, VRAM MB -> GB)
  double rxVram = pcStat("gpu1_vram");
  double rxVramT = pcStat("gpu1_vramt");
  drawGpuCard(c, 4, 134, "RX 6600", C_AMD,
              pcStat("gpu1_temp"), smoothedStat(0, "gpu1_load"), pcStat("gpu1_clock"),
              isnan(rxVram) ? NAN : rxVram / 1024.0, isnan(rxVramT) ? NAN : rxVramT / 1024.0, rxLevel);

  double rtxVram = pcStat("gpu2_vram");
  double rtxVramT = pcStat("gpu2_vramt");
  drawGpuCard(c, 122, 134, "RTX 3050", C_NV,
              pcStat("gpu2_temp"), smoothedStat(1, "gpu2_load"), pcStat("gpu2_clock"),
              isnan(rtxVram) ? NAN : rtxVram / 1024.0, isnan(rtxVramT) ? NAN : rtxVramT / 1024.0, rtxLevel);

  // Fans card
  drawCard(c, 4, 238, 114, 78);
  c.setFont(fontSmall());
  c.setTextColor(C_FAN);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString("FANS", 12, 244);

  c.setTextColor(C_FAINT);
  c.drawString("CPU", 12, 264);
  double cpuFan = pcStat("cpu_fan");
  if (isnan(cpuFan)) snprintf(buf, sizeof(buf), "--");
  else snprintf(buf, sizeof(buf), "%.0f rpm", cpuFan);
  c.setTextColor(C_TEXT);
  c.setTextDatum(lgfx::textdatum_t::top_right);
  c.drawString(buf, 110, 264);

  c.setTextColor(C_FAINT);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString("SYS", 12, 284);
  double sysFan = pcStat("sys_fan");
  if (isnan(sysFan)) snprintf(buf, sizeof(buf), "--");
  else snprintf(buf, sizeof(buf), "%.0f rpm", sysFan);
  c.setTextColor(C_TEXT);
  c.setTextDatum(lgfx::textdatum_t::top_right);
  c.drawString(buf, 110, 284);

  // Drive C: card
  drawCard(c, 122, 238, 114, 78);
  c.setFont(fontSmall());
  c.setTextColor(C_DISK);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString("C:", 130, 244);

  double cUsed = pcStat("c_used");
  double cTotal = pcStat("c_total");
  double dp = (isnan(cUsed) || isnan(cTotal) || cTotal <= 0.0) ? NAN : (cUsed / cTotal);
  if (isnan(dp)) {
    snprintf(buf, sizeof(buf), "--");
    c.setTextColor(C_DIM);
  } else {
    snprintf(buf, sizeof(buf), "%.0f%%", dp * 100.0);
    c.setTextColor(sev(dp * 100.0, 85.0, 95.0));
  }
  c.setTextDatum(lgfx::textdatum_t::top_right);
  c.drawString(buf, 228, 244);

  if (isnan(cUsed) || isnan(cTotal)) snprintf(buf, sizeof(buf), "--");
  else snprintf(buf, sizeof(buf), "%.0f/%.0f GB", cUsed, cTotal);
  c.setTextColor(C_TEXT);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString(buf, 130, 264);

  drawBar(c, 130, 284, 98, 6, isnan(dp) ? 0.0f : (float)dp, C_DISK);

  if (isnan(cUsed) || isnan(cTotal)) snprintf(buf, sizeof(buf), "--");
  else snprintf(buf, sizeof(buf), "%.0f GB free", cTotal - cUsed);
  c.setTextColor(C_FAINT);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString(buf, 130, 296);
}

// Screen 2: Media playback & uptime / network face.
static void drawMedia(LGFX_Sprite &c) {
  c.fillScreen(C_BG);

  // PC usage warnings take the weather box's place
  PcWarn pcw[PCWARN_MAX];
  int nPcw = pcWarnList(pcw);
  int boxH = 0;
  if (nPcw) {
    boxH = drawPcWarnBox(c, 4, pcw, nPcw);
  } else if (warnBoxShown()) {
    drawWarnBox(c, 4);
    boxH = 44;
  }
  bool hasRain = boxH > 0;
  int top = hasRain ? boxH + 10 : 0;

  // media.ok from the service only says the helper file is fresh; a session has a title
  bool mediaOk = (pcStat("media.ok") == 1.0) && pcText("media.title")[0];

  if (mediaOk) {
    int s = hasRain ? 104 - (boxH - 44) : 144;   // a taller box takes it from the art
    int artX = 120 - s / 2;
    int artY = 8 + top;

    double artIdStat = pcStat("media.art_id");
    uint32_t artId = isnan(artIdStat) ? 0 : (uint32_t)artIdStat;
    const uint16_t *artPixels = artId ? artGet(artId) : nullptr;

    if (artPixels) {
      if (!hasRain) {
        c.pushImage(artX, artY, 144, 144, (const lgfx::swap565_t *)artPixels);
      } else {
        c.pushImageRotateZoom((float)artX, (float)artY, 0.0f, 0.0f, 0.0f,
                              s / 144.0f, s / 144.0f, 144, 144,
                              (const lgfx::swap565_t *)artPixels);
      }
    } else {
      // Placeholder card with music note glyph
      c.fillRoundRect(artX, artY, s, s, 6, C_CARD);
      c.setFont(fontIconsSm());
      c.setTextColor(C_DIM);
      c.setTextDatum(lgfx::textdatum_t::middle_center);
      c.drawString("\uF001", 120, artY + s / 2);
    }

    int ty = 8 + top + s + 8;
    const char *title = pcText("media.title");
    drawMarquee(c, "media.title", title[0] ? title : "--", ty, fontHead(), C_TEXT);

    const char *artist = pcText("media.artist");
    c.setFont(fontBody());
    c.setTextColor(C_DIM);
    c.setTextDatum(lgfx::textdatum_t::top_center);
    textImgDraw(c, "media.artist", artist[0] ? artist : "--", 120, ty + 26);

    // Source icon & name
    const char *src = pcText("media.source");
    uint16_t iconCol = C_DIM;
    const char *srcIcon = getMediaSourceIcon(src[0] ? src : "", iconCol);
    // one baseline for icons and text: the icon font's ascent is taller, so
    // top datums put the icons ~4 px low
    int rowBase = ty + 48 + 11;
    c.setFont(fontIconsSm());
    c.setTextColor(iconCol);
    c.setTextDatum(lgfx::textdatum_t::baseline_left);
    c.drawString(srcIcon, 8, rowBase);

    c.setFont(fontSmall());
    c.setTextColor(iconCol);
    c.setTextDatum(lgfx::textdatum_t::baseline_left);
    textImgDraw(c, "media.source", src[0] ? src : "--", 28, rowBase);

    // Local position advancing while playing
    static uint32_t lastSampleSeq = 0;
    static int lastSamplePos = 0;
    static uint32_t lastSampleMs = 0;

    uint32_t curSeq = pcStatsSeq();
    double posStat = pcStat("media.pos_s");
    double lenStat = pcStat("media.len_s");
    int pos = isnan(posStat) ? 0 : (int)posStat;
    int len = isnan(lenStat) ? 0 : (int)lenStat;
    bool playing = (pcStat("media.playing") == 1.0);

    if (curSeq != lastSampleSeq) {
      lastSampleSeq = curSeq;
      lastSamplePos = pos;
      lastSampleMs = nowMs();
    } else if (playing && len > 0) {
      uint32_t elapsed = (nowMs() - lastSampleMs) / 1000;
      pos = lastSamplePos + (int)elapsed;
      if (pos > len) pos = len;
    }
    if (pos < 0) pos = 0;

    char timeBuf[32];
    snprintf(timeBuf, sizeof(timeBuf), "%d:%02d / %d:%02d", pos / 60, pos % 60, len / 60, len % 60);
    c.setFont(fontSmall());
    c.setTextColor(C_DIM);
    c.setTextDatum(lgfx::textdatum_t::baseline_right);
    c.drawString(timeBuf, 232, rowBase);

    int tw = c.textWidth(timeBuf);
    c.setFont(fontIconsSm());
    c.setTextColor(C_DIM);
    c.setTextDatum(lgfx::textdatum_t::baseline_right);
    c.drawString(playing ? "\uF04B" : "\uF04C", 232 - tw - 4, rowBase);

    float pFrac = len > 0 ? ((float)pos / (float)len) : 0.0f;
    drawBar(c, 8, ty + 66, 224, 4, pFrac, C_TEXT);
  } else {
    // "Nothing playing" layout
    int y0 = top ? (top + 10) : 32;
    c.fillCircle(108, y0 + 64, 14, C_FAINT);
    c.fillCircle(140, y0 + 56, 14, C_FAINT);
    c.fillRect(118, y0 + 8, 4, 58, C_FAINT);
    c.fillRect(150, y0, 4, 58, C_FAINT);
    c.fillRect(118, y0, 36, 8, C_FAINT);

    c.setFont(fontBody());
    c.setTextColor(C_DIM);
    c.setTextDatum(lgfx::textdatum_t::top_center);
    c.drawString("Nothing playing", 120, y0 + 100);
  }

  // Uptime and Network throughput card
  drawCard(c, 4, 240, 232, 76);
  c.setFont(fontSmall());
  c.setTextColor(C_FAINT);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString("UPTIME", 12, 246);

  char upBuf[32];
  formatUptime(pcStat("uptime"), upBuf, sizeof(upBuf));
  c.setFont(fontHead());
  c.setTextColor(C_TEXT);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString(upBuf, 12, 262);

  // Network down throughput
  drawNetArrow(c, 220, 251, true, C_CPU);
  char rateBuf[32];
  formatRate(pcStat("net_down"), rateBuf, sizeof(rateBuf));
  c.setFont(fontSmall());
  c.setTextColor(C_TEXT);
  c.setTextDatum(lgfx::textdatum_t::top_right);
  c.drawString(rateBuf, 214, 246);

  // Network up throughput
  drawNetArrow(c, 220, 271, false, C_RAM);
  formatRate(pcStat("net_up"), rateBuf, sizeof(rateBuf));
  c.drawString(rateBuf, 214, 266);

  // 56-sample net_down sparkline
  drawSparkline(c);
}

// Screen 2: Weather face (when PC is off/asleep/restarting/updating).
static void drawWeather(LGFX_Sprite &c) {
  const WeatherNow &w = weatherNow();
  c.fillScreen(C_WX_BG);

  if (!w.valid) {
    c.setFont(fontBody());
    c.setTextColor(C_DIM);
    c.setTextDatum(lgfx::textdatum_t::top_center);
    c.drawString("No weather yet", 120, 140);
    c.setFont(fontSmall());
    c.setTextColor(C_DIM);
    c.setTextDatum(lgfx::textdatum_t::top_center);
    c.drawString("needs Wi-Fi", 120, 165);
    return;
  }

  // Location and update time
  c.setFont(fontBody());
  c.setTextColor(C_TEXT);
  c.setTextDatum(lgfx::textdatum_t::top_center);
  textImgDraw(c, "wx.place", w.place[0] ? w.place : "Home", 120, 6);

  char upBuf[32];
  snprintf(upBuf, sizeof(upBuf), "updated %s", w.updated[0] ? w.updated : "--");
  c.setFont(fontSmall());
  c.setTextColor(C_WX_TEXT_DIM);
  c.setTextDatum(lgfx::textdatum_t::top_center);
  c.drawString(upBuf, 120, 26);

  // 44 px weather icon
  c.setFont(fontIcons());
  c.setTextColor(C_WX_CLOUD);
  c.setTextDatum(lgfx::textdatum_t::top_center);
  c.drawString(getWxIconUtf8(w.icon, w.isDay), 120, 40);

  // 56 px temperature
  char tBuf[16];
  snprintf(tBuf, sizeof(tBuf), "%.0f\u00B0", (double)w.temp);
  c.setFont(fontDigits());
  c.setTextColor(C_TEXT);
  c.setTextDatum(lgfx::textdatum_t::top_center);
  c.drawString(tBuf, 120, 86);

  // Today High / Low
  char hlBuf[32];
  snprintf(hlBuf, sizeof(hlBuf), "%d\u00B0 / %d\u00B0", w.tmax, w.tmin);
  c.setFont(fontBody());
  c.setTextColor(C_WX_TEXT_MID);
  c.setTextDatum(lgfx::textdatum_t::top_center);
  c.drawString(hlBuf, 120, 152);

  // Wind arrow pointing where the wind goes (windFromDeg + 180)
  float dirDeg = (float)(w.windFromDeg + 180);
  float theta = (dirDeg - 90.0f) * (float)(M_PI / 180.0f);
  float cx = 20.0f, cy = 186.0f;
  float x0 = cx + 8.0f * cosf(theta);
  float y0 = cy + 8.0f * sinf(theta);
  float x1 = cx + 6.0f * cosf(theta + 2.4f);
  float y1 = cy + 6.0f * sinf(theta + 2.4f);
  float x2 = cx + 6.0f * cosf(theta - 2.4f);
  float y2 = cy + 6.0f * sinf(theta - 2.4f);
  c.fillTriangle((int)roundf(x0), (int)roundf(y0),
                 (int)roundf(x1), (int)roundf(y1),
                 (int)roundf(x2), (int)roundf(y2), C_TEXT);

  static const char *dirs16[] = {"N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
                                 "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"};
  int dirIdx = (int)((w.windFromDeg + 11.25f) / 22.5f) % 16;
  char wBuf[32];
  snprintf(wBuf, sizeof(wBuf), "%.0f km/h %s", (double)w.windKmh, dirs16[dirIdx]);
  c.setFont(fontSmall());
  c.setTextColor(C_TEXT);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString(wBuf, 32, 180);

  // AQI chip
  if (w.aqi >= 0) {
    const char *lvl = "Low";
    uint16_t chipCol = C_GOOD;
    if (w.aqi > 200) { lvl = "Dangerous"; chipCol = C_PURPLE; }
    else if (w.aqi > 100) { lvl = "High"; chipCol = C_BAD; }
    else if (w.aqi > 50) { lvl = "Moderate"; chipCol = C_WARN; }

    char aqiBuf[32];
    snprintf(aqiBuf, sizeof(aqiBuf), "AQI %d %s", w.aqi, lvl);
    c.setFont(fontSmall());
    int tw = c.textWidth(aqiBuf);
    int chipW = tw + 16;
    if (chipW < 82) chipW = 82;
    int chipX = 232 - chipW;

    c.fillRoundRect(chipX, 177, chipW, 20, 10, chipCol);
    c.setTextColor(0x0000);
    c.setTextDatum(lgfx::textdatum_t::top_center);
    c.drawString(aqiBuf, chipX + chipW / 2, 180);
  }

  // Rain forecast / radar box
  const RainAlert &rain = rainAlertHome();
  if (warnBoxShown()) {
    drawWarnBox(c, 204);
  } else if (rain.raining) {
    // raining: no warning, just how long it lasts
    c.fillRoundRect(8, 204, 224, 44, 6, C_WX_BOX_BG);
    c.setFont(fontSmall());
    c.setTextColor(C_WX_TEXT_MID);
    c.setTextDatum(lgfx::textdatum_t::top_left);
    char line[48];
    struct tm at;
    if (rain.endUnix && boardLocalTimeAt((time_t)rain.endUnix, &at))
      snprintf(line, sizeof line, "%s until ~%02d:%02d", wxTierName(rain.tier), at.tm_hour, at.tm_min);
    else
      snprintf(line, sizeof line, "%s, no end in sight", wxTierName(rain.tier));
    c.drawString(line, 18, 210);
    int left = rainLeftMin(rain);
    if (left >= 0) {
      char dur[24];
      fmtMinutes(dur, sizeof dur, left);
      snprintf(line, sizeof line, "~%s left, %.1f mm/h", dur, rain.mmh);
    } else {
      snprintf(line, sizeof line, "%.1f mm/h", rain.mmh);
    }
    c.setTextColor(C_WX_TEXT_DIM);
    c.drawString(line, 18, 227);
  } else {
    c.fillRoundRect(8, 204, 224, 44, 6, C_WX_BOX_BG);
    c.setFont(fontSmall());
    c.setTextColor(C_WX_TEXT_MID);
    c.setTextDatum(lgfx::textdatum_t::top_left);
    c.drawString("No rain in the next 2 h", 18, 210);

    char radBuf[48];
    if (rain.dryKm > 0) {
      snprintf(radBuf, sizeof(radBuf), "radar: dry within %u km", (unsigned)rain.dryKm);
    } else {
      snprintf(radBuf, sizeof(radBuf), "radar: clear");
    }
    c.setTextColor(C_WX_TEXT_DIM);
    c.drawString(radBuf, 18, 227);
  }

  // 3-day forecast rows
  for (int i = 0; i < 3; i++) {
    int y = 256 + i * 21;
    int base = y + 11;   // shared baseline, as in the media row
    c.setFont(fontSmall());
    c.setTextColor(C_TEXT);
    c.setTextDatum(lgfx::textdatum_t::baseline_left);
    c.drawString(w.days[i].name, 16, base);

    c.setFont(fontIconsSm());
    c.setTextColor(C_WX_CLOUD);
    c.setTextDatum(lgfx::textdatum_t::baseline_left);
    c.drawString(getWxIconUtf8(w.days[i].icon, true), 96, base);

    char dTemp[32];
    snprintf(dTemp, sizeof(dTemp), "%d\u00B0/%d\u00B0", w.days[i].tmax, w.days[i].tmin);
    c.setFont(fontSmall());
    c.setTextColor(C_TEXT);
    c.setTextDatum(lgfx::textdatum_t::baseline_right);
    c.drawString(dTemp, 224, base);

    if (i < 2) c.fillRect(16, y + 19, 208, 1, C_WX_DIV);
  }
}

// Screen 3: Claude & Antigravity quota / Local AI summary card.
static void drawAiUsage(LGFX_Sprite &c) {
  c.fillScreen(C_BG);

  // Claude card
  drawCard(c, 4, 4, 232, 130);
  c.setFont(fontSmall());
  c.setTextColor(C_CLAUDE);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString("CLAUDE", 12, 10);

  const char *cPlan = pcText("claude.plan");
  if (!strncmp(cPlan, "Claude ", 7)) cPlan += 7;   // the card already says CLAUDE
  drawChip(c, 228, 8, cPlan[0] ? cPlan : "Pro", C_CLAUDE, 0x0000, true);

  c.setFont(fontSmall());
  c.setTextColor(C_DIM);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString("Session Remaining", 12, 32);

  // both cards show what is left; colour still follows how much is used
  double sPct = pcStat("claude.s_pct");
  char buf[48];
  if (isnan(sPct)) snprintf(buf, sizeof(buf), "--");
  else snprintf(buf, sizeof(buf), "%.0f%%", 100.0 - sPct);
  c.setFont(fontBody());
  c.setTextColor(sev(sPct, 75.0, 90.0));
  c.setTextDatum(lgfx::textdatum_t::top_right);
  c.drawString(buf, 228, 30);

  drawBar(c, 12, 52, 216, 6, isnan(sPct) ? 0.0f : (float)(1.0 - sPct / 100.0), C_CLAUDE);

  drawReset(c, "claude.s_reset_ts", false, 12, 62, 216);

  c.setFont(fontSmall());
  c.setTextColor(C_DIM);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString("Weekly Remaining", 12, 84);

  double wPct = pcStat("claude.w_pct");
  if (isnan(wPct)) snprintf(buf, sizeof(buf), "--");
  else snprintf(buf, sizeof(buf), "%.0f%%", 100.0 - wPct);
  c.setFont(fontBody());
  c.setTextColor(sev(wPct, 80.0, 95.0));
  c.setTextDatum(lgfx::textdatum_t::top_right);
  c.drawString(buf, 228, 82);

  drawBar(c, 12, 104, 216, 6, isnan(wPct) ? 0.0f : (float)(1.0 - wPct / 100.0), C_CLAUDE);

  drawReset(c, "claude.w_reset_ts", true, 12, 114, 216);

  // Antigravity card: same layout as Claude's (the PC sends remaining %)
  drawCard(c, 4, 138, 232, 130);
  c.setFont(fontSmall());
  c.setTextColor(C_AGY);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString("ANTIGRAVITY", 12, 144);

  int agyN = 0;
  const char *active = pcText("agy.active");
  if (active && strchr(active, '@')) {
    for (int n = 1; n <= 4; n++) {
      char k[16]; snprintf(k, sizeof(k), "agy%d.email", n);
      if (strcmp(pcText(k), active) == 0) { agyN = n; break; }
    }
  }
  if (agyN == 0) {
    for (int n = 1; n <= 4; n++) {
      char k[16]; snprintf(k, sizeof(k), "agy%d.email", n);
      const char *em = pcText(k);
      if (em && em[0]) { agyN = n; break; }
    }
  }
  if (agyN == 0) agyN = 1;

  char k[32];
  snprintf(k, sizeof(k), "agy%d.plan", agyN);
  const char *aPlan = pcText(k);
  drawChip(c, 228, 142, aPlan[0] ? aPlan : "Pro", C_AGY, 0x0000, true);

  // flash_pct: the IDE's older single value is the 5-hour window
  char k5[32], k5r[32], kw[32], kwr[32];
  snprintf(k5, sizeof(k5), "agy%d.flash_5h_pct", agyN);
  if (isnan(pcStat(k5))) snprintf(k5, sizeof(k5), "agy%d.flash_pct", agyN);
  snprintf(k5r, sizeof(k5r), "agy%d.flash_5h_reset_ts", agyN);
  snprintf(kw, sizeof(kw), "agy%d.flash_wk_pct", agyN);
  snprintf(kwr, sizeof(kwr), "agy%d.flash_wk_reset_ts", agyN);

  struct Row { const char *label, *pctKey, *resetKey; double warn, bad; int y; bool weekly; };
  const Row rows[2] = {{"Session Remaining", k5, k5r, 75.0, 90.0, 166, false},
                       {"Weekly Remaining", kw, kwr, 80.0, 95.0, 218, true}};
  for (const Row &r : rows) {
    double left = pcStat(r.pctKey);
    double used = isnan(left) ? left : 100.0 - left;
    c.setFont(fontSmall());
    c.setTextColor(C_DIM);
    c.setTextDatum(lgfx::textdatum_t::top_left);
    c.drawString(r.label, 12, r.y);

    if (isnan(left)) snprintf(buf, sizeof(buf), "--");
    else snprintf(buf, sizeof(buf), "%.0f%%", left);
    c.setFont(fontBody());
    c.setTextColor(sev(used, r.warn, r.bad));
    c.setTextDatum(lgfx::textdatum_t::top_right);
    c.drawString(buf, 228, r.y - 2);

    drawBar(c, 12, r.y + 20, 216, 6, isnan(left) ? 0.0f : (float)(left / 100.0), C_AGY);

    drawReset(c, r.resetKey, r.weekly, 12, r.y + 30, 216);
  }

  // Local AI summary card
  drawCard(c, 4, 272, 232, 44);
  c.setFont(fontSmall());
  c.setTextColor(C_PURPLE);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString("LOCAL AI", 12, 278);

  const char *phaseName = pcText("ai.phase_name");
  if (!phaseName[0] || !strcmp(phaseName, "asleep")) phaseName = "Not Active";
  drawChip(c, 228, 276, phaseName, C_TRACK, C_DIM, true);

  // ai.model_name is the load state (asleep/waking/awake), not the model
  c.setFont(fontSmall());
  c.setTextColor(C_DIM);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString("Occamy-1.0", 12, 298);
  c.setTextColor(C_FAINT);

  double reqs = pcStat("ai.requests");
  double errs = pcStat("ai.errors");
  char rBuf[48];
  snprintf(rBuf, sizeof(rBuf), "%.0f req, %.0f err", isnan(reqs) ? 0.0 : reqs, isnan(errs) ? 0.0 : errs);
  c.setTextDatum(lgfx::textdatum_t::top_right);
  c.drawString(rBuf, 228, 298);
}

// Screen 3: Local AI full-screen working face.
static void drawLocalAi(LGFX_Sprite &c) {
  c.fillScreen(C_AI_BG);

  c.setFont(fontSmall());
  c.setTextColor(C_PURPLE);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString("LOCAL AI", 12, 8);

  bool pulse = ((nowMs() / 500) % 2) == 0;
  c.fillCircle(220, 16, 5, pulse ? C_PURPLE : C_AI_PULSE_DIM);

  c.setFont(fontBody());
  c.setTextColor(C_TEXT);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString("Occamy-1.0 35B-A3B", 12, 26);

  const char *phase = pcText("ai.phase_name");
  if (!strcmp(phase, "asleep")) phase = "Not Active";
  drawChip(c, 12, 50, phase[0] ? phase : "generating", C_PURPLE, 0x0000, false);

  double el = pcStat("ai.elapsed_s");
  uint32_t elSec = isnan(el) ? 0 : (uint32_t)el;
  char elBuf[32];
  snprintf(elBuf, sizeof(elBuf), "%lu:%02lu", (unsigned long)(elSec / 60), (unsigned long)(elSec % 60));
  c.setFont(fontSmall());
  c.setTextColor(C_DIM);
  c.setTextDatum(lgfx::textdatum_t::top_right);
  c.drawString(elBuf, 228, 51);

  // ai.gen_tok_s is live only while generating (phase 4); otherwise it holds
  // the last request's speed, so the face shows 0.0 (dimmed) until the next
  // generation starts. While the prompt is processed (phase 3) there is no
  // live speed at all (llama-server does not report one).
  double phaseN = pcStat("ai.phase");
  bool generating = !isnan(phaseN) && phaseN == 4.0;
  bool prompting = !isnan(phaseN) && phaseN == 3.0;
  double tps = pcStat("ai.gen_tok_s");
  if (!generating || isnan(tps)) tps = 0.0;
  char tpsBuf[16];
  snprintf(tpsBuf, sizeof(tpsBuf), "%.1f", tps);
  c.setFont(fontDigits());
  c.setTextColor(generating ? C_TEXT : C_DIM);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString(tpsBuf, 12, 70);

  c.setFont(fontBody());
  c.setTextColor(C_DIM);
  c.setTextDatum(lgfx::textdatum_t::top_right);
  c.drawString(prompting ? "reading prompt" : "tok/s", 228, 110);

  // Step n / max: the local agent harness (Bridge job) step counter; plain
  // chat requests have no steps, the hub then reports 0 / 0
  double step = pcStat("ai.step");
  double stepMax = pcStat("ai.step_max");
  bool haveSteps = !isnan(step) && !isnan(stepMax) && stepMax > 0.0;
  char stepBuf[32];
  if (!haveSteps) snprintf(stepBuf, sizeof(stepBuf), "no agent job");
  else snprintf(stepBuf, sizeof(stepBuf), "Step %.0f / %.0f", step, stepMax);
  c.setFont(fontSmall());
  c.setTextColor(C_DIM);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString(stepBuf, 12, 142);

  const char *tool = pcText("ai.tool");
  char toolBuf[48];
  snprintf(toolBuf, sizeof(toolBuf), "tool: %s", tool[0] ? tool : "--");
  c.setTextDatum(lgfx::textdatum_t::top_right);
  c.drawString(toolBuf, 228, 142);

  float sFrac = (isnan(step) || isnan(stepMax) || stepMax <= 0.0) ? 0.0f : (float)(step / stepMax);
  drawBar(c, 12, 160, 216, 6, sFrac, C_PURPLE);

  // Six detail cells
  struct CellInfo {
    const char *label;
    char val[24];
  };
  CellInfo cells[6];

  cells[0].label = "GPU";
  double gpuUtil = pcStat("ai.gpu_util");
  if (isnan(gpuUtil)) snprintf(cells[0].val, sizeof(cells[0].val), "--");
  else snprintf(cells[0].val, sizeof(cells[0].val), "%.0f%%", gpuUtil);

  cells[1].label = "CPU E-CORES";
  double ecore = pcStat("cpu_ecore_load");
  if (isnan(ecore)) snprintf(cells[1].val, sizeof(cells[1].val), "--");
  else snprintf(cells[1].val, sizeof(cells[1].val), "%.0f%%", ecore);

  cells[2].label = "VRAM (AI)";
  double vramAi = pcStat("ai.vram_mb");
  if (isnan(vramAi)) snprintf(cells[2].val, sizeof(cells[2].val), "--");
  else snprintf(cells[2].val, sizeof(cells[2].val), "%.1f GB", vramAi / 1024.0);

  cells[3].label = "RAM (AI)";
  double ramAi = pcStat("llm_ram_mb");
  if (isnan(ramAi)) snprintf(cells[3].val, sizeof(cells[3].val), "--");
  else {
    double ramGb = (ramAi >= 100.0) ? (ramAi / 1024.0) : ramAi;
    snprintf(cells[3].val, sizeof(cells[3].val), "%.1f GB", ramGb);
  }

  cells[4].label = "GPU TEMP";
  double gpuTemp = pcStat("ai.gpu_temp");
  if (isnan(gpuTemp)) snprintf(cells[4].val, sizeof(cells[4].val), "--");
  else snprintf(cells[4].val, sizeof(cells[4].val), "%.0f\u00B0" "C", gpuTemp);

  cells[5].label = "GPU+CPU";
  double gpuW = pcStat("ai.gpu_power_w");
  double cpuW = pcStat("cpu_power");
  if (isnan(gpuW) && isnan(cpuW)) snprintf(cells[5].val, sizeof(cells[5].val), "--");
  else {
    double totW = (isnan(gpuW) ? 0.0 : gpuW) + (isnan(cpuW) ? 0.0 : cpuW);
    snprintf(cells[5].val, sizeof(cells[5].val), "%.0f W", totW);
  }

  for (int i = 0; i < 6; i++) {
    int cx = 4 + (i % 2) * 118;
    int cy = 174 + (i / 2) * 48;
    c.fillRoundRect(cx, cy, 114, 44, 6, C_AI_CELL);
    c.setFont(fontSmall());
    c.setTextColor(C_FAINT);
    c.setTextDatum(lgfx::textdatum_t::top_left);
    c.drawString(cells[i].label, cx + 8, cy + 4);

    c.setFont(fontBody());
    c.setTextColor(C_TEXT);
    c.setTextDatum(lgfx::textdatum_t::top_left);
    c.drawString(cells[i].val, cx + 8, cy + 20);
  }
}

// Screen 3: Big stacked clock face with PC status chip.
static void drawClock(LGFX_Sprite &c) {
  c.fillScreen(C_BG);

  GoingKind going = pcGoing();
  const char *chipText;
  uint16_t dotCol;
  if (going == GO_RESTART) {
    chipText = "PC Restarting";
    dotCol = C_WARN;
  } else if (going == GO_UPDATE_RESTART || going == GO_UPDATE_SHUTDOWN) {
    chipText = "PC Updating";
    dotCol = C_WARN;
  } else {
    chipText = "PC Offline / Disconnected";
    dotCol = C_BAD;
  }

  c.setFont(fontSmall());
  int tw = c.textWidth(chipText);
  int cw = tw + 28;
  int cx0 = 120 - cw / 2;
  c.fillRoundRect(cx0, 10, cw, 22, 11, C_CARD);
  c.fillCircle(cx0 + 12, 21, 4, dotCol);
  c.setTextColor(C_TEXT);
  c.setTextDatum(lgfx::textdatum_t::top_left);
  c.drawString(chipText, cx0 + 20, 13);

  struct tm tmNow;
  bool haveTime = boardLocalTime(&tmNow);
  if (haveTime) {
    char hh[4], mm[4];
    snprintf(hh, sizeof(hh), "%02d", tmNow.tm_hour);
    snprintf(mm, sizeof(mm), "%02d", tmNow.tm_min);

    // digits centred between the status chip (ends y 32) and the date (y 268)
    c.setFont(fontClock());
    c.setTextColor(C_CLOCK_HOURS);
    c.setTextDatum(lgfx::textdatum_t::top_center);
    c.drawString(hh, 120, 33);

    c.setTextColor(C_TEXT);
    c.setTextDatum(lgfx::textdatum_t::top_center);
    c.drawString(mm, 120, 141);

    // two centred lines under the minutes: "Wednesday" / "September 30th 2026"
    static const char *wdays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    static const char *months[] = {"January", "February", "March", "April", "May", "June", "July",
                                   "August", "September", "October", "November", "December"};
    int md = tmNow.tm_mday;
    const char *suf = (md % 100 >= 11 && md % 100 <= 13) ? "th" : md % 10 == 1 ? "st" : md % 10 == 2 ? "nd" : md % 10 == 3 ? "rd" : "th";
    char ds[32];
    snprintf(ds, sizeof(ds), "%s %d%s %d", months[tmNow.tm_mon % 12], md, suf, tmNow.tm_year + 1900);
    c.setFont(fontBody());
    c.setTextColor(C_DIM);
    c.setTextDatum(lgfx::textdatum_t::top_center);
    c.drawString(wdays[tmNow.tm_wday % 7], 120, 268);
    c.drawString(ds, 120, 292);
  } else {
    c.setFont(fontDigits());
    c.setTextColor(C_CLOCK_HOURS);
    c.setTextDatum(lgfx::textdatum_t::top_center);
    c.drawString("--", 120, 40);

    c.setTextColor(C_TEXT);
    c.setTextDatum(lgfx::textdatum_t::top_center);
    c.drawString("--", 120, 148);

    c.setFont(fontBody());
    c.setTextColor(C_DIM);
    c.setTextDatum(lgfx::textdatum_t::top_center);
    c.drawString("--", 120, 284);
  }
}

// ---- screens.h API Implementation ----

Face screensForce[N_PANELS];
bool screensNight;

Face screensFace(View v) {
  GoingKind going = pcGoing();
  bool restartish = (going == GO_RESTART || going == GO_UPDATE_RESTART || going == GO_UPDATE_SHUTDOWN);
  bool on = (pcState() == PC_ON && !restartish && !screensNight);

  switch (v) {
    case VIEW_PC:
      if (on) return FACE_PC_STATS;
      if (going == GO_RESTART) return FACE_GIF_RESTART;
      if (going == GO_UPDATE_RESTART || going == GO_UPDATE_SHUTDOWN) return FACE_GIF_UPDATE;
      return FACE_GIF;

    case VIEW_MEDIA:
      return on ? FACE_MEDIA : FACE_WEATHER;

    case VIEW_AI:
      return on ? (localAiBusy() ? FACE_LOCAL_AI : FACE_AI_USAGE) : FACE_CLOCK;

    default:
      return FACE_NONE;
  }
}

bool screensIsGif(Face f) {
  return f == FACE_GIF || f == FACE_GIF_RESTART || f == FACE_GIF_UPDATE;
}

void screensDraw(Face f, LGFX_Sprite &c) {
  // The weather place (secrets.h, may be non-ASCII): the PC renders it once
  // and the board keeps the image in flash for when the PC is off.
  textImgPin("wx.place");
  const WeatherNow &wx = weatherNow();
  if (wx.place[0]) textImgPrefetch("wx.place", wx.place, fontBody());

  switch (f) {
    case FACE_PC_STATS:
      drawPcStats(c);
      break;

    case FACE_GIF:
      // Plain GIF face: no canvas drawing needed (flush task copies pre-packed frames)
      break;

    case FACE_GIF_RESTART:
      drawWarnBox(c, "PC is Restarting", nullptr);
      break;

    case FACE_GIF_UPDATE: {
      char sub[48];
      uint32_t mins = pcGoingAgeMs() / 60000;
      snprintf(sub, sizeof(sub), "installing, %lu min", (unsigned long)mins);
      drawWarnBox(c, "PC is Updating", sub);
      break;
    }

    case FACE_MEDIA:
      drawMedia(c);
      break;

    case FACE_WEATHER:
      drawWeather(c);
      break;

    case FACE_AI_USAGE:
      drawAiUsage(c);
      break;

    case FACE_LOCAL_AI:
      drawLocalAi(c);
      break;

    case FACE_CLOCK:
      drawClock(c);
      break;

    default:
      break;
  }
}

uint32_t screensPeriodMs(Face f) {
  switch (f) {
    case FACE_PC_STATS:
    case FACE_GIF:
    case FACE_GIF_RESTART:
    case FACE_GIF_UPDATE:
    case FACE_AI_USAGE:
      return 0;

    case FACE_MEDIA:
      return mediaMarqueeMoving() ? 40 : 1000;

    case FACE_WEATHER:
      return 5000;

    case FACE_LOCAL_AI:
      return 250;

    case FACE_CLOCK:
      return 1000;

    default:
      return 0;
  }
}

bool localAiBusy() {
  if (pcStat("ai.ok") != 1.0) return false;
  if (pcStat("ai.active") == 1.0) return true;
  double phase = pcStat("ai.phase");
  if (isnan(phase)) return false;
  int p = (int)phase;
  return p == 1 || p == 3 || p == 4 || p == 5 || p == 6;
}
