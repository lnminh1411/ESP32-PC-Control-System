# ESP32 PC Control System

### Why would I put myself through the suffering

I needed my PC online at all time, because sometime at school or work I need to remote
control the PC back at home. And as I measured, the idle time alone costs me ~40KWh (~7usd) per month.
So, I built this for 30usd instead of buying a boring and expensive IP KVM, while adding a tons more
features to fit my over-engineering stuffs hobby.

<table><tr><td width="190" valign="top">
<img src="docs/images/pcb_silkscreen_top.png" alt="Carrier PCB, rev 3.1: top silkscreen over F.Cu" width="180">
</td><td valign="top">

An ESP32-S3 carrier board that lives inside a PC case. It drives three 2.4"
displays, presses the motherboard's hardware-reset line, and works a 4-button
door remote, all from one ESP32-S3-N16R8 module plugged into a custom 2-layer
PCB.

### The board

- 45.7 x 138.2 mm, 2 layers, DRC 0/0 (rev 3.1, pictured: top silkscreen
  over the top copper)
- ESP32-S3-N16R8 module on sockets, three ST7789 panels on one SPI bus
- Optocouplers for the motherboard reset and the four keyfob buttons
- Native USB to a motherboard header: PC power state, stats link and USB
  power commands in one cable
- Wake-on-LAN, Discord bot and OTA over Wi-Fi

</td></tr></table>

## Screens

![All screen faces, captured from the board](docs/images/screens.png)

*Captured from the board itself with `shot` (see [Screenshots](#screenshots)).
The stats in this image are made up; the layout is real.*

Each panel shows a set of faces and picks one from the PC's state:

| Panel | PC on | PC off / asleep | Restarting / updating |
|---|---|---|---|
| 1 | PC stats: CPU, RAM, both GPUs, fans, C: | GIF, scaled to fill the screen height | GIF with a "PC is Restarting" / "PC is Updating" box on top |
| 2 | Media: cover art, title, artist, player, progress; uptime and network | Weather: now, AQI, 2-hour rain nowcast, 3-day forecast | Weather |
| 3 | AI usage: Claude and Antigravity quota left, local AI; full-screen local AI face while the model is working | Clock | Clock with the restart/update state |

Notes on what is shown:
- **Quota cards** show what is *left*, with a countdown and the reset time:
  `Resets in 02:13:41 | 14:09` for the 5-hour window,
  `Resets in 03:04:59:59 | Oct 28` (days:hours:minutes:seconds) for the
  weekly one. The board counts down on its own from the reset time the PC
  sends.
- **Non-Latin text** (Japanese song titles, artist names) is rendered on
  the PC with Windows' fonts and font fallback, then sent to the board as a
  4-bit alpha bitmap. The board caches the last 8 bitmaps.
- **Cover art** is 144x144, sent as RGB444 (31 KB per cover).
- **Weather warnings:** rain within 60 min puts a box on the media and weather
  faces (Light Rain / Heavy Shower / Storm, the ETA counting down each minute,
  how long it lasts); yellow until 20 min out, then red. Typhoons tracked from
  GDACS get a red box in their last 6 h and while they are here.

## What it does

| Function | How |
|---|---|
| 3x status displays | GMT024-08-SPI8P 2.4" ST7789 panels (240x320) on one shared SPI bus, each with its own CS, RST and PWM backlight. They plug straight into the board's right-angle sockets. |
| Motherboard reset | Optocoupler (PC817) across the motherboard's RST pins, fully isolated from the rest of the board. |
| Door remote | Decased 4-button keyfob, powered from an onboard 5V->12V boost. Four optos press its buttons. |
| PC power state | Read from the module's native USB link to the motherboard header (USB active = PC on, suspend = asleep/off). See [docs/pc-power-detection.md](docs/pc-power-detection.md). |
| Status LED | 5 mm LED below the module, for a light pipe in the enclosure. |

## Power control

| Action | Path |
|---|---|
| Power on from shutdown | Wake-on-LAN magic packet on the home network (`wake`) |
| Shutdown / sleep / hibernate / restart | Over the module's USB-OTG link to the PC (`power <sleep\|hibernate\|restart\|shutdown>`), with a HID System Control fallback |
| Hard reset of a hung PC | RST opto (`reset` command) |
| Power cut | BIOS "Always Power On after AC loss" boots the PC when power returns |

What the PC needs for this:
- BIOS: Wake-on-LAN enabled, "Restore on AC power loss" = Power On. The
  ErP / deep-sleep option must be off, or the network card loses power at
  shutdown and WoL cannot work.
- Windows: Fast Startup off (it can stop WoL from working after a shutdown), and the network
  adapter's "Wake on Magic Packet" enabled. Updates sometimes reset these.
- Wired Ethernet: most WiFi adapters can't wake from shutdown.

If the ESP32 itself gets stuck, flipping the PSU switch power-cycles it, and
the AC-loss setting brings the PC back up. See
[docs/recovery-and-physical-access.md](docs/recovery-and-physical-access.md).

## Pin map (rev 3.1)

| Signal | GPIO |
|---|---|
| Display bus: MOSI / SCK / DC | 11 / 12 / 9 (MOSI and SCK through 33R) |
| Display 1: CS / RST / BL | 8 / 10 / 3 |
| Display 2: CS / RST / BL | 18 / 16 / 6 |
| Display 3: CS / RST / BL | 17 / 15 / 5 |
| Door buttons 1-4 (opto rows 1-4) | 39 / 40 / 41 / 42 |
| Motherboard reset (opto row 5) | 1 |
| Status LED | 2 |

Every opto GPIO has a 10k pull-down, so nothing gets pressed while the chip
boots. (That turned out not to hold in ROM download mode; see
[the eFuse note](#keyfob-presses-during-flashing-efuse-fix).) The netlist
([pcb/main.ato](pcb/main.ato)) is the source of truth;
`python scripts/check_pins.py` checks it against
[firmware/carrier_main/config.h](firmware/carrier_main/config.h).
[firmware/carrier_idf/main/config.h](firmware/carrier_idf/main/config.h)
carries the same map.

## Assembly notes

- **Bridge the module's `IN-OUT` solder jumper.** On ESP32-S3-N16R8 boards
  of this type, the `5VIN` pin is input-only by default: a diode stops USB
  5V from reaching it. The carrier takes all of its 5V (all three displays
  and the boost) from that pin, so with the jumper open the displays stay
  dark. Once it is bridged, never feed `5VIN` from another supply while USB
  is plugged in.
- Display panels hang **down** below the board, screen facing out, header
  edge up. That is the only way round that puts panel pin 1 (GND) on socket
  pad 1. Match the GND silk label: plugged in the wrong way, every pin is
  reversed and 5V lands on CS.
- The module rides on its sockets ~11 mm above the board. The boost, the
  five optos and most resistors sit underneath it.
- Set the boost's A+B bridges for 12V and measure the output before
  connecting the keyfob.
- Unplug the motherboard RST cable before flashing any other test firmware
  that toggles GPIOs.
- On a new module, burn the `DIS_USB_OTG_DOWNLOAD_MODE` eFuse before the
  keyfob is connected (below).

### Keyfob presses during flashing (eFuse fix)

With the keyfob connected, its LED lit every time the board was flashed:
door buttons B1/B2 were being pressed.

Cause: in ROM download mode the ESP32-S3 ROM sets up USB-OTG download and
drives GPIO38/39/40 as outputs for an external USB PHY (VMO / VPO / OEN).
GPIO39 and GPIO40 are door rows 1 and 2, and a driven output beats the 10k
pull-downs. The application firmware never runs in download mode, so it
cannot stop this.

Fix: burn the one-time eFuse `DIS_USB_OTG_DOWNLOAD_MODE`:

```bash
espefuse.py -p <ROM port> burn_efuse DIS_USB_OTG_DOWNLOAD_MODE 1
```

After this, download mode leaves GPIO32 and up as inputs and the door rows
stay low. eFuses can only be written once, so this cannot be undone. What
still works afterwards:
- flashing over the USB-Serial-JTAG port (a 1200-baud touch on the app's COM
  port reboots into the ROM, which shows up as a new COM port;
  `scripts/idf_flash.py` does this);
- the application's own USB-OTG device (CDC console + vendor HID stats link).

Only USB-OTG *download* is gone, which this board never used. A later PCB
revision could move the door rows off GPIO38-42 instead.

## Repository layout

| Path | Contents |
|---|---|
| [`firmware/carrier_idf/`](firmware/carrier_idf/) | The board's firmware, ESP-IDF 5.5 (below) |
| [`firmware/carrier_main/`](firmware/carrier_main/) | The earlier Arduino firmware, kept for reference; CI still builds it |
| [`host/pc_stats/`](host/pc_stats/) | Windows service and per-user helper that feed the board: PC stats, media, local-AI status, Claude and Antigravity usage ([host/pc_stats/README.md](host/pc_stats/README.md)) |
| [`pcb/`](pcb/) | atopile netlist ([`main.ato`](pcb/main.ato)), KiCad board, custom footprints, design notes ([pcb/README.md](pcb/README.md)) |
| [`scripts/`](scripts/) | PCB build chain (netlist -> placement -> routes -> checks -> renders -> Gerbers), flashing, GIF/layout/OTA upload, screenshots ([scripts/README.md](scripts/README.md)) |
| [`docs/`](docs/) | Bench findings and design notes, [weather](docs/weather.md), [dashboard layouts](docs/layouts.md) |

## Firmware

`firmware/carrier_idf` is plain ESP-IDF 5.5 with LovyanGFX (a git submodule
behind a wrapper component) for drawing. Each panel draws into a PSRAM
sprite; the transport underneath is esp_lcd with RGB444 on the wire.

| Module | Does |
|---|---|
| `config.h`, `display_config.h` | Pins, timings, panel table |
| `display` | Panel init, flush task on core 0, dirty-rectangle engine, RGB444 push over SPI + DMA, backlights, SPI budget |
| `views` | What each panel shows (`pc`, `media`, `ai`, `gif`, `dash`, test views), saved in NVS |
| `screens` | The faces in the image above and which one each view picks |
| `layout` | JSON dashboard pages from `/ffat/layout.json` ([docs/layouts.md](docs/layouts.md)) |
| `pcstats`, `textimg`, `art` | Stats the PC sends; PC-rendered text bitmaps; cover art |
| `pc_state`, `pc_going` | PC power state from USB events; restart/update detection; status LED |
| `optos`, `power` | Door presses and the reset pulse; Wake-on-LAN and USB power commands |
| `weather`, `nowcast`, `cyclone`, `owntracks`, `pushover` | Open-Meteo forecast and AQI, rain nowcast and warnings, GDACS typhoon tracking, phone location over MQTT, rain alerts ([docs/weather.md](docs/weather.md)) |
| `board_time` | Wall clock: SNTP over Wi-Fi, else the time the PC sends |
| `net`, `discord` | Wi-Fi, mDNS, HTTP OTA and layout upload; Discord bot with an `/actions` panel |
| `gif_store` | GIF frames pre-packed as RGB444 with per-frame dirty rects, saved to FAT |
| `commands`, `console` | The text command set; USB CDC, UART and vendor-HID transports |

How the display pipeline works:
- **Dirty rectangles:** the flush task compares each canvas with what is
  already on the glass in 8-row bands and sends only the changed
  rectangles, so a changed number costs about 1 KB and a static face costs
  nothing.
- **Event-driven redraw with an SPI budget:** faces redraw when their data
  changes or their own timer is due (the marquee at 25 fps, the clock once
  a second), and a token bucket keeps the shared bus under 85%. GIFs slow
  down instead of dropping frames when the bus is busy. `fps` shows and
  tunes it.
- **RGB444 on the wire** (12 bits per pixel, 25% less data than RGB565).
- **Two cores:** core 1 draws, core 0 diffs and sends. One GIF plays at its
  native 29.7 fps with the bus at 47%.

Other behaviour:
- **Door buttons B1-B4** (GPIO39-42): 500 ms presses, one at a time, with a
  500 ms gap enforced in firmware (the board has no hardware interlock).
- **Motherboard reset** (GPIO1): 200 ms pulse, only on the `reset` command,
  never at boot. All opto pins are driven LOW first thing at boot.
- **PC power state** from the module's own USB link, with a 30 s guard
  against the brief USB dropout at power-on. The backlights are meant to
  turn off when the PC sleeps or shuts down (not yet tested on
  `carrier_idf`).
- **Status LED** (GPIO2, PWM). `led <0-100>` is perceptual brightness,
  capped at 35% duty.

  | LED | Means |
  |---|---|
  | Quick pulses, 100 ms | Loading: the board is booting, or the PC is on but its stats have not arrived yet |
  | Solid | Board and PC online |
  | Quick pulses, 200 ms | PC restarting or updating |
  | Slow breathing (3 s fade in and out) | PC asleep or off |

### Commands

One per line on the USB CDC console or the CH343 UART:

| Command | Does |
|---|---|
| `status`, `perf`, `pcstats [text]` | Board state; per-core CPU and bus load (run twice); stats received from the PC |
| `view <1\|2\|3> <pc\|media\|ai\|gif\|dash\|status\|stats\|metrics>` | What a panel shows (saved) |
| `face <1\|2\|3> <auto\|pc\|gif\|restart\|update\|media\|weather\|ai\|localai\|clock>` | Force one face, for testing and screenshots (not saved) |
| `shot <1\|2\|3>` | Screenshot of a panel, streamed as base64 on the CDC port |
| `pcsim <on\|off\|auto>` | Pretend the PC is on or off (LED, faces, backlights), for testing |
| `door <1-4> [ms]`, `reset` | Press a door button; pulse the motherboard reset |
| `wake [force \| mac <AA:BB:..>]`, `power <sleep\|hibernate\|restart\|shutdown>` | Wake-on-LAN; power commands over USB |
| `going <restart\|shutdown\|update_restart\|update_shutdown\|sleep\|clear>` | Tell the board what the PC is about to do |
| `bl <1\|2\|3\|all> <0-100\|on\|off>`, `led <auto\|on\|off\|sweep\|0-100>` | Backlights; status LED |
| `fps [budget <20-95> \| cap <1\|2\|3> <1-60\|off>]` | SPI budget and per-panel frame caps |
| `layout <show\|reload\|default\|...>`, `weather <show\|demo [rain\|typhoon]\|clear\|storms\|home <lat>,<lon>>` | Dashboard layouts; weather |

### Build and flash

```bash
firmware/carrier_idf/idf.cmd build
python scripts/idf_flash.py --no-build
```

`idf_flash.py` stops the PC stats service, reboots the board into the ROM
with a 1200-baud touch, flashes over the ROM's USB-Serial-JTAG port and
starts the service again. `--idf coredump-info` runs any other `idf.py`
action on that port instead. Wi-Fi, Discord and OTA settings go in
`main/secrets.h` (copy `secrets_example.h`; the real file is gitignored).

The Discord bot uses up to four channels, each set by its channel id in
`secrets.h` and off when empty:

| Key | Channel | Posts |
|---|---|---|
| `DISCORD_CH_PC` | #pc-announcement | Board online, PC on/off, restart and update, power keys, door, reset |
| `DISCORD_CH_WEATHER` | #weather-announcement | Rain and typhoon warnings, "cleared" posts, phone rain alerts, a weather update every 2 h from 06:00 to 20:00 |
| `DISCORD_CH_LOG` | #logging | The board's log, timestamped, in batches every 10 s |
| `DISCORD_CH_CONTROL` | #control | One control panel message with the `/actions` buttons, kept up to date |

PC and weather posts fall back to `DISCORD_CHANNEL_ID` when their own key is
empty. Only `DISCORD_OWNER_ID` can use the buttons.

`WAKE_DIM_BACKLIGHTS` (secrets.h, default false) is for motherboards that drag
the USB 5 V rail down while the PC powers on from shutdown: the board browns
out and restarts (`boot: last reset: brownout` in the log). With it on, a wake
turns the backlights off while the WoL / USB wake goes out, for
`WAKE_DIM_MS` after it (default 100 ms; 500 and 100 both tested fine). Seen on
the author's board, both with Wake PC and with the PC's own power button;
waking from sleep is not affected. Two of two shutdown-then-wake tests passed
with it on.

Status: not reproducible. Since 2026-10-26 the brownout no longer happens on
the author's board, with the setting on or off, by Wake PC or by the PC's
power button. The firmware changes in between don't touch what the board draws
at power-on, so the cause is unknown. At power-on the status LED's slow blink
now stalls for a few milliseconds and carries on, with no reset. A dip that
no longer reaches the brownout threshold would look like that. The settings
stay in as a safety measure.

It does not cover a power-on from the PC's own power button, which never goes
through the board. The real fix is a supply that the PC's power-on cannot pull
down. The ESP32 board's second USB port is not that supply: its 5 V in and out
are bridged, so feeding 5 V there while the PC's USB also powers the board would
damage it. What would work is an 18650 cell with a 5 V boost converter feeding
the board, plus some way to charge the cell; not worth it on the author's setup,
but an option if your board browns out on every power-on.
Once Wi-Fi is up, `python scripts/ota_upload.py` flashes over the network.

### Screenshots

```bash
python scripts/panel_shot.py 2 media.png --scale 2
python scripts/readme_shots.py docs/images/screens.png
```

`panel_shot.py` saves one panel. `readme_shots.py` builds the image at the
top: it stops the stats service, feeds the board a made-up stats frame,
forces every face in turn, captures it and tiles the results (run it from an
elevated shell).

## PC side

`host/pc_stats` builds two programs with the .NET Framework 4 compiler that
ships with Windows:

- **`esp32-pcstats.exe`** (Windows service): reads sensors through
  LibreHardwareMonitor and Windows performance counters, polls the local AI
  status hub and Claude's usage, and streams everything to the board over a
  vendor HID interface (no driver; the COM port stays free). It also renders
  non-Latin text for the board and sends cover art.
- **`esp32-agyquota.exe`** (per-user helper, a scheduled task): media info
  from Windows' media session (Spotify, browsers), Antigravity quota, and
  the Claude Code login refresh.

Details worth knowing:
- **CPU load** uses `% Processor Time`. `% Processor Utility` (what Task
  Manager shows) is scaled by clock speed and goes past 100% when the CPU
  boosts.
- **GPU load** is averaged over ~2 s on the board. At idle clocks the
  RTX 3050 reports bursty utilisation: it renders the desktop and wallpaper
  at 210-315 MHz, so the raw value jumps between 0 and 50%.
- **Claude usage** comes from the same endpoint Claude Code uses. The
  helper refreshes the Claude Code login shortly before it expires and
  writes the new token back to `~/.claude/.credentials.json`, so the card
  keeps working when Claude Code is not running.
- **Antigravity quota:** the 5-hour Flash window comes from Google's API
  with agy's stored login. The weekly Gemini window is only shown in agy's
  own `/usage` panel, so every 10 minutes the helper runs `agy` in a pseudo
  console, types `/usage` and reads the result. It then removes the lines
  this adds to agy's prompt history.
- **Windows updates:** the service sends `pc.update=1` while Windows is
  installing updates with nobody signed in (a servicing worker such as
  `TiWorker` running and no `explorer`): the "Working on updates" screen
  before the restart and "Updates are underway" after it. The board keeps
  its Updating faces until that goes back to 0 and the PC is back.
- Tokens are never logged.

See [host/pc_stats/README.md](host/pc_stats/README.md) for `stats.ini`,
install and command-line options.

## Bring-up findings (2026-10, assembled rev 3.1 board)

- **SPI runs at 40 MHz.** 80 MHz gives blank panels on the carrier's long
  shared bus, even with full drive strength on SCK/MOSI/DC. The S3's SPI
  clock is 80 MHz divided by a whole number, so 60 MHz is not possible: the
  driver rounds it down to 40.
- **Bus budget:** a full 240x291 frame takes 29 ms as RGB565 at 40 MHz, or
  about 21 ms as RGB444. That is enough for one full-motion panel at about
  30 fps. More full-motion panels share the bus, which is why the faces
  rely on dirty rectangles.
- **Broadcast mode** (all CS lines held LOW, one frame sent to all three
  panels at once) plays the same GIF on all three at its native 29.7 fps.
  It is only useful when all three show identical content.
- **Panel init:** do not issue a software reset (`esp_lcd_panel_reset`
  without an RST pin). The ST7789 ignores the wake-up command for 120 ms
  after a reset, so the panel stays asleep: backlight on, no picture. The
  firmware uses the hardware RST lines instead.
- **Backlight:** the GMT024 panels are dim even at 100% PWM, because of the
  module's own backlight parts. Firmware can't change that.
- **Opto resistors:** the 270R opto resistors are required. Without them
  the LED still lights, but the GPIO runs at its current limit.
- **ROM download mode drives GPIO38-40**, which pressed the keyfob during
  flashing. Fixed with an eFuse, see
  [above](#keyfob-presses-during-flashing-efuse-fix).
- **Main task stack:** 16 KB. With 8 KB, drawing the 128 px clock digits
  overflowed the stack and rebooted the board.

## Building the PCB

Toolchain: [atopile](https://atopile.io) 0.2.x, KiCad 10 (its bundled
Python provides `pcbnew`), Freerouting, and Python with pymupdf + Pillow for
the renders. The full rebuild, from `ato build` to DRC, renders and Gerbers,
is listed step by step in [scripts/README.md](scripts/README.md). Renders
land in `pcb/build/renders/` (not tracked); the image above is a copy.

## Checks

CI ([.github/workflows/ci.yml](.github/workflows/ci.yml)) runs
`scripts/check_pins.py`, lints the scripts and builds `carrier_main` on
every push. It does not build `carrier_idf` yet.

## Status

Rev 3.1 is fabricated and assembled, and `carrier_idf` runs on it:
- all three panels and every face shown above;
- PC power detection over the module's own USB link, the motherboard RST
  output (checked with an LED on J6) and the door/reset commands;
- the stats link, media with cover art, PC-rendered text, Claude and
  Antigravity quota;
- the eFuse fix: no more keyfob presses while flashing.

Still to do:
- fill `secrets.h` (Wi-Fi, Discord, OTA), then test Wake-on-LAN, the
  Discord bot, OTA and live weather (`weather home <lat>,<lon>`);
- test the USB power commands and door buttons B1-B4 on the real PC and
  keyfob;
- build `carrier_idf` in CI and retire `carrier_main`.
