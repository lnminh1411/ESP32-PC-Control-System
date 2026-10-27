#!/usr/bin/env python3
"""Upload dashboard layout JSON to ESP32 PC Controller over Serial or HTTP."""

import argparse
import base64
import http.client
import json
import os
import re
import sys
import time
import urllib.parse
import serial


def find_ota_pass(repo_root):
    candidates = [   # settings.h; secrets.h is its old name
        os.path.join(repo_root, "firmware", "main", "settings.h"),
        os.path.join(repo_root, "firmware", "main", "secrets.h"),
    ]
    for path in candidates:
        if os.path.isfile(path):
            try:
                with open(path, "r", encoding="utf-8") as f:
                    for line in f:
                        m = re.search(r'#define\s+OTA_PASS\s+"([^"]*)"', line)
                        if m:
                            return m.group(1)
            except OSError:
                pass
    return ""


def validate_layout_file(path):
    if not os.path.isfile(path):
        sys.stderr.write(f"Error: file not found: {path}\n")
        sys.exit(1)

    file_size = os.path.getsize(path)
    if file_size > 16384:
        sys.stderr.write(f"Error: layout file exceeds 16 KB limit ({file_size} bytes)\n")
        sys.exit(1)

    with open(path, "rb") as f:
        data_bytes = f.read()

    try:
        data = json.loads(data_bytes.decode("utf-8"))
    except Exception as e:
        sys.stderr.write(f"Error: invalid JSON in {path}: {e}\n")
        sys.exit(1)

    if not isinstance(data, dict):
        sys.stderr.write("Error: root JSON must be an object\n")
        sys.exit(1)

    pages = data.get("pages")
    if not isinstance(pages, list) or len(pages) == 0:
        sys.stderr.write("Error: 'pages' array missing or empty\n")
        sys.exit(1)

    if len(pages) > 6:
        sys.stderr.write(f"Warning: {len(pages)} pages provided; board only displays up to 6\n")

    for p_idx, page in enumerate(pages[:6]):
        if not isinstance(page, dict):
            sys.stderr.write(f"Error: page {p_idx} must be an object\n")
            sys.exit(1)
        widgets = page.get("widgets", [])
        if not isinstance(widgets, list):
            sys.stderr.write(f"Error: page {p_idx} 'widgets' must be an array\n")
            sys.exit(1)
        if len(widgets) > 32:
            sys.stderr.write(f"Warning: page {p_idx} has {len(widgets)} widgets (limit 32)\n")

    return data_bytes


def upload_serial(port, data_bytes, baud=115200):
    b64_str = base64.b64encode(data_bytes).decode("ascii")
    chunk_size = 240
    chunks = [b64_str[i:i + chunk_size] for i in range(0, len(b64_str), chunk_size)]

    print(f"Connecting to {port} at {baud} baud...")
    try:
        ser = serial.Serial(port, baudrate=baud, timeout=0.2)
    except Exception as e:
        sys.stderr.write(f"Error opening serial port {port}: {e}\n")
        sys.exit(1)

    time.sleep(0.1)
    ser.reset_input_buffer()

    def send_cmd(cmd):
        ser.write((cmd + "\n").encode("utf-8"))
        ser.flush()
        time.sleep(0.02)
        while ser.in_waiting:
            line = ser.readline().decode("utf-8", errors="replace").strip()
            if line:
                print(line)

    print("Sending layout begin...")
    send_cmd("layout begin")

    for chunk in chunks:
        send_cmd(f"layout data {chunk}")

    print("Sending layout end...")
    send_cmd("layout end")

    # Read replies
    deadline = time.time() + 2.0
    while time.time() < deadline:
        if ser.in_waiting:
            line = ser.readline().decode("utf-8", errors="replace").strip()
            if line:
                print(line)
        else:
            time.sleep(0.05)

    ser.close()


def upload_http(host, password, data_bytes):
    if host.startswith("http://") or host.startswith("https://"):
        parsed = urllib.parse.urlparse(host)
        host = parsed.netloc

    port = 80
    if ":" in host:
        host, port_str = host.split(":", 1)
        port = int(port_str)

    print(f"Target: {host}:{port}")
    print(f"Uploading layout ({len(data_bytes)} bytes)...")

    conn = http.client.HTTPConnection(host, port=port, timeout=15)
    try:
        conn.putrequest("PUT", "/layout")
        conn.putheader("Host", host)
        conn.putheader("User-Agent", "layout_upload.py")
        conn.putheader("X-OTA-Pass", password if password else "")
        conn.putheader("Content-Length", str(len(data_bytes)))
        conn.putheader("Content-Type", "application/json")
        conn.endheaders()
        conn.send(data_bytes)

        resp = conn.getresponse()
        body = resp.read().decode("utf-8", errors="replace")
        if resp.status == 200:
            print("Upload OK")
        else:
            sys.stderr.write(f"Upload failed: HTTP {resp.status} {resp.reason}\n")
            if body:
                sys.stderr.write(f"{body}\n")
            sys.exit(1)
    except Exception as e:
        sys.stderr.write(f"Error during HTTP upload: {e}\n")
        sys.exit(1)
    finally:
        conn.close()


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.dirname(script_dir)

    parser = argparse.ArgumentParser(description="Upload dashboard layout JSON to ESP32 PC Controller.")
    parser.add_argument("file", help="Path to layout JSON file")
    parser.add_argument("--port", default=None, help="Serial port (e.g. COM12)")
    parser.add_argument("--host", default=None, help="Target hostname or IP address (e.g. esp32-pc-control.local)")
    parser.add_argument("--password", default=None, help="OTA password for HTTP PUT (default: read from settings.h)")
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate (default: 115200)")
    args = parser.parse_args()

    data_bytes = validate_layout_file(args.file)

    if args.port and args.host:
        sys.stderr.write("Error: specify either --port or --host, not both\n")
        sys.exit(1)

    if args.host:
        password = args.password if args.password is not None else find_ota_pass(repo_root)
        upload_http(args.host, password, data_bytes)
    else:
        port = args.port if args.port is not None else "COM12"
        upload_serial(port, data_bytes, baud=args.baud)


if __name__ == "__main__":
    main()
