"""Capture every carrier face from the board and tile them for the README.

The PC stats service is stopped for the run and this script feeds the board
a made-up but plausible stats frame instead (same @s/@t/@e lines the service
sends), so the screenshots do not show the real machine. Each panel is forced
to a face with `face <n> <name>`, grabbed with `shot <n>`, and everything is
put back afterwards (faces on auto, demo weather cleared, service started).

  python scripts/readme_shots.py docs/images/screens.png [--port COM12] [--keep DIR]

Needs an elevated shell (sc stop/start). The media cover is whatever the
board last received; the title must still be in the board's text cache when
it is not plain ASCII.
"""
import argparse
import os
import random
import subprocess
import threading
import time

import serial
from PIL import Image, ImageDraw, ImageFont

from panel_shot import shot_on

SERVICE = "ESP32PcStats"

# A busy afternoon: build running, music on, local model generating.
BASE_STATS = {
    "uptime": 12 * 86400 + 7 * 3600 + 41 * 60, "cpu_temp": 64.5, "cpu_load": 37.2, "cpu_ecore_load": 21.4,
    "cpu_clock": 5240, "cpu_power": 88.6, "cpu_fan": 1420, "sys_fan": 1180,
    "ram_load": 58.3, "ram_used": 37.1, "ram_free": 26.4,
    "gpu1_temp": 61, "gpu1_clock": 2450, "gpu1_load": 64, "gpu1_vram": 5120, "gpu1_vramt": 8176,
    "gpu2_temp": 58, "gpu2_clock": 1785, "gpu2_load": 41, "gpu2_vram": 4620, "gpu2_vramt": 6144,
    "c_used": 412.6, "c_total": 919.6, "net_up": 384000,
    "media.ok": 1, "media.playing": 1, "media.pos_s": 74, "media.len_s": 179,
    "claude.ok": 1, "claude.stale": 0, "claude.s_pct": 38, "claude.w_pct": 22,
    "agy.ok": 1, "agy2.flash_5h_pct": 64, "agy2.flash_wk_pct": 88,
    "ai.ok": 1, "ai.model": 2, "ai.phase": 2, "ai.active": 0, "ai.requests": 148, "ai.errors": 2,
    "ai.gen_tok_s": 38.6, "ai.elapsed_s": 48, "ai.step": 7, "ai.step_max": 30,
    "ai.gpu_util": 92, "ai.vram_mb": 5480, "ai.gpu_temp": 66, "ai.gpu_power_w": 61.4, "llm_ram_mb": 9216,
    "pc.going_id": 0, "pc.tz_min": 420,
}
BASE_TEXT = {
    "media.title": "ゆめ (feat. 重音テト)", "media.artist": "d0tc0mmie", "media.source": "Spotify",
    "claude.plan": "Claude Max 5x", "claude.s_pace_v": "ok",
    "agy.active": "user@example.com", "agy2.email": "user@example.com", "agy2.plan": "Pro",
    "ai.phase_name": "idle", "ai.tool": "grep_search",
}
LOCAL_AI_BUSY = ({"ai.phase": 4, "ai.active": 1}, {"ai.phase_name": "generating"})

# (caption, [(panel, face, setup command, label, (stat overrides, text overrides))])
ROWS = [
    ("PC online", [(1, "pc", None, "PC stats", None),
                   (2, "media", None, "Media + network", None),
                   (3, "ai", None, "AI usage", None)]),
    ("PC offline", [(1, "gif", None, "GIF", None),
                    (2, "weather", "weather demo", "Weather", None),
                    (3, "clock", None, "Clock", None)]),
    ("States", [(1, "localai", None, "Local AI working", LOCAL_AI_BUSY),
                (2, "weather", "weather demo rain", "Rain warning", None),
                (3, "restart", None, "Restarting", None),
                (3, "update", None, "Updating", None)]),
]

SCALE = 2
GAP = 32
CAPTION_H = 56
LABEL_H = 44
BG = (14, 16, 24)
FG = (220, 224, 235)
DIM = (130, 138, 160)


class Feeder:
    """Sends the fake frame twice a second until stopped."""

    def __init__(self, ser):
        self.ser, self.lock = ser, threading.Lock()
        self.stats, self.text = dict(BASE_STATS), dict(BASE_TEXT)
        self.stop = threading.Event()
        threading.Thread(target=self.run, daemon=True).start()

    def send(self, line):
        with self.lock:
            self.ser.write((line + "\n").encode("utf-8"))

    def frame(self):
        now = int(time.time())
        st = dict(self.stats)
        st["pc.epoch"] = now
        st["net_down"] = random.choice([0.4, 0.9, 1.6, 2.4, 3.1, 0.7]) * 1048576
        st["claude.s_reset_ts"] = now + 2 * 3600 + 13 * 60 + 42
        st["claude.w_reset_ts"] = now + 3 * 86400 + 5 * 3600
        st["agy2.flash_5h_reset_ts"] = now + 3 * 3600 + 2 * 60
        st["agy2.flash_wk_reset_ts"] = now + 5 * 86400 + 9 * 3600
        items = [f"{k}={v}" for k, v in st.items()]
        line = "@s"
        for it in items:   # the console keeps 400-byte lines
            if len(line) + len(it) > 300:
                self.send(line)
                line = "@s"
            line += " " + it
        self.send(line)
        for k, v in self.text.items():
            self.send(f"@t {k} {v}")
        self.send("@e")

    def run(self):
        while not self.stop.is_set():
            self.frame()
            time.sleep(0.5)


def font(size, bold=False):
    name = "segoeuib.ttf" if bold else "segoeui.ttf"
    try:
        return ImageFont.truetype(os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts", name), size)
    except OSError:
        return ImageFont.load_default()


def compose(shots, out):
    cw, ch = shots[0][1][0][0].size
    cols = max(len(r) for _, r in shots)
    width = GAP + cols * (cw + GAP)
    height = GAP + len(shots) * (CAPTION_H + ch + LABEL_H) + GAP
    sheet = Image.new("RGB", (width, height), BG)
    d = ImageDraw.Draw(sheet)
    fc, fl = font(30, True), font(22)
    y = GAP
    for caption, row in shots:
        d.text((GAP, y + 8), caption, fill=FG, font=fc)
        y += CAPTION_H
        x = GAP
        for img, label in row:
            sheet.paste(img, (x, y))
            d.text((x + cw // 2, y + ch + 10), label, fill=DIM, font=fl, anchor="mt")
            x += cw + GAP
        y += ch + LABEL_H
    sheet.save(out, optimize=True)
    print(f"saved {out} ({width}x{height})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--port", default="COM12")
    ap.add_argument("--keep", help="also save each capture here")
    a = ap.parse_args()

    subprocess.run(["sc", "stop", SERVICE], capture_output=True)
    time.sleep(2)
    ser = serial.Serial(a.port, 115200, timeout=0.5)
    feed = Feeder(ser)
    shots = []
    try:
        time.sleep(20)   # fill the network sparkline
        for caption, cells in ROWS:
            row = []
            for panel, face, setup, label, over in cells:
                feed.stats, feed.text = dict(BASE_STATS), dict(BASE_TEXT)
                if over:
                    feed.stats.update(over[0])
                    feed.text.update(over[1])
                if setup:
                    feed.send(setup)
                feed.send(f"face {panel} {face}")
                time.sleep(2)   # let the face render (and a GIF reach a real frame)
                with feed.lock:
                    img = shot_on(ser, panel)
                if a.keep:
                    os.makedirs(a.keep, exist_ok=True)
                    img.save(os.path.join(a.keep, label.lower().replace(" + ", "-").replace(" ", "-") + ".png"))
                row.append((img.resize((img.width * SCALE, img.height * SCALE), Image.NEAREST), label))
                print(f"captured {label}")
            shots.append((caption, row))
    finally:
        for p in (1, 2, 3):
            feed.send(f"face {p} auto")
        feed.send("weather clear")
        feed.stop.set()
        time.sleep(0.6)
        ser.close()
        subprocess.run(["sc", "start", SERVICE], capture_output=True)
    compose(shots, a.out)


if __name__ == "__main__":
    main()
