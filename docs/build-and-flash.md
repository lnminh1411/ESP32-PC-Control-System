# Build and flash

The firmware needs ESP-IDF 5.5.6 (`firmware/idf.cmd` expects it in
`C:\Espressif\frameworks\esp-idf-v5.5.6`) and a `firmware/main/settings.h`
([settings.md](settings.md)). The PC helper is built and installed
separately ([helper.md](helper.md)); the PCB build chain is in
[hardware.md](hardware.md#building-the-pcb).

## Firmware

```bash
firmware/idf.cmd build
python scripts/idf_flash.py --no-build
```

`idf_flash.py` stops the PC stats service, reboots the board into the ROM
with a 1200-baud touch on the app's CDC port (`--port`, default COM12),
flashes over the ROM's USB-Serial-JTAG port and starts the service again.
`--idf coredump-info` runs any other `idf.py` action on that port instead.
It needs pyserial: run it with ESP-IDF's own Python
(`C:\Espressif\python_env\idf5.5_py3.12_env\Scripts\python.exe`).

## Build details

The build compiles only the ESP-IDF components `main` needs (`MINIMAL_BUILD`).
`idf.cmd` caches what ESP-IDF's `export.bat` sets in
`%LOCALAPPDATA%\esp-idf-env\` (it saves about 3.5 s per call); delete that folder
after reinstalling or moving ESP-IDF.

## Screenshots

```bash
python scripts/panel_shot.py 2 media.png --scale 2
python scripts/readme_shots.py docs/images/screens.png
```

`panel_shot.py` saves one panel. `readme_shots.py` builds
[images/screens.png](images/screens.png): it stops the stats service, feeds the board a made-up stats frame,
forces every face in turn, captures it and tiles the results (run it from an
elevated shell). `--real-media` keeps the track the PC is playing, with its real
cover art, in place of the made-up one.

## Changing the GIFs

The GIFs are part of the firmware. Their sources are in `firmware/gifs/`, and
`firmware/gifs/gifs.json` lists them in play order (`every` keeps every Nth
frame, up to 8 GIFs and 512 frames in all; the 10 MB app slot is the real
limit, about 19 KB a frame). After changing either, regenerate the pack and
flash:

```bash
python scripts/gif_pack.py
```

It writes `firmware/main/gifs.bin` (committed) and prints the pack size, the
size of the frames once inflated and the colour error of each palette. The
board keeps the frames compressed in flash and inflates each as it plays,
about 15 ms of core 1 a frame.
`GIF_SWITCH_S` sets how long each GIF plays.

The two `*_20fps.gif` files are made from the originals kept beside them with
the RIFE models from FluidFrames, which only give the midpoint of two frames:

- `evil_neuro_20fps.gif` is all 10 frames of `evil_neuro.gif` plus a RIFE Lite
  midpoint after each, last-to-first pair included.
- `neuro_20fps.gif` is every third frame of `neuro.gif` (33 frames at 30 fps)
  plus the RIFE midpoint of the two real frames between them. In the source
  the tail jumps sides from one frame to the next, and RIFE cuts it there, so
  output frames 5, 9, 13 and 15 use the later real frame instead.

## Other uploads

| Script | Does |
|---|---|
| `python scripts/layout_upload.py FILE.json [--port COMn \| --host <name>.local]` | Checks a dashboard layout and uploads it over the console or HTTP `PUT /layout` ([layouts.md](layouts.md)) |
| `python scripts/make_vlw.py FONT.otf SIZE NAME OUT.cpp` | Turns a font into a LovyanGFX VLW array for `firmware/main/font_*.cpp` |
| `python scripts/check_pins.py` | Checks the `PIN_*` settings against `pcb/main.ato` |
| `python scripts/settings_doc.py` | Regenerates [settings-reference.md](settings-reference.md) |

Every script, the PCB ones included: [scripts/README.md](../scripts/README.md)
and [Scripts.md](../Scripts.md).

## Checks

CI ([.github/workflows/ci.yml](../.github/workflows/ci.yml)) runs
`scripts/check_pins.py` and lints the scripts with pyflakes on every push and
pull request. The ESP-IDF firmware is not built in CI yet.
