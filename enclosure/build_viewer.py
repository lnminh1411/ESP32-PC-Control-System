"""Inline out/viewer_data.json into viewer_template.html -> out/viewer.html (three.js from cdnjs)."""
import os
here = os.path.dirname(os.path.abspath(__file__))
data = open(os.path.join(here, "out", "viewer_data.json"), encoding="utf-8").read()
tpl = open(os.path.join(here, "viewer_template.html"), encoding="utf-8").read()
open(os.path.join(here, "out", "viewer.html"), "w", encoding="utf-8").write(tpl.replace("__DATA__", data))
print("wrote out/viewer.html", round(len(data) / 1e6, 2), "MB data")
