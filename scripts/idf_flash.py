"""
Flash the firmware in firmware/ over the native USB port, hands-off.

The running firmware reboots into the ROM downloader on a 1200-baud touch;
the ROM then shows up as a USB-Serial-JTAG COM port (a different COM number).
The ESP32PcStats service opens any Espressif COM port as its serial fallback,
so it is stopped for the flash and started again afterwards (UAC prompt).

    python scripts/idf_flash.py [--port COM12] [--no-build] [--full] [--idf coredump-info]

Only the regions that changed are written: the board's MD5 of each image in
build/flasher_args.json (bootloader, partition table, app, GIF pack) is
compared first, so a code change writes the 1.7 MB app and leaves the 6 MB
GIF pack alone. --full writes everything (idf.py flash).

Run it with the ESP-IDF Python (it needs pyserial):
    C:\\Espressif\\python_env\\idf5.5_py3.12_env\\Scripts\\python.exe scripts\\idf_flash.py
"""
import argparse
import json
import os
import subprocess
import sys
import time

import serial
import serial.tools.list_ports as list_ports

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROJECT = os.path.join(ROOT, "firmware")
IDF_CMD = os.path.join(PROJECT, "idf.cmd")
BUILD = os.path.join(PROJECT, "build")
CHIP = "esp32s3"
SERVICE = "ESP32PcStats"
ESPRESSIF_VID = 0x303A
JTAG_PID = 0x1001
JTAG_INTERFACE = "MI_00"   # ROM's CDC is interface 0; the app's CDC is MI_01


def service_state():
    """'RUNNING', 'STOPPED', ... from a non-elevated sc query; None when not installed."""
    r = subprocess.run(["sc", "query", SERVICE], capture_output=True, text=True, check=False)
    for line in r.stdout.splitlines():
        if "STATE" in line:
            return line.split()[-1]
    return None


def service(action):
    """sc start/stop elevated; returns once sc has finished. Skips the UAC prompt
    when the service is missing or already in the wanted state."""
    state = service_state()
    if state is None or (action == "stop" and state == "STOPPED") or \
            (action == "start" and state in ("RUNNING", "START_PENDING")):
        return
    subprocess.run(["powershell", "-NoProfile", "-Command",
                    f"Start-Process sc.exe -Verb RunAs -Wait -WindowStyle Hidden -ArgumentList '{action}','{SERVICE}'"],
                   check=False)


def idf(*args):
    return subprocess.run(["cmd", "/c", IDF_CMD, *args]).returncode


def esptool(port, before, after, *cmd):
    """esptool in this Python (the IDF env); returns (exit code, output). Printed as it runs."""
    p = subprocess.Popen([sys.executable, "-m", "esptool", "--chip", CHIP, "-p", port, "-b", "460800",
                          "--before", before, "--after", after, *cmd],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    out = []
    for line in p.stdout:
        # progress lines, and verify_flash's own failure for regions that only need writing
        if not line.startswith(("Writing at 0x", "A fatal error occurred: Verify failed")):
            print(line, end="")
        out.append(line)
    return p.wait(), "".join(out)


def flash_changed(port):
    """Writes the images whose MD5 on the board differs; None when the check
    itself failed (the caller falls back to a full flash)."""
    with open(os.path.join(BUILD, "flasher_args.json")) as f:
        fa = json.load(f)
    files = sorted(fa["flash_files"].items(), key=lambda kv: int(kv[0], 16))
    pairs = [x for addr, name in files for x in (addr, os.path.normpath(os.path.join(BUILD, name)))]
    settings = fa["write_flash_args"]   # verify_flash patches the bootloader header with them, as write_flash does
    # the stub stays up for the write that follows
    _, out = esptool(port, "default_reset", "no_reset_stub", "verify_flash", *settings, *pairs)
    verdicts = [l for l in out.splitlines() if l.startswith("-- verify")]
    if len(verdicts) != len(files):
        return None
    changed = [(addr, pairs[2 * i + 1]) for i, (addr, _) in enumerate(files) if "OK" not in verdicts[i]]
    if not changed:
        print("Nothing changed on the board; restarting it")
        return esptool(port, "no_reset", "hard_reset", "chip_id")[0]
    names = ", ".join(os.path.basename(f) for _, f in changed)
    print(f"Writing {names}; {len(files) - len(changed)} unchanged region(s) skipped")
    return esptool(port, "no_reset", "hard_reset", "write_flash", *settings,
                   *[x for pair in changed for x in pair])[0]


def rom_port(exclude):
    for p in list_ports.comports():
        if p.vid == ESPRESSIF_VID and p.pid == JTAG_PID and p.device not in exclude and JTAG_INTERFACE in (p.hwid or "").upper() + (p.location or "").upper():
            return p.device
    for p in list_ports.comports():   # hwid formats vary: fall back to any new Espressif port
        if p.vid == ESPRESSIF_VID and p.device not in exclude:
            return p.device
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("--port", default="COM12", help="the running firmware's CDC port")
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--full", action="store_true", help="write every region, changed or not")
    ap.add_argument("--idf", default="flash", help="idf.py action to run on the ROM port (e.g. coredump-info)")
    args = ap.parse_args()

    if args.idf == "flash" and not args.no_build and idf("build"):
        sys.exit("build failed")

    service("stop")
    try:
        try:
            s = serial.Serial(args.port, 1200)
            s.dtr = False
            time.sleep(0.2)
            s.close()
        except serial.SerialException as e:
            print(f"1200-baud touch on {args.port} failed ({e}); looking for a board already in download mode")
        port = None
        for _ in range(200):   # up to 20 s
            port = rom_port({args.port})
            if port:
                break
            time.sleep(0.1)
        if not port:
            sys.exit("no download-mode port appeared")
        print(f"ROM downloader on {port}")
        time.sleep(1)   # let Windows finish installing the port
        rc = None
        if args.idf == "flash" and not args.full:
            rc = flash_changed(port)
            if rc is None:
                print("MD5 check failed; flashing everything")
        if rc is None:
            rc = idf("-p", port, *args.idf.split())
    finally:
        service("start")
    sys.exit(rc)


if __name__ == "__main__":
    main()
