#!/usr/bin/env bash
# Build-time local patch of yoziru/tesla-ble so the ESP32-C5 becomes a buildable target.
#
# Why this exists: tesla-ble v5.1.1 ships an idf_component.yml whose `targets:` list is
# esp32 / esp32s3 / esp32c3 / esp32c6 — esp32c5 is missing, so the ESP-IDF Component Manager
# refuses the chip at DEPENDENCY RESOLUTION (before compile), and it never even stages the
# source into managed_components/ (so there is nothing to patch after the fact). The library's
# code is target-agnostic and already builds for RISC-V (c3/c6); C5 is RISC-V too, so the only
# blocker is that one manifest line.
#
# Fix, without editing managed_components/ (regenerated) or forking upstream: clone the exact
# pinned tag into third_party/tesla-ble (gitignored) and append esp32c5 to its `targets:`.
# main/idf_component.yml then routes ONLY the esp32c5 target through this local copy via a
# `path:` dependency gated on `if: target == esp32c5`; the other four targets keep resolving
# byte-identically from git. Run this once before an esp32c5 build; ci-build-all.sh runs it
# automatically. Idempotent + a warm checkout is reused, so re-runs are cheap.
#
# Usage:  ./scripts/prepare-tesla-ble-c5.sh
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

url="https://github.com/yoziru/tesla-ble.git"
dest="third_party/tesla-ble"
manifest="$dest/idf_component.yml"

# Single source of truth for the version: the git dep in main/idf_component.yml. Keep the two
# tesla-ble references (the git dep and this local copy) on the same tag so the esp32c5 image
# is the same library revision as the other four targets.
version="$(grep -oE 'version:[[:space:]]*"v[0-9]+\.[0-9]+(\.[0-9]+)?"' main/idf_component.yml \
  | grep -oE 'v[0-9]+\.[0-9]+(\.[0-9]+)?' | head -n1)"
[ -n "${version:-}" ] || {
  echo "prepare-tesla-ble-c5: could not read tesla-ble version from main/idf_component.yml" >&2
  exit 1
}

# (Re)clone only when missing or checked out at the wrong tag — keeps this idempotent and fast.
need_clone=1
if [ -d "$dest/.git" ]; then
  cur="$(git -C "$dest" describe --tags --exact-match 2>/dev/null || echo)"
  [ "$cur" = "$version" ] && need_clone=0
fi
if [ "$need_clone" = 1 ]; then
  rm -rf "$dest"
  mkdir -p "$(dirname "$dest")"
  git clone --quiet -c advice.detachedHead=false --branch "$version" --depth 1 "$url" "$dest"
  echo "prepare-tesla-ble-c5: cloned yoziru/tesla-ble $version into $dest"
fi

# Append esp32c5 to the library's targets: list so the Component Manager accepts the chip.
# Idempotent (skip if already present); awk so it is portable across BSD (macOS host) and GNU
# (CI) — no sed dialect gotchas. Insert right after the esp32c6 line (last entry in v5.1.1).
if grep -qE '^[[:space:]]*-[[:space:]]*esp32c5[[:space:]]*$' "$manifest"; then
  echo "prepare-tesla-ble-c5: $manifest already patched for esp32c5"
else
  tmp="$(mktemp)"
  awk '
    { print }
    /^[[:space:]]*-[[:space:]]*esp32c6[[:space:]]*$/ { print "  - esp32c5" }
  ' "$manifest" > "$tmp"
  if ! grep -qE '^[[:space:]]*-[[:space:]]*esp32c5[[:space:]]*$' "$tmp"; then
    rm -f "$tmp"
    echo "prepare-tesla-ble-c5: could not find the esp32c6 anchor in $manifest to patch" >&2
    exit 1
  fi
  mv "$tmp" "$manifest"
  echo "prepare-tesla-ble-c5: appended esp32c5 to $manifest targets:"
fi
