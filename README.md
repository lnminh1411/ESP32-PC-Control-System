# ESP32 PC Control System

An ESP32-S3 carrier board that lives inside a PC case. It drives three 2.4"
displays, presses the motherboard's hardware-reset line, and works a 4-button
door remote, all from one ESP32-S3-N16R8 module plugged into a custom 2-layer
PCB.

## Why would I put myself through the suffering

I needed my PC online at all time, because sometime at school or work I need to remote
control the PC back at home. And as I measured, the idle time alone costs me ~40KWh (~7usd) per month.
So, I built this for 30usd instead of buying a boring and expensive IP KVM, while adding a tons more
features to fit my over-engineering stuffs hobby.

![Carrier PCB, rev 3.1: top silkscreen over F.Cu](docs/images/pcb_silkscreen_top.png)

*Carrier PCB rev 3.1, top silkscreen over the top copper layer:
45.7 x 138.2 mm, 2 layers, DRC 0/0.*

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
| Power on from shutdown | Wake-on-LAN magic packet on the home network |
| Shutdown / sleep / hibernate / restart | Over the module's USB-OTG link to the PC |
| Hard reset of a hung PC | RST opto (`reset` command) |
| Power cut | BIOS "Always Power On after AC loss" boots the PC when power returns |

What the PC needs for this:
- BIOS: Wake-on-LAN enabled, "Restore on AC power loss" = Power On.
- Windows: Fast Startup off (it can stop WoL from working after a shutdown), and the network
  adapter's "Wake on Magic Packet" enabled. Updates sometimes reset these.
- Wired Ethernet: most WiFi adapters can't wake from shutdown.

`carrier_main` in this repo covers the reset pulse and the USB power-state
sensing. WoL and the USB power commands are not in its code yet.

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
boots or flashes. The netlist ([pcb/main.ato](pcb/main.ato)) is the source of
truth; the firmware's copy is
[firmware/carrier_main/config.h](firmware/carrier_main/config.h), and
`python scripts/check_pins.py` checks that the two agree.

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

## Repository layout

| Path | Contents |
|---|---|
| [`firmware/carrier_main/`](firmware/carrier_main/) | The board's firmware (below) |
| [`host/pc_stats/`](host/pc_stats/) | Windows service that streams PC stats, local-AI status and Claude usage to the board over USB ([host/pc_stats/README.md](host/pc_stats/README.md)) |
| [`pcb/`](pcb/) | atopile netlist ([`main.ato`](pcb/main.ato)), KiCad board, custom footprints, design notes ([pcb/README.md](pcb/README.md)) |
| [`scripts/`](scripts/) | Python build chain: netlist -> placement -> hand routes -> checks -> renders -> Gerbers, plus the GIF uploader and the pin check ([scripts/README.md](scripts/README.md)) |
| [`docs/`](docs/) | Bench findings and design notes |

## Firmware

`firmware/carrier_main` is an Arduino sketch for the `esp32:esp32` core
(3.3.x), split into modules:

| Module | Does |
|---|---|
| `config.h` | Pins, timings, build switches |
| `display` | Panel init, flush task on core 0, dirty-rectangle engine, RGB444 push over SPI + DMA, backlights |
| `views` | What each panel shows: `gif`, `status` or `stats` |
| `optos` | Door button presses and the motherboard reset pulse |
| `pc_state` | PC power state from USB events, status LED (PWM) |
| `gif_store` | GIF frames pre-packed as RGB444 with per-frame dirty rects, saved to FAT, upload protocol |
| `commands` | The text command set |
| `console` | Command transports (USB CDC, UART, vendor HID) and GIF uploads |
| `pcstats` | Key/value store for the stats the host service sends |
| `net` | Wi-Fi, mDNS, OTA (off until `secrets.h` is filled in) |
| `discord` | Discord bot: `/actions` control panel, notifications |
| `ha` | Optional Home Assistant MQTT discovery |

What it does:
- **Displays:** each panel shows its own view: `gif`, `status` (PC state,
  boot-guard countdown, uptime, last action, door/RST state) or `stats` (bus
  load, per-panel fps and KB/s, memory, chip temperature). Default: GIF /
  status / stats.
- **Dirty-rectangle engine:** each panel draws into a PSRAM canvas. The
  firmware compares it with what is already on screen in 8-row bands and
  sends only the changed rectangles, so a changed number costs about 1 KB
  and a static dashboard costs nothing.
- **RGB444 on the wire** (12 bits per pixel, 25% less data than RGB565).
- **Two-core pipeline:** core 1 draws, a flush task on core 0 diffs and
  sends. The GIF is converted once at load and only its changed regions are
  sent. Measured: the GIF plays at its native 29.7 fps with the bus at 47%
  and both cores mostly idle; three GIFs at once run at 20 fps each with
  the bus at 95%. `perf` (run twice) prints per-core CPU and wire load.
- **Door buttons B1-B4** (GPIO39-42): 500 ms presses, one at a time, with a
  500 ms gap enforced in firmware (the board has no hardware interlock).
- **Motherboard reset** (GPIO1): 200 ms pulse, only on the `reset` command,
  never at boot. All opto pins are driven LOW first thing at boot.
- **PC power state** from the module's own USB link to the motherboard, with
  a 30 s guard against the brief USB dropout at power-on. When the PC sleeps
  or shuts down, the backlights turn off.
- **Status LED** (GPIO2, PWM): solid = PC on, slow blink = asleep/off,
  short blip every 2 s = no USB host yet. `led <0-100>` is perceptual
  brightness, capped at 35% duty.
- **PC stats link:** the host service in `host/pc_stats` sends stats as
  text lines over a vendor HID interface (no driver needed, the COM port
  stays free); `pcstats [text]` lists what arrived. No screen shows them yet.
- **Commands**, one per line on either USB port (the module's own USB, or
  the CH343 UART at 3 Mbaud):
  `help | status | view <1|2|3> <gif|status|stats> | door <1-4> | reset | bl <1|2|3|all> <0-100|on|off> | led <auto|on|off|sweep|0-100> | perf | pcstats [text]`.
- **GIF upload:** `python scripts/gif_stream.py <file.gif> --port <CH343 port>`.
  The GIF is saved to the FAT partition and replays on every boot.

Build + flash. The firmware needs `USBMode=default` (TinyUSB) for the USB
power events. With `CDCOnBoot=cdc`, `Serial` is the module's own USB port
and `Serial0` is the UART. `UploadMode=cdc` lets you flash through the
module's own USB port (COM12 here since the HID interface was added; it was
COM11 before):

```bash
arduino-cli lib install "Adafruit GFX Library" "ArduinoJson" "WebSockets"
arduino-cli compile --fqbn esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,USBMode=default,CDCOnBoot=cdc,UploadMode=cdc firmware/carrier_main
arduino-cli upload  --fqbn esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,USBMode=default,CDCOnBoot=cdc,UploadMode=cdc -p COM12 firmware/carrier_main
```

Bench-only switch: `-DMOBO_RST_HOLD_TEST=1` holds RST HIGH from boot, for
testing with an LED on J6. On a real motherboard that keeps the PC in
reset, so the build refuses it unless you also pass
`-DBENCH_NO_MOTHERBOARD=1`.

### Bring-up findings (2026-10, assembled rev 3.1 board)

- **SPI runs at 40 MHz.** 80 MHz gives blank panels on the carrier's long
  shared bus, even with full drive strength on SCK/MOSI/DC. The S3's SPI
  clock is 80 MHz divided by a whole number, so 60 MHz is not possible: the
  driver rounds it down to 40.
- **Bus budget:** a full 240x291 frame takes 29 ms as RGB565 at 40 MHz, or
  about 21 ms as RGB444. That is enough for one full-motion panel at about
  30 fps. More full-motion panels share the bus, which is why the
  dashboards rely on dirty rectangles.
- **Broadcast mode** (all CS lines held LOW, one frame sent to all three
  panels at once) plays the same GIF on all three at its native 29.7 fps.
  It is only useful when all three show identical content.
- **Panel init:** do not issue a software reset (`esp_lcd_panel_reset`
  without an RST pin). The ST7789 ignores the wake-up command for 120 ms
  after a reset, so the panel stays asleep: backlight on, no picture. The
  firmware uses the hardware RST lines instead.
- **Panel orientation:** MADCTL 0, which is rotation 2 in Adafruit's
  numbering.
- **Backlight:** the GMT024 panels are dim even at 100% PWM, because of the
  module's own backlight parts. Firmware can't change that.
- **Opto resistors:** the 270R opto resistors are required. Without them
  the LED still lights, but the GPIO runs at its current limit.

## Building the PCB

Toolchain: [atopile](https://atopile.io) 0.2.x, KiCad 10 (its bundled
Python provides `pcbnew`), Freerouting, and Python with pymupdf + Pillow for
the renders. The full rebuild, from `ato build` to DRC, renders and Gerbers,
is listed step by step in [scripts/README.md](scripts/README.md). Renders
land in `pcb/build/renders/` (not tracked); the image above is a copy.

## Checks

CI ([.github/workflows/ci.yml](.github/workflows/ci.yml)) builds
`carrier_main` and runs `scripts/check_pins.py` plus a lint of the scripts
on every push.

## Status

Rev 3.1 is fabricated and assembled. `carrier_main` has been tested on the
board:
- all three panels work, both in broadcast and on their own CS lines;
- the motherboard RST output was checked with an LED on J6;
- PC power detection works over the module's own USB link;
- door and reset commands respond.

Still to do:
- test door buttons B1-B4 with the real keyfob connected;
- WoL and the USB power commands in the firmware;
- write the real dashboard content (the status and stats views are test
  placeholders);
- fill `secrets.h` (Wi-Fi password, Discord bot) and test the Discord bot;
- dashboards that show the PC stats, AI status and Claude usage.
