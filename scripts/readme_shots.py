"""Capture every carrier face from the board and tile them for the README.

The PC stats service is stopped for the run and this script feeds the board
a made-up but plausible stats frame instead (same @s/@t/@e lines the service
sends), so the screenshots do not show the real machine. Each panel is forced
to a face with `face <n> <name>`, grabbed with `shot <n>`, and everything is
put back afterwards (faces on auto, demo weather cleared, service started).
The cover art (generated here) and the title bitmap (rendered with the
service's own GDI calls and font) are sent too, so the media face shows
neither the track the PC is playing nor `?` for the Japanese title.

  python scripts/readme_shots.py docs/images/screens.png [--port COM12] [--keep DIR]

Needs an elevated shell (sc stop/start).
"""
import argparse
import base64
import ctypes
import os
import random
import subprocess
import threading
import time
import zlib
from ctypes import wintypes as wt

import serial
from PIL import Image, ImageDraw, ImageFont

from panel_shot import shot_on

SERVICE = "ESP32PcStats"
ART_ID = 4242
TITLE_FONT = (20, True)   # fontHead() in textimg.cpp's fontSize()

# A busy afternoon: build running, music on, local model generating.
BASE_STATS = {
    "uptime": 12 * 86400 + 7 * 3600 + 41 * 60, "cpu_temp": 64.5, "cpu_load": 37.2, "cpu_ecore_load": 21.4,
    "cpu_clock": 5240, "cpu_power": 88.6, "cpu_fan": 1420, "sys_fan": 1180,
    "ram_load": 58.3, "ram_used": 37.1, "ram_free": 26.4, "ram_temp": 44, "vrm_temp": 52,
    "gpu1_temp": 61, "gpu1_clock": 2450, "gpu1_load": 64, "gpu1_vram": 5120, "gpu1_vramt": 8176,
    "gpu2_temp": 58, "gpu2_clock": 1785, "gpu2_load": 41, "gpu2_vram": 4620, "gpu2_vramt": 6144,
    "c_used": 412.6, "c_total": 919.6, "net_up": 384000,
    "gpu1_power": 72.4, "gpu2_power": 61.4, "hw.rgb.bright": 60, "hw.cpu_fans.pct": 48, "hw.rear_fan.pct": 45,
    "hw.front_fans.pct": 42, "hw.water_pump.pct": 100,
    "media.ok": 1, "media.art_id": ART_ID, "media.playing": 1, "media.pos_s": 74, "media.len_s": 179,
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
# PC usage warnings (pcwarn.cpp, default limits): fed as real stats so the
# board's 2 s averages trip them, not with `pcwarn demo`
WARN_PC = ({"cpu_temp": 86, "gpu1_load": 96, "ram_load": 84}, {})
WARN_ONE = ({"ram_load": 84}, {})
WARN_ALL = ({"ram_load": 93, "cpu_load": 96, "gpu1_load": 97, "gpu2_load": 95, "gpu1_vram": 7700,
             "cpu_temp": 86, "gpu1_temp": 84, "gpu2_temp": 79, "ram_temp": 77, "vrm_temp": 78}, {})
WARN_SETTLE_S = 4   # the 2 s window must hold only the new values

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
    ("PC usage warnings", [(1, "pc", None, "PC stats with warnings", WARN_PC),
                           (2, "media", None, "One warning", WARN_ONE),
                           (2, "media", None, "Every warning at once", WARN_ALL)]),
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

    def send_blob(self, kind, begin, end_id, data):
        """One `@a`/`@x` transfer, as the service's AppendArtChunk/AppendTextChunk send it."""
        b64 = base64.b64encode(data).decode()
        self.send(f"@{kind} begin {begin}")
        for i in range(0, len(b64), 160):
            self.send(f"@{kind} d {b64[i:i + 160]}")
        self.send(f"@{kind} end {end_id} {zlib.crc32(data):08x}")

    def send_media(self):
        """Cover art and the non-ASCII title, which the board would ask the service for."""
        self.send_blob("a", f"{ART_ID} 144 144 444", ART_ID, rgb444(cover()))
        title = self.text["media.title"]
        tid = zlib.crc32(title.encode("utf-8"))
        w, h, ascent, packed = render_text(title, *TITLE_FONT)
        self.send_blob("x", f"{tid} {TITLE_FONT[0]} {int(TITLE_FONT[1])} {w} {h} {ascent}", tid, packed)

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


def cover():
    """A made-up 144x144 record sleeve."""
    img = Image.new("RGB", (144, 144))
    d = ImageDraw.Draw(img)
    for y in range(144):
        t = y / 143
        d.line([(0, y), (143, y)], fill=(int(255 - 150 * t), int(96 - 40 * t), int(170 + 60 * t)))
    d.ellipse((18, 18, 126, 126), fill=(22, 20, 30))
    for r in range(50, 24, -6):
        d.ellipse((72 - r, 72 - r, 72 + r, 72 + r), outline=(48, 44, 62))
    d.ellipse((52, 52, 92, 92), fill=(255, 196, 90))
    d.ellipse((69, 69, 75, 75), fill=(22, 20, 30))
    return img


def rgb444(img):
    """Two pixels in three bytes, the `@a begin ... 444` format art.cpp unpacks."""
    px = [(r >> 4, g >> 4, b >> 4) for r, g, b in img.getdata()]
    out = bytearray()
    for (r0, g0, b0), (r1, g1, b1) in zip(px[0::2], px[1::2]):
        out += bytes((r0 << 4 | g0, b0 << 4 | r1, g1 << 4 | b1))
    return bytes(out)


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG), ("biPlanes", wt.WORD),
                ("biBitCount", wt.WORD), ("biCompression", wt.DWORD), ("biSizeImage", wt.DWORD),
                ("biXPelsPerMeter", wt.LONG), ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD),
                ("biClrImportant", wt.DWORD)]


def render_text(text, px, bold):
    """RenderText from PcStats.cs: GDI DrawTextW (font linking covers the
    Japanese), alpha packed 4 bits a pixel. Returns (w, h, ascent, packed)."""
    gdi, user = ctypes.windll.gdi32, ctypes.windll.user32
    vp = ctypes.c_void_p
    gdi.CreateCompatibleDC.restype = vp
    gdi.CreateCompatibleDC.argtypes = [vp]
    gdi.CreateFontW.restype = vp
    gdi.CreateFontW.argtypes = [ctypes.c_int] * 5 + [wt.DWORD] * 8 + [ctypes.c_wchar_p]
    gdi.SelectObject.restype = vp
    gdi.SelectObject.argtypes = [vp, vp]
    gdi.CreateDIBSection.restype = vp
    gdi.CreateDIBSection.argtypes = [vp, ctypes.POINTER(BITMAPINFOHEADER), ctypes.c_uint,
                                     ctypes.POINTER(vp), vp, wt.DWORD]
    gdi.GetTextMetricsW.argtypes = [vp, vp]
    gdi.SetTextColor.argtypes = [vp, wt.DWORD]
    gdi.SetBkMode.argtypes = [vp, ctypes.c_int]
    gdi.DeleteObject.argtypes = [vp]
    gdi.DeleteDC.argtypes = [vp]
    user.DrawTextW.argtypes = [vp, ctypes.c_wchar_p, ctypes.c_int, ctypes.POINTER(wt.RECT), ctypes.c_uint]

    family = "Segoe UI"
    name = f"ComicShannsMonoNerdFontPropo-{'Bold' if bold else 'Regular'}.otf"
    for d in (os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts"),
              os.path.expandvars(r"%LOCALAPPDATA%\Microsoft\Windows\Fonts")):
        path = os.path.join(d, name)
        if os.path.exists(path):
            gdi.AddFontResourceExW(path, 0x10, None)   # FR_PRIVATE
            family = ImageFont.truetype(path, px).getname()[0]
            break

    dc = gdi.CreateCompatibleDC(None)
    font_h = gdi.CreateFontW(-px, 0, 0, 0, 700 if bold else 400, 0, 0, 0, 1, 0, 0, 4, 0, family)
    old_font = gdi.SelectObject(dc, font_h)
    rc = wt.RECT()
    user.DrawTextW(dc, text, -1, ctypes.byref(rc), 0x400 | 0x20 | 0x800)   # CALCRECT|SINGLELINE|NOPREFIX
    w = min(max(rc.right - rc.left, 0) + 2, 1600)
    tm = (ctypes.c_long * 16)()   # TEXTMETRICW: tmHeight, tmAscent first
    gdi.GetTextMetricsW(dc, tm)
    h, ascent = max(tm[0], 1), tm[1]

    bmi = BITMAPINFOHEADER(ctypes.sizeof(BITMAPINFOHEADER), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
    bits = vp()
    bmp = gdi.CreateDIBSection(dc, ctypes.byref(bmi), 0, ctypes.byref(bits), None, 0)
    old_bmp = gdi.SelectObject(dc, bmp)
    gdi.SetTextColor(dc, 0x00FFFFFF)
    gdi.SetBkMode(dc, 1)   # TRANSPARENT
    draw = wt.RECT(1, 0, w, h)
    user.DrawTextW(dc, text, -1, ctypes.byref(draw), 0x20 | 0x800 | 0x100)   # SINGLELINE|NOPREFIX|NOCLIP
    dib = ctypes.string_at(bits, w * h * 4)
    gdi.SelectObject(dc, old_bmp)
    gdi.DeleteObject(bmp)
    gdi.SelectObject(dc, old_font)
    gdi.DeleteObject(font_h)
    gdi.DeleteDC(dc)

    stride = (w + 1) // 2
    packed = bytearray(stride * h)
    for y in range(h):
        for x in range(w):
            o = (y * w + x) * 4
            a = max(dib[o], dib[o + 1], dib[o + 2]) >> 4
            packed[y * stride + x // 2] |= a << 4 if x % 2 == 0 else a
    return w, h, ascent, bytes(packed)


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
                if face == "media":
                    feed.send_media()
                feed.send(f"face {panel} {face}")
                # let the face render (and a GIF reach a real frame)
                time.sleep(WARN_SETTLE_S if over and over[0].keys() & WARN_ALL[0].keys() else 2)
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
