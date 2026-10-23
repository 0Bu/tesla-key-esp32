#!/usr/bin/env bash
# Real Linux browser checks; no native browser process is started on the host.
set -euo pipefail

[[ $# -eq 0 ]] || { echo "usage: scripts/run-browser-tests-linux.sh" >&2; exit 2; }
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
command -v docker >/dev/null 2>&1 || { echo "Linux browser gate requires Docker" >&2; exit 2; }
docker info >/dev/null 2>&1 || { echo "Linux browser gate requires a running Docker daemon" >&2; exit 2; }
work="$(mktemp -d "${TMPDIR:-/tmp}/tesla-browser-linux.XXXXXX")"
trap 'rm -rf -- "$work"' EXIT

docker build --iidfile "$work/image-id" --file "$root/tools/browser/Dockerfile" "$root/tools/browser"
browser_image="$(cat "$work/image-id")"
[[ "$browser_image" =~ ^sha256:[0-9a-f]{64}$ ]] || { echo "Docker returned an invalid browser image ID" >&2; exit 2; }
printf 'Linux browser image: %s\n' "$browser_image"
docker run --rm --network none --read-only --cap-drop ALL --security-opt no-new-privileges \
    --cpus 1.5 --memory 1800m --tmpfs /tmp:rw,nosuid,mode=1777,size=512m \
    --env XDG_CONFIG_HOME=/tmp/browser-config --env XDG_CACHE_HOME=/tmp/browser-cache \
    --mount "type=bind,source=$root,target=/project,readonly" \
    "$browser_image" /bin/sh -ec '
        chromium --version
        python3 test/test_web_ui_browser_gate.py
        python3 test/web_ui_browser_gate.py --require-browser
    '
