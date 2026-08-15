#!/usr/bin/env python3
"""Safely provision tesla-key-esp32 through its transactional HTTP APIs.

This tool deliberately does *not* generate or flash an NVS partition image.  A
generated image covers the complete 0x6000-byte NVS partition and would erase
the vehicle private key, pairing sessions and every configuration key that was
not present in the generator CSV.

First-time setup (join the ``tesla-key-esp32-setup`` access point first)::

    python3 provision.py --url http://192.168.4.1 --ssid MyNetwork --vin 5YJ...

Changing WiFi on an already reachable device uses ``POST /set_wifi`` and its
one-shot rollback transaction::

    python3 provision.py --url http://tesla-key-esp32.local --ssid MyNetwork

The WiFi password is prompted without echo.  Automation can use
``--password-stdin`` or a mode-0600 ``--password-file``.  An open network must
be selected explicitly with ``--open-network``.
"""

from __future__ import annotations

import argparse
import getpass
import json
import pathlib
import re
import stat
import sys
import urllib.error
import urllib.parse
import urllib.request


DEFAULT_TIMEOUT = 15.0
VIN_RE = re.compile(r"^[A-HJ-NPR-Z0-9]{17}$")


class ProvisionError(RuntimeError):
    """A safe, user-actionable provisioning failure."""


def normalize_base_url(value: str) -> str:
    value = value.strip().rstrip("/")
    parsed = urllib.parse.urlsplit(value)
    if parsed.scheme != "http" or not parsed.netloc or parsed.path not in ("", "/"):
        raise ProvisionError("--url must be an HTTP origin, for example http://192.168.4.1")
    if parsed.query or parsed.fragment or parsed.username or parsed.password:
        raise ProvisionError("--url must not contain credentials, a query string or fragment")
    return urllib.parse.urlunsplit((parsed.scheme, parsed.netloc, "", "", ""))


def validate_vin(value: str) -> str:
    vin = value.strip().upper()
    if vin and not VIN_RE.fullmatch(vin):
        raise ProvisionError("VIN must be 17 uppercase letters/digits excluding I, O and Q")
    return vin


def validate_wifi(ssid: str, password: str) -> None:
    ssid_len = len(ssid.encode("utf-8"))
    password_len = len(password.encode("utf-8"))
    if not 1 <= ssid_len <= 32:
        raise ProvisionError("SSID must be 1-32 UTF-8 bytes")
    raw_psk = password_len == 64 and len(password) == 64 and re.fullmatch(r"[0-9A-Fa-f]{64}", password)
    if password and not (8 <= password_len <= 63 or raw_psk):
        raise ProvisionError(
            "WiFi password must be 8-63 UTF-8 bytes or exactly 64 ASCII hex characters "
            "(raw PSK); use --open-network for an empty password"
        )


def read_password(args: argparse.Namespace) -> str:
    if args.password is not None:
        raise ProvisionError(
            "--password is refused because command-line secrets leak through shell history and "
            "process listings; use the prompt, --password-stdin or --password-file"
        )
    if args.open_network:
        return ""
    if args.password_stdin:
        value = sys.stdin.readline()
        if value == "":
            raise ProvisionError("--password-stdin received no data")
        return value.rstrip("\r\n")
    if args.password_file:
        path = pathlib.Path(args.password_file)
        try:
            mode = stat.S_IMODE(path.stat().st_mode)
        except OSError as error:
            raise ProvisionError(f"cannot stat password file {path}: {error}") from error
        if mode & 0o077:
            raise ProvisionError(f"password file {path} must not be group/world accessible (use chmod 600)")
        try:
            return path.read_text(encoding="utf-8").rstrip("\r\n")
        except OSError as error:
            raise ProvisionError(f"cannot read password file {path}: {error}") from error
    if not sys.stdin.isatty():
        raise ProvisionError(
            "no interactive terminal for the password prompt; use --password-stdin, "
            "--password-file or --open-network"
        )
    return getpass.getpass("WiFi password (not echoed): ")


def request(
    base_url: str,
    path: str,
    *,
    payload: dict[str, str] | None = None,
    form: bool = False,
    timeout: float = DEFAULT_TIMEOUT,
) -> tuple[str, str]:
    data = None
    headers = {"Accept": "application/json, text/html;q=0.9"}
    method = "GET"
    if payload is not None:
        method = "POST"
        if form:
            data = urllib.parse.urlencode(payload).encode("utf-8")
            headers["Content-Type"] = "application/x-www-form-urlencoded"
        else:
            data = json.dumps(payload, separators=(",", ":")).encode("utf-8")
            headers["Content-Type"] = "application/json"
    req = urllib.request.Request(base_url + path, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            return response.read().decode("utf-8", "replace"), response.headers.get_content_type()
    except urllib.error.HTTPError as error:
        body = error.read().decode("utf-8", "replace")
        raise ProvisionError(f"{path} returned HTTP {error.code}: {body[:300]}") from error
    except (urllib.error.URLError, TimeoutError, OSError) as error:
        raise ProvisionError(f"cannot reach {base_url}{path}: {error}") from error


def detect_mode(base_url: str, timeout: float) -> tuple[str, dict[str, object] | None]:
    """Return (``lan`` or ``setup``, parsed status when available)."""
    body, content_type = request(base_url, "/status", timeout=timeout)
    if content_type == "application/json":
        try:
            status_body = json.loads(body)
        except json.JSONDecodeError as error:
            raise ProvisionError(f"{base_url}/status returned malformed JSON") from error
        if isinstance(status_body, dict) and "version" in status_body:
            return "lan", status_body
    # In setup mode the captive handler redirects unknown paths to the HTML form.
    return "setup", None


def provision_setup(base_url: str, ssid: str, password: str, vin: str, timeout: float) -> None:
    body, _ = request(
        base_url,
        "/save",
        payload={"ssid": ssid, "pass": password, "vin": vin},
        form=True,
        timeout=timeout,
    )
    if "Saved" not in body:
        raise ProvisionError(f"setup portal did not confirm the save: {body[:300]}")


def provision_wifi(base_url: str, ssid: str, password: str, timeout: float) -> None:
    body, content_type = request(
        base_url,
        "/set_wifi",
        payload={"ssid": ssid, "pass": password},
        timeout=timeout,
    )
    if content_type != "application/json":
        raise ProvisionError("/set_wifi did not return JSON")
    try:
        result = json.loads(body)
    except json.JSONDecodeError as error:
        raise ProvisionError("/set_wifi returned malformed JSON") from error
    if not isinstance(result, dict) or result.get("result") is not True:
        raise ProvisionError(f"device refused WiFi update: {body[:300]}")


def provision_vin(base_url: str, vin: str, timeout: float) -> None:
    body, content_type = request(base_url, "/set_vin", payload={"vin": vin}, timeout=timeout)
    if content_type != "application/json":
        raise ProvisionError("/set_vin did not return JSON")
    try:
        result = json.loads(body)
    except json.JSONDecodeError as error:
        raise ProvisionError("/set_vin returned malformed JSON") from error
    if not isinstance(result, dict) or result.get("result") is not True:
        raise ProvisionError(f"device refused VIN update: {body[:300]}")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Safely provision tesla-key-esp32 through its transactional HTTP API",
        epilog=(
            "Raw serial NVS writes are intentionally unsupported: they replace the complete NVS "
            "partition and destroy the vehicle key/pairing. Use the nvs-backup workflow before "
            "any separate low-level recovery operation."
        ),
    )
    parser.add_argument("--url", help="device/setup origin, e.g. http://192.168.4.1")
    parser.add_argument("--mode", choices=("auto", "setup", "lan"), default="auto")
    parser.add_argument("--ssid", help="WiFi SSID (not needed with --vin-only)")
    password_sources = parser.add_mutually_exclusive_group()
    password_sources.add_argument("--password-file", help="read password from a mode-0600 file")
    password_sources.add_argument("--password-stdin", action="store_true", help="read one line from stdin")
    password_sources.add_argument("--open-network", action="store_true", help="explicitly configure no password")
    # Accept only to produce a specific safe migration error instead of argparse's generic one.
    parser.add_argument("--password", help=argparse.SUPPRESS)
    parser.add_argument("--vin", default="", help="optional vehicle VIN during first-time setup")
    parser.add_argument(
        "--vin-only",
        action="store_true",
        help="change only the VIN on a reachable device; requires --confirm-vin-change",
    )
    parser.add_argument(
        "--confirm-vin-change",
        action="store_true",
        help="acknowledge that a changed VIN intentionally clears the old pairing",
    )
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    # Legacy option retained solely so old invocations fail with the data-loss explanation.
    parser.add_argument("--port", "-p", help=argparse.SUPPRESS)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.port:
            raise ProvisionError(
                "serial NVS provisioning has been retired: it overwrote the complete 0x6000-byte "
                "partition and erased the vehicle private key and sessions. Join the setup AP and "
                "use --url http://192.168.4.1, or use the nvs-backup workflow before low-level recovery"
            )
        if not args.url:
            raise ProvisionError("--url is required")
        if args.timeout <= 0:
            raise ProvisionError("--timeout must be positive")
        base_url = normalize_base_url(args.url)
        vin = validate_vin(args.vin)

        mode = args.mode
        status_body: dict[str, object] | None = None
        if mode == "auto":
            mode, status_body = detect_mode(base_url, args.timeout)

        if args.vin_only:
            if mode != "lan":
                raise ProvisionError("--vin-only requires --mode lan or an auto-detected reachable device")
            if not vin:
                raise ProvisionError("--vin-only requires --vin")
            if not args.confirm_vin_change:
                raise ProvisionError(
                    "a changed VIN clears pairing with the old vehicle; repeat with --confirm-vin-change"
                )
            provision_vin(base_url, vin, args.timeout)
            print("VIN transaction accepted; the device is rebooting. Existing NVS was not overwritten.")
            return 0

        if not args.ssid:
            raise ProvisionError("--ssid is required unless --vin-only is used")
        password = read_password(args)
        validate_wifi(args.ssid, password)

        if mode == "setup":
            provision_setup(base_url, args.ssid, password, vin, args.timeout)
            print("Setup transaction accepted; the device is rebooting. Existing key/session NVS was preserved.")
            return 0

        if vin:
            if status_body is None:
                _, status_body = detect_mode(base_url, args.timeout)
            current_vin = str((status_body or {}).get("vin", ""))
            if current_vin not in (vin, ""):
                raise ProvisionError(
                    "--vin differs from the reachable device. Refusing to combine a vehicle identity "
                    "change with WiFi: use --vin-only --confirm-vin-change, wait for reboot, then update WiFi"
                )
            if current_vin == "":
                raise ProvisionError(
                    "the reachable device has no VIN. Set it separately with --vin-only "
                    "--confirm-vin-change, wait for reboot, then update WiFi"
                )
        provision_wifi(base_url, args.ssid, password, args.timeout)
        print("WiFi transaction accepted with one-shot rollback; the device is rebooting.")
        return 0
    except ProvisionError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
