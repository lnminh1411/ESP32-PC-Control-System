// bench: board benchmarks and usage, from the console
// (`bench [all|usage|cpu|mem|flash|spi|usb <KB>]`)
// and over Wi-Fi (GET /bench?kb=N streams N KB for the PC to time).
// Each test runs a bounded time and yields between tests, so the watchdog
// stays quiet; the panels freeze for the few seconds `bench all` takes.
#include "bench.h"

#include <esp_chip_info.h>
#include <esp_image_format.h>
#include <esp_ota_ops.h>
#include <esp_vfs_fat.h>
#include <nvs.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_partition.h>
#include <esp_psram.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "display.h"
#include "log.h"
#include "sys.h"
#include "usb.h"

static volatile uint32_t sink;   // results land here so loops aren't optimised away

static int64_t us() { return esp_timer_get_time(); }
static double mbps(double bytes, int64_t dt) { return dt > 0 ? bytes / (double)dt : 0; }   // bytes/us = MB/s

static void yieldTick() { vTaskDelay(1); }

static void benchInfo() {
  esp_chip_info_t ci;
  esp_chip_info(&ci);
  uint32_t flashSize = 0;
  esp_flash_get_size(nullptr, &flashSize);
  logMsg("bench: ESP32-S3 rev %d.%d, %d cores @ %d MHz | flash %lu MB | PSRAM %u MB",
         ci.revision / 100, ci.revision % 100, ci.cores, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
         (unsigned long)(flashSize >> 20), (unsigned)(esp_psram_get_size() >> 20));
  logMsg("bench: heap internal %u free (largest %u) | PSRAM %u free",
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static void kbLine(const char *what, size_t used, size_t total, const char *extra) {
  logMsg("usage %-14s %7u KB used of %7u KB (%2.0f%%)%s", what, (unsigned)(used / 1024), (unsigned)(total / 1024),
         total ? 100.0 * used / total : 0.0, extra);
}

// What is in use: RAM heaps, flash partitions, FAT, NVS, task stacks
static void benchUsage() {
  char extra[96];
  // Internal SRAM: 512 KB on the S3; what the heap doesn't own is code
  // (IRAM), static data/bss, ROM reservations and the cache
  size_t iTotal = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
  size_t iFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  snprintf(extra, sizeof extra, " | heap %u KB, lowest free ever %u KB, largest block %u KB", (unsigned)(iTotal / 1024),
           (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
  kbLine("SRAM", 512 * 1024 - iFree, 512 * 1024, extra);
  logMsg("usage   SRAM static (code, data, ROM, cache) %u KB, heap in use %u KB",
         (unsigned)((512 * 1024 - iTotal) / 1024), (unsigned)((iTotal - iFree) / 1024));

  size_t pTotal = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  size_t pFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  snprintf(extra, sizeof extra, " | lowest free ever %u KB, largest block %u KB",
           (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) / 1024),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
  kbLine("PSRAM", pTotal - pFree, pTotal, extra);

  uint32_t flashSize = 0;
  esp_flash_get_size(nullptr, &flashSize);
  size_t parts = 0;
  esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
  for (; it; it = esp_partition_next(it)) parts += esp_partition_get(it)->size;
  esp_partition_iterator_release(it);
  snprintf(extra, sizeof extra, " | partitioned (bootloader + table below 0x10000 not counted)");
  kbLine("flash", parts, flashSize, extra);

  const esp_partition_t *run = esp_ota_get_running_partition();
  if (run) {
    esp_partition_pos_t pos = {run->address, run->size};
    esp_image_metadata_t meta = {};
    if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta) == ESP_OK) {
      snprintf(extra, sizeof extra, " | running from %s (OTA slot, the other is the same size)", run->label);
      kbLine("app image", meta.image_len, run->size, extra);
    }
  }

  uint64_t fTotal = 0, fFree = 0;
  if (esp_vfs_fat_info("/ffat", &fTotal, &fFree) == ESP_OK) {
    kbLine("FAT /ffat", (size_t)(fTotal - fFree), (size_t)fTotal, "");
  }

  nvs_stats_t ns;
  if (nvs_get_stats(nullptr, &ns) == ESP_OK)
    logMsg("usage NVS            %u of %u entries used (%u namespaces)", (unsigned)ns.used_entries,
           (unsigned)ns.total_entries, (unsigned)ns.namespace_count);

  // task stacks: high-water mark = the least stack ever left free
  TaskStatus_t st[32];
  UBaseType_t n = uxTaskGetSystemState(st, 32, nullptr);
  char line[200];
  int len = snprintf(line, sizeof line, "usage stacks (least free, bytes):");
  for (UBaseType_t i = 0; i < n; i++) {
    int add = snprintf(line + len, sizeof line - len, " %s %u", st[i].pcTaskName, (unsigned)st[i].usStackHighWaterMark);
    if (len + add >= (int)sizeof line - 1) {
      logMsg("%s", line);
      len = snprintf(line, sizeof line, "usage stacks:");
      add = snprintf(line + len, sizeof line - len, " %s %u", st[i].pcTaskName, (unsigned)st[i].usStackHighWaterMark);
    }
    len += add;
  }
  logMsg("%s", line);
}

static void benchCpu() {
  const uint32_t N = 20000000;
  uint32_t x = 1;
  int64_t t0 = us();
  for (uint32_t i = 0; i < N; i++) x = x * 1664525u + 1013904223u;
  int64_t dt = us() - t0;
  sink = x;
  logMsg("bench cpu: int mul-add %.1f M/s (%.2f cycles each)", N / (double)dt,
         (double)dt * CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ / N);
  yieldTick();

  const uint32_t F = 10000000;
  float a = 1.0f, b = 1.0000001f, c = 1e-7f;
  t0 = us();
  for (uint32_t i = 0; i < F; i++) a = a * b + c;
  dt = us() - t0;
  sink = (uint32_t)a;
  logMsg("bench cpu: float mul-add %.1f MFLOPS (single precision, FPU)", 2.0 * F / dt);
  yieldTick();

  const uint32_t D = 1000000;
  double d = 1.0, e = 1.0000001, g = 1e-7;
  t0 = us();
  for (uint32_t i = 0; i < D; i++) d = d * e + g;
  dt = us() - t0;
  sink = (uint32_t)d;
  logMsg("bench cpu: double mul-add %.2f MFLOPS (software)", 2.0 * D / dt);
  yieldTick();
}

// memcpy / memset / read bandwidth of a buffer pair, total bytes moved per test
static void memTriplet(const char *name, uint8_t *a, uint8_t *b, size_t len, size_t total) {
  size_t rounds = total / len;
  int64_t t0 = us();
  for (size_t r = 0; r < rounds; r++) memcpy(b, a, len);
  int64_t dCopy = us() - t0;
  yieldTick();
  t0 = us();
  for (size_t r = 0; r < rounds; r++) memset(b, (int)r, len);
  int64_t dSet = us() - t0;
  yieldTick();
  uint32_t s = 0;
  const volatile uint32_t *w = (const volatile uint32_t *)a;
  t0 = us();
  for (size_t r = 0; r < rounds; r++)
    for (size_t i = 0; i < len / 4; i++) s += w[i];
  int64_t dRead = us() - t0;
  sink = s;
  logMsg("bench %s: copy %.1f MB/s | write %.1f MB/s | read %.1f MB/s  (%u KB buffers)", name,
         mbps((double)rounds * len, dCopy), mbps((double)rounds * len, dSet), mbps((double)rounds * len, dRead),
         (unsigned)(len / 1024));
  yieldTick();
}

// average time of dependent random 32-bit reads (pointer chase), cache misses included
static void memLatency(const char *name, uint32_t *buf, size_t words) {
  for (size_t i = 0; i < words; i++) buf[i] = (uint32_t)((i * 2654435761u + 12345u) % words);
  const uint32_t N = 200000;
  uint32_t idx = 0;
  int64_t t0 = us();
  for (uint32_t i = 0; i < N; i++) idx = buf[idx];
  int64_t dt = us() - t0;
  sink = idx;
  logMsg("bench %s: random read %.0f ns each (%u KB span)", name, dt * 1000.0 / N, (unsigned)(words * 4 / 1024));
  yieldTick();
}

static void benchMem() {
  const size_t IL = 4096;
  uint8_t *ia = (uint8_t *)heap_caps_malloc(IL, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  uint8_t *ib = (uint8_t *)heap_caps_malloc(IL, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (ia && ib) {
    memset(ia, 0x5A, IL);
    memTriplet("SRAM", ia, ib, IL, 16u << 20);
    memLatency("SRAM", (uint32_t *)ia, IL / 4);
  } else {
    logMsg("bench SRAM: no 4 KB internal buffers free");
  }

  const size_t PL = 1u << 20;
  uint8_t *pa = (uint8_t *)heap_caps_malloc(PL, MALLOC_CAP_SPIRAM);
  uint8_t *pb = (uint8_t *)heap_caps_malloc(PL, MALLOC_CAP_SPIRAM);
  if (pa && pb) {
    memset(pa, 0x5A, PL);
    memTriplet("PSRAM", pa, pb, PL, 8u << 20);
    memLatency("PSRAM", (uint32_t *)pa, PL / 4);
    if (ia) {   // PSRAM -> SRAM in 4 KB pieces, what the flush task does
      int64_t t0 = us();
      for (int r = 0; r < 4; r++)
        for (size_t off = 0; off < PL; off += IL) memcpy(ia, pa + off, IL);
      int64_t dt = us() - t0;
      logMsg("bench PSRAM->SRAM: copy %.1f MB/s", mbps(4.0 * PL, dt));
      yieldTick();
    }
  } else {
    logMsg("bench PSRAM: no 1 MB buffers free");
  }
  free(pa);
  free(pb);
  free(ia);
  free(ib);
}

// PSRAM integrity: fill up to mb 1 MB blocks with an address-keyed pattern,
// then read it all back. 1 MB blocks are far past the 32 KB cache, so every
// word makes a real round trip through the PSRAM. Prints the chip
// temperature with the result: bit errors that come with heat point at the
// module's octal PSRAM, not the code.
static void benchPsram(int mb, int passes) {
  if (mb < 1) mb = 4;
  if (mb > 6) mb = 6;
  if (passes < 1) passes = 8;
  uint32_t *blk[6] = {};
  const size_t WORDS = (1u << 20) / 4;
  int n = 0;
  while (n < mb && (blk[n] = (uint32_t *)heap_caps_malloc(WORDS * 4, MALLOC_CAP_SPIRAM))) n++;
  if (!n) {
    logMsg("bench psram: no 1 MB block free");
    return;
  }
  float t0 = chipTempC();
  uint32_t errors = 0, shown = 0;
  int64_t start = us();
  for (int pass = 0; pass < passes; pass++) {
    uint32_t seed = 0x9E3779B9u * (uint32_t)(pass + 1);
    for (int b = 0; b < n; b++) {
      for (size_t i = 0; i < WORDS; i++) {
        uint32_t x = (uint32_t)(uintptr_t)&blk[b][i] ^ seed;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        blk[b][i] = (pass & 1) ? ~x : x;
      }
      esp_task_wdt_reset();
    }
    for (int b = 0; b < n; b++) {
      for (size_t i = 0; i < WORDS; i++) {
        uint32_t x = (uint32_t)(uintptr_t)&blk[b][i] ^ seed;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        uint32_t want = (pass & 1) ? ~x : x, got = blk[b][i];
        if (got == want) continue;
        errors++;
        if (shown++ < 6)
          logMsg("bench psram: pass %d at %p want %08lx got %08lx (bits %08lx)", pass, &blk[b][i], (unsigned long)want,
                 (unsigned long)got, (unsigned long)(want ^ got));
      }
      esp_task_wdt_reset();
      yieldTick();
    }
  }
  int64_t dt = us() - start;
  for (int b = 0; b < n; b++) free(blk[b]);
  logMsg("bench psram: %d MB x %d passes, %lu bad words, %.0f ms, chip %.1f -> %.1f C", n, passes,
         (unsigned long)errors, dt / 1000.0, t0, chipTempC());
}

static void benchFlash() {
  const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "ffat");
  if (!part) {
    logMsg("bench flash: no ffat partition");
    return;
  }
  const size_t CH = 4096, TOTAL = 1u << 20;
  uint8_t *buf = (uint8_t *)heap_caps_malloc(CH, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  if (!buf) {
    logMsg("bench flash: no DMA buffer");
    return;
  }
  int64_t t0 = us();
  for (size_t off = 0; off < TOTAL; off += CH) esp_partition_read(part, off, buf, CH);
  int64_t dt = us() - t0;
  logMsg("bench flash: raw read %.2f MB/s (4 KB reads, 1 MB)", mbps(TOTAL, dt));
  yieldTick();
  free(buf);

  esp_partition_mmap_handle_t mh;
  const void *map = nullptr;
  if (esp_partition_mmap(part, 0, TOTAL, ESP_PARTITION_MMAP_DATA, &map, &mh) == ESP_OK) {
    const volatile uint32_t *w = (const volatile uint32_t *)map;
    uint32_t s = 0;
    t0 = us();
    for (size_t i = 0; i < TOTAL / 4; i++) s += w[i];
    dt = us() - t0;
    sink = s;
    esp_partition_munmap(mh);
    logMsg("bench flash: mapped read %.2f MB/s (through the cache, 1 MB)", mbps(TOTAL, dt));
  } else {
    logMsg("bench flash: mmap failed");
  }
  yieldTick();

}

static void benchSpi() {
  const double theory = SPI_HZ / 8.0 / 1e6;   // MB/s on the wire
  for (int i = 0; i < N_PANELS; i++) {
    Panel &p = panels[i];
    displayWait(i);
    memset(p.sent, 0xA5, (size_t)TFT_W * TFT_H * 2);   // force a full-frame push
    int64_t t0 = us();
    displaySubmit(i);
    displayWait(i);
    int64_t dt = us() - t0;
    uint32_t bytes = p.lastFrameBytes ? p.lastFrameBytes : (uint32_t)TFT_W * TFT_H * 3 / 2;
    logMsg("bench SPI panel %d: full frame %.1f ms, %u B -> %.2f MB/s (%.0f%% of the %d MHz wire)%s", i + 1, dt / 1000.0,
           (unsigned)bytes, mbps(bytes, dt), 100.0 * mbps(bytes, dt) / theory, SPI_HZ / 1000000,
           p.gifMode ? " [GIF]" : "");
    yieldTick();
  }
}

static void benchUsb(uint32_t kb) {
  if (!kb || kb > 4096) kb = 256;
  static char line[1025];
  memset(line, 'U', 1023);
  line[1023] = '\n';
  line[1024] = 0;
  logMsg("bench usb: sending %lu KB", (unsigned long)kb);
  int64_t t0 = us();
  uint32_t sent = 0;
  for (uint32_t i = 0; i < kb; i++) {   // stop when the PC stops reading: each failed write blocks 2 s
    if (!usbCdcWriteAll(line, 1024)) break;
    sent += 1024;
  }
  int64_t dt = us() - t0;
  logMsg("bench usb: CDC write %.2f MB/s (%lu KB in %.0f ms)", mbps(sent, dt), (unsigned long)(sent / 1024), dt / 1000.0);
}

void benchCommand(const char *a1, const char *a2) {
  bool all = !a1 || !strcmp(a1, "all");
  benchInfo();
  if (all || !strcmp(a1, "usage")) benchUsage();
  if (all || !strcmp(a1, "cpu")) benchCpu();
  if (all || !strcmp(a1, "mem")) benchMem();
  if (all || !strcmp(a1, "flash")) benchFlash();
  if (all || !strcmp(a1, "spi")) benchSpi();
  if (a1 && !strcmp(a1, "usb")) benchUsb(a2 ? (uint32_t)atoi(a2) : 256);
  if (a1 && !strcmp(a1, "psram")) benchPsram(a2 ? atoi(a2) : 4, 8);
  logMsg("bench: done");
}

// GET /bench?kb=N: N KB of zeros, for timing Wi-Fi from the PC
esp_err_t benchHttpHandler(httpd_req_t *req) {
  char q[32] = "", v[12] = "";
  uint32_t kb = 4096;
  if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK && httpd_query_key_value(q, "kb", v, sizeof v) == ESP_OK)
    kb = (uint32_t)atoi(v);
  if (!kb || kb > 65536) kb = 4096;
  const size_t CH = 4096;
  char *buf = (char *)heap_caps_calloc(1, CH, MALLOC_CAP_SPIRAM);
  if (!buf) return httpd_resp_send_500(req);
  httpd_resp_set_type(req, "application/octet-stream");
  int64_t t0 = us();
  esp_err_t err = ESP_OK;
  for (uint32_t i = 0; i < kb / 4 && err == ESP_OK; i++) err = httpd_resp_send_chunk(req, buf, CH);
  if (err == ESP_OK) httpd_resp_send_chunk(req, nullptr, 0);
  int64_t dt = us() - t0;
  free(buf);
  logMsg("bench wifi: sent %lu KB in %.0f ms -> %.2f MB/s (board side)", (unsigned long)kb, dt / 1000.0, mbps(kb * 1024.0, dt));
  return err;
}
