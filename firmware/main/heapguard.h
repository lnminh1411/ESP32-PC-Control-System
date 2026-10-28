#pragma once

// Debug aid for heap corruption (needs CONFIG_HEAP_POISONING_LIGHT): ROM
// printf goes to a ring in RTC memory, a task checks the internal heap's
// canaries every 20 ms (PSRAM is not swept), and a failed check saves the
// bytes around the overrun and restarts. The next boot logs what was saved.
void heapGuardEarly();   // app_main, first thing: ROM printf into the ring
void heapGuardStart();   // once logging works: report the last boot, start the checks
