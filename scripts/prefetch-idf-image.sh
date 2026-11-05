#!/usr/bin/env bash
# Warm the exact pinned image on this runner while key-free artifact checks run.
# This is not signing authority; idf-docker.sh independently selects the pin and limits.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
mode="${1:-}"
state="${2:-}"
wait_seconds="${3:-600}"
if [[ "$#" -lt 2 || "$#" -gt 3 || "$state" != /* || "$state" == / ]] \
    || [[ "$mode" != start && "$mode" != wait ]] \
    || [[ "$mode" == start && "$#" -ne 2 ]] \
    || ! [[ "$wait_seconds" =~ ^[1-9][0-9]*$ ]] \
    || (( ${#wait_seconds} > 3 || wait_seconds > 600 )); then
  echo "usage: prefetch-idf-image.sh {start|wait} <absolute-private-state> [wait-seconds:1..600]" >&2
  exit 2
fi
image="$("$repo_root/scripts/idf-version.sh" --image)"

if [[ "$mode" == start ]]; then
  umask 077
  # Exclusive creation prevents a previous job/attempt from supplying a ready marker.
  mkdir -m 700 "$state"
  printf '%s\n' "$image" > "$state/image"
  (
    result=0
    docker pull "$image" || result=$?
    printf '%s\n' "$result" > "$state/result.tmp"
    mv "$state/result.tmp" "$state/result"
  ) < /dev/null > "$state/pull.log" 2>&1 &
  echo "signing image prefetch: started (no key provisioned)"
  exit 0
fi

[[ -d "$state" && ! -L "$state" && -f "$state/image" && ! -L "$state/image" ]] \
  && [[ "$(cat "$state/image")" == "$image" ]] || {
    echo "signing image prefetch: missing state or changed toolchain pin" >&2
    exit 1
  }
for ((second=0; second<wait_seconds; second++)); do
  [[ ! -e "$state/result" && ! -L "$state/result" ]] || break
  sleep 1
done
if [[ ! -f "$state/result" || -L "$state/result" ]] \
    || [[ "$(cat "$state/result")" != 0 ]]; then
  echo "signing image prefetch: failed or timed out before key provisioning" >&2
  if [[ -f "$state/pull.log" && ! -L "$state/pull.log" ]]; then
    # Preserve bounded, key-free Docker diagnostics on ephemeral CI runners.
    tail -c 4096 "$state/pull.log" >&2 || true
  fi
  exit 1
fi
# A marker alone cannot stand in for the immutable image being present in this daemon.
docker image inspect "$image" > /dev/null
echo "signing image prefetch: ready (exact pinned image)"
