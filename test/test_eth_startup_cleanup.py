#!/usr/bin/env python3
"""Compile the actual Ethernet cleanup, reject unsafe mutations, then restore the positive.

Only passive SDK doubles run. This does not establish task quiescence, ABI, RX delivery or hardware.
"""

from __future__ import annotations

import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from generate_eth_startup_adapter import ROOT, TOOLCHAIN, W5500, render_adapter, verify_model_pins


def replace_once(source: str, before: str, after: str) -> str:
    if source.count(before) != 1:
        raise AssertionError("ambiguous Ethernet control transformation: " + before)
    return source.replace(before, after, 1)


def run_case(directory: Path, source: str, label: str, negative: bool) -> None:
    fixture = directory / label
    fixture.mkdir()
    (fixture / "eth_startup_adapter.generated.hpp").write_text(render_adapter(source))
    compiler = shutil.which(os.environ.get("CXX", "c++"))
    if compiler is None:
        raise AssertionError("C++17 host compiler is required")
    binary = fixture / "cleanup"
    compiled = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                               "-I" + str(fixture), str(ROOT / "test/test_eth_startup_cleanup.cpp"),
                               "-o", str(binary)], capture_output=True, text=True, timeout=30)
    if compiled.returncode != 0:
        raise AssertionError(label + " must compile before testing its runtime oracle:\n" + compiled.stderr[-3000:])
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
    if negative:
        if result.returncode != 42 or "Ethernet CHECK" not in result.stderr:
            raise AssertionError(label + " failed to reach the runtime safety oracle")
    elif result.returncode != 0 or not result.stdout.startswith("PASS ") or result.stderr:
        raise AssertionError(label + " positive failed:\n" + result.stderr[-3000:])


def main() -> None:
    verify_model_pins()
    source = (ROOT / "main/net.cpp").read_text()
    no_quarantine = replace_once(source, "if (r.start_attempted) {", "if (false && r.start_attempted) {")
    premature_netif = re.search(r"^    if \(r\.netif\) \{.*?^    \}", source, re.M | re.S)
    if premature_netif is None:
        raise AssertionError("missing actual netif destruction block")
    netif_block = premature_netif.group(0)
    reordered = replace_once(source, netif_block, "")
    reordered = replace_once(reordered, "\n    if (r.handle) {", "\n" + netif_block + "\n    if (r.handle) {")
    uninstall = source.index("const esp_err_t err = esp_eth_driver_uninstall(r.handle);")
    fail_return = source.index("return false;", uninstall)
    unconfirmed_uninstall = source[:fail_return] + source[fail_return:].replace("return false;", ";", 1)
    mac = source.index("const esp_err_t err = r.mac->del(r.mac);")
    fail_return = source.index("return false;", mac)
    unconfirmed_mac = source[:fail_return] + source[fail_return:].replace("return false;", ";", 1)
    with tempfile.TemporaryDirectory(prefix="tesla-eth-cleanup-") as temporary:
        directory = Path(temporary)
        pins = directory / "pins"
        (pins / "main").mkdir(parents=True)
        (pins / "esp-idf-toolchain.txt").write_text(TOOLCHAIN + "\n")
        manifest = (ROOT / "main/idf_component.yml").read_text()
        (pins / "main/idf_component.yml").write_text(manifest)
        verify_model_pins(pins)
        for changed in (
            replace_once(manifest, f'    version: "{W5500}"', '    version: "' + "0" * 40 + '"') +
            "\n# Retained historical pin " + W5500 + "\n",
            replace_once(manifest, '    path: "w5500"', '    path: "other-driver"'),
            replace_once(manifest, '    git: "https://github.com/espressif/esp-eth-drivers.git"',
                         '    git: "https://example.invalid/other.git"'),
        ):
            (pins / "main/idf_component.yml").write_text(changed)
            try:
                verify_model_pins(pins)
            except AssertionError:
                pass
            else:
                raise AssertionError("changed actual W5500 source bypassed reviewed model admission")
        run_case(directory, source, "positive", False)
        for label, mutant in (("activated-quarantine-omitted", no_quarantine),
                              ("old-netif-before-driver-order", reordered),
                              ("uninstall-failure-continues", unconfirmed_uninstall),
                              ("mac-failure-continues", unconfirmed_mac)):
            run_case(directory, mutant, label, True)
        run_case(directory, source, "restored-positive", False)
    print("Ethernet actual cleanup: PASS (2 positives, 4 executable safety controls, 3 pin controls; passive SDK model only)")


if __name__ == "__main__":
    main()
