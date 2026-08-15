#!/usr/bin/env bash
# Assemble the GitHub Pages Web Serial installer from the four trusted signed target trees.
#
# Usage: ./scripts/build-pages.sh <out_dir> <version> [source-sha]
#
# The output directory is deliberately restricted: this script replaces it recursively, so only
# the two repository staging names or a purpose-named direct child of RUNNER_TEMP/TMPDIR is accepted.
# Each manifest part carries a byte length and SHA-256.  ota_data_initial is the fourth/final part;
# the browser validates every download first, writes the immutable parts, then writes otadata as the
# activation step.  Changing the order/offsets is a schema change, not an incidental refactor.
set -euo pipefail

out_arg="${1:?usage: build-pages.sh <out_dir> <version> [source-sha]}"
version="${2:?usage: build-pages.sh <out_dir> <version> [source-sha]}"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
fw="${FIRMWARE_STAGE_DIR:-$repo_root/_fw}"
docs="$repo_root/docs"
source_sha="${3:-$(git -C "$repo_root" rev-parse HEAD)}"

[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?$ ]] || {
  echo "invalid Pages version (expected X.Y.Z or X.Y.Z-prerelease): $version" >&2
  exit 2
}
[[ "$source_sha" =~ ^[0-9a-f]{40}$ ]] || {
  echo "invalid source SHA (expected 40 lowercase hex characters): $source_sha" >&2
  exit 2
}

canonical_path() {
  python3 -c 'import os,sys; print(os.path.realpath(os.path.abspath(sys.argv[1])))' "$1"
}

out="$(canonical_path "$out_arg")"
temp_root="$(canonical_path "${RUNNER_TEMP:-${TMPDIR:-/tmp}}")"
case "$out" in
  "$repo_root/_site"|"$repo_root/_pr_site"|"$temp_root"/tesla-key-pages.*) ;;
  *)
    echo "unsafe Pages output '$out_arg' resolves to '$out'" >&2
    echo "allowed: $repo_root/_site, $repo_root/_pr_site, or $temp_root/tesla-key-pages.*" >&2
    exit 2
    ;;
esac
[[ "$out" != / && "$out" != "$repo_root" && "$out" != "$temp_root" ]] || {
  echo "refusing broad Pages output path: $out" >&2; exit 2;
}
[[ ! -L "$out_arg" ]] || { echo "refusing symlink Pages output: $out_arg" >&2; exit 2; }
[[ -d "$fw" && ! -L "$fw" ]] || { echo "signed firmware stage missing or unsafe: $fw" >&2; exit 1; }

# A direct Pages invocation is itself a release boundary: never rely on a broader test job having
# verified the browser runtime beforehand.  This local check is offline, exact-hash and fail-closed.
"$repo_root/scripts/verify-vendored-esptool-js.sh"

file_size() {
  stat -c %s "$1" 2>/dev/null || stat -f %z "$1"
}

file_sha256() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    shasum -a 256 "$1" | awk '{print $1}'
  fi
}

chip_family() {
  case "$1" in
    esp32) echo ESP32 ;; esp32s3) echo ESP32-S3 ;; esp32c3) echo ESP32-C3 ;; esp32c6) echo ESP32-C6 ;;
    *) echo "unknown target: $1" >&2; exit 2 ;;
  esac
}

boot_offset() { [[ "$1" == esp32 ]] && echo 4096 || echo 0; }

image_suffix() {
  case "$1" in
    esp32) echo "" ;; esp32s3) echo -s3 ;; esp32c3) echo -c3 ;; esp32c6) echo -c6 ;;
    *) echo "unknown target: $1" >&2; exit 2 ;;
  esac
}

TARGETS="esp32 esp32s3 esp32c3 esp32c6"
expected_sorted="esp32 esp32c3 esp32c6 esp32s3"
actual_sorted="$(find "$fw" -mindepth 1 -maxdepth 1 -type d -exec basename {} \; | LC_ALL=C sort | tr '\n' ' ' | sed 's/ $//')"
[[ "$actual_sorted" == "$expected_sorted" ]] || {
  echo "signed firmware stage must contain exactly: $expected_sorted" >&2
  echo "found: ${actual_sorted:-<none>}" >&2
  exit 1
}

rm -rf -- "$out"
mkdir -p "$out"

cp "$docs/index.html" "$docs/installer-bootstrap.mjs" \
  "$docs/serial-port-release.mjs" "$docs/web-installer.mjs" "$out/"
mkdir -p "$out/vendor"
cp "$docs/vendor/esptool-js-0.6.1.bundle.js" \
  "$docs/vendor/esptool-js-0.6.1.LICENSE" "$out/vendor/"
for md in "$docs"/*.md; do
  [[ -e "$md" ]] && cp "$md" "$out/"
done

builds=""
for target in $TARGETS; do
  source_dir="$fw/$target"
  for name in bootloader.bin partition-table.bin tesla-key-esp32.bin ota_data_initial.bin; do
    path="$source_dir/$name"
    [[ -f "$path" && ! -L "$path" ]] || {
      echo "missing/unsafe signed Pages input: $path" >&2; exit 1;
    }
  done

  bo="$(boot_offset "$target")"
  boot_size="$(file_size "$source_dir/bootloader.bin")"
  partition_size="$(file_size "$source_dir/partition-table.bin")"
  app_size="$(file_size "$source_dir/tesla-key-esp32.bin")"
  otadata_size="$(file_size "$source_dir/ota_data_initial.bin")"
  (( boot_size > 0 && boot_size <= 32768 - bo )) || {
    echo "$target bootloader size $boot_size overlaps partition table at 0x8000" >&2; exit 1;
  }
  (( partition_size > 0 && partition_size <= 0x1000 )) || {
    echo "$target partition table size $partition_size exceeds 0x1000" >&2; exit 1;
  }
  (( app_size > 0 && app_size <= 0x1f0000 )) || {
    echo "$target app size $app_size exceeds OTA slot 0x1f0000" >&2; exit 1;
  }
  (( otadata_size == 0x2000 )) || {
    echo "$target ota_data_initial size must be exactly 0x2000, got $otadata_size" >&2; exit 1;
  }

  suffix="$(image_suffix "$target")"
  boot_name="bootloader-$target.bin"
  partition_name="partition-table-$target.bin"
  app_name="tesla-key-esp32$suffix.bin"
  otadata_name="ota_data_initial-$target.bin"
  cp "$source_dir/bootloader.bin" "$out/$boot_name"
  cp "$source_dir/partition-table.bin" "$out/$partition_name"
  cp "$source_dir/tesla-key-esp32.bin" "$out/$app_name"
  cp "$source_dir/ota_data_initial.bin" "$out/$otadata_name"

  entry=$(cat <<JSON
    {
      "chipFamily": "$(chip_family "$target")",
      "parts": [
        { "path": "$boot_name", "offset": $bo, "size": $boot_size, "sha256": "$(file_sha256 "$out/$boot_name")" },
        { "path": "$partition_name", "offset": 32768, "size": $partition_size, "sha256": "$(file_sha256 "$out/$partition_name")" },
        { "path": "$app_name", "offset": 131072, "size": $app_size, "sha256": "$(file_sha256 "$out/$app_name")" },
        { "path": "$otadata_name", "offset": 61440, "size": $otadata_size, "sha256": "$(file_sha256 "$out/$otadata_name")" }
      ]
    }
JSON
)
  builds="${builds:+$builds,
}$entry"
done

cat > "$out/manifest.json" <<JSON
{
  "name": "tesla-key-esp32",
  "layoutVersion": 2,
  "sourceSha": "$source_sha",
  "version": "$version",
  "new_install_prompt_erase": true,
  "builds": [
$builds
  ]
}
JSON

python3 "$repo_root/scripts/check-pages-manifest.py" "$out" \
  --source-sha "$source_sha" --version "$version"
echo "Built verified Pages site in '$out' for version $version from $source_sha"
