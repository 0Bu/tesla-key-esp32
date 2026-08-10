#!/usr/bin/env bash
# Print the ESP-IDF tag, digest-pinned Docker tag suffix, or full image reference from the one
# repository-owned toolchain contract. Both local builds and GitHub Actions use this reader.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
contract="$repo_root/esp-idf-toolchain.txt"
value="$(tr -d '[:space:]' < "$contract")"

if ! [[ "$value" =~ ^v[0-9]+\.[0-9]+\.[0-9]+@sha256:[0-9a-f]{64}$ ]]; then
  echo "idf-version: invalid toolchain contract in $contract" >&2
  exit 1
fi

case "${1:-}" in
  "")              printf '%s\n' "${value%%@*}" ;;
  --image-version) printf '%s\n' "$value" ;;
  --image)         printf 'espressif/idf:%s\n' "$value" ;;
  *)               echo "usage: idf-version.sh [--image-version|--image]" >&2; exit 2 ;;
esac
