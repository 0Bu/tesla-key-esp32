#!/usr/bin/env bash
# Fast, INCREMENTAL local pre-check of the deterministic post-build baseline gates for all four
# targets: the firmware size budget and the reviewed stack-frame baseline.
#
# Why this exists: in CI those two gates only run after a full per-target compile (~7 min), so a
# baseline violation costs a whole push -> wait -> fix -> push cycle. Measured over 24 days,
# 24 of the 25 late build failures were exactly these gates, and 18 of those 24 failed ONLY on
# esp32/esp32s3/esp32c3 -- a pre-push check that built just esp32c6 was blind to them. The numbers
# are a pure function of the source tree and the pinned toolchain image, and a local rebuild in that
# image reproduces CI's figures byte for byte, so a local failure here IS the CI failure.
#
# What it deliberately is NOT: a replacement for CI. It runs neither the effective-compiler
# semantics gate nor the reproducibility rebuild nor any release/signing contract; those stay
# authoritative in CI (scripts/ci-build-all.sh, scripts/ci-build-verify.sh). Use
# scripts/check-firmware-size.sh for the slow, authoritative single-target equivalent.
#
# It is fast because every target keeps a persistent build directory in idf-docker.sh's Fast-Build
# volume (/build_cache/precheck/<target>), so a typical push only recompiles the objects it touched.
# The volume is outside the checkout: it keeps the repo clean and avoids macOS VirtioFS latency. A
# change to any configuration input (SDK defaults, dependency lock, partition table, CMake, patches,
# toolchain pin) invalidates that target's cache and re-runs `set-target` from scratch, exactly like
# a fresh CI build.
#
# ccache is switched OFF here, as it is in CI, and NOT merely for speed parity: with a ccache base
# directory it rewrites source paths in the preprocessed input to relative ones, GCC records those in
# the .su stack sidecars, and the reviewed stack baseline (keyed on /COMPONENT_MAIN_DIR/... names)
# then reports every frame as drifted.
#
# Usage:
#   scripts/precheck-firmware.sh                 # all four targets
#   scripts/precheck-firmware.sh --target esp32s3 [--target esp32c3 ...]
#   scripts/precheck-firmware.sh --clean         # drop the incremental caches first
#   scripts/precheck-firmware.sh --self-test
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$repo_root"

ALL_TARGETS=(esp32 esp32s3 esp32c3 esp32c6)
# These three constants must stay identical to scripts/ci-build-all.sh; --self-test asserts it.
APP_POLICY_LIMIT=$((0x1e8000))
SIGNATURE_ALIGNMENT=$((0x10000))
SIGNATURE_SECTOR=$((0x1000))
# Mounted by scripts/idf-docker.sh when IDF_FAST_BUILD=1; a per-checkout Docker volume, not the repo.
WORK_ROOT="/build_cache/precheck"

usage() {
  awk '/^# Usage:/{p=1} /^set -euo/{p=0} p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

valid_target() {
  local candidate="$1" known
  for known in "${ALL_TARGETS[@]}"; do
    [[ "$candidate" == "$known" ]] && return 0
  done
  return 1
}

self_test() {
  local ci="$repo_root/scripts/ci-build-all.sh"
  grep -Fq 'APP_POLICY_LIMIT=$((0x1e8000))' "$ci" \
    || { echo "precheck-firmware self-test: APP_POLICY_LIMIT drifted from ci-build-all.sh" >&2; exit 1; }
  grep -Fq 'SIGNATURE_ALIGNMENT=$((0x10000))' "$ci" \
    || { echo "precheck-firmware self-test: SIGNATURE_ALIGNMENT drifted from ci-build-all.sh" >&2; exit 1; }
  grep -Fq 'SIGNATURE_SECTOR=$((0x1000))' "$ci" \
    || { echo "precheck-firmware self-test: SIGNATURE_SECTOR drifted from ci-build-all.sh" >&2; exit 1; }
  grep -Fq 'TARGETS="esp32 esp32s3 esp32c3 esp32c6"' "$ci" \
    || { echo "precheck-firmware self-test: target list drifted from ci-build-all.sh" >&2; exit 1; }
  local target
  for target in esp32 esp32s3 esp32c3 esp32c6; do
    valid_target "$target" || { echo "precheck-firmware self-test: $target rejected" >&2; exit 1; }
  done
  if valid_target esp32c2 || valid_target ""; then
    echo "precheck-firmware self-test: unsupported target accepted" >&2
    exit 1
  fi
  echo "precheck-firmware self-test: PASS"
}

# ---- inner mode: runs INSIDE the pinned ESP-IDF container ---------------------------------------

# Everything that decides the generated sdkconfig / dependency set / partition layout. If any of it
# changes the cached configuration is stale and must be regenerated from scratch.
config_stamp() {
  local target="$1" path
  {
    for path in sdkconfig.defaults "sdkconfig.defaults.$target" "dependencies.lock.$target" \
        partitions.csv CMakeLists.txt main/CMakeLists.txt main/idf_component.yml \
        esp-idf-toolchain.txt scripts/apply-tesla-ble-patches.sh; do
      if [[ -f "$path" ]]; then
        sha256sum "$path"
      fi
    done
    if [[ -d patches ]]; then
      find patches -type f -print0 | sort -z | xargs -0 sha256sum
    fi
    printf 'idf=%s\n' "${IDF_PATH:-unset}"
  } | sha256sum | cut -d' ' -f1
}

precheck_target() {
  local target="$1"
  local work="$WORK_ROOT/$target"
  local build="$work/build"
  local generated_config="$work/sdkconfig"
  local lock="dependencies.lock.$target"
  local started=$SECONDS

  [[ -f "$lock" ]] || { echo "ERROR: $lock is missing" >&2; return 1; }
  local lock_before
  lock_before="$(sha256sum "$lock" | cut -d' ' -f1)"

  local stamp mode
  stamp="$(config_stamp "$target")"
  if [[ "$(cat "$work/stamp" 2>/dev/null || true)" == "$stamp" && -f "$generated_config" ]]; then
    mode="incremental"
  else
    mode="fresh configure"
    echo "precheck $target: no warm cache for this checkout -> full compile (about 3 min at the default" \
      "1.5-CPU cap); later runs only rebuild what changed and take seconds"
    rm -rf "$work"
    mkdir -p "$work"
    idf.py -B "$build" -D "SDKCONFIG=$generated_config" -D "PROJECT_VER=local" set-target "$target"
    python3 scripts/check-sdkconfig-defaults.py --target "$target" --generated "$generated_config"
    printf '%s\n' "$stamp" > "$work/stamp"
  fi
  idf.py -B "$build" -D "SDKCONFIG=$generated_config" -D "PROJECT_VER=local" build

  local lock_after
  lock_after="$(sha256sum "$lock" | cut -d' ' -f1)"
  if [[ "$lock_before" != "$lock_after" ]]; then
    echo "ERROR: $lock changed during a normal build; run idf.py update-dependencies for $target" >&2
    echo "deliberately and commit the resulting lockfile." >&2
    return 1
  fi

  local unsigned_bin="$build/tesla-key-esp32.bin"
  local unsigned_size projected
  unsigned_size="$(wc -c < "$unsigned_bin" | tr -d ' ')"
  projected=$(( (unsigned_size + SIGNATURE_ALIGNMENT - 1) / SIGNATURE_ALIGNMENT * SIGNATURE_ALIGNMENT + SIGNATURE_SECTOR ))
  if (( projected > APP_POLICY_LIMIT )); then
    echo "ERROR: $target projected signed app is $projected B, over policy limit $APP_POLICY_LIMIT B" >&2
    return 1
  fi

  python3 scripts/check-stack-usage.py \
    --target "$target" --stack-root "$build/esp-idf/main" \
    --write-observed "$work/stack-usage.json"
  python -m esp_idf_size --format json2 "$build/tesla-key-esp32.map" > "$work/size.json"
  python3 scripts/report-firmware-size.py \
    --idf-size "$work/size.json" --unsigned-app "$unsigned_bin" \
    --projected-signed-size "$projected" --policy-limit "$APP_POLICY_LIMIT" \
    --target "$target" \
    --budget-baseline scripts/firmware-size-baseline.json --enforce-budget > "$work/size.md"
  python3 scripts/check-stack-usage.py \
    --target "$target" --observed-json "$work/stack-usage.json" \
    --baseline scripts/firmware-stack-baseline.json

  echo "precheck $target: OK ($mode, $((SECONDS - started))s) unsigned=$unsigned_size B projected-signed=$projected B"
}

run_inner() {
  local targets=("$@") target failed=()
  [[ -d /build_cache && -w /build_cache ]] || {
    echo "ERROR: /build_cache is not mounted; run this through scripts/precheck-firmware.sh" >&2
    return 1
  }
  if [[ "$clean" -eq 1 ]]; then
    rm -rf "${WORK_ROOT:?}"
  fi
  mkdir -p "$WORK_ROOT"
  # Same diagnostic-only compile additions as ci-build-all.sh, so the stack sidecars exist and the
  # generated objects are byte-identical to the CI build of the same tree.
  export EXTRA_CFLAGS="-fstack-usage"
  export EXTRA_CXXFLAGS="-fstack-usage"
  # idf-docker.sh enables ccache for non-authoritative commands; undo it (see the header comment).
  export IDF_CCACHE_ENABLE=0
  local ccache_variable
  while IFS= read -r ccache_variable; do
    [[ -n "$ccache_variable" ]] && unset "$ccache_variable"
  done < <(compgen -A variable CCACHE_)
  unset IDF_TARGET
  local rc
  for target in "${targets[@]}"; do
    echo "::group::precheck $target"
    # Bash ignores `set -e` for everything inside a function that runs as an `if`/`||` condition, so
    # a failed build would fall through to stale artifacts. Run each target in its own errexit
    # subshell instead and read its status explicitly.
    set +e
    ( set -e; precheck_target "$target" )
    rc=$?
    set -e
    if (( rc != 0 )); then
      failed+=("$target")
    fi
    echo "::endgroup::"
  done
  if (( ${#failed[@]} != 0 )); then
    echo "" >&2
    echo "precheck-firmware: FAILED for: ${failed[*]}" >&2
    echo "These are the same gates CI enforces after its full build. If the growth is intentional and" >&2
    echo "reviewed, regenerate the baselines from an authoritative build and commit them:" >&2
    echo "  size:  scripts/check-firmware-size.sh --all --update-baseline" >&2
    echo "  stack: python3 scripts/check-stack-usage.py --observed-dir dist \\" >&2
    echo "           --write-baseline scripts/firmware-stack-baseline.json" >&2
    return 1
  fi
  echo "precheck-firmware: all targets within the reviewed size and stack baselines (${targets[*]})"
}

# ---- host mode ------------------------------------------------------------------------------------

targets=()
inner=0
clean=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --inner) inner=1; shift ;;
    --clean) clean=1; shift ;;
    --all) targets=("${ALL_TARGETS[@]}"); shift ;;
    --target)
      shift
      [[ $# -gt 0 ]] || { echo "ERROR: --target requires an argument" >&2; exit 2; }
      valid_target "$1" || { echo "ERROR: unsupported target '$1'" >&2; exit 2; }
      targets+=("$1")
      shift
      ;;
    --self-test) self_test; exit 0 ;;
    -h|--help) usage; exit 0 ;;
    *)
      if [[ "$inner" -eq 1 ]] && valid_target "$1"; then
        targets+=("$1")
        shift
      else
        echo "ERROR: unknown argument: $1" >&2
        exit 2
      fi
      ;;
  esac
done
if (( ${#targets[@]} == 0 )); then
  targets=("${ALL_TARGETS[@]}")
fi

if [[ "$inner" -eq 1 ]]; then
  run_inner "${targets[@]}"
  exit $?
fi

if ! command -v docker >/dev/null 2>&1 || ! docker info >/dev/null 2>&1; then
  echo "ERROR: Docker is not running or not accessible. The pinned ESP-IDF image is required." >&2
  exit 1
fi
echo "==> Incremental size/stack baseline pre-check: ${targets[*]}"
inner_args=(--inner)
if [[ "$clean" -eq 1 ]]; then
  inner_args+=(--clean)
fi
IDF_FAST_BUILD=1 exec "$repo_root/scripts/idf-docker.sh" ./scripts/precheck-firmware.sh \
  "${inner_args[@]}" "${targets[@]}"
