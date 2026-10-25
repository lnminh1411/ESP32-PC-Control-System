#include "board_time.h"

#include <math.h>
#include <nvs.h>
#include <sys/time.h>

#include "log.h"
#include "pcstats.h"

static int32_t tzMin;          // local offset from UTC in minutes
static bool tzLoaded;

static void loadTz() {
  if (tzLoaded) return;
  tzLoaded = true;
  nvs_handle_t h;
  if (nvs_open("time", NVS_READONLY, &h) == ESP_OK) {
    nvs_get_i32(h, "tz_min", &tzMin);
    nvs_close(h);
  }
}

void boardTimeOnSample() {
  double epoch = pcStat("pc.epoch"), tz = pcStat("pc.tz_min");
  if (isnan(epoch) || epoch < 1700000000.0) return;
  loadTz();
  if (!isnan(tz) && (int32_t)tz != tzMin) {   // kept for when the PC is off after a reboot
    tzMin = (int32_t)tz;
    nvs_handle_t h;
    if (nvs_open("time", NVS_READWRITE, &h) == ESP_OK) {
      nvs_set_i32(h, "tz_min", tzMin);
      nvs_commit(h);
      nvs_close(h);
    }
  }
  struct timeval now;
  gettimeofday(&now, nullptr);
  if (fabs((double)now.tv_sec - epoch) > 2.0) {   // the PC is the reference; SNTP keeps it later
    struct timeval tv = {(time_t)epoch, 0};
    settimeofday(&tv, nullptr);
    logMsg("time: set from PC (UTC%+ld min)", (long)tzMin);
  }
}

bool boardLocalTime(struct tm *out) {
  time_t t = time(nullptr);
  if (t < 1700000000) return false;   // never set since boot
  loadTz();
  t += (time_t)tzMin * 60;
  gmtime_r(&t, out);
  return true;
}
