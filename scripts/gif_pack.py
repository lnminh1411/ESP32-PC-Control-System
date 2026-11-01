"""
Builds firmware/main/gifs.bin, the pack for the board's gifs partition, from the
GIFs listed in firmware/gifs/gifs.json. Format: gif_store.h.

    python scripts/gif_pack.py

Each GIF is scaled to the panel's height and cropped (or padded black) to its
width, quantized to RGB444 and given its own palette of at most 256 colours
(the most used ones; the rest take the nearest palette colour). "every" keeps
every Nth frame; the dropped frames' time goes to the frame kept before them.
Frame 0 of each GIF stores the whole screen, every other frame only the pixels
of its dirty rects against the frame before it in the GIF's loop. The rects
are the cheapest cover of the changed 8-row bands by at most 8 rects (pixels
pushed plus a fixed cost per rect), each frame LZ4-HC compressed.

Needs numpy, Pillow and lz4.
"""
import json
import multiprocessing
import os
import struct

import lz4.block
import numpy as np
from PIL import Image, ImageSequence

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GIF_DIR = os.path.join(ROOT, "firmware", "gifs")
OUT = os.path.join(ROOT, "firmware", "main", "gifs.bin")
PANEL_W, PANEL_H = 240, 320
BAND = 8                # rows, as computeFrameRects did on the board
MAX_DIRTY_RECTS = 8     # gif_store.h
MAX_GIF_SEGS = 12       # gif_store.h
MAX_FRAMES = 512        # config.h
PARTITION = 6 << 20     # gifs in firmware/partitions.csv
RECT_COST = 200         # pixels: a rect's CASET/RASET/RAMWR and DMA restart take about as long on the bus
PACK_VERSION = 3        # 1 was zlib frames, 2 had no prediction
BLOCK = 8               # px: GIF_BLOCK, gif_store.h
SHIFT = 24              # px, the furthest a block looks in the frame before
SKIP = 255              # GIF_SKIP, gif_store.h: this pixel comes from its block's shift


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
    """The 255 most used colours (commonest first; 255 is SKIP) and a colour -> index
    table; the worst colour error as (RGB444 distance, share of pixels moved at all)."""
    colours, counts = np.unique(np.stack(frames), return_counts=True)
    order = np.argsort(-counts, kind="stable")
    colours, counts = colours[order], counts[order]
    palette = colours[:SKIP]
    lut = np.zeros(4096, dtype=np.uint8)
    lut[palette] = np.arange(len(palette))
    worst = 0.0
    for c in colours[SKIP:]:
        d = ((rgb_of(palette) - rgb_of(c)) ** 2).sum(axis=1)
        k = int(d.argmin())
        lut[c] = k
        worst = max(worst, float(d[k]) ** 0.5)
    return palette, lut, worst, counts[SKIP:].sum() / counts.sum()


def flatten_backdrop(frames, levels):
    """Colours within levels (of 15) of the commonest one become it, where they touch
    it from the top or sides (figures run off the bottom): grain off the backdrop, pale
    skin inside the outlines kept."""
    colours, counts = np.unique(np.stack(frames), return_counts=True)
    bg = colours[counts.argmax()]
    out = []
    for f in frames:
        near = np.abs(rgb_of(f) - rgb_of(bg)).max(axis=2) <= levels
        reached = np.zeros_like(near)
        reached[0] = f[0] == bg
        reached[:, [0, -1]] |= f[:, [0, -1]] == bg
        while True:
            grown = reached.copy()
            grown[1:] |= reached[:-1]
            grown[:-1] |= reached[1:]
            grown[:, 1:] |= reached[:, :-1]
            grown[:, :-1] |= reached[:, 1:]
            grown &= near
            if (grown == reached).all():
                break
            reached = grown
        out.append(np.where(reached, bg, f))
    return out


def band_diff(cur, prev, y, n):
    """Changed x range of rows [y, y+n), whole 2-pixel pairs; None = unchanged."""
    pairs = (cur[y:y + n] != prev[y:y + n]).reshape(n, PANEL_W // 2, 2).any(axis=(0, 2))
    hit = np.flatnonzero(pairs)
    return (int(hit[0]) * 2, int(hit[-1]) * 2 + 1) if hit.size else None


def frame_rects(cur, prev):
    """Fewest pixels (plus RECT_COST a rect) covering the changed bands with at
    most MAX_DIRTY_RECTS rects, each a run of whole bands at their changed x
    range. best[i][r]: the cost of bands i.. with r rects left."""
    ranges = [band_diff(cur, prev, y, BAND) for y in range(0, PANEL_H, BAND)]
    n, k, inf = len(ranges), MAX_DIRTY_RECTS, float("inf")
    best = [[inf] * (k + 1) for _ in range(n)] + [[0] * (k + 1)]
    pick = [[None] * (k + 1) for _ in range(n)]   # (last band, x0, x1) of the rect starting at band i
    for i in range(n - 1, -1, -1):
        for r in range(k + 1):
            if ranges[i] is None:                 # clean: no rect needs to start here
                best[i][r] = best[i + 1][r]
                continue
            if r == 0:
                continue
            x0, x1 = PANEL_W, -1
            for j in range(i, n):
                if ranges[j] is not None:
                    x0, x1 = min(x0, ranges[j][0]), max(x1, ranges[j][1])
                v = (x1 - x0 + 1) * BAND * (j - i + 1) + RECT_COST + best[j + 1][r - 1]
                if v < best[i][r]:
                    best[i][r], pick[i][r] = v, (j, x0, x1)
    rects, i, r = [], 0, k
    while i < n:
        if pick[i][r] is None:                    # clean band (or nothing left to cover)
            i += 1
            continue
        j, x0, x1 = pick[i][r]
        rects.append((x0, i * BAND, x1 - x0 + 1, (j - i + 1) * BAND))
        i, r = j + 1, r - 1
    return rects


def rect_pixels(idx, rects):
    return b"".join(idx[y:y + h, x:x + w].tobytes() for x, y, w, h in rects)


def near_table(palette, levels):
    """near[a, b]: colour a may stand in for colour b, at most levels (of 15) off.
    The backdrop (palette[0]) trades with nothing else, so it stays clean."""
    rgb = rgb_of(palette).astype(np.int16)
    near = np.zeros((SKIP + 1, SKIP + 1), bool)
    n = len(palette)
    near[:n, :n] = np.abs(rgb[:, None] - rgb[None]).max(axis=2) <= levels
    near[0], near[:, 0] = False, False
    near[np.arange(n), np.arange(n)] = True
    return near


def best_shifts(cur, ref, near):
    """Each block's shift into ref: the fewest pixels that have to be stored, the
    nearest shift on a tie. The whole block stays on the frame."""
    rows, cols = PANEL_H // BLOCK, PANEL_W // BLOCK
    wide = np.pad(ref, SHIFT, constant_values=SKIP)
    best = np.full((rows, cols), BLOCK * BLOCK + 1)
    mv = np.zeros((rows, cols, 2), np.int8)
    ys, xs = np.arange(rows)[:, None] * BLOCK, np.arange(cols)[None] * BLOCK
    span = range(-SHIFT, SHIFT + 1)
    for dy, dx in sorted(((y, x) for y in span for x in span), key=lambda s: s[0] ** 2 + s[1] ** 2):
        y, x = SHIFT + dy, SHIFT + dx
        wrong = ~near[wide[y:y + PANEL_H, x:x + PANEL_W], cur]
        n = wrong.reshape(rows, BLOCK, cols, BLOCK).sum(axis=(1, 3))
        on = (ys + dy >= 0) & (ys + dy + BLOCK <= PANEL_H) & (xs + dx >= 0) & (xs + dx + BLOCK <= PANEL_W)
        hit = (n < best) & on
        best[hit] = n[hit]
        mv[hit] = (dy, dx)
        if not best.any():
            break
    return mv


def predicted(ref, mv):
    """ref's pixels through the shifts."""
    out = np.empty_like(ref)
    for r in range(PANEL_H // BLOCK):
        for c in range(PANEL_W // BLOCK):
            y, x = r * BLOCK, c * BLOCK
            dy, dx = int(mv[r, c, 0]), int(mv[r, c, 1])
            out[y:y + BLOCK, x:x + BLOCK] = ref[y + dy:y + dy + BLOCK, x + dx:x + dx + BLOCK]
    return out


def replay_check(indexed, stored):
    """Keyframe, then the deltas, then round the loop, as gifApplyFrame does: must
    give every frame back."""
    n = len(indexed)
    mv_len = (PANEL_H // BLOCK) * (PANEL_W // BLOCK) * 2
    cur = None
    for i in list(range(n)) + [0]:
        rects, z = stored[i]
        if i == 0:
            raw = lz4.block.decompress(z, uncompressed_size=PANEL_W * PANEL_H)
            cur = np.frombuffer(raw, np.uint8).reshape(PANEL_H, PANEL_W).copy()
        else:
            n_px = sum(w * h for _, _, w, h in rects)
            raw = lz4.block.decompress(z, uncompressed_size=n_px + mv_len)
            mv = np.frombuffer(raw, np.int8, offset=n_px).reshape(PANEL_H // BLOCK, PANEL_W // BLOCK, 2)
            pred = predicted(cur, mv)
            off = 0
            for x, y, w, h in rects:
                px = np.frombuffer(raw, np.uint8, w * h, off).reshape(h, w)
                cur[y:y + h, x:x + w] = np.where(px == SKIP, pred[y:y + h, x:x + w], px)
                off += w * h
        assert (cur == indexed[i]).all(), f"frame {i} does not replay"


def pack_gif(name, every, flatten, lossy):
    frames, durations = load_frames(os.path.join(GIF_DIR, name), every)
    if flatten:
        frames = flatten_backdrop(frames, flatten)
    palette, lut, worst, moved = build_palette(frames)
    source = [lut[f] for f in frames]
    near = near_table(palette, lossy)
    indexed, shifts, skips = [source[0]], [None], [None]    # indexed: what the panel shows
    for cur in source[1:]:
        mv = best_shifts(cur, indexed[-1], near)
        pred = predicted(indexed[-1], mv)
        skip = near[pred, cur]
        indexed.append(np.where(skip, pred, cur))
        shifts.append(mv)
        skips.append(np.where(skip, SKIP, cur).astype(np.uint8))
    stored, psram, pushed = [], 0, 0
    for i, idx in enumerate(indexed):
        rects = frame_rects(idx, indexed[i - 1])   # frame 0 against the last one: the loop
        raw = idx.tobytes() if i == 0 else rect_pixels(skips[i], rects) + shifts[i].tobytes()
        z = lz4.block.compress(raw, mode="high_compression", compression=12, store_size=False)
        stored.append((rects, z))
        psram += len(raw)
        pushed += sum(w * h for _, _, w, h in rects)
    replay_check(indexed, stored)

    body = struct.pack("<2H", len(palette), len(frames)) + palette.astype("<u2").tobytes()
    for i, (rects, z) in enumerate(stored):
        body += struct.pack("<HBB", durations[i], 1 if i == 0 else 0, len(rects))
        body += b"".join(struct.pack("<4H", *r) for r in rects)
        body += struct.pack("<I", len(z)) + z
    print(f"{name}: {len(frames)} frames, {sum(durations)} ms a loop, {len(palette)} colours, "
          f"worst colour error {worst:.2f} of 15 (RGB444 distance, {moved * 100:.2f}% of pixels moved), "
          f"pack {len(body) / 1024:.1f} KB, {psram / 1024:.1f} KB inflated, "
          f"rects {100 * pushed / (len(frames) * PANEL_W * PANEL_H):.0f}% of the screen a frame")
    return body, len(frames), psram


def main():
    with open(os.path.join(GIF_DIR, "gifs.json"), encoding="utf-8") as f:
        gifs = json.load(f)
    assert 1 <= len(gifs) <= MAX_GIF_SEGS, "too many GIFs"
    with multiprocessing.Pool(6) as pool:   # the shift search takes about a second a frame
        packed = pool.starmap(pack_gif, [(g["file"], g.get("every", 1), g.get("flatten", 0), g.get("lossy", 0)) for g in gifs], 1)
    total = sum(p[1] for p in packed)
    assert total <= MAX_FRAMES, "too many frames"
    psram = sum(p[2] for p in packed)
    data = b"GIFP" + struct.pack("<2B3HI", PACK_VERSION, len(gifs), PANEL_W, PANEL_H, total, psram)
    data += b"".join(p[0] for p in packed)
    assert len(data) <= PARTITION, "pack bigger than the gifs partition"
    with open(OUT, "wb") as f:
        f.write(data)
    print(f"{OUT}: {len(gifs)} GIFs, {total} frames, pack {len(data)} bytes ({len(data) / 1024:.1f} KB) "
          f"of the {PARTITION >> 20} MB gifs partition, {psram / 1024:.1f} KB decoded, frame by frame as they play")


if __name__ == "__main__":
    main()
