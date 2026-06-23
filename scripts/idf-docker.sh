#!/usr/bin/env bash
# Run any command in the SAME ESP-IDF Docker image the CI uses — so local
# build/debug never drifts from CI. The version is read at runtime from the
# single source of truth, .github/workflows/build.yml (`esp_idf_version:`,
# kept current by Renovate). When that bumps, the next build here auto-pulls
# the matching image; nothing else to update.
#
# There is no local ESP-IDF install on this machine (removed on purpose) — this
# wrapper is the only build path. Flashing still happens on the HOST with
# `esptool` (Docker Desktop on macOS has no USB passthrough).
#
# Usage:
#   scripts/idf-docker.sh idf.py set-target esp32s3 build
#   scripts/idf-docker.sh idf.py menuconfig            # interactive (-it auto)
#   scripts/idf-docker.sh sh -c 'if [ -f sdkconfig ]; then idf.py build; \
#                                else idf.py set-target esp32s3 build; fi'
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ci_yml="$repo_root/.github/workflows/build.yml"

[ -f "$ci_yml" ] || { echo "idf-docker: $ci_yml not found" >&2; exit 1; }

# Pull the exact `esp_idf_version: vX.Y[.Z]` value CI builds with.
idf_version="$(grep -oE 'esp_idf_version:[[:space:]]*v[0-9]+\.[0-9]+(\.[0-9]+)?' "$ci_yml" \
  | grep -oE 'v[0-9]+\.[0-9]+(\.[0-9]+)?' | head -n1)"
[ -n "${idf_version:-}" ] || {
  echo "idf-docker: could not read esp_idf_version from $ci_yml" >&2; exit 1
}
image="espressif/idf:${idf_version}"
echo "idf-docker: using ${image} (from .github/workflows/build.yml)" >&2

# Interactive TTY only when actually attached, so `menuconfig` works from a
# terminal but piped/automated runs (e.g. `... | tail`) don't break.
tty_flags=()
if [ -t 0 ] && [ -t 1 ]; then tty_flags=(-it); fi

# -u maps to the host user so build/ artifacts aren't root-owned; HOME=/tmp gives
# that non-root user a writable home; GIT_CONFIG safe.directory='*' avoids git
# "dubious ownership" on the mounted repo and on /opt/esp/idf.
exec docker run --rm ${tty_flags[@]+"${tty_flags[@]}"} \
  -v "$repo_root":/project -w /project \
  -u "$(id -u):$(id -g)" -e HOME=/tmp \
  -e GIT_CONFIG_COUNT=1 -e GIT_CONFIG_KEY_0=safe.directory -e GIT_CONFIG_VALUE_0='*' \
  "$image" "$@"
