#!/usr/bin/env python3
"""Upload firmware binary to ESP32 carrier board over HTTP OTA."""

import argparse
import http.client
import os
import re
import sys
import time
import urllib.parse


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


def parse_args():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.dirname(script_dir)
    default_bin = os.path.join(repo_root, "firmware", "build", "carrier.bin")

    parser = argparse.ArgumentParser(description="Upload firmware over HTTP OTA.")
    parser.add_argument(
        "--host",
        default="esp32-pc-control.local",
        help="Target hostname or IP address (default: esp32-pc-control.local)",
    )
    parser.add_argument(
        "--password",
        default=None,
        help="OTA password (default: read from OTA_PASS in settings.h)",
    )
    parser.add_argument(
        "bin",
        nargs="?",
        default=default_bin,
        help=f"Path to .bin firmware image (default: {default_bin})",
    )
    args = parser.parse_args()

    if args.password is None:
        args.password = find_ota_pass(repo_root)

    return args


def upload_ota(host, password, bin_path):
    if not os.path.isfile(bin_path):
        sys.stderr.write(f"Error: binary file not found: {bin_path}\n")
        sys.exit(1)

    file_size = os.path.getsize(bin_path)
    if file_size == 0:
        sys.stderr.write("Error: binary file is empty\n")
        sys.exit(1)

    # Clean host if URL was provided
    if host.startswith("http://") or host.startswith("https://"):
        parsed = urllib.parse.urlparse(host)
        host = parsed.netloc

    port = 80
    if ":" in host:
        host, port_str = host.split(":", 1)
        port = int(port_str)

    print(f"Target: {host}:{port}")
    print(f"Firmware: {bin_path} ({file_size} bytes)")

    conn = http.client.HTTPConnection(host, port=port, timeout=30)
    try:
        conn.putrequest("POST", "/ota")
        conn.putheader("Host", host)
        conn.putheader("User-Agent", "ota_upload.py")
        conn.putheader("X-OTA-Pass", password)
        conn.putheader("Content-Length", str(file_size))
        conn.putheader("Content-Type", "application/octet-stream")
        conn.endheaders()

        chunk_size = 4096
        sent = 0
        start_time = time.time()
        with open(bin_path, "rb") as f:
            while True:
                chunk = f.read(chunk_size)
                if not chunk:
                    break
                conn.send(chunk)
                sent += len(chunk)
                if sent % 65536 < chunk_size or sent == file_size:   # progress every 64 KB, not every chunk
                    pct = (sent / file_size) * 100
                    sys.stdout.write(f"\rUploading: {sent}/{file_size} bytes ({pct:.1f}%)")
                    sys.stdout.flush()

        elapsed = time.time() - start_time
        speed_kb = (sent / 1024.0) / elapsed if elapsed > 0 else 0
        print(f"\nUpload complete in {elapsed:.1f}s ({speed_kb:.1f} kB/s). Waiting for reply...")

        resp = conn.getresponse()
        reply_body = resp.read().decode("utf-8", errors="replace").strip()
        print(f"Server response: {resp.status} {resp.reason}")
        if reply_body:
            print(f"Reply: {reply_body}")

        if resp.status == 200:
            print("OTA update successful. Target is rebooting.")
        else:
            print(f"OTA update failed with status {resp.status}.")
            sys.exit(1)

    except OSError as err:
        sys.stderr.write(f"Connection error: {err}\n")
        sys.exit(1)
    finally:
        conn.close()


def main():
    args = parse_args()
    upload_ota(args.host, args.password, args.bin)


if __name__ == "__main__":
    main()
