"""
Flash firmware/carrier_idf over the native USB port, hands-off.

The running firmware reboots into the ROM downloader on a 1200-baud touch;
the ROM then shows up as a USB-Serial-JTAG COM port (a different COM number).
The ESP32PcStats service opens any Espressif COM port as its serial fallback,
so it is stopped for the flash and started again afterwards (UAC prompt).

    python scripts/idf_flash.py [--port COM12] [--no-build]

Run it with the ESP-IDF Python (it needs pyserial):
    C:\\Espressif\\python_env\\idf5.5_py3.12_env\\Scripts\\python.exe scripts\\idf_flash.py
"""
import argparse
import os
import subprocess
import sys
import time

import serial
import serial.tools.list_ports as list_ports

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROJECT = os.path.join(ROOT, "firmware", "carrier_idf")
IDF_CMD = os.path.join(PROJECT, "idf.cmd")
SERVICE = "ESP32PcStats"
ESPRESSIF_VID = 0x303A
JTAG_PID = 0x1001
JTAG_INTERFACE = "MI_00"   # ROM's CDC is interface 0; the app's CDC is MI_01


def service(action):
    """sc start/stop elevated; returns once sc has finished."""
    subprocess.run(["powershell", "-NoProfile", "-Command",
                    f"Start-Process sc.exe -Verb RunAs -Wait -WindowStyle Hidden -ArgumentList '{action}','{SERVICE}'"],
                   check=False)


def idf(*args):
    return subprocess.run(["cmd", "/c", IDF_CMD, *args]).returncode


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
    args = ap.parse_args()

    if not args.no_build and idf("build"):
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
        for _ in range(40):
            time.sleep(0.5)
            port = rom_port({args.port})
            if port:
                break
        if not port:
            sys.exit("no download-mode port appeared")
        print(f"ROM downloader on {port}")
        time.sleep(1)   # let Windows finish installing the port
        rc = idf("-p", port, "flash")
    finally:
        service("start")
    sys.exit(rc)


if __name__ == "__main__":
    main()
