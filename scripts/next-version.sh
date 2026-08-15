#!/usr/bin/env bash
# Print the next stable release version.
#
# version.txt is the FLOOR: bump it to cut a manual minor/major release. Otherwise stable tags
# contribute their next patch and prerelease tags contribute their stable core (promotion):
#
#   no valid tags                         -> version.txt verbatim
#   latest stable v1.4.76                 -> max(floor, 1.4.77)
#   only prerelease v1.5.0-rc.1           -> max(floor, 1.5.0)
#   stable v1.4.76 + v1.5.0-rc.1          -> max(floor, 1.4.77, 1.5.0)
#
# Every valid vX.Y.Z[-prerelease] tag is considered; version-sort alone is not SemVer-correct
# around prereleases. Invalid v* tags are ignored. Numeric components are parsed explicitly as
# base-10, reject leading zeroes/overflow-sized shell integers, and never reach arithmetic unless
# validated. Requires tags to be fetched before calling in CI.
# Usage: ./scripts/next-version.sh [--self-test]
set -euo pipefail

CORE_RE='^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$'
TAG_RE='^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z.-]+)?$'

normalize_core() {
  local core="$1" major minor patch
  [[ "$core" =~ $CORE_RE ]] || return 2
  major="${BASH_REMATCH[1]}"
  minor="${BASH_REMATCH[2]}"
  patch="${BASH_REMATCH[3]}"
  # Keep Bash arithmetic deterministic and fail closed instead of overflowing a hostile tag.
  ((${#major} <= 9 && ${#minor} <= 9 && ${#patch} <= 9)) || return 2
  printf '%d.%d.%d\n' "$((10#$major))" "$((10#$minor))" "$((10#$patch))"
}

bump_stable_patch() {
  local core="$1" normalized major rest minor patch
  normalized="$(normalize_core "$core")" || return
  major="${normalized%%.*}"
  rest="${normalized#*.}"
  minor="${rest%%.*}"
  patch="${rest##*.}"
  ((patch < 999999999)) || return 2
  printf '%s.%s.%d\n' "$major" "$minor" "$((patch + 1))"
}

next_release_version() {
  local repo_root="$1" base base_normalized tag version core candidate result
  local -a candidates=()
  base="$(tr -d '[:space:]' < "$repo_root/version.txt")" || return
  base_normalized="$(normalize_core "$base")" || {
    echo "invalid version.txt floor (expected canonical X.Y.Z with bounded decimals): $base" >&2
    return 2
  }
  candidates+=("$base_normalized")

  while IFS= read -r tag; do
    [[ "$tag" =~ $TAG_RE ]] || continue
    version="${tag#v}"
    core="${version%%-*}"
    if [[ "$version" == *-* ]]; then
      candidate="$(normalize_core "$core")" || continue
    else
      candidate="$(bump_stable_patch "$core")" || continue
    fi
    candidates+=("$candidate")
  done < <(git -C "$repo_root" tag -l 'v*')

  result="$(printf '%s\n' "${candidates[@]}" | sort -V | tail -n1)" || return
  [[ "$result" =~ $CORE_RE ]] || {
    echo "could not derive a canonical next release version" >&2
    return 2
  }
  printf '%s\n' "$result"
}

self_test_case() {
  local root="$1" name="$2" expected="$3" base="$4" got
  shift 4
  local repo="$root/$name" tag
  mkdir -p "$repo"
  git -C "$repo" init -q
  git -C "$repo" config user.name test
  git -C "$repo" config user.email test@example.invalid
  git -C "$repo" config commit.gpgsign false
  git -C "$repo" config tag.gpgsign false
  printf '%s\n' "$base" > "$repo/version.txt"
  git -C "$repo" add version.txt
  git -C "$repo" commit -qm initial
  for tag in "$@"; do git -C "$repo" tag "$tag"; done
  got="$(next_release_version "$repo")" || {
    echo "$name: version selection failed" >&2; return 1;
  }
  [[ "$got" == "$expected" ]] || {
    echo "$name: expected=$expected actual=$got" >&2; return 1;
  }
}

self_test() {
  local tmp bad
  tmp="$(mktemp -d "${TMPDIR:-/tmp}/tesla-next-version.XXXXXX")"
  trap 'rm -rf -- "$tmp"' RETURN
  self_test_case "$tmp" rc-only 1.4.76 1.2.0 v1.4.76-rc.1
  self_test_case "$tmp" stable-newer-rc 1.5.0 1.2.0 v1.4.76 v1.5.0-rc.1
  self_test_case "$tmp" stable-older-rc 1.4.77 1.2.0 v1.4.76 v1.4.70-rc.9
  self_test_case "$tmp" invalid-tags 1.2.0 1.2.0 vbanana v1.2.x v01.2.3 v1.2.3_bad
  self_test_case "$tmp" current-stable 1.2.2 1.2.0 v1.2.1

  bad="$tmp/bad-floor"
  mkdir -p "$bad"
  git -C "$bad" init -q
  printf '01.2.0\n' > "$bad/version.txt"
  if next_release_version "$bad" >/dev/null 2>&1; then
    echo "invalid decimal floor was accepted" >&2
    return 1
  fi
  echo "next-version SemVer self-test: PASS"
}

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
case "${1:-}" in
  --self-test)
    [[ $# -eq 1 ]] || { echo "usage: $0 [--self-test]" >&2; exit 2; }
    self_test
    ;;
  '')
    [[ $# -eq 0 ]] || exit 2
    next_release_version "$repo_root"
    ;;
  *) echo "usage: $0 [--self-test]" >&2; exit 2 ;;
esac
