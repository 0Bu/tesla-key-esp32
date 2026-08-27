#!/usr/bin/env bash
# Fast, dependency-free host tests for destructive-path, target and manifest contracts.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
"$repo_root/scripts/release-relevance.sh" --self-test
"$repo_root/scripts/select-release-version.sh" --self-test
"$repo_root/scripts/check-reproducible-build.sh" --self-test
python3 "$repo_root/scripts/check-dependency-contract.py" --self-test
python3 "$repo_root/scripts/check-otadata-contract.py" --self-test
python3 "$repo_root/scripts/check-partition-contract.py" --self-test
python3 "$repo_root/scripts/check-partition-contract.py" --csv "$repo_root/partitions.csv" >/dev/null
python3 "$repo_root/scripts/check-firmware-artifacts.py" --self-test
python3 "$repo_root/scripts/check-build-semantics.py" --self-test
python3 "$repo_root/scripts/check-build-artifact-inventory.py" --self-test --source-root "$repo_root"
python3 "$repo_root/scripts/report-firmware-size.py" --self-test
python3 "$repo_root/scripts/check-release-assets.py" --self-test
python3 "$repo_root/scripts/check-signed-root-inventory.py" --self-test
python3 "$repo_root/scripts/prepare-reused-release.py" --self-test
python3 "$repo_root/scripts/check-published-release.py" --self-test
python3 "$repo_root/scripts/check-pages-source.py" --self-test
python3 "$repo_root/scripts/check-stack-usage.py" --self-test
python3 "$repo_root/scripts/check-build-gate-contract.py" --self-test
temp="$(mktemp -d)"
trap 'rm -rf "$temp"' EXIT
stage="$temp/fw"
mkdir -p "$stage"

make_bytes() {
  local path="$1" count="$2"
  dd if=/dev/zero of="$path" bs=1 count="$count" status=none
}

make_ff_bytes() {
  local path="$1" count="$2"
  python3 - "$path" "$count" <<'PY'
from pathlib import Path
import sys

Path(sys.argv[1]).write_bytes(b"\xff" * int(sys.argv[2]))
PY
}

for target in esp32 esp32s3 esp32c3 esp32c6; do
  mkdir -p "$stage/$target"
  make_bytes "$stage/$target/bootloader.bin" 1024
  make_bytes "$stage/$target/partition-table.bin" 1024
  make_bytes "$stage/$target/tesla-key-esp32.bin" 4096
  make_ff_bytes "$stage/$target/ota_data_initial.bin" 8192
done

sha=0123456789abcdef0123456789abcdef01234567
temp_root="${RUNNER_TEMP:-${TMPDIR:-/tmp}}"
site="$(mktemp -d "$temp_root/tesla-key-pages.XXXXXX")"
trap 'rm -rf "$temp" "$site"' EXIT

if FIRMWARE_STAGE_DIR="$stage" "$repo_root/scripts/build-pages.sh" . 1.2.3 "$sha" >/dev/null 2>&1; then
  echo "build-pages path guard self-test failed: repository root accepted" >&2
  exit 1
fi
if FIRMWARE_STAGE_DIR="$stage" "$repo_root/scripts/build-pages.sh" "$site" release-1 "$sha" >/dev/null 2>&1; then
  echo "build-pages version self-test failed: browser-incompatible version accepted" >&2
  exit 1
fi
if FIRMWARE_STAGE_DIR="$stage" "$repo_root/scripts/build-pages.sh" "$site" 01.2.3 "$sha" >/dev/null 2>&1; then
  echo "build-pages version self-test failed: non-canonical leading-zero version accepted" >&2
  exit 1
fi
if "$repo_root/scripts/test-release-contract.sh" release-1 "$sha" "$stage" >/dev/null 2>&1; then
  echo "release-contract version self-test failed: browser-incompatible version accepted" >&2
  exit 1
fi
if "$repo_root/scripts/test-release-contract.sh" 01.2.3 "$sha" "$stage" >/dev/null 2>&1; then
  echo "release-contract version self-test failed: non-canonical leading-zero version accepted" >&2
  exit 1
fi
signer_error="$temp/signer-version-error.txt"
if "$repo_root/scripts/ci-sign-artifacts.sh" 01.2.3 "$stage" "$sha" . "$stage" \
    >/dev/null 2>"$signer_error"; then
  echo "signer version self-test failed: non-canonical leading-zero version accepted" >&2
  exit 1
fi
grep -Fq "invalid display version" "$signer_error" || {
  echo "signer version self-test failed after the version boundary" >&2
  exit 1
}

FIRMWARE_STAGE_DIR="$stage" "$repo_root/scripts/build-pages.sh" "$site" 1.2.3 "$sha" >/dev/null
python3 "$repo_root/scripts/check-pages-manifest.py" "$site" --source-sha "$sha" --version 1.2.3 >/dev/null
python3 - "$site/manifest.json" <<'PY'
import json
from pathlib import Path
import sys

path = Path(sys.argv[1])
manifest = json.loads(path.read_text(encoding="utf-8"))
manifest["version"] = "01.2.3"
path.write_text(json.dumps(manifest), encoding="utf-8")
PY
if python3 "$repo_root/scripts/check-pages-manifest.py" "$site" \
    --source-sha "$sha" --version 01.2.3 >/dev/null 2>&1; then
  echo "manifest version self-test failed: non-canonical leading-zero version accepted" >&2
  exit 1
fi
FIRMWARE_STAGE_DIR="$stage" "$repo_root/scripts/build-pages.sh" "$site" 1.2.3 "$sha" >/dev/null
python3 - "$site/manifest.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1], encoding="utf-8"))
assert d["layoutVersion"] == 2 and len(d["builds"]) == 4
assert all(len(build["parts"]) == 4 and build["parts"][-1]["offset"] == 0xF000 for build in d["builds"])
PY

# A same-size otadata mutation with a matching manifest digest must still be rejected by the
# semantic erased-partition gate; digest/length checking alone cannot establish safe activation.
python3 - "$site" <<'PY'
import hashlib, json, pathlib, sys

site = pathlib.Path(sys.argv[1])
manifest_path = site / "manifest.json"
manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
part = manifest["builds"][0]["parts"][3]
path = site / part["path"]
data = bytearray(path.read_bytes())
data[123] = 0
path.write_bytes(data)
part["sha256"] = hashlib.sha256(data).hexdigest()
manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
PY
if python3 "$repo_root/scripts/check-pages-manifest.py" "$site" \
    --source-sha "$sha" --version 1.2.3 >/dev/null 2>&1; then
  echo "manifest otadata semantic self-test failed: non-erased bytes accepted" >&2
  exit 1
fi
FIRMWARE_STAGE_DIR="$stage" "$repo_root/scripts/build-pages.sh" "$site" 1.2.3 "$sha" >/dev/null

# Model the four exact versioned merged.bin assets attached to a GitHub Release, then prove every
# one of the 16 Pages parts is the byte-identical slice at its declared flash offset.
release="$temp/release"
mkdir -p "$release"
python3 - "$site" "$release" 1.2.3 <<'PY'
import json, pathlib, sys
site = pathlib.Path(sys.argv[1])
release = pathlib.Path(sys.argv[2])
version = sys.argv[3]
manifest = json.loads((site / "manifest.json").read_text(encoding="utf-8"))
names = {
    "ESP32": f"tesla-key-esp32-{version}-merged.bin",
    "ESP32-S3": f"tesla-key-esp32-s3-{version}-merged.bin",
    "ESP32-C3": f"tesla-key-esp32-c3-{version}-merged.bin",
    "ESP32-C6": f"tesla-key-esp32-c6-{version}-merged.bin",
}
for build in manifest["builds"]:
    end = max(part["offset"] + part["size"] for part in build["parts"])
    merged = bytearray(b"\xff" * end)
    for part in build["parts"]:
        data = (site / part["path"]).read_bytes()
        merged[part["offset"] : part["offset"] + len(data)] = data
    (release / names[build["chipFamily"]]).write_bytes(merged)
PY
python3 "$repo_root/scripts/check-release-pages-bytes.py" "$site" "$release" \
  --version 1.2.3 >/dev/null

# The merged image contract covers every byte, not only the four declared slices. NVS/gap bytes
# must remain erased, and the app end is the exact end of the merged asset.
python3 - "$release/tesla-key-esp32-1.2.3-merged.bin" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
data = bytearray(path.read_bytes())
data[0x9000] = 0
path.write_bytes(data)
PY
if python3 "$repo_root/scripts/check-release-pages-bytes.py" "$site" "$release" \
    --version 1.2.3 >/dev/null 2>&1; then
  echo "release/Pages merged-gap self-test failed: non-erased NVS gap accepted" >&2
  exit 1
fi
# Restore the exact merged fixture, then prove a trailing byte is rejected as undeclared data.
python3 - "$site" "$release" 1.2.3 <<'PY'
import json, pathlib, sys

site = pathlib.Path(sys.argv[1])
release = pathlib.Path(sys.argv[2])
version = sys.argv[3]
manifest = json.loads((site / "manifest.json").read_text(encoding="utf-8"))
build = manifest["builds"][0]
end = max(part["offset"] + part["size"] for part in build["parts"])
merged = bytearray(b"\xff" * end)
for part in build["parts"]:
    payload = (site / part["path"]).read_bytes()
    merged[part["offset"] : part["offset"] + len(payload)] = payload
path = release / f"tesla-key-esp32-{version}-merged.bin"
path.write_bytes(merged + b"\xff")
PY
if python3 "$repo_root/scripts/check-release-pages-bytes.py" "$site" "$release" \
    --version 1.2.3 >/dev/null 2>&1; then
  echo "release/Pages merged-length self-test failed: trailing byte accepted" >&2
  exit 1
fi
# Restore all canonical Release fixtures before later adversarial cases.
python3 - "$site" "$release" 1.2.3 <<'PY'
import json, pathlib, sys

site = pathlib.Path(sys.argv[1])
release = pathlib.Path(sys.argv[2])
version = sys.argv[3]
manifest = json.loads((site / "manifest.json").read_text(encoding="utf-8"))
names = {
    "ESP32": f"tesla-key-esp32-{version}-merged.bin",
    "ESP32-S3": f"tesla-key-esp32-s3-{version}-merged.bin",
    "ESP32-C3": f"tesla-key-esp32-c3-{version}-merged.bin",
    "ESP32-C6": f"tesla-key-esp32-c6-{version}-merged.bin",
}
for build in manifest["builds"]:
    end = max(part["offset"] + part["size"] for part in build["parts"])
    merged = bytearray(b"\xff" * end)
    for part in build["parts"]:
        payload = (site / part["path"]).read_bytes()
        merged[part["offset"] : part["offset"] + len(payload)] = payload
    (release / names[build["chipFamily"]]).write_bytes(merged)
PY
python3 "$repo_root/scripts/check-release-pages-bytes.py" "$site" "$release" \
  --version 1.2.3 >/dev/null

# Adversarial case: replace a Pages app and update its manifest digest, leaving version, sourceSha,
# family, offsets and internal manifest validation intact. Only Release byte binding catches it.
python3 - "$site" <<'PY'
import hashlib, json, pathlib, sys
site = pathlib.Path(sys.argv[1])
manifest_path = site / "manifest.json"
manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
part = manifest["builds"][0]["parts"][2]
path = site / part["path"]
data = bytearray(path.read_bytes())
data[0] ^= 0x01
path.write_bytes(data)
part["sha256"] = hashlib.sha256(data).hexdigest()
manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
PY
python3 "$repo_root/scripts/check-pages-manifest.py" "$site" \
  --source-sha "$sha" --version 1.2.3 >/dev/null
if python3 "$repo_root/scripts/check-release-pages-bytes.py" "$site" "$release" \
    --version 1.2.3 >/dev/null 2>&1; then
  echo "release/Pages byte-binding self-test failed: substituted Pages app accepted" >&2
  exit 1
fi

# Restore the canonical snapshot for the remaining manifest integrity cases.
FIRMWARE_STAGE_DIR="$stage" "$repo_root/scripts/build-pages.sh" "$site" 1.2.3 "$sha" >/dev/null

mv "$stage/esp32c6" "$temp/esp32c6"
if FIRMWARE_STAGE_DIR="$stage" "$repo_root/scripts/build-pages.sh" "$site" 1.2.3 "$sha" >/dev/null 2>&1; then
  echo "build-pages target self-test failed: missing target accepted" >&2
  exit 1
fi
mv "$temp/esp32c6" "$stage/esp32c6"

mkdir "$stage/esp32c5"
if FIRMWARE_STAGE_DIR="$stage" "$repo_root/scripts/build-pages.sh" "$site" 1.2.3 "$sha" >/dev/null 2>&1; then
  echo "build-pages target self-test failed: unexpected target accepted" >&2
  exit 1
fi
rmdir "$stage/esp32c5"

FIRMWARE_STAGE_DIR="$stage" "$repo_root/scripts/build-pages.sh" "$site" 1.2.3 "$sha" >/dev/null
printf x >> "$site/tesla-key-esp32.bin"
if python3 "$repo_root/scripts/check-pages-manifest.py" "$site" >/dev/null 2>&1; then
  echo "manifest digest/length self-test failed: tampered file accepted" >&2
  exit 1
fi

echo "build/manifest contract self-test: PASS"
