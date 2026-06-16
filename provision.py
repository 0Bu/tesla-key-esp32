#!/usr/bin/env python3
"""
provision.py — Write WiFi credentials and VIN to the ESP32 NVS partition
without recompiling the firmware.

Usage:
    python provision.py --port /dev/tty.usbserial-0001 \
        --ssid "MyNetwork" --password "secret" --vin "5YJ3E1EA1JF000001"

Requires ESP-IDF to be installed and the IDF_PATH environment variable set
(run `get_idf` or `. ~/esp/esp-idf/export.sh` first).
"""

import argparse
import csv
import os
import subprocess
import sys
import tempfile

NVS_NAMESPACE = "tesla_cfg"
NVS_OFFSET    = "0x9000"
NVS_SIZE_INT  = 0x6000    # 24 KB — matches partitions.csv


def get_idf_path() -> str:
    idf = os.environ.get("IDF_PATH")
    if not idf:
        print("ERROR: IDF_PATH not set. Run 'get_idf' ('. ~/esp/esp-idf/export.sh') first.",
              file=sys.stderr)
        sys.exit(1)
    return idf


def get_nvs_gen_script(idf_path: str) -> str:
    script = os.path.join(
        idf_path,
        "components", "nvs_flash", "nvs_partition_generator", "nvs_partition_gen.py"
    )
    if not os.path.exists(script):
        print(f"ERROR: nvs_partition_gen.py not found at {script}", file=sys.stderr)
        sys.exit(1)
    return script


def generate_nvs_csv(ssid: str, password: str, vin: str, ble_mac: str) -> str:
    """Write a temporary NVS CSV and return its path."""
    rows = [
        ["key",         "type",      "encoding", "value"],
        [NVS_NAMESPACE, "namespace", "",         ""],
        ["wifi_ssid",   "data",      "string",   ssid],
        ["wifi_pass",   "data",      "string",   password],
    ]
    if vin:
        rows.append(["vin",     "data", "string", vin])
    if ble_mac:
        rows.append(["ble_mac", "data", "string", ble_mac])

    tmp = tempfile.NamedTemporaryFile(
        suffix=".csv", delete=False, mode="w", newline=""
    )
    csv.writer(tmp).writerows(rows)
    tmp.close()
    return tmp.name


def run(cmd: list):
    print("$", " ".join(str(c) for c in cmd))
    subprocess.run([str(c) for c in cmd], check=True)


def main():
    parser = argparse.ArgumentParser(
        description="Provision esp32-tesla-key NVS config (WiFi + VIN)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("--port",     "-p", required=True,
                        help="Serial port, e.g. /dev/tty.usbserial-0001 or COM3")
    parser.add_argument("--ssid",     "-s", required=True, help="WiFi SSID")
    parser.add_argument("--password", "-P", required=True, help="WiFi password")
    parser.add_argument("--vin",      "-v", default="",
                        help="Tesla VIN (17 chars, optional if already set)")
    parser.add_argument("--ble-mac",  "-m", default="",
                        help="Tesla BLE MAC AA:BB:CC:DD:EE:FF (optional, auto-discovered otherwise)")
    parser.add_argument("--baud",     "-b", type=int, default=921600,
                        help="Flash baud rate (default 921600)")
    parser.add_argument("--dry-run",        action="store_true",
                        help="Generate binary but do not flash")
    args = parser.parse_args()

    if args.vin and len(args.vin) != 17:
        print(f"ERROR: VIN must be 17 characters, got {len(args.vin)}", file=sys.stderr)
        sys.exit(1)

    idf_path    = get_idf_path()
    nvs_gen     = get_nvs_gen_script(idf_path)
    esptool_cmd = [sys.executable, "-m", "esptool"]

    # 1. Generate NVS CSV
    csv_path = generate_nvs_csv(args.ssid, args.password, args.vin, args.ble_mac)
    bin_path = csv_path.replace(".csv", ".bin")
    print(f"CSV: {csv_path}")

    try:
        # 2. CSV → NVS binary
        run([sys.executable, nvs_gen,
             "generate",
             csv_path,
             bin_path,
             hex(NVS_SIZE_INT)])

        if args.dry_run:
            import shutil
            out = "nvs_config.bin"
            shutil.copy(bin_path, out)
            print(f"\nDry run — NVS binary saved to ./{out}")
            print(f"Flash manually: esptool.py --port {args.port} write_flash {NVS_OFFSET} {out}")
            return

        # 3. Flash NVS partition only (app partition untouched)
        run([*esptool_cmd,
             "--port", args.port,
             "--baud", str(args.baud),
             "write_flash",
             NVS_OFFSET, bin_path])

        print("\n✓ Provisioning complete.")
        print("  Reset the ESP32 (press RESET button) to apply the new credentials.")

    finally:
        for f in (csv_path, bin_path):
            if os.path.exists(f):
                os.unlink(f)


if __name__ == "__main__":
    main()
