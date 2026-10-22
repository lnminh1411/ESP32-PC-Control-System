"""
Uploads a GIF to the carrier firmware (firmware/carrier_main) over the
CH343 USB-UART port as raw RGB565: every frame of the original GIF, scaled
to fit 240x320 with the aspect ratio kept. Protocol: gif_store.h.

    python scripts/gif_stream.py <file.gif> [--port COM15] [--watch 15]

Resets the board, uploads in its 1.5 s boot window, prints the per-frame
acks (a frame with a bad checksum is resent), waits while the device saves
the GIF to flash (it then replays on every boot without the PC), then shows
the device log for --watch seconds.

Needs pyserial, numpy and Pillow.
"""
import argparse
import struct
import sys
import time

import numpy as np
import serial
from PIL import Image, ImageSequence

BAUD = 3_000_000
PANEL_W, PANEL_H = 240, 320


def load_frames(path):
    im = Image.open(path)
    w, h = im.size
    scale = min(PANEL_W / w, PANEL_H / h)
    size = (round(w * scale), round(h * scale))
    frames, durations = [], []
    for fr in ImageSequence.Iterator(im):
        rgb = np.asarray(fr.convert("RGB").resize(size, Image.LANCZOS), dtype=np.uint16)
        px = ((rgb[..., 0] & 0xF8) << 8) | ((rgb[..., 1] & 0xFC) << 3) | (rgb[..., 2] >> 3)
        frames.append(px.astype(">u2").tobytes())          # big-endian, as the panel wants it
        durations.append(fr.info.get("duration", im.info.get("duration", 33)) or 33)
    return size, frames, durations


def readline(ser, timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        line = ser.readline().decode(errors="replace").strip()
        if line:
            return line
    raise TimeoutError("no reply from device")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("gif")
    ap.add_argument("--port", default="COM15")
    ap.add_argument("--watch", type=float, default=15, help="seconds of device log to show")
    a = ap.parse_args()

    (w, h), frames, durations = load_frames(a.gif)
    print(f"{len(frames)} frames, {w}x{h}, {len(frames[0])} bytes/frame, "
          f"{sum(map(len, frames)) / 1e6:.2f} MB total")

    ser = serial.Serial(a.port, BAUD, timeout=0.5)
    ser.dtr = False                       # IO0 high: run, not bootloader
    ser.rts = True; time.sleep(0.1); ser.rts = False   # pulse EN -> clean boot
    while True:
        line = readline(ser)
        print("dev:", line)
        if line == "READY":
            break

    ser.write(b"GIF5" + struct.pack(f"<3H{len(frames)}H", w, h, len(frames), *durations))
    while (line := readline(ser)) == "READY":
        pass
    print("dev:", line)
    if line != "OK":
        sys.exit(1)

    t0 = time.time()
    for i, fr in enumerate(frames):
        while True:
            ser.write(fr + struct.pack("<I", sum(fr)))
            line = readline(ser)
            if line == f"K{i}":
                break
            print("dev:", line, "-> resend" if line == f"E{i}" else "")
            if not line.startswith("E"):
                sys.exit(1)
        print(f"\rframe {i + 1}/{len(frames)}  {(i + 1) * len(fr) / (time.time() - t0) / 1e3:.0f} KB/s",
              end="", flush=True)
    print()
    while (line := readline(ser, timeout=180)) != "PLAY":   # device saves to flash first
        print("dev:", line)
    print("dev:", line)

    end = time.time() + a.watch
    while time.time() < end:
        line = ser.readline().decode(errors="replace").strip()
        if line:
            print("dev:", line)
    ser.close()


if __name__ == "__main__":
    main()
