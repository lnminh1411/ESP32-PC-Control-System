"""Save what a carrier panel shows as a PNG.

Sends `shot <n>` on the board's CDC console and decodes the RGB565 canvas it
streams back (SHOT begin / d <b64> / end lines).

  python scripts/panel_shot.py 1 docs/img/screen1.png [--port COM12] [--scale 2]

GIF faces come back with the current GIF frame under the canvas overlay, so
the shot matches the glass.
"""
import argparse
import base64
import time

import serial
from PIL import Image


def shot(port, n):
    s = serial.Serial(port, 115200, timeout=0.5)
    time.sleep(0.3)
    try:
        return shot_on(s, n)
    finally:
        s.close()


def shot_on(s, n):
    """shot() on a port the caller already holds open."""
    s.reset_input_buffer()
    s.write(f"shot {n}\n".encode())
    data, w, h = bytearray(), 0, 0
    end = time.time() + 30
    while time.time() < end:
        line = s.readline().decode(errors="replace").strip()
        if line.startswith("SHOT begin"):
            _, _, _, w, h = line.split()
            w, h = int(w), int(h)
        elif line.startswith("SHOT d ") and w:
            data += base64.b64decode(line[7:])
        elif line == "SHOT end":
            break
    if not w or len(data) != w * h * 2:
        raise SystemExit(f"incomplete shot ({len(data)} bytes)")
    img = Image.new("RGB", (w, h))
    px = img.load()
    for i in range(w * h):
        v = (data[2 * i] << 8) | data[2 * i + 1]   # sprite stores RGB565 big-endian
        r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
        px[i % w, i // w] = (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("panel", type=int)
    ap.add_argument("out")
    ap.add_argument("--port", default="COM12")
    ap.add_argument("--scale", type=int, default=1)
    a = ap.parse_args()
    img = shot(a.port, a.panel)
    if a.scale > 1:
        img = img.resize((img.width * a.scale, img.height * a.scale), Image.NEAREST)
    img.save(a.out)
    print(f"saved {a.out}")


if __name__ == "__main__":
    main()
