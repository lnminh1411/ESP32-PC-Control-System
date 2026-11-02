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
after reinstalling or moving ESP-IDF. ESP-IDF itself carries local patches,
listed in [firmware/idf_patches](../firmware/idf_patches/README.md); apply
them again after a reinstall.

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

The GIFs have their own 6 MB `gifs` partition. Their sources are in
`firmware/gifs/`, and `firmware/gifs/gifs.json` lists them (`every` keeps
every Nth frame, up to 12 GIFs and 512 frames in all; the partition is the
real limit, about 13 KB a frame). The board plays them in random cycles:
it keeps a list of what played, so every GIF comes once a cycle, and a GIF stays
away for at least half the GIFs when a new cycle starts. After changing either, regenerate the
pack and flash with `idf_flash.py` (`idf.py flash` writes the partition,
`app-flash` does not):

```bash
python scripts/gif_pack.py
```

It needs numpy, Pillow and lz4 (`pip install lz4`). It writes
`firmware/main/gifs.bin` (committed) and prints the pack size, the size of
the frames once decoded, how much of the screen their rects cover and the
colour error of each palette. The board keeps the frames LZ4-compressed in
flash and decodes each as it plays, 13-15 ms of core 1 a frame.
`GIF_SWITCH_S` sets how long each GIF plays.

`flatten` in `gifs.json` (levels of 15, default 0) takes the grain off a GIF's
backdrop: colours that near its commonest one become it, where they touch it from
the top or sides. Check the result when raising it: pale skin or hair at a panel
edge can go with the backdrop, which is why Filian and Herta stay at 0 and Miku at 1.

`lossy` in `gifs.json` (levels of 15, default 0) lets a pixel be that far off when
the frame before can supply it, which is what keeps the frames this small: every GIF
is at 1. The packer searches 8x8 blocks for the best shift, about a second a frame,
so a full pack takes a few minutes.

The `*_20fps.gif` files are made from the originals kept beside them with the
RIFE models from FluidFrames, which only give the midpoint of two frames:

- `evil_neuro_20fps.gif` is all 10 frames of `evil_neuro.gif` plus a RIFE Lite
  midpoint after each, last-to-first pair included.
- `neuro_20fps.gif` is every third frame of `neuro.gif` (33 frames at 30 fps)
  plus a RIFE midpoint after each, last-to-first pair included. In the source
  the tail jumps sides from one frame to the next, so RIFE cuts it in a few
  frames (9 and 15 show it most); the motion is smoother this way than with
  real frames in their place.
- `firefly_20fps.gif` and `miku_20fps.gif` are every frame of the 10 fps
  original plus a RIFE midpoint after each, last-to-first pair included, laid
  out at 240x320. Firefly is on white at 0.75 scale. Miku fills the width with
  her top kept, so her legs run off the bottom.
- `filian_20fps.gif` is 8 frames at quarter steps through the 6-frame original,
  at 1.44 scale, cut so about a third of her pointing arm stays on screen.
- `kita_20fps.gif` is the first 12 frames of `kita.gif` (the rest repeats them)
  brought to 16 at 20 fps with RIFE, sized to 320x320 with the left 240 columns
  kept, so her arm runs off the right edge.
- `teto_20fps.gif` is frames 179-241 of `teto.gif`, a closing loop, brought from
  63 frames to 50 with RIFE. The side bars are cut and she fills the width with
  her top kept.
- `herta_20fps.gif` is the 8 frames of `herta.gif` plus a RIFE midpoint after
  each, last-to-first pair included, 0.8 s a loop. She fills the height on white.
- `bocchi_20fps.gif` is the 6 poses of one 433 ms cycle of `bocchi.gif` brought to
  9 frames with RIFE, filling the height, about a third of her arm on screen.
- `miku_player_20fps.gif` is 26522132 on Tenor sized to 240x320, already 20 fps; its
  11.8 MB original is not kept here.
- `teto_pppp_20fps.gif` is the 28 frames of `teto_pppp.gif` at their own 80 and 160
  ms, with RIFE in-betweens on the 50 ms grid, 5% past filling the height. The crop
  follows the figure, eased over the loop.

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
