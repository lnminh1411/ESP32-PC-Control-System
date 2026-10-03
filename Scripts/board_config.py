"""
Board-wide build settings shared by the layout chain (imported by
make_blank_pcb.py, layout_board.py, add_pours.py, route_spi.py and
route_board.py).

COPPER_LAYERS picks the stackup variant:
  4 -- rev 2.4: L1 signal, In1 solid GND plane, In2 5V plane, L4 low-speed.
  2 -- rev 2.4b (cost-down): F.Cu carries all routing, B.Cu is a GND pour
       that signals may only cross with short via jumpers; 5V is a routed
       power track instead of a plane.
Changing it means rebuilding the whole chain from make_blank_pcb.py.
"""
COPPER_LAYERS = 2
REVISION = "2.4b"
DATE = "2026-10-03"
TITLE = "ESP32 PC Controller System"

# Power nets (routed as tracks on the 2-layer board) -- net class "Power"
# in pcb/layouts/default/default.kicad_pro sets their width
POWER_NETS = ("v5", "v12")
POWER_TRACK_W, POWER_VIA_D, POWER_VIA_DRILL = 0.5, 0.8, 0.4  # mm, keep = .kicad_pro

