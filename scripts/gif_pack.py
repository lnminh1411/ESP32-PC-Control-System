"""
Builds firmware/main/gifs.bin, the GIF pack embedded in the firmware, from the
GIFs listed in firmware/gifs/gifs.json (in play order). Format: gif_store.h.

    python scripts/gif_pack.py

Each GIF is scaled to the panel's height and cropped (or padded black) to its
width, quantized to RGB444 and given its own palette of at most 256 colours
(the most used ones; the rest take the nearest palette colour). "every" keeps
every Nth frame; the dropped frames' time goes to the frame kept before them.
Frame 0 of each GIF stores the whole screen, every other frame only the pixels
of its dirty rects against the frame before it in the GIF's loop.

Needs numpy and Pillow.
"""
import json
import os
import struct
import zlib

import numpy as np
from PIL import Image, ImageSequence

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GIF_DIR = os.path.join(ROOT, "firmware", "gifs")
OUT = os.path.join(ROOT, "firmware", "main", "gifs.bin")
PANEL_W, PANEL_H = 240, 320
BAND = 8                # rows, as computeFrameRects did on the board
MAX_DIRTY_RECTS = 8     # gif_store.h
MAX_GIF_SEGS = 4        # gif_store.h
MAX_FRAMES = 64         # config.h


def fill_panel(img):
    """Scaled to the panel's height, cropped or padded (black) to its width, centred."""
    w, h = img.size
    sw = round(w * PANEL_H / h)
    img = img.resize((sw, PANEL_H), Image.LANCZOS)
    out = Image.new("RGB", (PANEL_W, PANEL_H))
    out.paste(img, ((PANEL_W - sw) // 2, 0))
    return out


def to444(img):
    rgb = np.asarray(img, dtype=np.uint16) >> 4
    return (rgb[..., 0] << 8) | (rgb[..., 1] << 4) | rgb[..., 2]


def load_frames(path, every):
    im = Image.open(path)
    frames, durations = [], []
    for i, fr in enumerate(ImageSequence.Iterator(im)):
        ms = fr.info.get("duration", im.info.get("duration", 33)) or 33
        if i % every:   # dropped: shown as part of the frame kept before it
            durations[-1] += ms
            continue
        frames.append(to444(fill_panel(fr.convert("RGB"))))
        durations.append(ms)
    return frames, durations


def rgb_of(c):
    return np.stack([(c >> 8) & 15, (c >> 4) & 15, c & 15], axis=-1).astype(np.int32)


def build_palette(frames):
    """The 256 most used colours and a colour -> index table; the worst colour
    error as (RGB444 distance, share of pixels moved at all)."""
    colours, counts = np.unique(np.stack(frames), return_counts=True)
    order = np.argsort(-counts, kind="stable")
    colours, counts = colours[order], counts[order]
    palette = colours[:256]
    lut = np.zeros(4096, dtype=np.uint8)
    lut[palette] = np.arange(len(palette))
    worst = 0.0
    for c in colours[256:]:
        d = ((rgb_of(palette) - rgb_of(c)) ** 2).sum(axis=1)
        k = int(d.argmin())
        lut[c] = k
        worst = max(worst, float(d[k]) ** 0.5)
    return palette, lut, worst, counts[256:].sum() / counts.sum()


def band_diff(cur, prev, y, n):
    """Changed x range of rows [y, y+n), whole 2-pixel pairs; None = unchanged."""
    pairs = (cur[y:y + n] != prev[y:y + n]).reshape(n, PANEL_W // 2, 2).any(axis=(0, 2))
    hit = np.flatnonzero(pairs)
    return (int(hit[0]) * 2, int(hit[-1]) * 2 + 1) if hit.size else None


def frame_rects(cur, prev):
    """Runs of dirty bands, one rect per run; more than MAX_DIRTY_RECTS: the whole frame."""
    rects, run_y, rx0, rx1 = [], -1, PANEL_W, -1
    for y in range(0, PANEL_H + BAND, BAND):   # the extra pass closes the last run
        dirty = band_diff(cur, prev, y, BAND) if y < PANEL_H else None
        if dirty:
            if run_y < 0:
                run_y = y
            rx0, rx1 = min(rx0, dirty[0]), max(rx1, dirty[1])
        elif run_y >= 0:
            rects.append((rx0, run_y, rx1 - rx0 + 1, y - run_y))
            run_y, rx0, rx1 = -1, PANEL_W, -1
    return rects if len(rects) <= MAX_DIRTY_RECTS else [(0, 0, PANEL_W, PANEL_H)]


def rect_pixels(idx, rects):
    return b"".join(idx[y:y + h, x:x + w].tobytes() for x, y, w, h in rects)


def replay_check(indexed, stored):
    """Keyframe, then the deltas, then round the loop: must give every frame back."""
    n = len(indexed)
    cur = np.frombuffer(zlib.decompress(stored[0][1]), np.uint8).reshape(PANEL_H, PANEL_W).copy()
    for i in list(range(1, n)) + [0]:
        rects, z = stored[i]
        raw = zlib.decompress(z)
        if i == 0:
            cur = np.frombuffer(raw, np.uint8).reshape(PANEL_H, PANEL_W).copy()
        else:
            off = 0
            for x, y, w, h in rects:
                cur[y:y + h, x:x + w] = np.frombuffer(raw, np.uint8, w * h, off).reshape(h, w)
                off += w * h
            assert off == len(raw)
        assert (cur == indexed[i]).all(), f"frame {i} does not replay"


def pack_gif(name, every):
    frames, durations = load_frames(os.path.join(GIF_DIR, name), every)
    palette, lut, worst, moved = build_palette(frames)
    indexed = [lut[f] for f in frames]
    stored, psram = [], 0
    for i, idx in enumerate(indexed):
        rects = frame_rects(idx, indexed[i - 1])   # frame 0 against the last one: the loop
        raw = idx.tobytes() if i == 0 else rect_pixels(idx, rects)
        stored.append((rects, zlib.compress(raw, 9)))
        psram += len(raw)
    replay_check(indexed, stored)

    body = struct.pack("<2H", len(palette), len(frames)) + palette.astype("<u2").tobytes()
    for i, (rects, z) in enumerate(stored):
        body += struct.pack("<HBB", durations[i], 1 if i == 0 else 0, len(rects))
        body += b"".join(struct.pack("<4H", *r) for r in rects)
        body += struct.pack("<I", len(z)) + z
    print(f"{name}: {len(frames)} frames, {sum(durations)} ms a loop, {len(palette)} colours, "
          f"worst colour error {worst:.2f} of 15 (RGB444 distance, {moved * 100:.2f}% of pixels moved), "
          f"pack {len(body) / 1024:.1f} KB, PSRAM {psram / 1024:.1f} KB")
    return body, len(frames), psram


def main():
    with open(os.path.join(GIF_DIR, "gifs.json"), encoding="utf-8") as f:
        gifs = json.load(f)
    assert 1 <= len(gifs) <= MAX_GIF_SEGS, "too many GIFs"
    packed = [pack_gif(g["file"], g.get("every", 1)) for g in gifs]
    total = sum(p[1] for p in packed)
    assert total <= MAX_FRAMES, "too many frames"
    psram = sum(p[2] for p in packed)
    data = b"GIFP" + struct.pack("<2B3HI", 1, len(gifs), PANEL_W, PANEL_H, total, psram)
    data += b"".join(p[0] for p in packed)
    with open(OUT, "wb") as f:
        f.write(data)
    print(f"{OUT}: {len(gifs)} GIFs, {total} frames, pack {len(data)} bytes ({len(data) / 1024:.1f} KB), "
          f"PSRAM after decode {psram} bytes ({psram / 1024:.1f} KB)")


if __name__ == "__main__":
    main()
