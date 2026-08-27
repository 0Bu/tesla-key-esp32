#!/usr/bin/env bash
# Select the release version for a trusted main build.
#
# A trusted main selection first fetches origin/main and refuses any stale run. It reuses one stable
# vX.Y.Z tag that dereferences exactly to that commit only when it is also the newest valid release
# tag. Prerelease tags contribute their core to next-version selection but are never reused as the
# production GitHub Release/Pages identity. This makes a current-run retry idempotent without
# allowing an old run to roll Pages back. With no arguments (PR/manual validation), compute the next
# version without claiming release identity.
set -euo pipefail

STABLE_TAG_RE='^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$'
VERSION_RE='^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$'
MAX_VERSION_BYTES=31

valid_stable_version() {
  local version="$1"
  [[ "$version" =~ $VERSION_RE ]] && (( ${#version} <= MAX_VERSION_BYTES ))
}

valid_stable_tag() {
  local tag="$1"
  [[ "$tag" =~ $STABLE_TAG_RE ]] && (( ${#tag} - 1 <= MAX_VERSION_BYTES ))
}

validate_sha() {
  local repo_root="$1" sha="$2" label="$3"
  [[ "$sha" =~ ^[0-9a-f]{40}$ ]] \
    && git -C "$repo_root" cat-file -e "${sha}^{commit}" 2>/dev/null || {
      echo "invalid $label SHA: $sha" >&2; return 2;
    }
}

validate_current_main() {
  local repo_root="$1" source_sha="$2" current_main_sha="$3"
  validate_sha "$repo_root" "$source_sha" release-source || return
  validate_sha "$repo_root" "$current_main_sha" origin-main || return
  [[ "$source_sha" == "$current_main_sha" ]] || {
    echo "stale release run: source=$source_sha current-origin-main=$current_main_sha" >&2
    return 2
  }
}

fetch_tags() {
  # Remote tags are the release authority. Prune deleted tags as well as updating/adding them;
  # otherwise a checkout-local ghost tag could satisfy a current-release revalidation after the
  # corresponding remote tag/Release was removed.
  git -C "$1" fetch --quiet --tags --force --prune --prune-tags origin || return
}

fetch_current_main() {
  local repo_root="$1" current
  git -C "$repo_root" fetch --quiet --no-tags --force origin \
    refs/heads/main:refs/remotes/origin/main || return
  current="$(git -C "$repo_root" rev-parse refs/remotes/origin/main)" || return
  validate_sha "$repo_root" "$current" origin-main || return
  printf '%s\n' "$current"
}

latest_valid_tag() {
  local repo_root="$1" tag rows=""
  while IFS= read -r tag; do
    valid_stable_tag "$tag" || continue
    rows+="${rows:+$'\n'}$tag"
  done < <(git -C "$repo_root" tag -l 'v*')
  [[ -n "$rows" ]] || return 0
  printf '%s\n' "$rows" | sort -V | tail -n1
}

# latest_published_stable_version <repo>
#   Print the version of the latest complete production GitHub Release. This is the PR-preview
#   numeric base: using the raw newest v* tag would let v1.5.0-rc.1 outrank released v1.4.76 and
#   produce a 1.5.0-rc.1-PR-N image that the firmware later considers numerically equal to 1.5.0.
latest_published_stable_version() {
  local repo_root="$1" repository release_json tag version tag_sha
  command -v gh >/dev/null 2>&1 && command -v jq >/dev/null 2>&1 || {
    echo "gh and jq are required to select the latest published stable Release" >&2
    return 2
  }
  repository="${GITHUB_REPOSITORY:-}"
  [[ "$repository" =~ ^[0-9A-Za-z_.-]+/[0-9A-Za-z_.-]+$ ]] || {
    echo "invalid or missing GITHUB_REPOSITORY" >&2
    return 2
  }
  release_json="$(gh api "repos/$repository/releases/latest" 2>/dev/null)" || return 2
  tag="$(printf '%s' "$release_json" | jq -er '
    select(.draft == false and .prerelease == false and .immutable == true and
           (.tag_name | type == "string" and
            test("^v(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$"))) |
    .tag_name
  ' 2>/dev/null)" || return 2
  valid_stable_tag "$tag" || return 2
  version="${tag#v}"
  tag_sha="$(git -C "$repo_root" rev-parse "${tag}^{commit}" 2>/dev/null)" || return 2
  [[ "$tag_sha" =~ ^[0-9a-f]{40}$ ]] || return 2
  # Legacy Releases may retain target_commitish="main". For the preview numeric base this field is
  # not provenance: the exact published tag, its local dereferenced commit and the complete asset
  # set are sufficient. Current production publication remains SHA-bound by the stricter
  # require_published_release path below.
  printf '%s' "$release_json" | jq -e --arg v "$version" '
    ["tesla-key-esp32-" + $v + "-merged.bin",
     "tesla-key-esp32-s3-" + $v + "-merged.bin",
     "tesla-key-esp32-c3-" + $v + "-merged.bin",
     "tesla-key-esp32-c6-" + $v + "-merged.bin"] as $expected |
    . as $release |
    select(all($expected[]; . as $name |
      ([$release.assets[] | select(.name == $name)] |
       length == 1 and (.[0].id | type == "number") and
       (.[0].size | type == "number" and . > 0) and
       (.[0].digest | type == "string" and test("^sha256:[0-9a-f]{64}$")))))
  ' >/dev/null || {
    echo "latest stable Release lacks the exact four digest-bound merged assets" >&2
    return 2
  }
  printf '%s\n' "$version"
}

select_version() {
  local repo_root="$1" source_sha="$2" current_main_sha="$3" tag version latest_tag tag_rows resolved
  local -a matching=()

  validate_current_main "$repo_root" "$source_sha" "$current_main_sha" || return
  latest_tag="$(latest_valid_tag "$repo_root")" || return
  tag_rows="$(git -C "$repo_root" tag -l 'v*')" || return
  while IFS= read -r tag; do
    valid_stable_tag "$tag" || continue
    resolved="$(git -C "$repo_root" rev-parse "${tag}^{commit}")" || return
    [[ "$resolved" == "$source_sha" ]] || continue
    matching+=("$tag")
  done <<< "$tag_rows"

  if ((${#matching[@]} > 1)); then
    echo "multiple valid release tags point at $source_sha: ${matching[*]}" >&2
    return 2
  fi
  if ((${#matching[@]} == 1)); then
    [[ "${matching[0]}" == "$latest_tag" ]] || {
      echo "stale release tag ${matching[0]} is not newest valid tag ${latest_tag:-<none>}" >&2
      return 2
    }
    version="${matching[0]#v}"
    echo "reusing current release v$version for exact source $source_sha" >&2
    printf '%s\n' "$version"
    return 0
  fi

  "$repo_root/scripts/next-version.sh"
}

require_current_release() {
  local repo_root="$1" source_sha="$2" current_main_sha="$3" version="$4"
  local tag latest_tag candidate tag_rows resolved
  local -a matching=()
  validate_current_main "$repo_root" "$source_sha" "$current_main_sha" || return
  valid_stable_version "$version" || {
    echo "invalid or overlong release version: $version" >&2; return 2;
  }
  tag="v$version"
  latest_tag="$(latest_valid_tag "$repo_root")" || return
  tag_rows="$(git -C "$repo_root" tag -l 'v*')" || return
  while IFS= read -r candidate; do
    valid_stable_tag "$candidate" || continue
    resolved="$(git -C "$repo_root" rev-parse "${candidate}^{commit}")" || return
    [[ "$resolved" == "$source_sha" ]] || continue
    matching+=("$candidate")
  done <<< "$tag_rows"
  ((${#matching[@]} == 1)) && [[ "${matching[0]}" == "$tag" && "$latest_tag" == "$tag" ]] || {
    echo "release is not uniquely/currently bound: expected=$tag latest=${latest_tag:-<none>} source=$source_sha" >&2
    return 2
  }
}

require_published_release() {
  local repo_root="$1" source_sha="$2" current_main_sha="$3" version="$4"
  local repository release_json latest_json release_id latest_id tag metadata_file
  require_current_release "$repo_root" "$source_sha" "$current_main_sha" "$version" || return
  command -v gh >/dev/null 2>&1 && command -v jq >/dev/null 2>&1 || {
    echo "gh and jq are required to verify the published GitHub Release" >&2; return 2;
  }
  repository="${GITHUB_REPOSITORY:-}"
  [[ "$repository" =~ ^[0-9A-Za-z_.-]+/[0-9A-Za-z_.-]+$ ]] || {
    echo "invalid or missing GITHUB_REPOSITORY" >&2; return 2;
  }
  tag="v$version"
  release_json="$(gh api "repos/$repository/releases/tags/$tag")" || return
  metadata_file="$(mktemp "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/tesla-release-metadata.XXXXXX")" \
    || return 2
  if ! printf '%s' "$release_json" > "$metadata_file" \
      || ! python3 "$repo_root/scripts/check-release-assets.py" \
        "$metadata_file" --metadata-only --version "$version" --source-sha "$source_sha" \
        --expect-state published-immutable >/dev/null; then
    rm -f -- "$metadata_file"
    echo "GitHub Release $tag failed the exact 40-asset metadata contract" >&2
    return 2
  fi
  rm -f -- "$metadata_file"
  release_id="$(printf '%s' "$release_json" | jq -er --arg tag "$tag" \
      --arg sha "$source_sha" --arg v "$version" '
    select(.tag_name == $tag and .target_commitish == $sha and
           .draft == false and .prerelease == false and .immutable == true) |
    ["tesla-key-esp32-" + $v + "-merged.bin",
     "tesla-key-esp32-s3-" + $v + "-merged.bin",
     "tesla-key-esp32-c3-" + $v + "-merged.bin",
     "tesla-key-esp32-c6-" + $v + "-merged.bin"] as $expected |
    . as $release |
    select(all($expected[]; . as $name |
      ([$release.assets[] | select(.name == $name)] |
       length == 1 and (.[0].id | type == "number") and
       (.[0].size | type == "number" and . > 0) and
       (.[0].digest | type == "string" and test("^sha256:[0-9a-f]{64}$"))))) |
    .id | select(type == "number")
  ')" || {
    echo "GitHub Release $tag is absent, mutable, stale or lacks exact digest-bound merged assets" >&2
    return 2
  }
  latest_json="$(gh api "repos/$repository/releases/latest")" || return
  latest_id="$(printf '%s' "$latest_json" | jq -er \
    'select(.draft == false and .prerelease == false and .immutable == true) |
     .id | select(type == "number")')" || return
  [[ "$latest_id" == "$release_id" ]] || {
    echo "GitHub Release $tag is not the latest published Release" >&2
    return 2
  }
}

require_release_candidate() {
  local repo_root="$1" source_sha="$2" current_main_sha="$3" version="$4" selected
  valid_stable_version "$version" || {
    echo "invalid or overlong release version: $version" >&2; return 2;
  }
  selected="$(select_version "$repo_root" "$source_sha" "$current_main_sha")" || return
  [[ "$selected" == "$version" ]] || {
    echo "stale release candidate: selected=$version current-authorized=$selected" >&2
    return 2
  }
}

self_test() {
  local script_root tmp remote sha_a sha_b sha_c sha_d got current fake_legacy overlong_version
  local fake_mutable fake_without_immutable
  script_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
  tmp="$(mktemp -d "${TMPDIR:-/tmp}/tesla-release-version.XXXXXX")"
  remote="$tmp.remote.git"
  trap 'rm -rf -- "$tmp" "$remote"' RETURN
  mkdir -p "$tmp/scripts"
  cp "$script_root/next-version.sh" "$tmp/scripts/"
  # check-release-assets loads both validators by exact sibling path at import time. Keep the
  # isolated selector fixture closed but complete, so its published-Release test exercises the
  # same fail-closed validator import graph as CI instead of failing before the contract runs.
  for validator in \
    check-release-assets.py check-otadata-contract.py check-firmware-artifacts.py; do
    [[ -f "$script_root/$validator" && ! -L "$script_root/$validator" ]] || {
      echo "selector self-test validator import is missing/unsafe: $validator" >&2
      return 1
    }
    cp "$script_root/$validator" "$tmp/scripts/"
  done
  chmod +x "$tmp/scripts/next-version.sh"
  "$tmp/scripts/next-version.sh" --self-test >/dev/null
  printf '1.2.0\n' > "$tmp/version.txt"
  git -C "$tmp" init -q
  git -C "$tmp" config user.name test
  git -C "$tmp" config user.email test@example.invalid
  git -C "$tmp" config commit.gpgsign false
  git -C "$tmp" config tag.gpgsign false
  git -C "$tmp" add version.txt scripts/next-version.sh
  git -C "$tmp" commit -qm initial
  sha_a="$(git -C "$tmp" rev-parse HEAD)"

  got="$(select_version "$tmp" "$sha_a" "$sha_a")"
  [[ "$got" == 1.2.0 ]] || { echo "no-tag selection failed: $got" >&2; return 1; }
  git -C "$tmp" tag v1.2.0
  printf 'next\n' > "$tmp/change"
  git -C "$tmp" add change
  git -C "$tmp" commit -qm next
  sha_b="$(git -C "$tmp" rev-parse HEAD)"
  got="$(select_version "$tmp" "$sha_b" "$sha_b")"
  [[ "$got" == 1.2.1 ]] || { echo "next-version selection failed: $got" >&2; return 1; }
  require_release_candidate "$tmp" "$sha_b" "$sha_b" 1.2.1
  if require_release_candidate "$tmp" "$sha_b" "$sha_b" 1.2.2 >/dev/null 2>&1; then
    echo "wrong release candidate was accepted" >&2
    return 1
  fi

  git -C "$tmp" tag v1.2.1
  got="$(select_version "$tmp" "$sha_b" "$sha_b")"
  [[ "$got" == 1.2.1 ]] || { echo "same-SHA tag reuse failed: $got" >&2; return 1; }
  require_current_release "$tmp" "$sha_b" "$sha_b" 1.2.1
  overlong_version="12345678901234567890123456789012.0.0"
  (( ${#overlong_version} > MAX_VERSION_BYTES )) || {
    echo "overlong release fixture is not overlong" >&2; return 1;
  }
  git -C "$tmp" tag "v$overlong_version" "$sha_b"
  got="$(select_version "$tmp" "$sha_b" "$sha_b")"
  [[ "$got" == 1.2.1 ]] || {
    echo "overlong stable tag changed same-SHA selection: $got" >&2; return 1;
  }
  if require_current_release "$tmp" "$sha_b" "$sha_b" "$overlong_version" \
      >/dev/null 2>&1; then
    echo "overlong current release version was accepted" >&2
    return 1
  fi
  if require_release_candidate "$tmp" "$sha_b" "$sha_b" "$overlong_version" \
      >/dev/null 2>&1; then
    echo "overlong release candidate was accepted" >&2
    return 1
  fi
  git -C "$tmp" tag v1.2.1-rc.1 "$sha_a"
  got="$(select_version "$tmp" "$sha_b" "$sha_b")"
  [[ "$got" == 1.2.1 ]] || {
    echo "stable tag did not outrank same-core prerelease: $got" >&2; return 1;
  }
  fakebin="$tmp/fakebin"
  fake_release="$tmp/fake-release.json"
  fake_latest_newer="$tmp/fake-latest-newer.json"
  mkdir -p "$fakebin"
  printf '%s\n' '#!/usr/bin/env bash' 'set -eu' \
    'case "$2" in */releases/latest) cat "$GH_FAKE_LATEST" ;; *) cat "$GH_FAKE_RELEASE" ;; esac' \
    > "$fakebin/gh"
  chmod +x "$fakebin/gh"
  python3 - "$script_root/check-release-assets.py" "$fake_release" "$sha_b" <<'PY'
import importlib.util
import json
from pathlib import Path
import sys
import tempfile

spec = importlib.util.spec_from_file_location("release_assets", sys.argv[1])
module = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(module)
with tempfile.TemporaryDirectory(prefix="release-selector-assets-") as directory:
    root = Path(directory)
    for asset in module.expected_local_assets(root, "1.2.1").values():
        asset.parent.mkdir(parents=True, exist_ok=True)
        asset.write_bytes(b"x")
    release = module.make_full_release(
        root, "1.2.1", sys.argv[3], "published-immutable"
    )
    release["id"] = 7
    Path(sys.argv[2]).write_text(json.dumps(release), encoding="utf-8")
PY
  jq '.id = 8' "$fake_release" > "$fake_latest_newer"
  fake_mutable="$tmp/fake-release-mutable.json"
  fake_without_immutable="$tmp/fake-release-without-immutable.json"
  jq '.immutable = false' "$fake_release" > "$fake_mutable"
  jq 'del(.immutable)' "$fake_release" > "$fake_without_immutable"
  PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo \
    GH_FAKE_RELEASE="$fake_release" GH_FAKE_LATEST="$fake_release" \
    require_published_release "$tmp" "$sha_b" "$sha_b" 1.2.1
  jq '.assets = .assets[:-1]' "$fake_release" > "$tmp/fake-release-missing.json"
  jq '.assets += [{"id":999,"name":"extra.bin","size":1,"digest":
      "sha256:0000000000000000000000000000000000000000000000000000000000000000"}]' \
    "$fake_release" > "$tmp/fake-release-extra.json"
  for invalid in "$tmp/fake-release-missing.json" "$tmp/fake-release-extra.json"; do
    if PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo \
        GH_FAKE_RELEASE="$invalid" GH_FAKE_LATEST="$fake_release" \
        require_published_release "$tmp" "$sha_b" "$sha_b" 1.2.1 >/dev/null 2>&1; then
      echo "missing/extra immutable Release inventory was accepted" >&2
      return 1
    fi
  done
  if PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo \
      GH_FAKE_RELEASE="$fake_release" GH_FAKE_LATEST="$fake_latest_newer" \
      require_published_release "$tmp" "$sha_b" "$sha_b" 1.2.1 >/dev/null 2>&1; then
    echo "non-latest GitHub Release was accepted" >&2
    return 1
  fi
  for invalid in "$fake_mutable" "$fake_without_immutable"; do
    if PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo \
        GH_FAKE_RELEASE="$invalid" GH_FAKE_LATEST="$fake_release" \
        require_published_release "$tmp" "$sha_b" "$sha_b" 1.2.1 >/dev/null 2>&1; then
      echo "mutable/missing-immutable tagged GitHub Release was accepted" >&2
      return 1
    fi
    if PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo \
        GH_FAKE_RELEASE="$fake_release" GH_FAKE_LATEST="$invalid" \
        require_published_release "$tmp" "$sha_b" "$sha_b" 1.2.1 >/dev/null 2>&1; then
      echo "mutable/missing-immutable latest GitHub Release was accepted" >&2
      return 1
    fi
  done
  git -C "$tmp" tag not-a-release "$sha_a"
  got="$(select_version "$tmp" "$sha_b" "$sha_b")"
  [[ "$got" == 1.2.1 ]] || { echo "non-v tag changed selection: $got" >&2; return 1; }

  # (a) Even if an old run claims its old SHA is current, its tag is no longer the newest valid
  # release tag and must never be reused to roll Pages back.
  if select_version "$tmp" "$sha_a" "$sha_a" >/dev/null 2>&1; then
    echo "old tagged source was accepted after a newer release tag" >&2
    return 1
  fi

  # (b) An untagged source from an old run must not compute a new version after main advances.
  printf 'third\n' > "$tmp/change"
  git -C "$tmp" add change
  git -C "$tmp" commit -qm third
  sha_c="$(git -C "$tmp" rev-parse HEAD)"
  printf 'fourth\n' > "$tmp/change"
  git -C "$tmp" add change
  git -C "$tmp" commit -qm fourth
  sha_d="$(git -C "$tmp" rev-parse HEAD)"
  if select_version "$tmp" "$sha_c" "$sha_d" >/dev/null 2>&1; then
    echo "old untagged source was accepted after main advanced" >&2
    return 1
  fi

  git -C "$tmp" tag v1.2.2 "$sha_b"
  if select_version "$tmp" "$sha_b" "$sha_b" >/dev/null 2>&1; then
    echo "multiple same-SHA release tags were accepted" >&2
    return 1
  fi
  if select_version "$tmp" deadbeef "$sha_d" >/dev/null 2>&1; then
    echo "short/unknown source SHA was accepted" >&2
    return 1
  fi

  # Production is a stable-only channel. A prerelease on current main contributes its core to
  # next-version, but must never be reused as a non-prerelease GitHub Release/Pages identity.
  git -C "$tmp" tag v1.3.0-rc.1 "$sha_d"
  got="$(select_version "$tmp" "$sha_d" "$sha_d")"
  [[ "$got" == 1.3.0 ]] || {
    echo "current prerelease tag was not promoted to stable core: $got" >&2; return 1;
  }
  require_release_candidate "$tmp" "$sha_d" "$sha_d" 1.3.0
  if require_release_candidate "$tmp" "$sha_d" "$sha_d" 1.3.0-rc.1 >/dev/null 2>&1; then
    echo "prerelease candidate was accepted for the production channel" >&2
    return 1
  fi
  git -C "$tmp" tag v1.3.0 "$sha_d"
  got="$(select_version "$tmp" "$sha_d" "$sha_d")"
  [[ "$got" == 1.3.0 ]] || {
    echo "stable tag was not reused alongside its prerelease alias: $got" >&2; return 1;
  }
  require_current_release "$tmp" "$sha_d" "$sha_d" 1.3.0

  # PR previews must derive their numeric base from the latest complete stable GitHub Release,
  # never the raw newest v* tag. Otherwise a newer RC core makes the subsequent stable promotion
  # numerically equal to the preview for the firmware's three-integer OTA comparator.
  git -C "$tmp" tag v1.4.0-rc.1 "$sha_a"
  fake_legacy="$tmp/fake-release-legacy.json"
  jq '.target_commitish = "main"' "$fake_release" > "$fake_legacy"
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo \
    GH_FAKE_RELEASE="$fake_release" GH_FAKE_LATEST="$fake_legacy" \
    latest_published_stable_version "$tmp")"
  [[ "$got" == 1.2.1 ]] || {
    echo "prerelease tag displaced the latest published stable preview base: $got" >&2
    return 1
  }
  for invalid in "$fake_mutable" "$fake_without_immutable"; do
    if PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo \
        GH_FAKE_RELEASE="$fake_release" GH_FAKE_LATEST="$invalid" \
        latest_published_stable_version "$tmp" >/dev/null 2>&1; then
      echo "mutable/missing-immutable latest stable preview base was accepted" >&2
      return 1
    fi
  done

  # Exercise the exact remote-main fetch used by CI, not only the pure comparison helper.
  git init --bare -q "$remote"
  git -C "$tmp" branch -M main
  git -C "$tmp" remote add origin "$remote"
  git -C "$tmp" push -q origin main --tags
  current="$(fetch_current_main "$tmp")"
  [[ "$current" == "$sha_d" ]] || { echo "remote-main fetch mismatch: $current" >&2; return 1; }
  fetch_tags "$tmp"
  git -C "$tmp" push -q origin :refs/tags/v1.2.2
  git -C "$tmp" rev-parse -q --verify refs/tags/v1.2.2 >/dev/null || {
    echo "deleted-tag fixture was not present locally before pruning" >&2; return 1;
  }
  fetch_tags "$tmp"
  if git -C "$tmp" rev-parse -q --verify refs/tags/v1.2.2 >/dev/null; then
    echo "remote-deleted release tag survived authoritative tag fetch" >&2
    return 1
  fi
  echo "release-version selector self-test: PASS"
}

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
case "${1:-}" in
  --self-test)
    [[ $# -eq 1 ]] || { echo "usage: $0 [--self-test|--latest-published-stable|--select-main SHA|--require-current-main SHA|--require-release-candidate SHA VERSION|--require-current-release SHA VERSION|--require-published-release SHA VERSION]" >&2; exit 2; }
    self_test
    ;;
  --latest-published-stable)
    [[ $# -eq 1 ]] || { echo "usage: $0 --latest-published-stable" >&2; exit 2; }
    fetch_tags "$repo_root" || exit
    latest_published_stable_version "$repo_root"
    ;;
  --select-main)
    [[ $# -eq 2 ]] || { echo "usage: $0 --select-main SOURCE_SHA" >&2; exit 2; }
    fetch_tags "$repo_root" || exit
    current_main="$(fetch_current_main "$repo_root")" || exit
    select_version "$repo_root" "$2" "$current_main"
    ;;
  --require-current-main)
    [[ $# -eq 2 ]] || { echo "usage: $0 --require-current-main SOURCE_SHA" >&2; exit 2; }
    current_main="$(fetch_current_main "$repo_root")" || exit
    validate_current_main "$repo_root" "$2" "$current_main"
    ;;
  --require-release-candidate)
    [[ $# -eq 3 ]] || { echo "usage: $0 --require-release-candidate SOURCE_SHA VERSION" >&2; exit 2; }
    fetch_tags "$repo_root" || exit
    current_main="$(fetch_current_main "$repo_root")" || exit
    require_release_candidate "$repo_root" "$2" "$current_main" "$3"
    ;;
  --require-current-release)
    [[ $# -eq 3 ]] || { echo "usage: $0 --require-current-release SOURCE_SHA VERSION" >&2; exit 2; }
    fetch_tags "$repo_root" || exit
    current_main="$(fetch_current_main "$repo_root")" || exit
    require_current_release "$repo_root" "$2" "$current_main" "$3"
    ;;
  --require-published-release)
    [[ $# -eq 3 ]] || { echo "usage: $0 --require-published-release SOURCE_SHA VERSION" >&2; exit 2; }
    fetch_tags "$repo_root" || exit
    current_main="$(fetch_current_main "$repo_root")" || exit
    require_published_release "$repo_root" "$2" "$current_main" "$3"
    ;;
  '')
    [[ $# -eq 0 ]] || exit 2
    "$repo_root/scripts/next-version.sh"
    ;;
  *) echo "usage: $0 [--self-test|--select-main SHA|--require-current-main SHA|--require-release-candidate SHA VERSION|--require-current-release SHA VERSION|--require-published-release SHA VERSION]" >&2; exit 2 ;;
esac
