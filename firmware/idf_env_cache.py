"""Saves what ESP-IDF's export.bat changed in the environment as a .cmd file
that idf.cmd calls instead of export.bat (export.bat takes ~3.5 s per call).

    idf_env_cache.py <`set` dump taken before export.bat> <cache .cmd to write>

Run by idf.cmd right after export.bat, with ESP-IDF's Python. Writes nothing
(exit 1) when a value can't be replayed safely by cmd, so idf.cmd keeps
running export.bat.
"""
import os
import sys

before_file, out = sys.argv[1], sys.argv[2]
before = {}
with open(before_file, encoding="mbcs", errors="replace") as f:   # cmd writes `set` in the ANSI codepage
    for line in f:
        key, sep, value = line.rstrip("\r\n").partition("=")
        if sep:
            before[key.upper()] = value

lines = []
for key, value in os.environ.items():
    if key.upper() in ("PATH", "IDF_OLD_PATH", "PROMPT"):
        continue
    if before.get(key.upper()) != value:
        if "%" in value or '"' in value:
            sys.exit(1)
        lines.append(f'set "{key}={value}"')

# export.bat only prepends to PATH; store the prefix so the caller's PATH stays live
old, new = before.get("PATH", ""), os.environ["PATH"]
if not old or not new.endswith(old):
    sys.exit(1)
prefix = new[: len(new) - len(old)]
if "%" in prefix or '"' in prefix:
    sys.exit(1)
lines.append(f'set "PATH={prefix}%PATH%"')

os.makedirs(os.path.dirname(out), exist_ok=True)
with open(out + ".tmp", "w", encoding="mbcs", newline="\r\n") as f:
    f.write("@echo off\n" + "\n".join(lines) + "\n")
os.replace(out + ".tmp", out)
