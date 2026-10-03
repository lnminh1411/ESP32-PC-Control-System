"""
Refreshes the board renders in pcb/build/renders/: one PNG per copper layer,
the top silkscreen over F.Cu, and the slanted 3D overview (also copied to
pcb/build/board_preview.png). Run after the routing chain + DRC:

    python Scripts\\render_board.py

Every render carries the board measurements in mm, cm and inches (read from
the Edge.Cuts outline): dimension lines on the 2D views, a caption box on the
3D view (perspective would make dimension lines misleading).

Needs pymupdf + Pillow in the system Python; kicad-cli does the exports.
Intermediate SVGs go to K:\\scratch\\pdf. Writes K:\\scratch\\prev_fcu_crop.png
(the display-bus corner of F.Cu) for a quick look.
"""
import os
import re
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
    ("F.SilkS", "F.Cu,F.SilkS,Edge.Cuts", "6_silkscreen_top.png", "Top silkscreen over F.Cu (silk printed on top, as fabbed)"),
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
dim_font = ImageFont.truetype("arial.ttf", 34)

# ---- board measurements (read from the Edge.Cuts lines in the board file) --
with open(BOARD, encoding="utf-8") as f:
    pcb_text = f.read()
pts = []
for chunk in pcb_text.split("(gr_line")[1:]:
    chunk = chunk.split("(gr_")[0]
    m = re.search(r"\(start ([-\d.]+) ([-\d.]+)\)\s*\(end ([-\d.]+) ([-\d.]+)\)", chunk)
    if m and '(layer "Edge.Cuts")' in chunk:
        pts += [(float(m[1]), float(m[2])), (float(m[3]), float(m[4]))]
ux = sorted({round(x, 3) for x, _ in pts})
uy = sorted({round(y, 3) for _, y in pts})
BX0, BX1, BY0, BY1 = ux[0], ux[-1], uy[0], uy[-1]


def dim_label(mm):
    return f"{mm:.1f} mm  |  {mm / 10:.2f} cm  |  {mm / 25.4:.2f} in"


def size_label():
    w, h = BX1 - BX0, BY1 - BY0
    return (f"Board {w:.1f} x {h:.1f} mm  |  {w / 10:.2f} x {h / 10:.2f} cm  |  "
            f"{w / 25.4:.2f} x {h / 25.4:.2f} in")


DIM_COL = (255, 200, 60)
# dimensions drawn on every 2D render: (axis, from, to, offset side, at, name).
# The overall board size is in the caption; the lines give the L-shape's
# parts: main body width (top) + length (left, down to the step), display
# strip width (bottom) + its length below the main body (right).
if len(ux) > 2 and len(uy) > 2:
    DIMS = [("h", BX0, BX1, "top", BY0, "Main body width"),
            ("v", BY0, uy[1], "left", BX0, "Main body length"),
            ("h", ux[1], BX1, "bottom", BY1, "Strip width"),
            ("v", uy[1], BY1, "right", BX1, "Strip length (below main body)")]
else:   # plain rectangle
    DIMS = [("h", BX0, BX1, "top", BY0, "Width"), ("v", BY0, BY1, "right", BX1, "Length")]


def draw_dims(canvas, ox, oy, s):
    """dimension lines with end ticks + arrows, labelled with the part's name
    and its size in mm / cm / in; (ox, oy) = canvas pixel of the board's
    (BX0, BY0), s = px per mm"""
    d = ImageDraw.Draw(canvas)
    gap = 70                                   # line offset from the board edge
    for axis, a, b, side, at, name in DIMS:
        txt = dim_label(b - a)
        if axis == "h":
            y = oy + (at - BY0) * s + (-gap if side == "top" else gap)
            xa, xb = ox + (a - BX0) * s, ox + (b - BX0) * s
            edge = oy + (at - BY0) * s
            for x in (xa, xb):
                d.line([(x, edge), (y < edge and (x, y - 15) or (x, y + 15))], fill=DIM_COL, width=2)
            d.line([(xa, y), (xb, y)], fill=DIM_COL, width=3)
            for x, k in ((xa, 1), (xb, -1)):
                d.polygon([(x, y), (x + 18 * k, y - 8), (x + 18 * k, y + 8)], fill=DIM_COL)
            ty = y - 92 if side == "top" else y + 10
            for i, line in enumerate((name, txt)):
                tw = d.textlength(line, font=dim_font)
                tx = min(max((xa + xb - tw) / 2, 10), canvas.width - tw - 10)
                d.text((tx, ty + 44 * i), line, fill=DIM_COL, font=dim_font)
        else:
            x = ox + (at - BX0) * s + (gap if side == "right" else -gap)
            ya, yb = oy + (a - BY0) * s, oy + (b - BY0) * s
            edge = ox + (at - BX0) * s
            for yy in (ya, yb):
                d.line([(edge, yy), (x > edge and (x + 15, yy) or (x - 15, yy))], fill=DIM_COL, width=2)
            d.line([(x, ya), (x, yb)], fill=DIM_COL, width=3)
            for yy, k in ((ya, 1), (yb, -1)):
                d.polygon([(x, yy), (x - 8, yy + 18 * k), (x + 8, yy + 18 * k)], fill=DIM_COL)
            tw = int(max(d.textlength(t, font=dim_font) for t in (name, txt)))
            lab = Image.new("RGBA", (tw + 4, 88), (0, 0, 0, 0))
            ld = ImageDraw.Draw(lab)
            for i, line in enumerate((name, txt)):   # centred, name on the outer line
                lw = ld.textlength(line, font=dim_font)
                row = i if side == "right" else 1 - i
                ld.text(((tw + 4 - lw) / 2, 44 * row), line, fill=DIM_COL, font=dim_font)
            lab = lab.rotate(90, expand=True)
            lx = x + 10 if side == "right" else x - 10 - lab.width
            canvas.paste(lab, (int(lx), int((ya + yb - lab.height) / 2)), lab)


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
    # page = board bounding box (--page-size-mode 2): margins hold the dims
    padl, padr, top, bottom = 200, 200, 320, 200
    canvas = Image.new("RGB", (W + padl + padr, H + top + bottom), (13, 17, 23))
    canvas.paste(im, (padl, top), im)
    ImageDraw.Draw(canvas).text((padl, 20), title, fill=(230, 230, 230), font=font)
    ImageDraw.Draw(canvas).text((padl, 80), size_label(), fill=DIM_COL, font=dim_font)
    draw_dims(canvas, padl, top, W / (BX1 - BX0))
    canvas.save(os.path.join(OUT, out))
    if key == "F.Cu":
        canvas.crop((int(W * 0.55), 0, canvas.width, int(H * 0.5))).save(r"K:\scratch\prev_fcu_crop.png")
    print("wrote", out)

overview = os.path.join(OUT, "3d_overview.png")
subprocess.run([KC, "pcb", "render", "--rotate", "-45,0,35", "--perspective",
                "--quality", "high", "--floor", "-w", "2400", "-h", "2400",
                "--zoom", "1.15", "-o", overview, BOARD], check=True, capture_output=True)
# 3D view is in perspective, so no dimension lines -- a size caption instead
ov = Image.open(overview).convert("RGB")
d = ImageDraw.Draw(ov)
cap = [size_label()] + [f"{name}: {dim_label(b - a)}" for _, a, b, _, _, name in DIMS]
lh = 48
d.rectangle([(30, 30), (60 + max(d.textlength(c, font=dim_font) for c in cap), 50 + lh * len(cap))],
            fill=(13, 17, 23), outline=DIM_COL, width=3)
for i, c in enumerate(cap):
    d.text((45, 45 + lh * i), c, fill=DIM_COL if i == 0 else (230, 230, 230), font=dim_font)
ov.save(overview)
ov.save(os.path.join(ROOT, r"pcb\build\board_preview.png"))
print("wrote 3d_overview.png + board_preview.png")
