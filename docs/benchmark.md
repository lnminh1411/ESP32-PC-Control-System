# Benchmark

Carrier board (ESP32-S3 rev 0.2, 240 MHz, 16 MB flash, octal PSRAM at 40 MHz
with ECC, 7680 KB usable), firmware as of 2026-11-01, PC on, Wi-Fi at -18 dBm.
The dated sections below were measured at the time, the ones before
2026-10-28 with PSRAM at 80 MHz and without ECC (why it changed:
[firmware.md](firmware.md#psram-hardening)).
Run it again with the console command `bench all` (`bench usage` for memory use only,
`bench usb <KB>` for USB),
and time Wi-Fi from the PC with
`curl -o NUL -H "X-OTA-Pass: <OTA_PASS>" -w "%{speed_download}\n" http://<board-ip>/bench?kb=4096`.

## Usage

From `bench usage` (part of `bench all`), PC on, 472-frame GIF pack decoded as
it plays, after GIFs had run on all three panels. Each panel that shows one
holds 153 KB of PSRAM (its frame and its decoded rects).

| Memory | Used | Notes |
|---|---|---|
| SRAM (512 KB) | 397 KB (78%) | heap 366 KB; lowest free ever: 50 KB, largest block 31 KB |
| PSRAM (7680 KB) | 1887 KB (25%) | lowest free ever 5024 KB, largest free block 4992 KB |
| Flash (16 MB) | fully partitioned | one 8.9 MB app slot (no OTA), GIF pack 6 MB, FAT 1 MB, NVS 20 KB, core dump 64 KB |
| App image | 1638 KB of 9088 KB (18%) | |
| GIF pack | 6071 KB of 6144 KB (99%) | its own `gifs` partition: 12 GIFs, 472 frames |
| FAT /ffat | 4 KB of 972 KB (0%) | layout.json, saved text images |
| NVS | 232 of 630 entries | 12 namespaces |

Task stacks, least ever free (bytes): main 6720, discord 5528, httpd 4904,
websocket 4652, weather 3472, TinyUSB 2808, flush 2716, wifi 2568, gifdec 2200,
esp_timer 1988, mdns 1396, tiT 1372, Tmr Svc 1336, mqtt 1244, led 1200, sys_evt 788,
IDLE 688-696, ipc 436-508.

## Internal RAM, before and after 2026-10-26

Internal RAM was the tight one: free space fell to ~3 KB within 8 s of boot,
when the Wi-Fi login and three or four TLS handshakes ran at once, and
idled near 40 KB with an 8.7 KB largest block (a TLS handshake needs ~4 KB
blocks). Four changes:

1. Wi-Fi static TX buffers 16 -> 8 (`CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM`):
   with PSRAM on they default to static, ~1.6 KB each in internal RAM for good.
2. mbedTLS: dynamic buffers, config data and CA chain freed after the
   handshake, peer certificate not kept. (Dynamic buffers were turned off
   again on 2026-10-27; see below.)
3. Discord button clicks and slash commands handled in the bot task through a
   queue instead of in the websocket task: one REST TLS client instead of two,
   websocket task stack 12 -> 8 KB, and a slow PC command no longer holds the
   gateway connection.
4. The weather task waits for Discord's gateway (or 30 s) before its first
   HTTPS fetch after Wi-Fi comes up.

Same test both times: three rounds of `bench all`, `bench usb 2048`, three 4 MB
Wi-Fi downloads and 30 pings (`soak.py`).

| | Before (internal lwIP, 18:26) | After (21:00) |
|---|---|---|
| Internal free, lowest during the rounds | 40.9 KB | 73.1 KB |
| Largest internal block | 8.7 KB | 31.7 KB |
| Internal free, lowest ever since boot | ~2-3 KB | 34 KB |
| PSRAM free | 3.16 MB | 3.23 MB |
| Wi-Fi download, 9 runs | 0.60-0.88 MB/s, 0.80 average | 0.62-0.92 MB/s, 0.78 average |
| `bench usb 2048` CDC write | 0.23 MB/s | 0.23 MB/s |
| Ping loss | 0% | 0% |

Step by step, 90 s after boot: 40 KB free / 7.5 KB largest -> 54.5 / 22.5
(TX buffers) -> 68.7 / 29.7 (mbedTLS) -> 72.5 / 31.7 (queue; lowest ever 22 ->
38 KB) -> 72.6 / 31.7 (boot stagger, kept as a guard). The Wi-Fi difference is
within the radio's run-to-run swing.

## USB buffers, cache lines, one draw per pass (2026-10-26, later)

Three changes:

1. USB receive stream buffers (HID 24 KB, CDC 4 KB) store their bytes in
   PSRAM. FreeRTOS allocates them from internal RAM by default. The control
   blocks stay internal.
2. Data cache lines 32 -> 64 bytes (`CONFIG_ESP32S3_DATA_CACHE_LINE_64B`). The
   faces draw into PSRAM canvases, so most of their time is PSRAM traffic.
3. `viewsService` draws at most one canvas per pass. With three static faces
   due at once it drew all three back to back (20 + 19 + 30 ms), and the
   console waited up to 74 ms. Now the console waits for one face at most.
   GIF frames need no draw, so they still go in the same pass.

The task watchdog came back too: the main loop is subscribed, and a stall
panics after 20 s (core dump, reboot).

Wi-Fi was down on both runs (the 2.4 GHz network was off the air), so the
internal RAM numbers below are lower in use than with Discord and weather
running. Compare the difference, not the totals, with the tables above.

| | Before (HEAD 4112843) | After |
|---|---|---|
| Internal free | 133.1 KB | 162.6 KB |
| Largest internal block | 58 KB | 88 KB |
| Internal free, lowest ever since boot | 93 KB | 120 KB |
| PSRAM free | 3249 KB | 3220 KB |
| Main task CPU (core 1, faces pc/media/ai at 2 fps) | 24.2% | 20.9% |
| Worst main loop pass | 72-75 ms | one face, 27 ms at most |

Face draw time per frame, measured with a temporary profiler (30 frames each):

| Face | 32-byte lines | 64-byte lines |
|---|---|---|
| PC stats | 20.6 ms | 18.4 ms |
| Media | 19.3 ms | 14.8 ms |
| AI usage | 29.7 ms | 25.1 ms |

`fillScreen` alone takes 3-7 ms per face, and each card fills its area again.
Text is about 1 ms per string. The time is spent on pixel writes to PSRAM, so
further cuts need partial redraws (only the fields that changed).

## WinUSB bulk pipe and USB receive backpressure (2026-10-26, later)

1. The board gets a vendor interface with two bulk endpoints. Its MS OS 2.0
   descriptors make Windows bind WinUSB with no driver install, and the
   service uses it before HID.
2. USB serial (CDC) and the new pipe stop dropping bytes when the board's
   receive buffer is full: the bytes wait in TinyUSB's FIFO and the PC is
   told to wait (NAK). Before, a 41 KB burst over COM12 lost lines every time.

| | Before | After |
|---|---|---|
| Cover art (41 KB of base64) from the service, board's own timing | 688 ms (HID) | 115 ms (WinUSB) |
| 43 KB burst to COM12 with no pacing | lines lost (`art: bad chunk`) | intact, 0.4 MB/s |
| Internal RAM free | 162.4 KB | 158 KB |
| Largest internal block | 88 KB | 80 KB |

The pipe is no longer the limit: 115 ms is the board parsing the base64 text
lines one byte at a time, the same as for the burst over COM12 (117 ms).

## Speed

| Area | Test | Result |
|---|---|---|
| CPU | 32-bit integer multiply-add | 79 M/s (3 cycles each) |
| CPU | float multiply-add (FPU) | 79 MFLOPS |
| CPU | double multiply-add (software) | 3 MFLOPS |
| SRAM | copy / write / read | 239-377 / 238-738 / 119 MB/s (varies run to run with alignment) |
| SRAM | random read | 13 ns |
| PSRAM | copy / write / read (1 MB buffers) | 15 / 23 / 41 MB/s (31 / 48 / 72 at 80 MHz) |
| PSRAM | random read (1 MB span) | 1410-1420 ns (697 at 80 MHz) |
| PSRAM | stress, 4 MB x 8 passes | 0 bad words, 2.5 s |
| PSRAM to SRAM | copy in 4 KB pieces | 43 MB/s (86 at 80 MHz) |
| Flash | raw partition read (4 KB) | 10.8-11.4 MB/s |
| Flash | memory-mapped read (cache) | 27.5-30.9 MB/s |
| Display SPI | full 240x320 RGB444 frame (115 200 B) | 32.9-37.2 ms, 3.1-3.5 MB/s, 62-70% of the 40 MHz wire (27.8-28.9 ms, 4.0-4.1 MB/s with PSRAM at 80 MHz) |
| Display SPI | GIFs on all three panels | 14.9 fps each, 3.9 MB/s, 82% of the wire (2026-11-01, see below) |
| USB CDC | board to PC | 0.70 MB/s (`bench usb 2048`) |
| USB CDC | PC to board, text lines parsed | 0.4 MB/s (43 KB in 105 ms, 2026-10-26) |
| USB | cover art, PC service to board | 115 ms on WinUSB bulk, 688 ms on HID (2026-10-26) |
| Wi-Fi | HTTP download to the PC (4 MB, 3 runs) | 0.84-0.89 MB/s |
| Wi-Fi | ping | 2-5 ms, 3 ms average |

Notes:
- A full frame takes ~35 ms per panel, so all three panels repainting at once
  cap out near 10 fps; the dirty-rect diff is what keeps normal faces at their
  frame rates. SPI runs at 40 MHz because 80 MHz gave blank panels.
- `bench spi` reads the canvas from PSRAM. PSRAM went from 80 to 40 MHz on
  2026-10-28, the likely cause of the slower frame (not measured on its own).
- Wi-Fi throughput is low on purpose: 6 static RX and 6 static TX buffers and
  a BA window of 6 free internal RAM for TLS (see `sdkconfig.defaults`).
  Nothing on the board needs more than ~100 KB/s.

## Build changes and TLS dynamic buffers off, 2026-10-27

The build changes (MINIMAL_BUILD, lovyangfx driver requirements, cached IDF
environment) were measured against the main build. Both builds carried the
heapguard debug checker and light heap poisoning.

With the build changes in, the board crash-looped about 50 s after each boot.
Closing the GDACS cyclone download (200-350 KB over TLS) freed a corrupted
mbedTLS dynamic-buffer block in internal RAM: 2 bytes were written past its
end. `CONFIG_MBEDTLS_DYNAMIC_BUFFER` is now off.

| | Before (main) | After (build changes, dynamic buffers off) |
|---|---|---|
| Internal free during the rounds | 75.5 KB | 77.9 KB |
| Internal free, lowest ever since boot | 22 KB | 34 KB |
| Largest internal block | 31.7 KB | 31.7 KB |
| PSRAM free | 3.17 MB | 3.11 MB |
| App image | 1591 KB | 1586 KB |
| Core 1 (main loop) | 22.8 % | 21.8 % |
| Wi-Fi download, 9 runs | 0.99-1.14 MB/s | 0.44-0.97 MB/s, one stall (radio) |
| `bench usb 2048` CDC write | 0.23 MB/s | 0.14-0.22 MB/s (varies run to run) |
| Incremental build | 33-42 s | 16-23 s |
| Clean build | 269 s | 158-197 s |
| Crashes | 0 | 0, through both cyclone fetches |

## GIFs built into the firmware, 2026-11-01

The GIFs moved from an upload in FAT, decoded at boot into whole RGB444 frames,
to `gifs.bin` in the app image: per-GIF palettes, frame 0 whole and the other
frames as their dirty rects, zlib-compressed in flash and inflated into PSRAM
at boot. Both GIFs then played at 20 fps (Neuro 22 frames, Evil Neuro 20, 50 ms
each); the first pack had 27 frames at 15 and 10 fps. "Before" is the previous
firmware with its single test GIF, and its ~3.8 MB is an estimate. "Now" is
today's twelve GIFs: LZ4 in their own `gifs` partition, predicted from the frame
before and decoded as they play (the later sections cover each step).

| | Before | First pack, 27 frames | 42 frames | Now, 472 frames |
|---|---|---|---|---|
| GIF pack | none | 532 KB | 793 KB | 6071 KB |
| Frames in PSRAM | ~3.8 MB with tables and buffers | 1780 KB | 2722 KB | none: 38 KB of tables, 153 KB a panel showing a GIF |
| PSRAM used, `bench usage`, three 75 KB panel buffers in the older columns | 5179 KB | 3428 KB | 4374 KB | 1887 KB |
| PSRAM free, lowest ever without bench buffers | 1438 KB | 3426 KB | 2456 KB | 5024 KB |
| App image | 1650 KB | 2178 KB | 2439 KB | 1638 KB |
| FAT /ffat | 4508 KB | 4 KB | 4 KB | 4 KB |
| Decoding at boot | 4.5 MB read from FAT | 329 ms to inflate | 498 ms to inflate | none, 13-15 ms a frame as it plays |

Dirty rects covered most of the screen on most frames (86-95%), since most of
each picture moves, which is why the frames took this much PSRAM. Today's
optimal rects cover 58-99% depending on the GIF, and only the frame on screen
is decoded.

| GIF playback at 20 fps | Frame rate | Frame times | Bus |
|---|---|---|---|
| One panel | 19.5 fps (17.7-20.5) | 1.04x nominal | 1.9 MB/s, 41% |
| Two panels | 17.8 fps | 1.12x nominal | 70% |
| Three panels | 12.7 fps | 1.67x nominal | 73% |
| Now, one panel | 19.9 fps | 1.0x nominal | 1.3-2.0 MB/s, 34% |
| Now, two panels | 19.6 fps | 1.03x nominal | 74% |
| Now, three panels | 14.9 fps | 1.34x nominal | 82% |

A frame took 26-28 ms to push, now 21-25 ms. The GIF slows down rather than drop
frames, so on three panels it played at about 60% speed, now about 75%; three
panels at 20 fps need about 5.7 MB/s, more than the 40 MHz wire carries.

The app slots then grew from 3 MB to 5 MB each and FAT shrank from 9.9 MB
to 5.9 MB (`partitions.csv`, flashed over USB). The same image filled 48%
of its slot instead of 79%, leaving about 2.6 MB for more GIFs. The table is
now one 8.9 MB app slot, FAT 1 MB and a 6 MB `gifs` partition with 73 KB free.

### Frames inflated as they play

The frames then stopped being inflated into PSRAM at boot. They stay
compressed in flash, and the `gifdec` task (core 1) inflates each one just
before the flush task pushes it. Measured with the 42-frame pack, PC on, GIF
on panel 1:

| | Inflated at boot | Inflated as they play |
|---|---|---|
| PSRAM used (`bench usage`) | 4374 KB (three panel buffers) | 1584 KB (one panel buffer) |
| PSRAM lowest free | 2456 KB | 5094 KB |
| Frames in PSRAM | 2722 KB | none: 113 KB of tables and one 75 KB inflate buffer |
| SRAM lowest free | 29 KB | 25 KB |
| Boot | 498 ms to inflate | none |
| Frame cap | 64 | 512, and 8 GIFs (12 now) |

| Inflating | Core 1 | Panel 1 fps |
|---|---|---|
| `gifdec` at priority 0 (below main) | 80% | 9.8: it shared only the idle time, with IDLE1 |
| `gifdec` at main's priority (1) | 66-92% | 19.4-20.0 |

- **Per frame:** 15-16 ms of CPU. The inflate state sits in PSRAM: in internal
  RAM it was no faster and took 11 KB of the scarce SRAM (lowest free fell to
  14 KB).
- **CPU:** a 20 fps GIF takes about 30% of core 1. Core 1 reached 92% only
  with the media face also redrawing at 17 fps; with the other faces at 2 fps
  it sits near 66%.
- **Three panels:** 11-13 fps per panel, the same as before: the SPI bus is
  the limit, not inflating.
- **March 7th** then joined as a third GIF: 1 s of its 30 fps source (frames
  120-149, a loop that closes), RIFE-interpolated to 20 frames at 20 fps. The
  pack is 1201 KB for 62 frames, and the app image fills 56% of its slot.

## Internal RAM pass, 2026-11-01

OTA went first (a single factory app slot, now 8.9 MB, no rollback). Then:

- The heap-corruption hunt's debug code came out: the `heapguard` task, its
  buffers and comprehensive heap poisoning (local `sdkconfig` only).
- Heap and ringbuf functions and the SPI master ISR run from flash
  (`CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH`, `CONFIG_RINGBUF_PLACE_FUNCTIONS_INTO_FLASH`,
  SPI master ISR out of IRAM). FreeRTOS stays in IRAM: ESP-IDF 5.5 has no
  option to move it. A few hours after the first flash the main task crashed
  in the ISR's bus lock (`spi_bus_lock_bg_exit`, null `acquiring_dev`):
  ESP-IDF issue 18527, a race the slower ISR made likelier. ESP-IDF is now
  patched with the upstream fix (`firmware/idf_patches`) and the ISR is
  pinned to core 0 with the flush task, so the ISR went back to flash.
- Wi-Fi static RX and TX buffers and the BA window went from 8 to 6.
- About 8 KB of our own static buffers moved to PSRAM (`psramCalloc`): the
  `perf` history, the Discord command reply, the console line buffers, the
  two nowcast engines, the Discord and Pushover queue slots, the `bench usb`
  line.

Both columns are `bench usage` a few minutes after boot, PC on.

| | Before | After |
|---|---|---|
| SRAM static (IRAM code, data, bss) | 173 KB | 145 KB (IRAM code 75 -> 59 KB, .bss 47 -> 37 KB) |
| Internal heap free | 69 KB | 121 KB |
| Internal lowest free since boot | 25 KB | 56 KB |
| Largest internal block | 31 KB | 37 KB |
| GIF on panel 1 (`perf`) | 21.0 fps, 19.2 ms to inflate a frame | 19.9 fps, 18.8 ms |
| Core 1 / core 0 | 66% / 25% (heapguard 5.8%) | 68% / 23% |
| SPI full frame | 33.6-34.8 ms | 33.6-33.9 ms |
| Wi-Fi HTTP download | 0.84-0.89 MB/s | 0.83-0.85 MB/s |

The GIF runs at its 20 fps either way; the 21.0 before was a short window.
With the SPI ISR and heap back in IRAM for a while: 161 KB static, 105 KB
free. Back in flash with the patch and the core pin: 145 KB static, 121 KB
free, and 3.6 minutes with GIFs on all three panels plus `bench spi` every
25 s (bus 74%, 10.8-11.8 fps each) ran without a fault.

## GIF pipeline, 2026-11-01 (later)

Three changes, then two more after measuring:

- **Packer:** each frame's dirty rects are the cheapest cover of its changed
  8-row bands by at most 8 rects (pixels pushed plus 200 a rect for the
  window commands), found by dynamic programming. The old packer took one
  rect per run of changed bands, and more than 8 runs meant the whole frame.
  Rects now cover 64-70% of the screen a frame, down from 86-95%.
- **LZ4 instead of zlib:** pack format 2, one LZ4-HC block a frame, decoded
  on the board by a bounds-checked LZ4 block decoder (`gif_store.cpp`)
  instead of miniz's tinfl. The pack grew from 1199 to 1407 KB.
- **Packing RGB444:** gifdec decodes a frame's rects into a buffer of its own
  (`gifRaw`, back to back) and copies them into the panel's frame
  (`gifCur`) only on the overlay band's rows. Flush packs the pixels through
  the palette, 8 at a time, into the internal DMA chunks, each chunk while
  the one before it is on the wire. A band move, a segment switch or a
  loop's first frame rebuilds the whole frame and pushes it whole; `shot`
  rebuilds the frame it captures.

`perf`, PC on, GIF on panel 1 alone, then on all three:

| | Before | After |
|---|---|---|
| One panel | 19.9 fps, 18.8 ms to inflate a frame, `gifdec` 37.5% of core 1 | 19.9 fps, 5.9 ms to decode, `gifdec` 11.8% of core 1, flush packs in 2.7 ms |
| One panel, bus | 41% | 32% |
| Three panels | 10.8-11.8 fps, bus 74%, core 1 93% | 15.8 fps each, bus 76%, core 1 58%, core 0 46% |
| PSRAM a GIF panel | 75 KB, plus one shared 75 KB inflate buffer and 11 KB inflate state | 150 KB |

The steps measured with three panels, all at bus 62-76%:

| Step | fps | Core 1 | `gifdec` CPU a frame |
|---|---|---|---|
| zlib, old rects (before) | 10.8-11.8 | 93% | 18.8 ms (wall) |
| LZ4, new rects, gifdec packs RGB444 into PSRAM, flush DMAs it from there | 13.6-14.9 | 98% | ~16 ms, 8 ms of it packing |
| The same, packing with word stores | 14.9 | 98% | 8 ms packing: no change |
| A second gifdec on core 0 | 12.8-13.8 | 90% (core 0 91%) | ~25 ms |
| Flush packs into internal DMA chunks, `gifCur` copied on band rows only | 13.9-14.9 | 54% | 6.4 ms |
| Flush packs each chunk while the previous one is on the wire | 15.8 | 58% | 6.7 ms |

- **PSRAM bandwidth was the limit,** not the CPU. PSRAM at 40 MHz with ECC
  copies at 15 MB/s, and the first design moved about 370 KB a frame through
  it: the decode, the copy into `gifCur`, 80 KB of RGB444 written and read
  back by DMA. Word stores did not speed up packing, and a second decoder on
  the other core made each frame slower. The final design moves about
  110 KB a frame.
- **DMA straight from PSRAM works on the S3** (AHB GDMA version 1 needs no
  alignment, even with ECC): `psram_dma_direct` in esp_lcd and transfers up
  to 32 KB. It was dropped with the PSRAM push buffer.
- **The wire is the next limit:** a frame is about 77 KB, so three panels at
  20 fps would need 94% of the 40 MHz wire. Most rects fit one 15 KB chunk,
  so packing each chunk while the previous one is on the wire was worth
  1 fps.
- **The bus budget was not the limit:** with `fps budget 95` three panels ran
  at the same 14.9-15.8 fps, bus 76%, no frame deferred at either budget.
  The default went from 85% to 95% anyway, as headroom.

`bench all` after the change: CPU, SRAM, PSRAM and flash as in the Speed
table; a full frame 33.4-33.7 ms on the panels without a GIF.

## Six GIFs in their own partition, shuffled, 2026-11-01

Firefly, Miku and Filian joined Neuro, Evil Neuro and March 7th, all at 20 fps:
140 frames. The pack left the app image for a 5 MB `gifs` partition, memory-mapped
at boot, so the partition table is now app 9.9 MB, FAT 1 MB, GIFs 5 MB. GIFs take
turns in shuffled rounds: every GIF once a round, none twice in a row across rounds.

| GIF | Frames | Pack | Rects, share of the screen | Palette, worst error |
|---|---|---|---|---|
| Neuro | 22 | 525 KB | 70% | 256, 1.00 of 15 |
| Evil Neuro | 20 | 421 KB | 67% | 212, exact |
| March 7th | 20 | 461 KB | 64% | 256, 3.00 of 15 |
| Firefly | 20 | 631 KB | 83% | 181, exact |
| Miku | 50 | 1482 KB | 96% | 256, 8.54 of 15 on the rarest colours (2.4% of pixels) |
| Filian | 8 | 203 KB | 63% | 225, exact |

| GIF playback at 20 fps | Frame rate | Frame times | Bus |
|---|---|---|---|
| One panel, every GIF | 20.0 fps | 1.0x nominal | 1.5-1.6 MB/s, Miku 2.1-2.2 MB/s |
| Two panels | 19.8 fps | 1.0x nominal | 73% |
| Three panels | 15.9 fps, 12-13 fps on Miku | 1.25x nominal | 77% |

A frame takes 21-22 ms of push time. All 42 panel screenshots across the six GIFs
matched the packed frames exactly, and 15 turns in a row kept to the round rule.

## Nine GIFs, 2026-11-01

Kita, Teto and Herta bring the pack to 222 frames and 5707 KB, too much for 5 MB,
so the partition table is now app 8.9 MB, FAT 1 MB, GIFs 6 MB. Up to 12 GIFs.

| GIF | Frames | Pack | Rects, share of the screen | Palette, worst error |
|---|---|---|---|---|
| Kita | 16 | 741 KB | 99%: the paper grain changes every frame | 241, exact |
| Teto | 50 | 1044 KB | 93% | 256, 4.90 of 15 (2.9% of pixels) |
| Herta | 16 | 200 KB | 58% | 112, exact |

| GIF playback at 20 fps | Frame rate | Frame times | Bus |
|---|---|---|---|
| One panel | 20.0 fps | 1.0x nominal | 39%, 1.5-2.1 MB/s |
| Two panels | 19.1 fps median, 17-18 on Kita, Miku and Teto | 1.01x nominal | 65% |
| Three panels | 15.75 fps median, 12-13 on Kita, Miku and Teto | 1.25x nominal | 77% |

All 38 panel screenshots matched the packed frames exactly, and 19 turns kept to
the round rule. Lowest SRAM free 52 KB, no crashes in the log channel. Herta was
then replaced by a different GIF, so these playback figures and shots are from
the 138 KB version, whose rects covered 44% of the screen.

## Packing the GIFs smaller, 2026-11-01

The packer now flattens backdrops: colours within `flatten` levels of a GIF's
commonest colour become it, where they touch it from the top or sides. Applied to
every pixel that colour test also took pale skin, so it stops at the outlines, and
Filian and Herta stay at 0 (hair and sleeves leak), Miku at 1 (blue hair).

| GIF | Flatten | Pack before | Pack now | Rects before | Rects now |
|---|---|---|---|---|---|
| Neuro | 2 | 525 KB | 518 KB | 70% | 70% |
| Evil Neuro | 2 | 421 KB | 412 KB | 67% | 67% |
| March 7th | 2 | 461 KB | 457 KB | 64% | 63% |
| Firefly | 2 | 631 KB | 627 KB | 83% | 83% |
| Miku | 1 | 1482 KB | 1419 KB | 96% | 92% |
| Kita | 2 | 741 KB | 564 KB | 99% | 99% |
| Teto | 2 | 1044 KB | 965 KB | 93% | 89% |

The pack fell from 5707 KB to 5366 KB (-6%). Two panels 19.7 fps, three 15.9. All
20 panel screenshots matched the packed frames exactly. One panel was not measured
again.

Other ideas measured on the same frames. Sizes are frame data; the
first table's baseline is the pack before flattening, the second's after it.

| Before flattening (5701 KB) | Size | Change |
|---|---|---|
| One LZ4 block, or frames linked to the one before | 5482 KB | -3.6%; one block cannot decode frame by frame |
| zstd 19, each frame on its own | 4564 KB | -20%; decoder not tried on the board |
| 128 colours | 5600 KB | -2% |
| 16 rects instead of 8 | 5701 KB | 0% |
| Holding near-identical pixels | 6593-7185 KB | +16-26% |

| Motion compensation, after flattening (5359 KB) | Size | Change |
|---|---|---|
| Lossless, 8 or 16 px blocks, up to 24 px shift | 5624-5800 KB | +5-8% |
| 1 level of error, 8 px blocks | 3590 KB | -33%; faint speckle, 17-38% of pixels one level off |
| 2 levels of error, 8 px blocks | 2463 KB | -54%; visible speckle, grainy backdrops |

Exact blocks cover 5-64% of the frame depending on the GIF, and shifted ones only
3-9%: interpolated frames rarely repeat pixels exactly, so lossless motion
compensation pays for its vectors and gets little back.

## Twelve GIFs, frames predicted from the last one, 2026-11-01

The 1-level version went in (pack v3): each 8x8 block has a shift up to 24 px into
the frame before, and a pixel within 1 colour level of what that shift lands on is
stored as one symbol. The backdrop colour is exempt, which keeps white backdrops
clean. Rects now follow what the panel shows, so they shrink too. The same nine GIFs
went from 5366 KB to 3730 KB (-30%), which made room for Bocchi, the Miku player and
PPPP Teto. All 26 panel screenshots across the twelve matched the frames replayed
from `gifs.bin`, 13 turns kept to the round rule, and the log channel shows no crashes.

| GIF | Frames | Before | Now |
|---|---|---|---|
| Neuro | 22 | 518 KB | 293 KB |
| Evil Neuro | 20 | 412 KB | 220 KB |
| March 7th | 20 | 457 KB | 275 KB |
| Firefly | 20 | 627 KB | 393 KB |
| Miku | 50 | 1419 KB | 1146 KB |
| Filian | 8 | 203 KB | 125 KB |
| Kita | 16 | 564 KB | 418 KB |
| Teto | 50 | 965 KB | 704 KB |
| Herta | 16 | 200 KB | 156 KB |
| Bocchi | 9 | | 114 KB |
| Miku player | 176 | | 1152 KB |
| PPPP Teto | 65 | | 1075 KB |

| Decoding, one panel | Decode a frame | One panel fps |
|---|---|---|
| Before prediction | 6 ms | 20 |
| Predicted, filled then copied, level with main | 26 ms | 13-20, by GIF |
| One pass both ways, `gifdec` above main | 13-15 ms | 19.9 |

At 26 ms `gifdec` lost time to main drawing the other two panels, so one panel
came out slower than two. Now shifted pixels fill first, then one pass moves the
rest both ways between `gifRaw` and `gifCur`, and `gifdec` runs above main. Three
panels fell from 15.9 to 14.9 fps.
