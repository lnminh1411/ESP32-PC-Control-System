"""
Dump every through-hole pad of the carrier board to enclosure/pads.json (board mm), for the
pin-tail wells in the block. Run with KiCad's Python:

    "C:\Program Files\KiCad\10.0\bin\python.exe" enclosure/dump_pads.py
"""
import json, os
import pcbnew

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
board = pcbnew.LoadBoard(os.path.join(ROOT, "pcb", "layouts", "default", "default.kicad_pcb"))
pads = []
for fp in board.GetFootprints():
    for pad in fp.Pads():
        if pad.GetAttribute() == pcbnew.PAD_ATTRIB_PTH:
            p = pad.GetPosition()
            pads.append([round(p.x / 1e6, 3), round(p.y / 1e6, 3)])
with open(os.path.join(ROOT, "enclosure", "pads.json"), "w") as f:
    json.dump(pads, f)
print(len(pads), "through-hole pads")
