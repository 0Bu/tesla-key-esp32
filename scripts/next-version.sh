#!/usr/bin/env bash
# Print the next release version.
#
# version.txt is the FLOOR: bump it to cut a manual minor/major release. Otherwise
# the patch level auto-increments above the most recent v* tag, so every
# firmware-relevant change to main gets its own monotonic release without a manual
# version bump.
#
#   no tags yet            -> version.txt verbatim          (e.g. 1.0.0)
#   latest tag v1.0.3      -> max(version.txt, 1.0.4)        (auto patch bump)
#   version.txt 1.1.0 > tag-> 1.1.0                          (honor manual bump)
#
# Requires tags to be fetched (git fetch --tags) before calling in CI.
# Usage:  ./scripts/next-version.sh
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
base="$(tr -d '[:space:]' < "$repo_root/version.txt")"
latest="$(git -C "$repo_root" tag -l 'v*' --sort=-v:refname | head -n1 | sed 's/^v//')"

if [[ -z "$latest" ]]; then
  echo "$base"
  exit 0
fi

major="${latest%%.*}"
rest="${latest#*.}"
minor="${rest%%.*}"
patch="${rest##*.}"
bumped="${major}.${minor}.$((patch + 1))"

# Highest of the manual floor and the auto-bumped patch wins.
printf '%s\n%s\n' "$base" "$bumped" | sort -V | tail -n1
