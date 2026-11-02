#include "board_time.h"

#include <esp_sntp.h>
#include <math.h>
#include <nvs.h>
#include <sys/time.h>

#include "log.h"
#include "net.h"
#include "pcstats.h"
#include "settings_or_example.h"
#include "sys.h"

static int32_t tzMin;          // local offset from UTC in minutes
static bool tzLoaded;
static bool pcTzProvided;      // true once PC sends pc.tz_min
static bool sntpStarted;
static bool sntpSynced;

static void loadTz() {
  if (tzLoaded) return;
  tzLoaded = true;
  nvs_handle_t h;
  if (nvs_open("time", NVS_READONLY, &h) == ESP_OK) {
    nvs_get_i32(h, "tz_min", &tzMin);
    nvs_close(h);
  }
}

void boardTimeInit() { loadTz(); }

static void timeSyncNotificationCb(struct timeval *tv) {
  (void)tv;
  sntpSynced = true;
  logMsg("time: SNTP synced");
}

void boardTimeStartSntp() {
  if (sntpStarted || !netUp()) return;
  sntpStarted = true;
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, NET_NTP_SERVER);
  esp_sntp_set_time_sync_notification_cb(timeSyncNotificationCb);
  esp_sntp_init();
  logMsg("time: SNTP started (pool.ntp.org)");
}

void boardTimeSetUtcOffset(int32_t offsetSec) {
  loadTz();
  if (pcTzProvided) return;  // PC timezone takes priority
  int32_t newTz = offsetSec / 60;
  if (newTz != tzMin) {
    tzMin = newTz;
    nvs_handle_t h;
    if (nvs_open("time", NVS_READWRITE, &h) == ESP_OK) {
      nvs_set_i32(h, "tz_min", tzMin);
      nvs_commit(h);
      nvs_close(h);
    }
    logMsg("time: tz set from forecast (UTC%+ld min)", (long)tzMin);
  }
}

void boardTimeOnSample() {
  double epoch = pcStat("pc.epoch"), tz = pcStat("pc.tz_min");
  if (isnan(epoch) || epoch < 1700000000.0) return;
  loadTz();
  if (!isnan(tz) && tz >= -840 && tz <= 840) {   // UTC-14..+14: anything else isn't a time zone
    pcTzProvided = true;
    // kept for when the PC is off after a reboot; at most one flash write a
    // minute, so a PC process flipping the value can't wear the NVS sectors out
    static bool dirty;
    static uint32_t savedMs;
    if ((int32_t)tz != tzMin) { tzMin = (int32_t)tz; dirty = true; }
    if (dirty && (!savedMs || nowMs() - savedMs >= 60000)) {
      dirty = false;
      savedMs = nowMs() | 1;
      nvs_handle_t h;
      if (nvs_open("time", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "tz_min", tzMin);
        nvs_commit(h);
        nvs_close(h);
      }
    }
  }
  if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
    sntpSynced = true;
  }
  // PC time wins only if SNTP never synced
  if (!sntpSynced) {
    struct timeval now;
    gettimeofday(&now, nullptr);
    if (fabs((double)now.tv_sec - epoch) > 2.0) {
      struct timeval tv = {(time_t)epoch, 0};
      settimeofday(&tv, nullptr);
      logMsg("time: set from PC (UTC%+ld min)", (long)tzMin);
    }
  }
}

bool boardLocalTime(struct tm *out) {
  return boardLocalTimeAt(time(nullptr), out);
}

bool boardLocalTimeAt(time_t t, struct tm *out) {
  if (time(nullptr) < 1700000000) return false;   // never set since boot
  loadTz();
  t += (time_t)tzMin * 60;
  gmtime_r(&t, out);
  return true;
}
