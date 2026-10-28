#!/usr/bin/env python3
"""Extract the complete shipped Ethernet cleanup and caller bodies for passive host tests.

The doubles model reviewed IDF v6.1/W5500 failure ordering, not SDK ABI or RTOS quiescence.
Changing either pinned source requires reviewing that model before updating these admissions.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import runpy

ROOT = Path(__file__).resolve().parents[1]
TOOLCHAIN = "v6.1@sha256:81893c71bb5e570088901f21def8684c25cd2a9020281bd01b843a7655edb18c"
W5500 = "1f19456ec90b60424583a4138c075a78e1ab2297"


def verify_model_pins(root: Path = ROOT) -> None:
    if (root / "esp-idf-toolchain.txt").read_text().strip() != TOOLCHAIN:
        raise AssertionError("Ethernet host model requires fresh review for this IDF toolchain")
    manifest = (root / "main/idf_component.yml").read_text()
    blocks = re.findall(r"^  espressif/w5500:\n((?:(?: {4,}[^\n]*)?\n)*)", manifest, re.M)
    expected = {
        "git": '"https://github.com/espressif/esp-eth-drivers.git"',
        "path": '"w5500"',
        "version": f'"{W5500}"',
    }
    fields = re.findall(r"^    (git|path|version):[ \t]*([^\n]+)", blocks[0], re.M) if len(blocks) == 1 else []
    if len(fields) != len(expected) or dict(fields) != expected:
        raise AssertionError("Ethernet host model requires fresh review for this W5500 source")


def render_adapter(source: str) -> str:
    body_in = runpy.run_path(str(ROOT / "test/test_runtime_boundary_contract.py"))["function_body_in"]
    structures = re.findall(r"^struct EthStartupResources\s*\{.*?^\};", source, re.M | re.S)
    if len(structures) != 1:
        raise AssertionError("missing or ambiguous production Ethernet resource owner")
    parts = [structures[0]]
    for name in ("eth_cleanup_startup", "eth_startup_fallback", "eth_startup_fatal"):
        starts = list(re.finditer(r"^(?:\[\[noreturn\]\] )?static (?:bool|void) " + name + r"\b", source, re.M))
        if len(starts) != 1:
            raise AssertionError("missing or ambiguous production Ethernet function: " + name)
        start = starts[0].start()
        # The shared body locator does not admit noreturn declarations. Strip only that prefix
        # for lookup; the emitted signature and complete body remain the original source bytes.
        lookup = re.sub(r"^\[\[noreturn\]\] ", "", source, flags=re.M)
        body = body_in(lookup, name)
        prefix = source[start:source.index(body, start)]
        parts.append(prefix + body)
    return "// Complete production bodies; generated, not a mirrored cleanup.\n" + "\n\n".join(parts) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    verify_model_pins()
    source = ROOT / "main/net.cpp"
    if args.output.resolve() == source.resolve():
        raise AssertionError("adapter output must not overwrite production source")
    args.output.write_text(render_adapter(source.read_text()))


if __name__ == "__main__":
    main()
