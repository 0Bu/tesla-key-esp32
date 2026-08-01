#!/usr/bin/env bash
# Apply the repository-owned correctness patches to the pinned yoziru/tesla-ble
# dependency after ESP-IDF has materialised it. The Component Manager regenerates
# managed_components/, so the patch itself is committed while the patched checkout
# remains ignored.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
patch_file="$repo_root/patches/tesla-ble/0001-reject-replayed-carserver-responses.patch"

[ -f "$patch_file" ] || {
  echo "tesla-ble patch missing: $patch_file" >&2
  exit 1
}

found=0
for component in \
  "$repo_root/managed_components/yoziru__tesla-ble"
do
  [ -f "$component/src/vehicle.cpp" ] || continue
  found=1

  if patch -f -N -p1 -d "$component" --dry-run -i "$patch_file" >/dev/null 2>&1; then
    patch -f -N -p1 -d "$component" -i "$patch_file" >/dev/null
    echo "tesla-ble patch applied: $component"
  elif patch -f -R -p1 -d "$component" --dry-run -i "$patch_file" >/dev/null 2>&1; then
    echo "tesla-ble patch already applied: $component"
  else
    echo "tesla-ble patch does not apply cleanly to $component" >&2
    echo "Pinned upstream source changed; rebase and review the anti-replay patch." >&2
    exit 1
  fi
done

[ "$found" = 1 ] || {
  echo "tesla-ble source is not materialised; dependency resolution did not complete" >&2
  exit 1
}
