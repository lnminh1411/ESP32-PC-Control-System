#include "heapguard.h"

#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <esp_rom_sys.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_cpu.h>
#include <esp_ipc.h>
#include <esp_vfs_ops.h>
#include <lwip/sockets.h>
#include <soc/soc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

// Survives the panic restart; the magic tells a filled ring from noise
#define HG_MAGIC 0x48475244u
RTC_NOINIT_ATTR static uint32_t hgMagic;
RTC_NOINIT_ATTR static uint32_t hgHead;
RTC_NOINIT_ATTR static char hgRing[1536];

// The S3's cache error reason, kept over the panic reset: IDF reads it from
// the cache registers only to print it, and the core dump does not keep it.
// esp_cache_err_get_panic_info is wrapped (-Wl,--wrap, CMakeLists.txt).
#define HG_CERR_MAGIC 0x43455252u
RTC_NOINIT_ATTR static uint32_t hgCerrMagic;
RTC_NOINIT_ATTR static char hgCerrStr[96];
RTC_NOINIT_ATTR static uint32_t hgCerrAddr, hgCerrSize;

extern "C" {
typedef struct { const char *err_str; uint32_t vaddr; uint32_t size; } hg_cache_err_info_t;
void __real_esp_cache_err_get_panic_info(hg_cache_err_info_t *err);
void __wrap_esp_cache_err_get_panic_info(hg_cache_err_info_t *err) {
  __real_esp_cache_err_get_panic_info(err);
  const char *s = err && err->err_str ? err->err_str : "Cache disabled but cached memory region accessed (no error bit)";
  size_t i = 0;
  for (; s[i] && i < sizeof hgCerrStr - 1; i++) hgCerrStr[i] = (s[i] == 13 || s[i] == 10) ? 32 : s[i];
  hgCerrStr[i] = 0;
  hgCerrAddr = err ? err->vaddr : 0;
  hgCerrSize = err ? err->size : 0;
  hgCerrMagic = HG_CERR_MAGIC;
}
}

static void hgPutc(char c) {
  hgRing[hgHead % sizeof hgRing] = c;
  hgHead++;
}

static void hgPrintf(const char *fmt, ...) {
  char line[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  for (const char *p = line; *p; p++) hgPutc(*p);
}

// "CORRUPT HEAP: Bad tail at 0x3c1f2a10. ..." -> the bytes before the canary
// (the end of the block that ran over) and the canary itself
static void dumpAround(uintptr_t at) {
  if (!esp_ptr_external_ram((void *)at) && !esp_ptr_internal((void *)at)) return;
  const uint8_t *p = (const uint8_t *)(at - 96);
  hgPrintf("hg: 96 bytes before %08x:\n", (unsigned)at);
  for (int row = 0; row < 3; row++) {
    char txt[33];
    for (int i = 0; i < 32; i++) {
      uint8_t b = p[row * 32 + i];
      txt[i] = b >= 32 && b < 127 ? (char)b : '.';
    }
    txt[32] = 0;
    hgPrintf("hg: |%s|\n", txt);
  }
  // the bad bytes and what follows them, as hex and as text
  const uint8_t *q = (const uint8_t *)at;
  for (int row = 0; row < 2; row++) {
    char hex[16 * 3 + 1], txt[17];
    for (int i = 0; i < 16; i++) {
      uint8_t b = q[row * 16 + i];
      snprintf(hex + i * 3, 4, "%02x ", b);
      txt[i] = b >= 32 && b < 127 ? (char)b : '.';
    }
    txt[16] = 0;
    hgPrintf("hg: +%02d %s|%s|\n", row * 16, hex, txt);
  }
}

// ---- lwIP's socket-VFS entry (10-27 21:09 and 10-28 09:16 crashes) ----
// esp_vfs_select follows s_vfs[i]->vfs->select->socket_select; twice the
// entry's vfs pointer or what it leads to was garbage, with every heap
// canary intact. Found by value at startup (s_vfs is static): the select ops
// hold &lwip_select, the fs ops point to them at ->select, the entry to the
// fs ops at ->vfs (vfs_entry_t: int flags, then the pointer). CPU stores to
// entry->vfs and ops->select trip a hardware watchpoint on either core (the
// panic backtrace names the writer); every guard tick also compares the
// chain, so a DMA write (no watchpoint) still gets caught and dumped.
struct HgVfsEntry { int flags; const esp_vfs_fs_ops_t *vfs; void *ctx; int offset; };
static HgVfsEntry *wEntry;
static const esp_vfs_fs_ops_t *wOps;
static const esp_vfs_select_ops_t *wSel;
static uint8_t wSelCopy[sizeof(esp_vfs_select_ops_t)];

// Every used heap block (internal, RTC and PSRAM: VFS allocates with
// MALLOC_CAP_DEFAULT) searched for a 32-bit value; up to 16 hits, our own
// stack's block skipped
struct HgFind {
  uint32_t value, skipFrom, skipTo;
  uint32_t *hit[16];
  int n;
};

static bool findCb(walker_heap_into_t, walker_block_info_t blk, void *user) {
  HgFind *f = (HgFind *)user;
  // internal only: the VFS tables are there (VFS_SELECT_IN_RAM), and a scan
  // of PSRAM under the heap lock stalls both cores
  if (!blk.used || f->n >= 16 || esp_ptr_external_ram(blk.ptr)) return true;
  uint32_t from = ((uint32_t)blk.ptr + 3) & ~3u, to = (uint32_t)blk.ptr + blk.size;
  if (from < f->skipTo && to > f->skipFrom) return true;
  for (uint32_t a = from; a + 4 <= to && f->n < 16; a += 4)
    if (*(volatile uint32_t *)a == f->value) f->hit[f->n++] = (uint32_t *)a;
  return true;
}

static int findWords(uint32_t value, HgFind *f) {
  uint32_t sf = (uint32_t)pxTaskGetStackStart(nullptr);
  f->value = value;
  f->skipFrom = sf;
  f->skipTo = sf + 3072;
  f->n = 0;
  heap_caps_walk_all(findCb, f);
  return f->n;
}

static void setWatch(void *) {
  esp_cpu_set_watchpoint(0, &wEntry->vfs, 4, ESP_CPU_WATCHPOINT_STORE);
  esp_cpu_set_watchpoint(1, (const void *)&wOps->select, 4, ESP_CPU_WATCHPOINT_STORE);
}

static HgFind fa, fb, fc;   // static: 3 x 80 B would crowd the 3 KB stack

static bool vfsWatchInstall() {
  // Locals only until the whole chain checks out: a static holding a
  // candidate would be found by the next search.
  findWords((uint32_t)&lwip_select, &fa);
  for (int i = 0; i < fa.n; i++) {
    auto *sel = (const esp_vfs_select_ops_t *)((uint8_t *)fa.hit[i] - offsetof(esp_vfs_select_ops_t, socket_select));
    if (!esp_ptr_executable((void *)sel->stop_socket_select) || !esp_ptr_executable((void *)sel->get_socket_select_semaphore))
      continue;
    findWords((uint32_t)sel, &fb);
    for (int j = 0; j < fb.n; j++) {
      auto *ops = (const esp_vfs_fs_ops_t *)((uint8_t *)fb.hit[j] - offsetof(esp_vfs_fs_ops_t, select));
      if (!esp_ptr_executable((void *)ops->read) || !esp_ptr_executable((void *)ops->write)) continue;
      findWords((uint32_t)ops, &fc);
      for (int k = 0; k < fc.n; k++) {
        auto *entry = (HgVfsEntry *)((uint8_t *)fc.hit[k] - offsetof(HgVfsEntry, vfs));
        if (entry->offset < 0 || entry->offset >= 8 || (uint32_t)entry->flags > 0xff) continue;
        wSel = sel;
        wOps = ops;
        wEntry = entry;
        memcpy(wSelCopy, wSel, sizeof wSelCopy);
        // watchpoints only reach memory the CPU stores to through the data
        // bus: internal RAM and PSRAM alike
        esp_ipc_call_blocking(0, setWatch, nullptr);
        esp_ipc_call_blocking(1, setWatch, nullptr);
        logMsg("heapguard: socket VFS entry %p (index %d, %s), ops %p (%s), select ops %p (%s): watchpoints on both cores",
               wEntry, wEntry->offset, esp_ptr_external_ram(wEntry) ? "PSRAM" : "internal", wOps,
               esp_ptr_external_ram(wOps) ? "PSRAM" : "internal", wSel, esp_ptr_external_ram(wSel) ? "PSRAM" : "internal");
        return true;
      }
    }
  }
  return false;
}

static void dumpHex(const char *what, const void *at, int before, int len) {
  const uint8_t *p = (const uint8_t *)at - before;
  hgPrintf("hg: %s %08x:\n", what, (unsigned)(uintptr_t)at);
  for (int row = 0; row < len / 16; row++) {
    char hex[16 * 3 + 1], txt[17];
    for (int i = 0; i < 16; i++) {
      uint8_t v = p[row * 16 + i];
      snprintf(hex + i * 3, 4, "%02x ", v);
      txt[i] = v >= 32 && v < 127 ? (char)v : '.';
    }
    txt[16] = 0;
    hgPrintf("hg: %+04d %s|%s|\n", row * 16 - before, hex, txt);
  }
}

// false once the chain changed: dumped into the ring, the caller aborts
static bool vfsWatchCheck() {
  if (!wEntry) return true;
  if (wEntry->vfs == wOps && wOps->select == wSel && !memcmp(wSel, wSelCopy, sizeof wSelCopy)) return true;
  hgPrintf("hg: socket VFS chain changed: entry->vfs %08x (was %08x), ops->select %08x (was %08x)\n",
           (unsigned)(uintptr_t)wEntry->vfs, (unsigned)(uintptr_t)wOps, (unsigned)(uintptr_t)wOps->select,
           (unsigned)(uintptr_t)wSel);
  dumpHex("entry", wEntry, 32, 96);
  dumpHex("select ops", wSel, 32, 96);
  return false;
}

static void guardTask(void *) {
  int64_t worstUs = 0, lastReportUs = esp_timer_get_time();
  for (uint32_t tick = 1;; tick++) {
    vTaskDelay(pdMS_TO_TICKS(20));
    uint32_t from = hgHead;
    // internal RAM every 20 ms: a block that overran its canary is caught
    // before the overwritten neighbour gets used (a 500 ms period lost that
    // race on 10-27 21:09). No PSRAM sweep: its ~150 ms scan held the heap
    // lock with interrupts off and likely caused the interrupt watchdog
    // resets (10-27..28); PSRAM bit errors show in the ECC log instead.
    int64_t t0 = esp_timer_get_time();
    bool ok = heap_caps_check_integrity(MALLOC_CAP_INTERNAL, true);
    int64_t dt = esp_timer_get_time() - t0;
    if (dt > worstUs) worstUs = dt;
    if (!wEntry && tick % 50 == 0 && tick <= 3000 && !vfsWatchInstall() && tick == 3000)
      logMsg("heapguard: socket VFS entry not found (%d heap words hold lwip_select), no watchpoints", fa.n);
    if (ok && !vfsWatchCheck()) {
      hgPrintf("hg: socket VFS corrupted without a watchpoint hit (DMA?), restarting\n");
      abort();
    }
    if (t0 - lastReportUs >= 10 * 60 * 1000000LL) {
      logMsg("heapguard: internal check took up to %lld us in the last 10 min", worstUs);
      worstUs = 0;
      lastReportUs = t0;
    }
    if (ok) continue;
    // the check printed its findings into the ring: find the first address
    char seen[256];
    uint32_t n = hgHead - from;
    if (n > sizeof seen - 1) n = sizeof seen - 1;
    for (uint32_t i = 0; i < n; i++) seen[i] = hgRing[(from + i) % sizeof hgRing];
    seen[n] = 0;
    const char *a = strstr(seen, " at 0x");
    if (a) dumpAround((uintptr_t)strtoul(a + 4, nullptr, 16));
    hgPrintf("hg: heap check failed, restarting\n");
    abort();
  }
}

// last boot's ring, copied before the hook clears it: the hook goes in early
// (heap checks during init print through it), logging works only later
static char hgPrev[sizeof hgRing + 1];
static bool hgPrevPanic;

void heapGuardEarly() {
  esp_reset_reason_t r = esp_reset_reason();
  hgPrevPanic = r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT;
  if (hgMagic == HG_MAGIC && hgPrevPanic) {
    uint32_t start = hgHead > sizeof hgRing ? hgHead - sizeof hgRing : 0, n = 0;
    for (uint32_t i = start; i < hgHead; i++) hgPrev[n++] = hgRing[i % sizeof hgRing];
    hgPrev[n] = 0;
  }
  hgMagic = HG_MAGIC;
  hgHead = 0;
  esp_rom_install_channel_putc(1, hgPutc);
}

void heapGuardStart() {
  if (hgCerrMagic == HG_CERR_MAGIC && hgPrevPanic)
    logMsg("heapguard: last panic was a cache error: %s, address 0x%08lx, size 0x%lx", hgCerrStr,
           (unsigned long)hgCerrAddr, (unsigned long)hgCerrSize);
  hgCerrMagic = 0;
  // the ring's last 1.5 KB, oldest first, one log line per text line
  char line[128];
  size_t len = 0;
  for (const char *c = hgPrev; *c; c++) {
    if (*c == '\n' || len == sizeof line - 1) {
      line[len] = 0;
      if (strstr(line, "CORRUPT") || strstr(line, "hg:") || strstr(line, "assert") || strstr(line, "Invalid data"))
        logMsg("heapguard: %s", line);
      len = 0;
    } else if (*c != '\r') {
      line[len++] = *c;
    }
  }
#ifdef CONFIG_HEAP_POISONING_DISABLED
  logMsg("heapguard: heap poisoning is off, checks find little");
#endif
  xTaskCreate(guardTask, "heapguard", 3072, nullptr, 3, nullptr);   // above the app tasks: checks run on time
}
