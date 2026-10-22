"""
Board-wide build settings and tool paths shared by the scripts in this
folder. Not run directly.

The board is 2-layer: F.Cu carries all routing, B.Cu is a GND pour that
signals may only cross with short via jumpers, 5V/12V are routed power
tracks. The hand routes in route_spi.py assume this stackup.

Tool and scratch paths default to the original Windows setup; override them
with the environment variables named below on any other machine.
"""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # repo root (worktree-safe)
BOARD_DIR = os.path.join(ROOT, "pcb", "layouts", "default")
BOARD_PATH = os.path.join(BOARD_DIR, "default.kicad_pcb")

REVISION = "3.1"
DATE = "2026-10-03"
TITLE = "ESP32 PC Control System"

# Power nets (routed as tracks) -- net class "Power" in
# pcb/layouts/default/default.kicad_pro sets their width
POWER_NETS = ("v5", "v12")
POWER_TRACK_W, POWER_VIA_D, POWER_VIA_DRILL = 0.5, 0.8, 0.4  # mm, keep = .kicad_pro

# ---- external tools / work folders ----
KICAD_DIR = os.environ.get("KICAD_DIR", r"C:\Program Files\KiCad\10.0")
KICAD_CLI = os.path.join(KICAD_DIR, "bin", "kicad-cli.exe" if os.name == "nt" else "kicad-cli")
KICAD_FOOTPRINTS = os.environ.get("KICAD_FOOTPRINTS", os.path.join(KICAD_DIR, "share", "kicad", "footprints"))
FREEROUTING_JAR = os.environ.get("FREEROUTING_JAR", r"K:\cache\freerouting\freerouting-2.4.1.jar")
SCRATCH_DIR = os.environ.get("PCB_SCRATCH", r"K:\scratch")       # render intermediates
ARTIFACTS_DIR = os.environ.get("PCB_ARTIFACTS", r"K:\artifacts")  # fab packages
