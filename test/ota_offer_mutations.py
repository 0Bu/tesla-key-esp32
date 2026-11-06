#!/usr/bin/env python3
"""Create one disposable actual-writer omission; never modify production or its pins."""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
source = (root / "main/ota_writer.cpp").read_text()
mutations = {
    "digest": ("actual != s.policy_.expected_sha256", "false"),
    "length": ("s.received_ != s.policy_.expected_bytes", "false"),
}
old, new = mutations[sys.argv[1]]
if source.count(old) != 1:
    raise SystemExit("actual OTA omission target differs; semantic review required")
Path(sys.argv[2]).write_text(source.replace(old, new, 1))
