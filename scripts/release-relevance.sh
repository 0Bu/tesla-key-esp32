#!/usr/bin/env bash
# Decide whether current main still contains release-relevant changes not represented by the
# authoritative root Pages snapshot, including the manifest actually served by GitHub Pages.
# Unlike github.event.before..sha, this cumulative baseline
# survives overlapping main runs: if firmware push A is made stale by later docs push B, B still
# sees A's firmware diff and completes the Release/Pages publication.
set -euo pipefail

contract_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
VERSION_RE='^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$'
SELF_TEST_PYTHON=""  # Set only by this script's isolated --self-test fixture.
RELEVANT_RE='^(main/|patches/tesla-ble/|docs/(index\.html|installer-bootstrap\.mjs|serial-port-release\.mjs|web-installer\.mjs|vendor/)|CMakeLists\.txt$|sdkconfig\.defaults(\.[a-z0-9]+)?$|dependencies\.lock\.[a-z0-9]+$|esp-idf-toolchain\.txt$|partitions\.csv$|version\.txt$|\.github/workflows/(build|signed-pr-preview|pr-preview-cleanup)\.yml$|scripts/firmware-(size|stack)-baseline\.json$|scripts/ota-signing-public-key\.sha256$|scripts/(ci-build-all|ci-build-verify|ci-sign-artifacts|next-version|select-release-version|release-relevance|test-release-contract|check-reproducible-build|idf-docker|idf-version|build-pages|publish-pages-branch|apply-tesla-ble-patches)\.sh$|scripts/(check-build-artifact-inventory|check-build-gate-contract|check-pages-manifest|check-pages-source|check-release-pages-bytes|check-release-assets|check-signed-root-inventory|prepare-reused-release|check-published-release|check-firmware-artifacts|check-partition-contract|check-dependency-contract|check-otadata-contract|check-build-semantics|check-stack-usage|check-sdkconfig-defaults|report-firmware-size)\.py$)'

validate_sha() {
  local repo_root="$1" sha="$2"
  [[ "$sha" =~ ^[0-9a-f]{40}$ ]] \
    && git -C "$repo_root" cat-file -e "${sha}^{commit}" 2>/dev/null
}

# find_published_pages_baseline <repo> <current-sha>
# Print a source SHA only when ALL authorities agree:
#   * root gh-pages manifest identity (layout/version/sourceSha),
#   * the same identity from the live URL returned by the repository Pages API,
#   * the matching immutable Git tag,
#   * the latest immutable, non-draft/non-prerelease GitHub Release with all four digest-bound
#     merged assets,
#   * and ancestry of that source SHA in the current main snapshot.
# Any missing, stale or unreadable authority is not a usable baseline.
find_published_pages_baseline() {
  local repo_root="$1" current_sha="$2" repository manifest identity source_sha version tag tag_sha
  local release_json pages_json live_base live_url live_manifest live_identity live_source live_version
  local acceptance_dir acceptance_json release_metadata_file python_cmd
  python_cmd="${SELF_TEST_PYTHON:-python3}"
  validate_sha "$repo_root" "$current_sha" || return 2
  command -v jq >/dev/null 2>&1 && command -v gh >/dev/null 2>&1 \
    && command -v curl >/dev/null 2>&1 || return 2
  repository="${GITHUB_REPOSITORY:-}"
  [[ "$repository" =~ ^[0-9A-Za-z_.-]+/[0-9A-Za-z_.-]+$ ]] || return 2

  git -C "$repo_root" fetch --quiet --tags --force --prune --prune-tags origin || return 2
  git -C "$repo_root" fetch --quiet --no-tags --force origin \
    refs/heads/gh-pages:refs/remotes/origin/gh-pages || return 2
  manifest="$(git -C "$repo_root" show refs/remotes/origin/gh-pages:manifest.json 2>/dev/null)" \
    || return 2
  identity="$(printf '%s' "$manifest" | jq -er '
    select(type == "object" and .name == "tesla-key-esp32" and .layoutVersion == 2 and
           (.builds | type == "array" and length == 4)) |
    [.sourceSha, .version] | select(all(.[]; type == "string")) | @tsv
  ' 2>/dev/null)" || return 2
  source_sha="${identity%%$'\t'*}"
  version="${identity#*$'\t'}"
  validate_sha "$repo_root" "$source_sha" || return 2
  [[ "$version" =~ $VERSION_RE ]] || return 2
  git -C "$repo_root" merge-base --is-ancestor "$source_sha" "$current_sha" || return 2

  tag="v$version"
  tag_sha="$(git -C "$repo_root" rev-parse "${tag}^{commit}" 2>/dev/null)" || return 2
  [[ "$tag_sha" == "$source_sha" ]] || return 2

  release_json="$(gh api "repos/$repository/releases/latest" 2>/dev/null)" || return 2
  printf '%s' "$release_json" | jq -e --arg tag "$tag" --arg sha "$source_sha" --arg v "$version" '
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
       (.[0].digest | type == "string" and test("^sha256:[0-9a-f]{64}$")))))
  ' >/dev/null || return 2
  release_metadata_file="$(mktemp "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/tesla-release-metadata.XXXXXX")" \
    || return 2
  if ! printf '%s' "$release_json" > "$release_metadata_file" \
      || ! "$python_cmd" "$contract_root/scripts/check-release-assets.py" \
        "$release_metadata_file" --metadata-only \
        --version "$version" --source-sha "$source_sha" \
        --expect-state published-immutable >/dev/null; then
    rm -f -- "$release_metadata_file"
    return 2
  fi
  rm -f -- "$release_metadata_file"

  # Branch+Release agreement alone does not prove that the OTA channel users actually fetch is
  # current: the configured Pages source can drift, a branch build can fail, or CDN publication can
  # lag. Read the configured Pages URL and require the live root manifest to advertise the exact
  # same stable release identity. Any API, TLS, HTTP, JSON or cache-liveness uncertainty fails
  # closed so a same-SHA rerun retries publication/deployment.
  pages_json="$(gh api "repos/$repository/pages" 2>/dev/null)" || return 2
  printf '%s' "$pages_json" \
    | "$python_cmd" "$contract_root/scripts/check-pages-source.py" - >/dev/null || return 2
  live_base="$(printf '%s' "$pages_json" | jq -er '
    .html_url | strings | select(test("^https://[^[:space:]?#]+/?$"))
  ' 2>/dev/null)" || return 2
  live_url="${live_base%/}/manifest.json?release-relevance=$current_sha"
  live_manifest="$(curl --fail --location --silent --show-error --max-time 20 \
    --retry 2 --retry-all-errors -H 'Cache-Control: no-cache' "$live_url" 2>/dev/null)" || return 2
  live_identity="$(printf '%s' "$live_manifest" | jq -er '
    select(type == "object" and .name == "tesla-key-esp32" and .layoutVersion == 2 and
           (.builds | type == "array" and length == 4)) |
    [.sourceSha, .version] | select(all(.[]; type == "string")) | @tsv
  ' 2>/dev/null)" || return 2
  live_source="${live_identity%%$'\t'*}"
  live_version="${live_identity#*$'\t'}"
  [[ "$live_source" == "$source_sha" && "$live_version" == "$version" ]] || return 2

  # A matching manifest is only an intent signal. It must never suppress reconciliation until all
  # 16 live Pages parts are byte-identical to the four immutable Release images. Reuse the same
  # bounded validator as the post-publication acceptance step so a partial CDN update (new
  # manifest, stale binary) keeps the cumulative release gate red on the next main run.
  acceptance_dir="$(mktemp -d "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/tesla-release-baseline.XXXXXX")" \
    || return 2
  acceptance_json="$acceptance_dir/release.json"
  if ! printf '%s' "$release_json" > "$acceptance_json" \
      || ! "$python_cmd" "$contract_root/scripts/check-published-release.py" \
        --pages-base-url "$live_base" \
        --release-base-url "https://github.com/$repository/releases/download/$tag" \
        --release-json "$acceptance_json" \
        --version "$version" --source-sha "$source_sha" \
        --attempts 1 --interval 0 --timeout 20 >/dev/null; then
    rm -rf -- "$acceptance_dir"
    return 2
  fi
  rm -rf -- "$acceptance_dir"

  printf '%s\n' "$source_sha"
}

changed_since_release() {
  local repo_root="$1" current_sha="$2" baseline files
  validate_sha "$repo_root" "$current_sha" || {
    echo "invalid current source SHA: $current_sha" >&2
    return 2
  }
  if ! baseline="$(find_published_pages_baseline "$repo_root" "$current_sha")"; then
    echo "no authoritative Release/Pages baseline; release relevance fails closed to yes" >&2
    printf 'yes\n'
    return 0
  fi
  # Disable rename folding so both the deletion and addition paths are considered. Otherwise a
  # `main/foo.cpp -> docs/foo.cpp` move is rendered only as docs/foo.cpp and loses the firmware
  # side of the change, suppressing a required Release.
  files="$(git -C "$repo_root" diff --name-only --no-renames "$baseline" "$current_sha")" || {
    echo "cannot diff Release/Pages baseline $baseline to $current_sha" >&2
    printf 'yes\n'
    return 0
  }
  if printf '%s\n' "$files" | grep -Eq "$RELEVANT_RE"; then
    printf 'yes\n'
  else
    printf 'no\n'
  fi
}

changed_for_pr() {
  local repo_root="$1" base_sha="$2" head_sha="$3" files
  validate_sha "$repo_root" "$base_sha" || {
    echo "invalid PR base SHA: $base_sha; failing closed to yes" >&2
    printf 'yes\n'
    return 0
  }
  validate_sha "$repo_root" "$head_sha" || {
    echo "invalid PR head SHA: $head_sha; failing closed to yes" >&2
    printf 'yes\n'
    return 0
  }
  files="$(git -C "$repo_root" diff --name-only --no-renames "$base_sha" "$head_sha")" || {
    echo "cannot diff PR base $base_sha to $head_sha; failing closed to yes" >&2
    printf 'yes\n'
    return 0
  }
  if printf '%s\n' "$files" | grep -Eq "$RELEVANT_RE"; then
    printf 'yes\n'
  else
    printf 'no\n'
  fi
}

write_fake_release() {
  local path="$1" version="$2" sha="$3"
  python3 - "$contract_root/scripts/check-release-assets.py" "$path" "$version" "$sha" <<'PY'
import importlib.util
import json
from pathlib import Path
import sys
import tempfile

spec = importlib.util.spec_from_file_location("release_assets", sys.argv[1])
module = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(module)
with tempfile.TemporaryDirectory(prefix="release-relevance-assets-") as directory:
    root = Path(directory)
    for asset in module.expected_local_assets(root, sys.argv[3]).values():
        asset.parent.mkdir(parents=True, exist_ok=True)
        asset.write_bytes(b"x")
    release = module.make_full_release(
        root, sys.argv[3], sys.argv[4], "published-immutable"
    )
    release["id"] = 7
    Path(sys.argv[2]).write_text(json.dumps(release), encoding="utf-8")
PY
}

write_pages_manifest() {
  local path="$1" version="$2" sha="$3"
  jq -n --arg version "$version" --arg sha "$sha" \
    '{name:"tesla-key-esp32",layoutVersion:2,sourceSha:$sha,version:$version,builds:[{},{},{},{}]}' \
    > "$path"
}

self_test() {
  local tmp remote fakebin fake_release fake_live sha_r sha_a sha_b sha_stack
  local sha_dependency sha_otadata sha_signed_root sha_public_key sha_reuse sha_c got real_python
  tmp="$(mktemp -d "${TMPDIR:-/tmp}/tesla-release-relevance.XXXXXX")"
  remote="$tmp.remote.git"
  trap 'rm -rf -- "$tmp" "$remote"' RETURN
  command -v jq >/dev/null 2>&1 || { echo "release relevance self-test needs jq" >&2; return 1; }
  real_python="$(command -v python3)" || return 1

  git -C "$tmp" init -q
  git -C "$tmp" config user.name test
  git -C "$tmp" config user.email test@example.invalid
  git -C "$tmp" config commit.gpgsign false
  git -C "$tmp" config tag.gpgsign false
  mkdir -p "$tmp/main" "$tmp/docs"
  printf 'released\n' > "$tmp/main/firmware.cpp"
  git -C "$tmp" add main/firmware.cpp
  git -C "$tmp" commit -qm released
  git -C "$tmp" branch -M main
  sha_r="$(git -C "$tmp" rev-parse HEAD)"
  git -C "$tmp" tag v1.0.0
  git init --bare -q "$remote"
  git -C "$tmp" remote add origin "$remote"
  git -C "$tmp" push -q origin main --tags

  git -C "$tmp" checkout -qb gh-pages
  write_pages_manifest "$tmp/manifest.json" 1.0.0 "$sha_r"
  git -C "$tmp" add manifest.json
  git -C "$tmp" commit -qm pages-r
  git -C "$tmp" push -q origin gh-pages
  git -C "$tmp" checkout -q main

  fakebin="$tmp/fakebin"
  fake_release="$tmp/fake-release.json"
  fake_live="$tmp/fake-live-manifest.json"
  mkdir -p "$fakebin"
  printf '%s\n' '#!/usr/bin/env bash' 'set -eu' \
    '[ "$1" = api ]' \
    'case "$2" in' \
    '  */releases/latest) cat "$GH_FAKE_RELEASE" ;;' \
    '  */pages) [ "${GH_FAKE_PAGES_FAIL:-0}" != 1 ] || exit 1; if [ "${GH_FAKE_PAGES_WORKFLOW:-0}" = 1 ]; then mode=workflow; else mode=legacy; fi; printf '\''{"build_type":"%s","source":{"branch":"gh-pages","path":"/"},"html_url":"https://owner.invalid/repo/"}\n'\'' "$mode" ;;' \
    '  *) exit 1 ;;' \
    'esac' > "$fakebin/gh"
  chmod +x "$fakebin/gh"
  printf '%s\n' '#!/usr/bin/env bash' 'set -eu' 'cat "$GH_FAKE_LIVE"' > "$fakebin/curl"
  chmod +x "$fakebin/curl"
  cat > "$fakebin/python3" <<'PYTHON_WRAPPER'
#!/usr/bin/env bash
set -eu
case "${1:-}" in
  */check-published-release.py)
    # The validator owns byte-level mutation tests. This wiring fixture makes a partial live
    # snapshot observable without opening a real HTTPS listener in the shell self-test.
    if [[ "${GH_FAKE_LIVE_BYTES_STALE:-0}" == 1 ]]; then
      exit 1
    fi
    exit 0
    ;;
esac
exec "$GH_FAKE_REAL_PYTHON" "$@"
PYTHON_WRAPPER
  chmod +x "$fakebin/python3"
  SELF_TEST_PYTHON="$fakebin/python3"
  export GH_FAKE_REAL_PYTHON="$real_python"
  hash -r
  write_fake_release "$fake_release" 1.0.0 "$sha_r"
  write_pages_manifest "$fake_live" 1.0.0 "$sha_r"

  # Timeline: A changes firmware, then docs-only B advances main before A can publish. The event
  # before..B diff is docs-only, but the authoritative published baseline R..B includes A.
  printf 'firmware-a\n' >> "$tmp/main/firmware.cpp"
  git -C "$tmp" add main/firmware.cpp
  git -C "$tmp" commit -qm firmware-a
  sha_a="$(git -C "$tmp" rev-parse HEAD)"
  printf 'docs-b\n' > "$tmp/docs/README.md"
  git -C "$tmp" add docs/README.md
  git -C "$tmp" commit -qm docs-b
  sha_b="$(git -C "$tmp" rev-parse HEAD)"
  git -C "$tmp" push -q origin main
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" \
    changed_since_release "$tmp" "$sha_b")"
  [[ "$got" == yes ]] || {
    echo "firmware A + docs B timeline lost cumulative release relevance: $got" >&2; return 1;
  }

  # Partial publication: Release A/tag already exist, but root Pages still advertises R. The
  # mismatch is not an authoritative baseline and must schedule reconciliation from current main.
  git -C "$tmp" tag v1.0.1 "$sha_a"
  git -C "$tmp" push -q origin v1.0.1
  write_fake_release "$fake_release" 1.0.1 "$sha_a"
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" \
    changed_since_release "$tmp" "$sha_b")"
  [[ "$got" == yes ]] || {
    echo "Release-created/Pages-old reconciliation was skipped: $got" >&2; return 1;
  }

  # A successful branch/Release publication is not enough until branch-backed Pages serves it. If
  # the branch build or CDN update lags/fails, a same-SHA rerun must remain relevant and retry rather
  # than going green while the public OTA manifest still advertises R.
  git -C "$tmp" checkout -q gh-pages
  write_pages_manifest "$tmp/manifest.json" 1.0.1 "$sha_a"
  git -C "$tmp" add manifest.json
  git -C "$tmp" commit -qm pages-a
  git -C "$tmp" push -q origin gh-pages
  git -C "$tmp" checkout -q main
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" \
    changed_since_release "$tmp" "$sha_b")"
  [[ "$got" == yes ]] || {
    echo "Release/gh-pages A with live Pages still R skipped deployment reconciliation: $got" >&2
    return 1
  }

  # Once branch, live Pages and latest Release all authoritatively represent A, docs-only B is not
  # firmware relevant and must not cut another Release.
  write_pages_manifest "$fake_live" 1.0.1 "$sha_a"
  got="$(
    export PATH="$fakebin:$PATH"
    export GITHUB_REPOSITORY=owner/repo
    export GH_FAKE_RELEASE="$fake_release"
    export GH_FAKE_LIVE="$fake_live"
    export GH_FAKE_LIVE_BYTES_STALE=1
    changed_since_release "$tmp" "$sha_b"
  )"
  [[ "$got" == yes ]] || {
    echo "new live manifest with a stale firmware part suppressed reconciliation: $got" >&2
    return 1
  }
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" \
    changed_since_release "$tmp" "$sha_b")"
  [[ "$got" == no ]] || {
    echo "fully published A incorrectly made docs-only B release-relevant: $got" >&2; return 1;
  }

  jq '.assets = .assets[:-1]' "$fake_release" > "$fake_release.tmp"
  mv "$fake_release.tmp" "$fake_release"
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" changed_since_release "$tmp" "$sha_b")"
  [[ "$got" == yes ]] || {
    echo "incomplete immutable Release became a publication baseline: $got" >&2; return 1;
  }
  write_fake_release "$fake_release" 1.0.1 "$sha_a"
  jq '.assets += [{"id":999,"name":"extra.bin","size":1,"digest":
      "sha256:0000000000000000000000000000000000000000000000000000000000000000"}]' \
    "$fake_release" > "$fake_release.tmp"
  mv "$fake_release.tmp" "$fake_release"
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" changed_since_release "$tmp" "$sha_b")"
  [[ "$got" == yes ]] || {
    echo "extra immutable Release asset became a publication baseline: $got" >&2; return 1;
  }
  write_fake_release "$fake_release" 1.0.1 "$sha_a"

  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" GH_FAKE_PAGES_WORKFLOW=1 \
    changed_since_release "$tmp" "$sha_b")"
  [[ "$got" == yes ]] || {
    echo "Actions Pages source mode failed open to a branch-backed baseline: $got" >&2; return 1;
  }

  # A published/stable Release is not an authority until the API explicitly reports immutable
  # true. Both false and an absent field must fail closed so older API fixtures or a repository
  # without immutable releases cannot suppress required publication reconciliation.
  jq '.immutable = false' "$fake_release" > "$fake_release.tmp"
  mv "$fake_release.tmp" "$fake_release"
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" changed_since_release "$tmp" "$sha_b")"
  [[ "$got" == yes ]] || {
    echo "mutable Release authority failed open to no: $got" >&2; return 1;
  }
  write_fake_release "$fake_release" 1.0.1 "$sha_a"
  jq 'del(.immutable)' "$fake_release" > "$fake_release.tmp"
  mv "$fake_release.tmp" "$fake_release"
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" changed_since_release "$tmp" "$sha_b")"
  [[ "$got" == yes ]] || {
    echo "Release authority without immutable field failed open to no: $got" >&2; return 1;
  }
  write_fake_release "$fake_release" 1.0.1 "$sha_a"

  # Reviewed build-resource baselines affect what bytes are allowed to ship. Pin the real stack
  # filename: a name drift here would let its change bypass the cumulative release decision.
  mkdir -p "$tmp/scripts"
  printf '{"schemaVersion":1}\n' > "$tmp/scripts/firmware-stack-baseline.json"
  git -C "$tmp" add scripts/firmware-stack-baseline.json
  git -C "$tmp" commit -qm firmware-stack-baseline
  sha_stack="$(git -C "$tmp" rev-parse HEAD)"
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" changed_since_release "$tmp" "$sha_stack")"
  [[ "$got" == yes ]] || {
    echo "firmware-stack baseline change was not release-relevant: $got" >&2; return 1;
  }

  # The trusted signer consumes both validators. A hardening or accidental weakening of either
  # contract must reconcile the published channel even when no firmware source changed.
  printf '# dependency contract\n' > "$tmp/scripts/check-dependency-contract.py"
  git -C "$tmp" add scripts/check-dependency-contract.py
  git -C "$tmp" commit -qm dependency-contract
  sha_dependency="$(git -C "$tmp" rev-parse HEAD)"
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" changed_since_release "$tmp" "$sha_dependency")"
  [[ "$got" == yes ]] || {
    echo "dependency contract change was not release-relevant: $got" >&2; return 1;
  }

  printf '# otadata contract\n' > "$tmp/scripts/check-otadata-contract.py"
  git -C "$tmp" add scripts/check-otadata-contract.py
  git -C "$tmp" commit -qm otadata-contract
  sha_otadata="$(git -C "$tmp" rev-parse HEAD)"
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" changed_since_release "$tmp" "$sha_otadata")"
  [[ "$got" == yes ]] || {
    echo "otadata contract change was not release-relevant: $got" >&2; return 1;
  }

  printf '# signed root inventory contract\n' > "$tmp/scripts/check-signed-root-inventory.py"
  git -C "$tmp" add scripts/check-signed-root-inventory.py
  git -C "$tmp" commit -qm signed-root-inventory-contract
  sha_signed_root="$(git -C "$tmp" rev-parse HEAD)"
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" changed_since_release "$tmp" "$sha_signed_root")"
  [[ "$got" == yes ]] || {
    echo "signed root inventory contract change was not release-relevant: $got" >&2; return 1;
  }

  # The reviewed digest is the production OTA authority. A pin change is firmware/recovery
  # relevant even if no source or workflow changed; otherwise the public channel could continue
  # advertising bytes under an authority the repository no longer accepts.
  printf '%064d\n' 0 > "$tmp/scripts/ota-signing-public-key.sha256"
  git -C "$tmp" add scripts/ota-signing-public-key.sha256
  git -C "$tmp" commit -qm ota-signing-public-key-pin
  sha_public_key="$(git -C "$tmp" rev-parse HEAD)"
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" changed_since_release "$tmp" "$sha_public_key")"
  [[ "$got" == yes ]] || {
    echo "OTA signing public-key pin change was not release-relevant: $got" >&2; return 1;
  }

  printf '# immutable release reuse contract\n' > "$tmp/scripts/prepare-reused-release.py"
  git -C "$tmp" add scripts/prepare-reused-release.py
  git -C "$tmp" commit -qm reused-release-contract
  sha_reuse="$(git -C "$tmp" rev-parse HEAD)"
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" changed_since_release "$tmp" "$sha_reuse")"
  [[ "$got" == yes ]] || {
    echo "immutable Release reuse contract change was not release-relevant: $got" >&2; return 1;
  }

  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" GH_FAKE_PAGES_FAIL=1 changed_since_release "$tmp" "$sha_b")"
  [[ "$got" == yes ]] || {
    echo "unreadable Pages configuration failed open to no: $got" >&2; return 1;
  }

  # Rename detection must not hide the deletion side of a firmware->docs move. Git's default
  # --name-only rename rendering reports only the destination and would otherwise suppress the
  # release even though main/ changed materially.
  git -C "$tmp" mv main/firmware.cpp docs/firmware.cpp
  git -C "$tmp" commit -qm move-firmware-out-of-main
  sha_c="$(git -C "$tmp" rev-parse HEAD)"
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" changed_since_release "$tmp" "$sha_c")"
  [[ "$got" == yes ]] || {
    echo "firmware-to-docs rename lost the relevant source deletion: $got" >&2; return 1;
  }

  # Unreadable/mismatched authority never suppresses a needed publication.
  if PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$tmp/missing.json" \
      GH_FAKE_LIVE="$fake_live" \
      changed_since_release "$tmp" "$sha_b" | grep -Fxq no; then
    echo "unreadable Release authority failed open to no" >&2
    return 1
  fi

  # PR relevance test: docs-only diff produces no, firmware diff produces yes, bad SHA fails closed
  git -C "$tmp" checkout -q -b pr-branch
  echo "test doc edit" >> "$tmp/docs/README.md"
  git -C "$tmp" commit -qam "edit docs"
  sha_pr_docs="$(git -C "$tmp" rev-parse HEAD)"
  got="$(changed_for_pr "$tmp" "$sha_c" "$sha_pr_docs")"
  [[ "$got" == no ]] || { echo "docs-only PR expected no, got: $got" >&2; return 1; }

  echo "void foo() {}" >> "$tmp/main/firmware.cpp"
  git -C "$tmp" add main/firmware.cpp
  git -C "$tmp" commit -qm "edit firmware"
  sha_pr_fw="$(git -C "$tmp" rev-parse HEAD)"
  got="$(changed_for_pr "$tmp" "$sha_c" "$sha_pr_fw")"
  [[ "$got" == yes ]] || { echo "firmware PR expected yes, got: $got" >&2; return 1; }

  got="$(changed_for_pr "$tmp" "bad-sha" "$sha_pr_fw" 2>/dev/null)"
  [[ "$got" == yes ]] || { echo "invalid PR SHA expected yes, got: $got" >&2; return 1; }

  echo "release relevance self-test: PASS"
}

repo_root="$contract_root"
case "${1:-}" in
  --changed)
    [[ $# -eq 2 ]] || { echo "usage: $0 --changed CURRENT_SHA | --changed-pr BASE_SHA HEAD_SHA | --self-test" >&2; exit 2; }
    changed_since_release "$repo_root" "$2"
    ;;
  --changed-pr)
    [[ $# -eq 3 ]] || { echo "usage: $0 --changed CURRENT_SHA | --changed-pr BASE_SHA HEAD_SHA | --self-test" >&2; exit 2; }
    changed_for_pr "$repo_root" "$2" "$3"
    ;;
  --self-test)
    [[ $# -eq 1 ]] || { echo "usage: $0 --changed CURRENT_SHA | --changed-pr BASE_SHA HEAD_SHA | --self-test" >&2; exit 2; }
    self_test
    ;;
  *) echo "usage: $0 --changed CURRENT_SHA | --changed-pr BASE_SHA HEAD_SHA | --self-test" >&2; exit 2 ;;
esac
