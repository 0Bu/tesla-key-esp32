#!/usr/bin/env bash
# Run any command in the SAME ESP-IDF Docker image the CI uses — so local
# build/debug never drifts from CI. The tag and immutable manifest-list digest are read at runtime
# from esp-idf-toolchain.txt. When Renovate updates that contract, both paths move together.
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
image="$("$repo_root/scripts/idf-version.sh" --image)"
echo "idf-docker: using ${image} (from esp-idf-toolchain.txt)" >&2

# Interactive TTY only when actually attached, so `menuconfig` works from a
# terminal but piped/automated runs (e.g. `... | tail`) don't break.
tty_flags=()
if [ -t 0 ] && [ -t 1 ]; then tty_flags=(-it); fi

mount_flags=(-v "$repo_root":/project)
git_common="$(git -C "$repo_root" rev-parse --git-common-dir 2>/dev/null || true)"
if [ -n "$git_common" ]; then
  git_common_abs="$(cd "$repo_root" && cd "$git_common" && pwd -P 2>/dev/null || true)"
  if [ -n "$git_common_abs" ]; then
    case "$git_common_abs" in
      "$repo_root"/*) ;;
      *)
        git_parent="$(dirname "$git_common_abs")"
        mount_flags+=(-v "$git_parent":"$git_parent")
        ;;
    esac
  fi
fi

# Hard limits keep a local ESP-IDF build from starving co-resident services. They are explicit
# here rather than inherited from a Docker daemon/shim default, so standard Docker and the k3s
# shim enforce the same ceiling. The complete S3 build is proven below both limits.
#
# -u maps to the host user so build/ artifacts aren't root-owned; HOME=/tmp gives that non-root
# user a writable home; GIT_CONFIG safe.directory='*' avoids git "dubious ownership" on the
# mounted repo and on /opt/esp/idf.
exec docker run --rm --cpus 1.5 --memory 1800m ${tty_flags[@]+"${tty_flags[@]}"} \
  "${mount_flags[@]}" -w /project \
  -u "$(id -u):$(id -g)" -e HOME=/tmp \
  -e GIT_CONFIG_COUNT=1 -e GIT_CONFIG_KEY_0=safe.directory -e GIT_CONFIG_VALUE_0='*' \
  "$image" "$@"
