# GIF bring-up on the bench display (2026-09-30 / 2026-10-01)

Bench-test firmware (`firmware/s3_gif_player*`), single display, pins
CS=3/DC=4/RST=5/MOSI=6/SCK=7, 80MHz SPI. Separate from, and uses a
different pin mapping than, the final 3-display PCB wiring documented in
`pcb/main.ato`/`pcb/README.md`.

## Decode/storage: 4 revisions, real profiling each step

1. Palette-indexed frames + PSRAM decode buffer + per-pixel LUT: badly
   slow (PSRAM is bad at the scattered per-pixel writes the lookup does).
2. Same, decode buffer moved to internal SRAM: still ~half the source
   GIF's natural frame rate.
3. WiFi-streamed raw RGB565 (PC pre-decodes, device just reads socket +
   pushes): even worse. UDP debug channel (serial was never reliable on
   this board) measured `avg_push_ms ~40`, `avg_read_ms ~230-260` --
   `WiFiClient`'s small socket receive buffer, not the SPI push, was the
   real bottleneck.
4. Flash-embedded, no palette, RLE-compressed RGB565, full 240x291 res,
   all 33 source frames, 80MHz SPI, no WiFi: full speed, confirmed. This
   is the current approach. Considered and rejected before building
   (checked against real frame data first, not assumed): dirty-rectangle
   diffing (93.5% of rows change every frame -- full-body bounce, not an
   idle animation, diffing saves ~7%) and always-white background removal
   as a *redraw* optimization (only 16.7% of positions are white in every
   single frame -- character's bbox is the full canvas, no dead margin).
   RLE measured 1.90-1.98x depending on how much got blackened by the
   background-removal work below.

## Background removal: 3 approaches tried, v2 kept

### v1: hand-rolled heuristic (superseded)

Threshold (`>=250` in all channels) + dilate-and-relax edge cleanup (grow
the confirmed-background mask a few px, catch near-white within that
ring too, to kill the anti-aliased halo a plain threshold leaves) +
connected-component size filter (kill small isolated near-white blobs
--background peeking through gaps between hair strands-- regardless of
distance from the main background, since skin tone itself samples at
`(243,215,216)`, above any threshold low enough to catch those gaps, so
the size cutoff is what keeps it from eating the face).

Found and fixed two real bugs this approach surfaced:
- Blackening background *after* the LANCZOS resize left a visible stray
  gray line (~RGB 193) at one exact row in every frame -- a pure-white
  source row blending with a dark row below it during resize. Fixed by
  blackening before resize instead.
- A static Tenor/attribution watermark baked into the bottom ~22 rows of
  the source canvas isn't background-white (it's small antialiased text,
  30-50 distinct colors per row), so the threshold never touched it --
  survived as a visible stray band. Fixed with a direct row crop, not a
  threshold.

Downside: this approach kept needing another targeted patch for each new
artifact (edge halo, then interior gaps) -- heuristic, not fundamentally
correct for this content.

### v2: IS-Net-Anime (current -- reinstated after v3 below)

Replaced the whole heuristic chain with real ML segmentation:
[SkyTNT/anime-segmentation](https://github.com/SkyTNT/anime-segmentation),
`isnetis.onnx` from
[huggingface.co/skytnt/anime-seg](https://huggingface.co/skytnt/anime-seg)
(~168MB), run via `onnxruntime` (CPU, no torch needed). Pre/post-processing
matches the model's own reference inference code (pad to 1024x1024 square,
normalize, CHW, run, crop, resize back -- see
`skytnt/anime-remove-background` HF Space's `app.py`).

Handles the outline and interior hair gaps natively -- no separate edge
cleanup or blob filter needed. Verified by decoding a frame straight back
out of the generated C header (not just checking the intermediate PNG) to
confirm the full pipeline, not just the segmentation step, is correct.

Generator: `convert_rle565_isnet.py` (session scratchpad, never persisted
into the repo).

**Remaining issue**: user still saw a bit of white after this pass --
not root-caused before moving to v3 below.

### v3: BiRefNet-massive (tried, reverted -- looked fine as a static
composite, broke the actual animation on hardware)

Swapped segmentation models again -- `not-lain/BiRefNet-massive` (the
[ZhengPeng7/BiRefNet](https://github.com/ZhengPeng7/BiRefNet) architecture,
swin_v1_large backbone, 220M params, trained on the "massive" combined
dataset) in place of IS-Net-Anime, same composite/RLE pipeline otherwise.

Loading this checkpoint is *not* a plain `transformers`
`AutoModelForImageSegmentation(..., trust_remote_code=True)` call like the
base `ZhengPeng7/BiRefNet` repo supports -- checked `not-lain/BiRefNet-massive`'s
`config.json` directly (via the HF API) and it's only `{"bb_pretrained":
false}`, no `auto_map`/`architectures`, so there's no remote code for
`transformers` to fetch. It's a `PyTorchModelHubMixin` checkpoint instead:
cloned the BiRefNet GitHub repo for its `models/birefnet.py` (code only, no
weights) and called `BiRefNet.from_pretrained("not-lain/BiRefNet-massive")`
directly, which rebuilds the repo's own default `config.py` architecture
(swin_v1_l, 1024x1024, task=DIS5K -- this checkpoint's exact training
config) and loads the safetensors state dict into it.

Ran on the RTX 3050 (CUDA, fp16) -- ~1.2s/frame, well under a minute for
all 33 frames. Inspected the frame-0 mask/composite before committing to
the full batch: clean edges, hair gaps handled, no white remnants -- looks
correct where IS-Net-Anime still left artifacts. RLE compression 1.97x,
consistent with the earlier two approaches.

Generator: `convert_rle565_birefnet.py` (session scratchpad, never
persisted into the repo -- see Open items).

**Environment note**: this machine's global Python install had a corrupted
`torch` (dist-info present, package files missing -- likely an interrupted
previous install/uninstall) and was missing `torchvision`/`timm`/`kornia`/
`scikit-image`/`tabulate`/`prettytable`, all of which BiRefNet's own
`requirements.txt` needs. Fixed by reinstalling `torch==2.5.1+cu121` +
matching `torchvision` from the PyTorch cu121 index, then installing the
rest from PyPI. `scikit-image`'s install pulled in `numpy>=2`
(`numpy 2.5.3`), which silently broke `onnxruntime` (compiled against the
numpy-1 C-ABI -- it only surfaced as a caught/logged `ImportError` deep in
one of `kornia`'s optional submodules, not a hard crash, but would have
broken anything else in this shared global environment that actually calls
`onnxruntime`). Pinned back to `numpy<2` (matches BiRefNet's own
`requirements.txt` pin) and reverified `cv2`/`onnxruntime`/`skimage`/
`kornia` all import cleanly before running the real conversion. This is a
shared, non-venv'd global Python environment (also used by an unrelated
`open-webui` install) -- a venv would have avoided this entirely; worth
doing if this pipeline gets reused again.

Model weights (885MB safetensors) were kept off `C:\` and off `K:\` (the
workspace rule explicitly excludes large model weights from the 10GB
scratch volume) by pointing `HF_HOME` at `E:\ModelCache\huggingface`
instead of letting `huggingface_hub` use its default `C:\Users\...\.cache`
location.

**Verdict: reverted.** The frame-0 mask and composite (checked before
committing to the full 33-frame batch) looked clean -- no white halo, hair
gaps handled. But on the actual panel, playing the full animation looked
wrong ("cooked the animation" -- not further diagnosed, could be per-frame
mask instability across the sequence, a color/contrast side effect of the
compositing, or something else entirely). A single static frame looking
right is not sufficient evidence for this kind of per-frame ML
segmentation -- would need to check several frames across the sequence,
ideally played back, before trusting a new approach again.

Reverted to v2 (IS-Net-Anime) and reflashed; v2 is what's on the board now.
Cleaned up after the experiment: `K:\scratch\BiRefNet` (cloned model code)
and `E:\ModelCache\huggingface` (885MB downloaded weights) both deleted.
`convert_rle565_birefnet.py` remains in the session scratchpad only, never
persisted into the repo, kept in case this gets revisited. The Python
package installs from this experiment (`torch` reinstall/fix,
`torchvision`, `timm`, `kornia`, `scikit-image`, `tabulate`, `prettytable`,
`numpy` pinned back to `<2`) were left in place in the shared global
env -- not undone, since the `torch` fix in particular was a repair of a
pre-existing breakage, not new bloat, and removing the rest risked
re-breaking something without clear benefit.

## Display driver: Adafruit_ST7789 vs LovyanGFX

Added a second sketch, `s3_gif_player_lovyan`, to compare LovyanGFX's
throughput against Adafruit_GFX/Adafruit_ST7789 (same pins, same RLE
frame data, same decode logic -- only the driver differs). Two real bugs
found and fixed getting it working:
- **180° flip**: `setRotation(0)` came out upside-down on this panel;
  `setRotation(2)` is correct.
- **Color corruption** (structure-correct but colors wrong, speckled --
  the classic byte-order symptom, not a simple polarity invert):
  `tft.setSwapBytes(true)` needed. `frameBuf` is plain native-uint16
  RGB565, the same bytes Adafruit's `writePixels()` took and byte-swapped
  internally for SPI; LovyanGFX's `pushImage()` needs that same swap told
  explicitly or it sends the wrong byte order.

No throughput comparison data gathered yet (serial logging unreliable on
this board all session, same as every earlier firmware attempt) -- both
builds run, speed hasn't been formally measured against each other.

## Toolchain

`arduino-cli` + ESP32 core + Adafruit/LovyanGFX libraries were set up once
in `K:\scratch\arduino-cli` (scratch, wiped without warning per workspace
rules) and then copied to the standard persistent locations so future
sessions don't need to redo this:
- `C:\Users\lnminh1411\bin\arduino-cli.exe` (binary, already in PATH)
- `C:\Users\lnminh1411\AppData\Local\Arduino15\` (ESP32 core + packages,
  ~2.1GB -- arduino-cli's actual default data dir on this machine,
  confirmed empirically)
- `C:\Users\lnminh1411\Documents\Arduino\libraries\` (Adafruit
  GFX/BusIO/ST7735-ST7789/seesaw, SD, LovyanGFX)

Build/flash:
```bash
"C:\Users\lnminh1411\bin\arduino-cli.exe" compile --fqbn esp32:esp32:esp32s3:PartitionScheme=huge_app "E:\ESP32 PC Controller\firmware\s3_gif_player"
"C:\Users\lnminh1411\bin\arduino-cli.exe" upload -p COM9 --fqbn esp32:esp32:esp32s3:PartitionScheme=huge_app "E:\ESP32 PC Controller\firmware\s3_gif_player"
```

## Open items

- v2 (IS-Net-Anime) is back on the board; its own known issue (a bit of
  white still visible) was never root-caused and is still open -- just
  no longer actively being chased this session.
- v3 (BiRefNet-massive) broke the animation on hardware despite a clean
  static-frame check -- not diagnosed further (see v3 section above). If
  revisited, check several frames across the sequence, not just frame 0,
  before reflashing.
- None of the three generators (`convert_rle565.py`,
  `convert_rle565_isnet.py`, `convert_rle565_birefnet.py`) are persisted
  into the repo -- scratchpad only. Worth deciding where the *script*
  should live (`Scripts/`, tracked) if this pipeline gets reused.
  `isnetis.onnx` (~168MB) is still sitting in the session scratchpad too;
  the BiRefNet weights were deleted after the revert.
- The global Python env this ran in is shared with unrelated tools
  (`open-webui`) and isn't a venv -- BiRefNet's dependency install
  (`torch`/`torchvision`/`timm`/`kornia`/`scikit-image`/etc.) changed
  versions of packages those other tools pin (see v3 environment note
  above). Nothing currently broken (reverified after the fact), but a
  dedicated venv would remove the risk for next time.
- LovyanGFX vs Adafruit_ST7789 throughput: not actually measured against
  each other yet, just both confirmed working.
- `gif_frames_rle.h` in `s3_gif_player/` and `s3_gif_player_lovyan/` are
  kept as two separate copies (no shared/symlinked header) -- remember to
  regenerate+copy to both when the source frame data changes again.
