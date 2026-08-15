#!/usr/bin/env bash
# Apply every repository-owned tesla-ble patch in lexical order, fail closed, and remain idempotent
# across repeated CMake configure passes.  managed_components/ is regenerated, so the patches are
# committed here while a small hash marker records exactly which series reached this materialisation.
set -euo pipefail
export LC_ALL=C

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"

hash_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    shasum -a 256 "$1" | awk '{print $1}'
  fi
}

die() { echo "tesla-ble patch series: $*" >&2; exit 1; }

apply_series() {
  local component="$1" patch_dir="$2"
  local marker="$component/.tesla-key-patch-series"
  local patch_file base digest applied=0 recognized=0
  local patches=()

  shopt -s nullglob
  patches=("$patch_dir"/*.patch)
  shopt -u nullglob
  ((${#patches[@]} > 0)) || die "no .patch files found in $patch_dir"

  for patch_file in "${patches[@]}"; do
    base="${patch_file##*/}"
    [[ "$base" == [0-9][0-9][0-9][0-9]-*.patch ]] || \
      die "patch name must use NNNN-description.patch ordering: $base"
  done

  if [[ -e "$marker" && (! -f "$marker" || -L "$marker") ]]; then
    die "unsafe patch-state marker: $marker"
  fi

  # A recorded patch may not disappear or change under an already-patched component: either would
  # leave source bytes that the current series no longer describes.  Delete/rematerialise the
  # managed component deliberately instead of pretending that mixed state is current.
  if [[ -f "$marker" ]]; then
    while read -r digest base extra; do
      [[ -z "${digest:-}" ]] && continue
      [[ -z "${extra:-}" && "$digest" =~ ^[0-9a-f]{64}$ && "$base" != */* ]] || \
        die "malformed patch-state marker $marker"
      patch_file="$patch_dir/$base"
      [[ -f "$patch_file" ]] || \
        die "previously applied patch $base was removed; rematerialise $component before building"
      [[ "$(hash_file "$patch_file")" == "$digest" ]] || \
        die "previously applied patch $base changed; rematerialise $component before building"
    done < "$marker"
  fi

  for patch_file in "${patches[@]}"; do
    base="${patch_file##*/}"
    digest="$(hash_file "$patch_file")"
    if [[ -f "$marker" ]] && grep -qxF "$digest $base" "$marker"; then
      recognized=$((recognized + 1))
      continue
    fi

    if patch -f -N -p1 -d "$component" --dry-run -i "$patch_file" >/dev/null 2>&1; then
      patch -f -N -p1 -d "$component" -i "$patch_file" >/dev/null
      echo "tesla-ble patch applied: $base"
      applied=$((applied + 1))
    elif [[ ! -f "$marker" ]] && \
         patch -f -R -p1 -d "$component" --dry-run -i "$patch_file" >/dev/null 2>&1; then
      # Migration from the old one-patch script, which had no marker.  This branch is intentionally
      # unavailable once a marker exists: an unrecorded reverse-applicable patch then indicates drift.
      echo "tesla-ble patch already present (adopting into series): $base"
      recognized=$((recognized + 1))
    else
      die "$base does not apply cleanly to $component; pinned upstream or patch order changed"
    fi
  done

  local marker_tmp="$marker.tmp.$$"
  : > "$marker_tmp"
  for patch_file in "${patches[@]}"; do
    printf '%s %s\n' "$(hash_file "$patch_file")" "${patch_file##*/}" >> "$marker_tmp"
  done
  mv -f "$marker_tmp" "$marker"
  if ((applied == 0)); then
    echo "tesla-ble patch series already applied: ${#patches[@]} patch(es)"
  else
    echo "tesla-ble patch series complete: $applied applied, $recognized already present"
  fi
}

self_test() {
  local temp
  temp="$(mktemp -d)"
  trap 'rm -rf "$temp"' RETURN
  mkdir -p "$temp/component/src" "$temp/patches"
  printf 'alpha\n' > "$temp/component/src/vehicle.cpp"
  cat > "$temp/patches/0001-first.patch" <<'PATCH'
--- a/src/vehicle.cpp
+++ b/src/vehicle.cpp
@@ -1 +1 @@
-alpha
+beta
PATCH
  cat > "$temp/patches/0002-second.patch" <<'PATCH'
--- a/src/vehicle.cpp
+++ b/src/vehicle.cpp
@@ -1 +1 @@
-beta
+gamma
PATCH
  TESLA_BLE_PATCH_DIR="$temp/patches" TESLA_BLE_COMPONENT_DIR="$temp/component" "$0" >/dev/null
  [[ "$(cat "$temp/component/src/vehicle.cpp")" == gamma ]] || die "self-test ordering failed"
  [[ "$(wc -l < "$temp/component/.tesla-key-patch-series" | tr -d ' ')" == 2 ]] || \
    die "self-test state marker failed"
  TESLA_BLE_PATCH_DIR="$temp/patches" TESLA_BLE_COMPONENT_DIR="$temp/component" "$0" >/dev/null
  cat > "$temp/patches/0003-broken.patch" <<'PATCH'
--- a/src/vehicle.cpp
+++ b/src/vehicle.cpp
@@ -1 +1 @@
-not-the-current-source
+broken
PATCH
  if TESLA_BLE_PATCH_DIR="$temp/patches" TESLA_BLE_COMPONENT_DIR="$temp/component" \
       "$0" >/dev/null 2>&1; then
    die "self-test accepted a non-applicable patch"
  fi
  echo "tesla-ble patch-series self-test: PASS"
}

if [[ "${1:-}" == --self-test ]]; then
  self_test
  exit 0
fi

patch_dir="${TESLA_BLE_PATCH_DIR:-$repo_root/patches/tesla-ble}"
component="${TESLA_BLE_COMPONENT_DIR:-$repo_root/managed_components/yoziru__tesla-ble}"
[[ -d "$patch_dir" && ! -L "$patch_dir" ]] || die "patch directory missing or unsafe: $patch_dir"
[[ -f "$component/src/vehicle.cpp" ]] || \
  die "tesla-ble source is not materialised at $component; dependency resolution did not complete"
apply_series "$component" "$patch_dir"
