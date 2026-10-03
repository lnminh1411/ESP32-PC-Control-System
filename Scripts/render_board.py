"""
Refreshes the board renders in pcb/build/renders/: one PNG per copper layer,
the top silkscreen over F.Cu, and the slanted 3D overview (also copied to
pcb/build/board_preview.png). Run after the routing chain + DRC:

    python Scripts\\render_board.py

Needs pymupdf + Pillow in the system Python; kicad-cli does the exports.
Intermediate SVGs go to K:\\scratch\\pdf. Writes K:\\scratch\\prev_fcu_crop.png
(the display-bus corner of F.Cu) for a quick look.
"""
import os
import subprocess

import pymupdf
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # repo root (worktree-safe)
KC = r"C:\Program Files\KiCad\10.0\bin\kicad-cli.exe"
BOARD = os.path.join(ROOT, r"pcb\layouts\default\default.kicad_pcb")
OUT = os.path.join(ROOT, r"pcb\build\renders")
TMP = r"K:\scratch\pdf"

ITEMS = [
    ("F.Cu", "F.Cu,Edge.Cuts", "1_top_F.Cu.png", "L1 Top (F.Cu) - display bus, daisy-chained, 45deg corners"),
    ("In1.Cu", "In1.Cu,Edge.Cuts", "2_inner1_GND.png", "L2 (In1.Cu) - GND plane"),
    ("In2.Cu", "In2.Cu,Edge.Cuts", "3_inner2_5V.png", "L3 (In2.Cu) - 5V plane"),
    ("B.Cu", "B.Cu,Edge.Cuts", "4_bottom_B.Cu.png", "L4 Bottom (B.Cu) - low-speed (X-ray)"),
    ("F.SilkS", "F.SilkS,F.Cu,Edge.Cuts", "6_silkscreen_top.png", "Top silkscreen over F.Cu"),
]

os.makedirs(TMP, exist_ok=True)
# 2-layer board (rev 2.4b): no inner layers to export; B.Cu is the GND pour
with open(BOARD, encoding="utf-8") as f:
    four_layer = '"In1.Cu"' in f.read(20000)
if not four_layer:
    ITEMS = [it for it in ITEMS if not it[0].startswith("In")]
    ITEMS = [(k, l, o, "L2 Bottom (B.Cu) - GND pour + short jumpers (X-ray)") if k == "B.Cu" else (k, l, o, t)
             for k, l, o, t in ITEMS]
os.makedirs(OUT, exist_ok=True)
font = ImageFont.truetype("arial.ttf", 44)
for key, layers, out, title in ITEMS:
    svg = os.path.join(TMP, key + ".svg")
    subprocess.run([KC, "pcb", "export", "svg", "--mode-single", "-l", layers,
                    "--page-size-mode", "2", "--exclude-drawing-sheet",
                    "--drill-shape-opt", "2", "-o", svg, BOARD],
                   check=True, capture_output=True)
    pg = pymupdf.open(svg)[0]
    z = 1800 / pg.rect.width
    pix = pg.get_pixmap(matrix=pymupdf.Matrix(z, z), alpha=True)
    im = Image.frombytes("RGBA", (pix.width, pix.height), pix.samples)
    W, H = im.size
    pad, top = 50, 90
    canvas = Image.new("RGB", (W + 2 * pad, H + pad + top), (13, 17, 23))
    canvas.paste(im, (pad, top), im)
    ImageDraw.Draw(canvas).text((pad, 20), title, fill=(230, 230, 230), font=font)
    canvas.save(os.path.join(OUT, out))
    if key == "F.Cu":
        canvas.crop((int(W * 0.55), 0, W + 2 * pad, int(H * 0.5))).save(r"K:\scratch\prev_fcu_crop.png")
    print("wrote", out)

overview = os.path.join(OUT, "3d_overview.png")
subprocess.run([KC, "pcb", "render", "--rotate", "-45,0,35", "--perspective",
                "--quality", "high", "--floor", "-w", "2400", "-h", "2400",
                "--zoom", "1.15", "-o", overview, BOARD], check=True, capture_output=True)
with open(overview, "rb") as src, open(os.path.join(ROOT, r"pcb\build\board_preview.png"), "wb") as dst:
    dst.write(src.read())
print("wrote 3d_overview.png + board_preview.png")
