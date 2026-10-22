"""
Exports the fab package for the carrier board: Gerbers (copper, mask, paste,
silkscreen, Edge.Cuts) + Excellon drills (PTH/NPTH split, Gerber X2 drill
maps) + the .gbrjob, named after the project title instead of the KiCad
file name ("default"), zipped for upload.

    python scripts/export_gerbers.py

Output: <ARTIFACTS_DIR>/<TITLE>_rev<REVISION>_gerbers/ (+ .zip next to it),
TITLE/REVISION from board_config.py. The folder is cleared and rewritten
each run. Plots the zone fill stored in the board -- the one the routing
chain made and DRC checked (no refill: a kicad-cli refill comes out
slightly different from the chain's fill). Run after the routing chain +
DRC pass.
"""
import glob
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from board_config import ARTIFACTS_DIR, BOARD_PATH as BOARD, KICAD_CLI as KC, REVISION, TITLE  # noqa: E402

OLD = "default"                                   # KiCad names outputs after the board file
NAME = TITLE.replace(" ", "_")                    # ESP32_PC_Control_System
OUT = os.path.join(ARTIFACTS_DIR, f"{NAME}_rev{REVISION}_gerbers")
LAYERS = "F.Cu,B.Cu,F.Paste,B.Paste,F.Silkscreen,B.Silkscreen,F.Mask,B.Mask,Edge.Cuts"

shutil.rmtree(OUT, ignore_errors=True)
os.makedirs(OUT)
subprocess.run([KC, "pcb", "export", "gerbers", "-l", LAYERS, "-o", OUT, BOARD], check=True, capture_output=True)
subprocess.run([KC, "pcb", "export", "drill", "--excellon-separate-th", "--generate-map",
                "--map-format", "gerberx2", "-o", OUT + os.sep, BOARD], check=True, capture_output=True)

# rename default-* -> <NAME>-*, and point every cross-reference at the new names:
# the .gbrjob lists the files, and each Gerber's X2 header carries the project id
for path in glob.glob(os.path.join(OUT, OLD + "-*")):
    with open(path, encoding="utf-8", errors="surrogateescape", newline="") as f:  # keep CRLF
        text = f.read()
    text = (text.replace(f'"{OLD}-', f'"{NAME}-')
                .replace(f"TF.ProjectId,{OLD},", f"TF.ProjectId,{NAME},")
                .replace(f'"Name": "{OLD}"', f'"Name": "{NAME}"'))
    with open(path, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
        f.write(text)
    os.rename(path, os.path.join(OUT, NAME + os.path.basename(path)[len(OLD):]))

zip_path = shutil.make_archive(OUT, "zip", OUT)
for name in sorted(os.listdir(OUT)):
    print(f"  {name:45s} {os.path.getsize(os.path.join(OUT, name)):>8d}")
print("wrote", zip_path)
