# Scripts

Two kinds of script live here:

- **`pcbnew` scripts** (the board build chain) run under KiCad's own bundled
  Python, the only one with the `pcbnew` module. Below, `<kicad-python>`
  means that interpreter, e.g. `"C:\Program Files\KiCad\10.0\bin\python.exe"`.
- **Plain-Python scripts** (`check_pins.py`, `gif_pack.py`,
  `make_fit_models.py`, `render_board.py`, `export_gerbers.py`, and the
  firmware tools under [Other tools](#other-tools)) run under the system
  Python. `idf_flash.py` runs under ESP-IDF's Python (it needs pyserial).

Every script finds the board relative to its own location, so running the
chain from a git worktree only touches that worktree's board.

## Paths (board_config.py)

Shared settings: `REVISION`, `DATE`, `TITLE` (title block and fab file
names), the Power net class values (`POWER_NETS`, track/via sizes, keep
equal to `pcb/layouts/default/default.kicad_pro`), and the tool paths.
Paths default to the original Windows setup; override them with these
environment variables:

| Variable | Default | Used for |
|---|---|---|
| `KICAD_DIR` | `C:\Program Files\KiCad\10.0` | `kicad-cli` (`bin/`) |
| `KICAD_FOOTPRINTS` | `$KICAD_DIR/share/kicad/footprints` | Stock footprint libraries |
| `FREEROUTING_JAR` | `K:\cache\freerouting\freerouting-2.4.1.jar` | `route_board.py` |
| `PCB_SCRATCH` | `K:\scratch` | Render intermediates |
| `PCB_ARTIFACTS` | `K:\artifacts` | Gerber packages |

## Full rebuild

From the repo root; each step needs the previous one's output:

```bash
cd pcb && PYTHONIOENCODING=utf-8 ato build && cd ..
<kicad-python> scripts/make_blank_pcb.py
<kicad-python> scripts/sync_netlist_to_board.py
<kicad-python> scripts/layout_board.py
<kicad-python> scripts/add_pours.py
<kicad-python> scripts/route_spi.py
<kicad-python> scripts/route_board.py
<kicad-python> scripts/chamfer_corners.py
<kicad-python> scripts/check_spi_topology.py
kicad-cli pcb drc --severity-all -o drc.rpt pcb/layouts/default/default.kicad_pcb
python scripts/render_board.py
python scripts/export_gerbers.py
```

`layout_board.py` and `route_board.py` must run on a fresh board, so after
any layout change rebuild from `make_blank_pcb.py`, not from the middle.

## The chain

### make_blank_pcb.py

Creates a blank 2-layer `pcb/layouts/default/default.kicad_pcb` with the
title block from `board_config.py`. **Overwrites unconditionally**: run it
only to start the layout over.

### sync_netlist_to_board.py

Puts `pcb/build/default.net` (from `ato build`) onto the board: loads every
component's footprint and assigns every pad to its net, using only the
`pcbnew` API. Neither atopile, `kicad-cli` nor KiCad's GUI tooling can
import a netlist headless, which is why this exists. Run it after every
`ato build` that changes parts or nets.

- **Idempotent**: existing footprints keep their positions (only nets are
  refreshed); a new component is grid-placed in open space; one removed
  from `main.ato` is left on the board with a warning, not deleted.
- Custom footprints live in `pcb/footprints/ESP32PCC.pretty` (lib name
  `ESP32PCC`, mapped via `LOCAL_LIBS`); everything else comes from KiCad's
  stock library. Part values (270R/10k/33R) are set from instance names via
  `INSTANCE_VALUES`, since atopile 0.2 drops `value`.
- **Board references come from atopile instance names** (`REF_BY_INSTANCE`):
  `mcu.left` = J1, `mcu.right` = J2, `display1-3` = J3-J5, `rst_out` = J6,
  `keyfob` = J7, `boost` = PS1, `optoN` = UN, `rN` = RN, `led1` = D1. A new
  instance with no mapping stops the script: add it to `REF_BY_INSTANCE`.

### layout_board.py

Places every footprint from measured geometry (courtyard boxes read live via
`pcbnew`), draws the L-shaped outline and mounting holes, and adds the
silkscreen: per-pin connector labels, module end markers, resistor values
inside the bodies, the PC817 pin-1 dots. Also fixes the axial resistor pad
pitch to the measured 8.128 mm (stock part: 10.16 mm) and stretches the
P7.62 3D model to match.

- Placement constants (`XL`, `XR`, `XD`, `PITCH`, `C1`, the 33R pad
  positions) are shared with `route_spi.py`: change both together.
- **Not idempotent in place**: re-running it on its own output corrupts
  live `pcbnew` SWIG references. Rebuild from `make_blank_pcb.py`.
- Never `fp.Remove()` footprint graphics here; it corrupts later
  footprints' SWIG wrappers. Move items to another layer instead.

### add_pours.py

Pours GND on B.Cu (0.3 mm pad clearance) and adds the antenna keepout rule
area (all copper layers, no tracks/vias/fill) under the module's antenna.
The keepout must exist before routing.

### route_spi.py

Hand-routes **every net** as locked tracks: the display bus as a true daisy
chain MCU -> display 1 -> 2 -> 3 on F.Cu (no T-junctions, no vias), CS/RST
point-to-point, BL1-3, display/boost 5V, the 12V bus inside the opto
corridor, the opto outputs + LED down the west channel, and the resistor
bank. All F.Cu except 5 locked short B.Cu jumpers. Freerouting would build
trees with mid-trace T-joins and drop most of the bus onto B.Cu.

### route_board.py

Safety net: auto-routes anything `route_spi.py` left, with
[Freerouting](https://github.com/freerouting/freerouting) (not committed;
path in `FREEROUTING_JAR`). Exports Specctra DSN, routes in two passes
(F.Cu only and locked, then F.Cu + B.Cu so leftovers come out as short
jumpers), imports the SES and re-fills the zones. Freerouting may print a
few phantom "unrouted" items; trust KiCad's DRC. **Only run on a fresh
board**: routing on top of its own output left a net unrouted.

### chamfer_corners.py

Replaces every 90° (and sharper) track corner with a 45° diagonal: both
legs cut back 0.5 mm, each diagonal collision-checked against other-net
copper and halved down to 0.12 mm if needed. Corners on a pad or via are
skipped. Freerouting's nm-long bend segments are reused as the diagonal
rather than deleted (`board.Remove()` corrupts other SWIG wrappers).

### check_spi_topology.py

Verifies the display bus after routing: F.Cu only, no vias, no node with
3+ track ends, and the chain order MCU -> display 1 -> 2 -> 3 per shared
net. Run after routing or any manual edit to those nets.

### render_board.py

Refreshes `pcb/build/renders/`: F.Cu, B.Cu, the top silkscreen over F.Cu
and a slanted 3D overview (also copied to `pcb/build/board_preview.png`),
each with the board measurements in mm/cm/in. Needs pymupdf + Pillow.

### export_gerbers.py

Writes the fab package to `<PCB_ARTIFACTS>/<TITLE>_rev<REVISION>_gerbers/`
plus a `.zip`: Gerbers for copper, mask, paste, silkscreen and Edge.Cuts,
Excellon drills split PTH/NPTH with X2 drill maps, and the `.gbrjob`, all
renamed from KiCad's `default-*` to the title. Plots the zone fill stored
in the board (the one DRC checked). Clears the output folder on every run.

### make_fit_models.py

Writes VRML fit-check models into `pcb/footprints/ESP32PCC.3dshapes/`: the
BS01 boost on its 4 pins, and an envelope of the plugged-in DevKit at its
real height, so renders show what has to fit under the module. Re-run after
changing `BOOST_H`.

## Other tools

### check_pins.py

Checks the firmware pin map (the `PIN_*` settings, read the way the build
reads them: `settings.h`, else `settings_example.h`, over the defaults in
`settings_or_example.h`) against `pcb/main.ato`: every `PIN_*` must be the
GPIO wired to its part pin, directly or through one series resistor. Run after changing either file;
CI runs it on every push.

```bash
python scripts/check_pins.py
```

### gif_pack.py

Builds `firmware/main/gifs.bin`, the GIF pack for the board's `gifs` partition, from
the GIFs listed in `firmware/gifs/gifs.json`: frames scaled to 240x320, one
palette of up to 256 RGB444 colours per GIF, and only each frame's dirty rects
stored (the cheapest cover of its changed 8-row bands by at most 8 rects).
Frames after the first are predicted from shifted 8x8 blocks of the previous
frame (`lossy` in `gifs.json` sets the colour error allowed, `flatten` the
backdrop flattening, `every` keeps every Nth frame), then LZ4-HC compressed.
It replays every frame to check the decode and refuses more than 12 GIFs,
512 frames or 6 MB. Format in `firmware/main/gif_store.h`. Needs numpy,
Pillow and lz4. `idf_flash.py` writes the pack to the `gifs` partition when
it changed.

```bash
python scripts/gif_pack.py
```

### idf_flash.py

Builds the firmware (`firmware/idf.cmd build`) and flashes it over the native
USB port, hands-off. It stops the `ESP32PcStats` service (it opens any
Espressif COM port as its serial fallback; elevation is asked only when the
service is not already stopped), sends a 1200-baud touch to the app's CDC
port so the firmware reboots into the ROM downloader, waits up to 20 s for
the ROM's USB-Serial-JTAG port, and starts the service again at the end.
Only the images whose MD5 on the board differs (bootloader, partition table,
app, GIF pack, from `build/flasher_args.json`) are written; nothing changed
means just a reset. This is the only firmware update path; there is no OTA.

```bash
python scripts/idf_flash.py [--port COM12] [--no-build] [--full] [--idf coredump-info]
```

`--full` writes everything (`idf.py flash`); `--idf <action>` runs that
`idf.py` action on the ROM port instead of flashing.

### layout_upload.py

Checks a `dash` layout JSON (16 KB at most) and uploads it without a reflash:
over the USB console with `--port` (`layout begin`, `layout data <base64
chunk>`, `layout end`), or over HTTP with `--host` (`PUT /layout`, header
`X-OTA-Pass` from the `OTA_PASS` environment variable, else read from
`settings.h`). See [docs/layouts.md](../docs/layouts.md).

### make_vlw.py

Converts a TTF/OTF font to a LovyanGFX smooth (anti-aliased) VLW font and
writes it as a C++ byte array (`font_*.cpp`). Default charset: printable
ASCII plus the degree and micro signs; `--extra` / `--chars` add or pick
glyphs (Basic Multilingual Plane only).

```bash
python scripts/make_vlw.py FONT.otf SIZE NAME OUT.cpp [--extra U+00B0,U+F2C9] [--chars "0123456789:"]
```

### panel_shot.py

Saves what one panel shows as a PNG: sends `shot <n>` on the board's CDC
console and decodes the RGB565 canvas streamed back. GIF faces come back
with the current frame under the overlay.

```bash
python scripts/panel_shot.py 1 out.png [--port COM12] [--scale 2]
```

### readme_shots.py

Captures every face for `docs/images/screens.png`. Needs an elevated shell:
it stops the helper service, feeds the board made-up stats (the same
`@s`/`@t`/`@e` lines, plus generated cover art and a GDI-rendered title as
`@a`/`@x`), forces each face with `face <n> <name>`, grabs it with
`shot <n>`, then puts the faces back on auto and starts the service.
`--real-media` keeps the track the PC is playing.

### settings_doc.py

Regenerates [docs/settings-reference.md](../docs/settings-reference.md)
from `firmware/main/settings_or_example.h` only (never `settings.h`),
skipping its secrets section. Run it after adding or changing a setting.

```bash
python scripts/settings_doc.py
```
