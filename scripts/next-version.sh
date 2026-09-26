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

bump_version() {
  local core="$1" level="$2" normalized major rest minor patch
  normalized="$(normalize_core "$core")" || return
  major="${normalized%%.*}"
  rest="${normalized#*.}"
  minor="${rest%%.*}"
  patch="${rest##*.}"
  case "$level" in
    patch)
      ((patch < 999999999)) || return 2
      printf '%s.%s.%d\n' "$major" "$minor" "$((patch + 1))"
      ;;
    minor)
      ((minor < 999999999)) || return 2
      printf '%s.%d.0\n' "$major" "$((minor + 1))"
      ;;
    major)
      ((major < 999999999)) || return 2
      printf '%d.0.0\n' "$((major + 1))"
      ;;
    *) return 2 ;;
  esac
}

bump_stable_patch() {
  bump_version "$1" patch
}

next_release_version() {
  local repo_root="$1" level="${2:-patch}" base base_normalized tag version core candidate result
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
      candidate="$(bump_version "$core" "$level")" || continue
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

next_dev_version() {
  local repo_root="$1" next_patch latest_tag="" tag version count
  next_patch="$(next_release_version "$repo_root" patch)" || return

  while IFS= read -r tag; do
    [[ "$tag" =~ $TAG_RE ]] || continue
    version="${tag#v}"
    if [[ "$version" != *-* ]]; then
      if [[ -z "$latest_tag" ]] || [[ "$(printf '%s\n%s\n' "$latest_tag" "$tag" | sort -V | tail -n1)" == "$tag" ]]; then
        latest_tag="$tag"
      fi
    fi
  done < <(git -C "$repo_root" tag -l 'v*')

  if [[ -n "$latest_tag" ]]; then
    git -C "$repo_root" merge-base --is-ancestor "$latest_tag" HEAD 2>/dev/null || {
      echo "latest tag $latest_tag is not reachable from HEAD" >&2
      return 2
    }
    count="$(git -C "$repo_root" rev-list --count "${latest_tag}..HEAD")" || return 2
  else
    count="$(git -C "$repo_root" rev-list --count HEAD)" || return 2
  fi
  printf '%s-dev.%s\n' "$next_patch" "$count"
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
  local tmp bad got_minor got_major got_dev
  tmp="$(mktemp -d "${TMPDIR:-/tmp}/tesla-next-version.XXXXXX")"
  trap 'rm -rf -- "$tmp"' RETURN
  self_test_case "$tmp" rc-only 1.4.76 1.2.0 v1.4.76-rc.1
  self_test_case "$tmp" stable-newer-rc 1.5.0 1.2.0 v1.4.76 v1.5.0-rc.1
  self_test_case "$tmp" stable-older-rc 1.4.77 1.2.0 v1.4.76 v1.4.70-rc.9
  self_test_case "$tmp" invalid-tags 1.2.0 1.2.0 vbanana v1.2.x v01.2.3 v1.2.3_bad
  self_test_case "$tmp" current-stable 1.2.2 1.2.0 v1.2.1

  got_minor="$(next_release_version "$tmp/current-stable" minor)"
  [[ "$got_minor" == "1.3.0" ]] || { echo "minor bump: expected 1.3.0 got $got_minor" >&2; return 1; }
  got_major="$(next_release_version "$tmp/current-stable" major)"
  [[ "$got_major" == "2.0.0" ]] || { echo "major bump: expected 2.0.0 got $got_major" >&2; return 1; }

  echo "test" > "$tmp/current-stable/dummy.txt"
  git -C "$tmp/current-stable" add dummy.txt
  git -C "$tmp/current-stable" commit -qm "commit 2"
  got_dev="$(next_dev_version "$tmp/current-stable")"
  [[ "$got_dev" == "1.2.2-dev.1" ]] || { echo "dev version: expected 1.2.2-dev.1 got $got_dev" >&2; return 1; }

  bad="$tmp/bad-floor"
  mkdir -p "$bad"
  git -C "$bad" init -q
  printf '01.2.0\n' > "$bad/version.txt"
  if next_release_version "$bad" >/dev/null 2>&1; then
    echo "invalid decimal floor was accepted" >&2
    return 1
  fi

  # Dev version fails closed when latest tag is not an ancestor of HEAD
  git -C "$tmp/current-stable" checkout -qb side-branch
  echo "side" > "$tmp/current-stable/side.txt"
  git -C "$tmp/current-stable" add side.txt
  git -C "$tmp/current-stable" commit -qm "side commit"
  git -C "$tmp/current-stable" tag v1.9.0
  git -C "$tmp/current-stable" checkout -q -
  if next_dev_version "$tmp/current-stable" >/dev/null 2>&1; then
    echo "dev version with unmerged tag succeeded instead of failing closed" >&2
    return 1
  fi

  # Test --exact validation
  local self_script
  self_script="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)/next-version.sh"
  if (cd "$tmp/current-stable" && "$self_script" --exact 1.1.0 >/dev/null 2>&1); then
    echo "--exact below version.txt floor was accepted" >&2
    return 1
  fi
  if (cd "$tmp/current-stable" && "$self_script" --exact 1.2.1 >/dev/null 2>&1); then
    echo "--exact for existing tag was accepted" >&2
    return 1
  fi
  local got_exact
  got_exact="$(cd "$tmp/current-stable" && "$self_script" --exact 1.3.0)"
  [[ "$got_exact" == "1.3.0" ]] || { echo "--exact: expected 1.3.0 got $got_exact" >&2; return 1; }

  echo "next-version SemVer self-test: PASS"
}

repo_root="${REPO_ROOT:-$(git rev-parse --show-toplevel 2>/dev/null || (cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P))}"
dev=no
level=patch
exact=""

while [ "$#" -gt 0 ]; do
  case "$1" in
    --self-test)
      [[ $# -eq 1 ]] || { echo "usage: $0 [--self-test]" >&2; exit 2; }
      self_test
      exit 0
      ;;
    --dev)
      dev=yes; shift ;;
    --exact)
      [ "$#" -ge 2 ] || { echo "$0: --exact requires X.Y.Z" >&2; exit 2; }
      exact="$2"; shift 2 ;;
    patch|minor|major)
      level="$1"; shift ;;
    *)
      echo "usage: $0 [--self-test | --dev | --exact X.Y.Z | patch | minor | major]" >&2
      exit 2
      ;;
  esac
done

if [ -n "$exact" ]; then
  [[ "$exact" =~ $CORE_RE ]] || {
    echo "$0: --exact requires strict canonical X.Y.Z (got '$exact')" >&2
    exit 2
  }
  base="$(tr -d '[:space:]' < "$repo_root/version.txt")" || exit 2
  base_normalized="$(normalize_core "$base")" || {
    echo "$0: invalid version.txt floor: $base" >&2
    exit 2
  }
  if [[ "$(printf '%s\n%s\n' "$base_normalized" "$exact" | sort -V | tail -n1)" != "$exact" ]]; then
    echo "$0: --exact version $exact is below version.txt floor $base_normalized" >&2
    exit 2
  fi
  if git -C "$repo_root" rev-parse -q --verify "refs/tags/v$exact" >/dev/null; then
    echo "$0: tag v$exact already exists" >&2
    exit 2
  fi
  printf '%s\n' "$exact"
  exit 0
fi

if [ "$dev" = yes ]; then
  next_dev_version "$repo_root"
else
  next_release_version "$repo_root" "$level"
fi
