"""
Writes simple VRML fit-check models into pcb/footprints/ESP32PCC.3dshapes/:

  Boost_CKCS_BS01_fit.wrl -- the BS01 boost on its 4 single male pins:
      2.5mm header plastic, module PCB + parts envelope of BOOST_H on top,
      pins trimmed under the board. Referenced by the Boost_CKCS_BS01 footprint.
  DevKit_N16R8_envelope.wrl -- the plugged-in ESP32-S3 DevKit as a slab at its
      real height on the sockets (8.5mm socket + 2.5mm DevKit header plastic),
      attached to U2 by layout_board.py. Only for seeing what fits under it.

    python scripts/make_fit_models.py

Geometry in footprint-local mm with Y flipped (KiCad's 3D Y points up);
KiCad scales VRML by 2.54 (1 unit = 0.1"), so everything is divided by 2.54.
Re-run after changing BOOST_H.
"""
import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # repo root (worktree-safe)

OUT = os.path.join(ROOT, "pcb", "footprints", "ESP32PCC.3dshapes")
U = 2.54
HEADER_PLASTIC = 2.5    # standard 2.54mm male header spacer
BOOST_PCB = 1.0         # module PCB thickness (approx.)
BOOST_H = 4.0           # module height above its PCB bottom, user-measured ~0.4cm
SOCKET_H = 8.5          # PinSocket_1x22 vertical body height
DEVKIT_SPACER = 2.5     # DevKit's own male header plastic
DEVKIT_PCB = 1.6


def box(cx, cy, cz, sx, sy, sz, rgb, transp=0.0):
    r, g, b = rgb
    return (f"Transform {{ translation {cx/U:.4f} {cy/U:.4f} {cz/U:.4f} children [ "
            f"Shape {{ appearance Appearance {{ material Material {{ diffuseColor {r} {g} {b} "
            f"transparency {transp} }} }} geometry Box {{ size {sx/U:.4f} {sy/U:.4f} {sz/U:.4f} }} }} ] }}\n")


def write(name, body):
    with open(os.path.join(OUT, name), "w", encoding="utf-8") as f:
        f.write("#VRML V2.0 utf8\n" + body)
    print("wrote", name)


# ---- boost: pads at local (0,0) (0,-3.9) (17.75,-7.9) (17.75,0) -> model Y flipped
pads = [(0, 0), (0, 3.9), (17.75, 7.9), (17.75, 0)]
b = ""
for x, y in pads:
    b += box(x, y, HEADER_PLASTIC / 2, 2.5, 2.5, HEADER_PLASTIC, (0.1, 0.1, 0.1))
    pin_lo, pin_hi = -1.6 - 1.5, HEADER_PLASTIC + 3.0   # trimmed below, 3mm short end up
    b += box(x, y, (pin_lo + pin_hi) / 2, 0.64, 0.64, pin_hi - pin_lo, (0.85, 0.7, 0.3))
# module PCB (fab rect X[-2.15,19.85] Y[-9.55,1.45] -> model Y[-1.45,9.55])
z0 = HEADER_PLASTIC
b += box(8.85, 4.05, z0 + BOOST_PCB / 2, 22.0, 11.0, BOOST_PCB, (0.05, 0.35, 0.6))
parts_h = BOOST_H - BOOST_PCB
b += box(8.85, 4.05, z0 + BOOST_PCB + parts_h / 2, 20.0, 9.0, parts_h, (0.5, 0.5, 0.5), 0.3)
write("Boost_CKCS_BS01_fit.wrl", b)

# ---- DevKit envelope, in U2's local frame (U2 = mcu.right, rotated 180deg on
# the board; pads at local (0, 2.54*(n-1)), the other row at local x=-25.4)
d = ""
row_cy = -2.54 * 10.5                       # model Y of the row's middle
for rx in (0.0, -25.4):
    d += box(rx, row_cy, SOCKET_H + DEVKIT_SPACER / 2, 2.5, 22 * 2.54, DEVKIT_SPACER, (0.1, 0.1, 0.1))
pcb_z = SOCKET_H + DEVKIT_SPACER
# board 28 x 70mm: USB-C end 6.66mm past pin 22, antenna end 10mm past pin 1
d += box(-12.7, -25.0, pcb_z + DEVKIT_PCB / 2, 28.0, 70.0, DEVKIT_PCB, (0.15, 0.15, 0.2), 0.45)
write("DevKit_N16R8_envelope.wrl", d)
print(f"boost top {HEADER_PLASTIC + BOOST_H:.1f}mm, pin tips {HEADER_PLASTIC + 3.0:.1f}mm, "
      f"DevKit underside {pcb_z:.1f}mm -> clearance {pcb_z - HEADER_PLASTIC - BOOST_H:.1f}mm")
