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
displays, turns the PC on and off, presses the motherboard's reset line and
works a 4-button door remote, all from one ESP32-S3-N16R8 module on a custom
2-layer PCB. A Windows helper feeds it the PC's stats and carries its
commands back to the PC.

</td></tr></table>

![All screen faces, captured from the board](docs/images/screens.png)

*Every face, captured from the board with `shot`. The stats are made up; the
layout is real. What each panel shows and when: [docs/screens.md](docs/screens.md).*

## What it does

- **Three status panels:** PC stats, media with cover art, Claude and
  Antigravity quota and the local AI while the PC is on; a GIF, the weather
  and a clock while it is off.
- **Power control:** Wake-on-LAN and USB wake, sleep / hibernate / restart /
  shutdown / lock through PcHwCtl on the PC, and a hardware reset for a hung PC.
- **Discord bot:** a control panel for the door, the PC, its RGB, fans,
  monitors and local AI agent, and the board itself; PC, weather and log
  channels, copied to Pushover.
- **Weather:** rain nowcast and warnings (Drizzle to Typhoon) that follow
  your phone through OwnTracks, OpenWeather minute rain and GDACS typhoons.
- **Night mode:** dims everything, parks background processes on E-cores,
  sleeps the PC after an idle hour at night and hibernates it when you leave
  home.
- **Door remote:** four opto-isolated keyfob buttons.

## Parts

- **Board:** rev 3.1 carrier, 45.7 x 138.2 mm, 3 ST7789 panels on one SPI
  bus, 5 optocouplers, a 12 V boost ([docs/hardware.md](docs/hardware.md)).
- **Firmware:** ESP-IDF 5.5.6 with LovyanGFX, in [`firmware/`](firmware/)
  ([docs/firmware.md](docs/firmware.md)).
- **PC helper:** `ESP32-Helper.exe`, a .NET 10 Windows service with a
  user-session half, in [`host/pc_stats/`](host/pc_stats/)
  ([docs/helper.md](docs/helper.md)).
- **PcHwCtl:** a separate repo on the PC that owns sensors, power actions,
  RGB, fans and monitors; the helper talks to it.
- **Case:** a 3D-printed block, lid and display mask in
  [`enclosure/`](enclosure/).

## Quick start

1. Copy `firmware/main/settings_example.h` to `settings.h` and fill in the
   secrets ([docs/settings.md](docs/settings.md)).
2. Build and flash over USB ([docs/build-and-flash.md](docs/build-and-flash.md)):

   ```bash
   firmware/idf.cmd build
   python scripts/idf_flash.py --no-build
   ```

3. Build and install the PC helper from an elevated shell
   ([docs/helper.md](docs/helper.md#build-and-install)):

   ```bash
   host/pc_stats/build.cmd
   powershell -ExecutionPolicy Bypass -File host/pc_stats/install.ps1
   ```

4. Set the BIOS and Windows up for Wake-on-LAN
   ([docs/power-and-night.md](docs/power-and-night.md)), then set a home with
   `weather home <lat>,<lon>` on the console ([docs/console.md](docs/console.md)).

## Documentation

| Page | Covers |
|---|---|
| [screens.md](docs/screens.md) | The faces, warnings on them, PC usage warnings |
| [hardware.md](docs/hardware.md) | The board, pin map, assembly, the eFuse fix, bring-up findings, building the PCB |
| [firmware.md](docs/firmware.md) | Modules, display pipeline, tasks, ESP-IDF options, PSRAM hardening |
| [console.md](docs/console.md) | Every console command |
| [power-and-night.md](docs/power-and-night.md) | Power control, Wake-on-LAN setup, night and low power mode, the power-on brownout |
| [discord.md](docs/discord.md) | Channels, the control panel, Pushover, screenshots |
| [weather.md](docs/weather.md) | Weather sources, nowcast, warnings, OwnTracks, Pushover, call budgets |
| [helper.md](docs/helper.md) | The PC helper; full reference in [host/pc_stats/README.md](host/pc_stats/README.md) |
| [settings.md](docs/settings.md) | `settings.h`, the secrets, and [every other key](docs/settings-reference.md) |
| [build-and-flash.md](docs/build-and-flash.md) | Building, flashing, uploads, screenshots, CI |
| [layouts.md](docs/layouts.md) | JSON dashboard layouts |
| [benchmark.md](docs/benchmark.md) | Memory and speed measurements |
| [pc-power-detection.md](docs/pc-power-detection.md), [recovery-and-physical-access.md](docs/recovery-and-physical-access.md) | Design notes |

## Repository layout

| Path | Contents |
|---|---|
| [`firmware/`](firmware/) | The board's firmware |
| [`host/pc_stats/`](host/pc_stats/) | The PC helper ([README](host/pc_stats/README.md)) |
| [`pcb/`](pcb/) | atopile netlist, KiCad board, footprints ([README](pcb/README.md)) |
| [`scripts/`](scripts/) | PCB build chain, flashing, uploads, screenshots ([README](scripts/README.md)) |
| [`enclosure/`](enclosure/) | CadQuery model of the case ([README](enclosure/README.md)) |
| [`docs/`](docs/) | The pages above |

## Status

Rev 3.1 is fabricated and assembled, and the firmware runs on it: all three
panels, PC power detection, the stats link, Wi-Fi, Wake PC, the Discord bot,
live weather with OwnTracks and Pushover, and night mode are in daily use.
Still to do: test the door buttons B1-B4 on the real keyfob, and
build the firmware in CI.
