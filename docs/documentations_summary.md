# Documentation summary

Every Markdown doc in this repo, grouped by area. For how the board, the
helper, PcHwCtl and LocalAIHost fit together across repos, see the system
design doc `E:\System Architecture Documentation\ESP32 PC Control System\ESP32-PC-Control-System.md`
(and its agent copy `ESP32-PC-Control-System.agents.md` in the same folder).
Third-party READMEs under `firmware/components/lovyangfx/` and
`firmware/vendor/esp_tinyusb/` are upstream files and are not listed.

## Overview

- [README.md](../README.md): what the project is (an ESP32-S3 carrier board
  inside the PC case plus a Windows helper), what it does (three status
  panels, power control, Discord bot, weather, night mode, door remote), the
  parts, a four-step quick start, the documentation index and the
  repository layout.
- [screens.md](screens.md): the faces each panel shows on its auto view for
  PC on, off and restarting, with the board-captured image; notes on quota
  cards, PC-rendered non-Latin text, cover art, weather warning boxes and PC
  usage warnings.
- [security.md](security.md): the 2026-11-02 audit record: risks accepted on
  purpose (writable `E:\`, no flash encryption, unauthenticated local
  console), which secrets live where (key names only), and a checklist for a
  suspected attack.
- [ideas.md](ideas.md): raw idea capture, not specs.

## Firmware

- [firmware.md](firmware.md): the firmware's modules, the display pipeline
  (dirty rectangles, RGB444 on the wire, SPI budget, two cores), other
  behaviour (door and reset optos, PC power state, brownout settle, status
  LED patterns), the task table, the ESP-IDF configuration and partition
  table, and the PSRAM hardening story (40 MHz with ECC since 2026-10-28).
- [console.md](console.md): every console command (UART, USB CDC, also
  reachable from Discord) with its arguments and effect.
- [discord.md](discord.md): the board's own Discord bot: the four channels and
  their keys, Pushover copies and priorities, the control panel's eight tabs
  and their controls, confirm prompts and refresh timing, and the screenshot
  flow (board -> helper -> PcHwCtl, webhook never on USB) with its setup.
- [power-and-night.md](power-and-night.md): power control paths (wake by USB
  and Wake-on-LAN, power actions through PcHwCtl, HID fallback, reset opto),
  BIOS/Windows prerequisites, night mode, low power mode, the guards against
  false triggers, the night pass on the PC (Process Lasso E-cores, efficiency
  mode, heavy exes), presence rules at the desk, timed sleep and leave-home
  hibernate, and the power-on brownout.
- [weather.md](weather.md): the weather engine: home and phone location
  (OwnTracks over MQTT, EMQX setup), Pushover setup, home/phone engine
  merging, the 17-point Open-Meteo nowcast, rain tiers and warnings, rain-end
  estimates, GDACS typhoon tracking, 2-hourly updates, call budgets,
  OpenWeather One Call 4.0, RainViewer radar, Tomorrow.io and the `weather`
  console commands.
- [layouts.md](layouts.md): the JSON dashboard engine (`dash` view): schema,
  widget types, fonts, common stat keys, uploading with
  `scripts/layout_upload.py` or `layout begin/data/end`, and the `layout`
  console commands.
- [pc-power-detection.md](pc-power-detection.md): how the board reads the
  PC's power state from USB bus events (TinyUSB attach/resume/suspend), the
  30 s boot guard, and how restart/update announcements from PcHwCtl (via the
  helper) latch, hold the screens and block wake.
- [benchmark.md](benchmark.md): measured board numbers (CPU, memory, flash,
  FAT, NVS, task stacks, SPI and GIF frame rates, USB and Wi-Fi throughput)
  and the dated optimisation passes behind them.
- [recovery-and-physical-access.md](recovery-and-physical-access.md): how to
  recover the board once it is sealed in the case (reflash over USB; PSU
  switch power cycle for a chip stuck in the ROM bootloader) and why no
  hardware recovery lines were added.
- [firmware/idf_patches/README.md](../firmware/idf_patches/README.md): the
  local ESP-IDF 5.5.6 and TinyUSB patches (SPI bus lock race, Xtensa LCOUNT,
  TinyUSB RX pending, ldgen dedupe), why each exists, and how to apply and
  check them after an ESP-IDF reinstall.

## PC helper

- [helper.md](helper.md): overview of `ESP32-Helper.exe`: the service and
  user-session halves and what each does, details worth knowing (CPU/GPU
  load, Claude token refresh, Antigravity quota, restart detection), build
  and install, and the trust model (`LINK_KEY` challenge, pipe server checks).
- [host/pc_stats/README.md](../host/pc_stats/README.md): the full helper
  reference: the two processes and their pipe lines, USB transports and the
  link challenge, build and install, command-line options, the `stats.ini`
  format, Claude usage keys, Antigravity quota keys, media and cover art
  (`@a`), the wire protocol and PC-rendered text (`!txt` / `@x`), power
  relay and Wake-on-LAN target, restart/update keys from PcHwCtl, and the
  PcHwCtl relays (sensor feed, monitors, presence, screenshots, night mode).

## Build, flash and scripts

- [build-and-flash.md](build-and-flash.md): building the firmware
  (`firmware/idf.cmd`, MINIMAL_BUILD, env cache), flashing over USB with
  `scripts/idf_flash.py` (changed regions only, no OTA), panel and README
  screenshots, changing the GIF pack (`gifs.json` options, per-GIF
  provenance), other upload scripts and CI.
- [scripts/README.md](../scripts/README.md): every script in `scripts/`: the
  KiCad/atopile PCB build chain step by step with its tool paths
  (`board_config.py`), plus the firmware tools (`check_pins.py`,
  `gif_pack.py`, `idf_flash.py`, `layout_upload.py`, `make_vlw.py`,
  `panel_shot.py`, `readme_shots.py`, `settings_doc.py`).
- [Scripts.md](../Scripts.md): a short index of the same scripts, a few
  lines each, with usage and path.

## Settings

- [settings.md](settings.md): the single `firmware/main/settings.h` file
  (template `settings_example.h`, defaults in `settings_or_example.h`), how
  the helper's `HOST_*` keys become C# constants, which `HOST_*` keys are
  live, and the table of secret key names.
- [settings-reference.md](settings-reference.md): generated by
  `scripts/settings_doc.py`: every non-secret key with its default and note,
  one table per section of `settings_or_example.h`.

## Hardware

- [hardware.md](hardware.md): the rev 3.1 carrier board: what each part
  does, the pin map, assembly notes, the `DIS_USB_OTG_DOWNLOAD_MODE` eFuse fix
  for keyfob presses during flashing, bring-up findings (SPI at 40 MHz, panel
  init, backlight, main stack), and a pointer to the PCB build chain.
- [pcb/README.md](../pcb/README.md): the atopile netlist and KiCad board:
  how to build, board geometry and layout decisions, part choices, and the
  opto GPIO rules the firmware follows.
- [enclosure/README.md](../enclosure/README.md): the 3D-printed case
  (CadQuery): block, lid and mask with sizes, print settings, joints, glass
  clearance and dimensions.
