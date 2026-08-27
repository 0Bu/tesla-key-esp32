#!/usr/bin/env bash
# Build one target in a genuinely fresh, isolated build directory and compare the unsigned app +
# unstripped ELF bytes. When a baseline pair is supplied, it is the first build; otherwise two
# distinct fresh directories are built. Run inside the pinned ESP-IDF container; signing is
# intentionally outside this contract.
set -euo pipefail

# Reproducibility is evidence only when it uses the same closed compiler-input boundary as the
# primary build. These variables are invisible to compile_commands.json and can inject headers,
# dependency outputs or a different compiler search path; reject presence even when empty.
for variable in \
  CPATH CPLUS_INCLUDE_PATH C_INCLUDE_PATH OBJC_INCLUDE_PATH \
  DEPENDENCIES_OUTPUT SUNPRO_DEPENDENCIES GCC_EXEC_PREFIX COMPILER_PATH LIBRARY_PATH; do
  if declare -p "$variable" &>/dev/null; then
    echo "ERROR: compiler-injection environment variable is set: $variable" >&2
    exit 2
  fi
done

# The pinned IDF image itself supplies IDF_CCACHE_ENABLE=1, but every CCACHE_* option is caller
# configuration capable of changing the hidden launcher. Reject all present caller options, then
# unconditionally disable the image-owned launcher before any self-test or build.
caller_ccache_variables=()
while IFS= read -r variable; do
  [[ -n "$variable" ]] && caller_ccache_variables+=("$variable")
done < <(compgen -A variable CCACHE_)
if (( ${#caller_ccache_variables[@]} != 0 )); then
  echo "ERROR: caller-provided ccache variables are forbidden:" >&2
  printf '  %s\n' "${caller_ccache_variables[@]}" >&2
  exit 2
fi
unset caller_ccache_variables
export IDF_CCACHE_ENABLE=0

# Overwrite rather than append caller flags so the effective diagnostic compiler additions are
# byte-for-byte identical to ci-build-all.sh.
export EXTRA_CFLAGS="-fstack-usage"
export EXTRA_CXXFLAGS="-fstack-usage"

if [[ "${1:-}" == --self-test ]]; then
  [[ $# -eq 1 ]] || { echo "usage: check-reproducible-build.sh --self-test" >&2; exit 2; }
  [[ "$IDF_CCACHE_ENABLE" == 0 && "$EXTRA_CFLAGS" == -fstack-usage \
      && "$EXTRA_CXXFLAGS" == -fstack-usage ]] || {
    echo "reproducible-build compiler boundary self-test failed" >&2
    exit 1
  }
  echo "reproducible-build compiler boundary self-test: PASS"
  exit 0
fi

target="${1:?usage: check-reproducible-build.sh <target> <display-version> [baseline-app baseline-elf]}"
case "$target" in esp32|esp32s3|esp32c3|esp32c6) ;; *) echo "unsupported target: $target" >&2; exit 2 ;; esac
version="${2:?usage: check-reproducible-build.sh <target> <display-version> [baseline-app baseline-elf]}"
VERSION_RE='^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z.-]+)?$'
[[ "$version" =~ $VERSION_RE && ${#version} -le 31 ]] || {
  echo "invalid display version: $version" >&2
  exit 2
}
baseline_app="${3:-}"
baseline_elf="${4:-}"
if [[ -n "$baseline_app" || -n "$baseline_elf" ]]; then
  [[ -n "$baseline_app" && -n "$baseline_elf" && $# -eq 4 ]] || {
    echo "baseline app and ELF must be supplied together" >&2; exit 2;
  }
  [[ -f "$baseline_app" && ! -L "$baseline_app" ]] || { echo "invalid baseline app: $baseline_app" >&2; exit 1; }
  [[ -f "$baseline_elf" && ! -L "$baseline_elf" ]] || { echo "invalid baseline ELF: $baseline_elf" >&2; exit 1; }
elif [[ $# -ne 2 ]]; then
  echo "usage: check-reproducible-build.sh <target> <display-version> [baseline-app baseline-elf]" >&2
  exit 2
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"
unset IDF_TARGET
temp_root="${RUNNER_TEMP:-${TMPDIR:-/tmp}}"
work_root="$(mktemp -d "$temp_root/tesla-key-repro.$target.XXXXXX")"

cleanup() {
  local status=$?
  trap - EXIT
  if [[ -n "${work_root:-}" && "$work_root" != / && -d "$work_root" ]]; then
    if ! rm -rf -- "$work_root"; then
      echo "reproducible-build $target: temporary workspace cleanup failed: $work_root" >&2
      exit 1
    fi
  fi
  exit "$status"
}
trap cleanup EXIT

build_once() {
  local pass="$1"
  local build_dir="$work_root/build-$pass"
  local generated_config="$work_root/sdkconfig-$pass"
  [[ ! -e "$build_dir" && ! -e "$generated_config" ]] || {
    echo "reproducible-build $target: pass $pass is not starting from a fresh path" >&2
    return 1
  }
  idf.py -B "$build_dir" -D "SDKCONFIG=$generated_config" \
    -D "PROJECT_VER=$version" set-target "$target"
  python3 scripts/check-sdkconfig-defaults.py \
    --target "$target" --generated "$generated_config"
  idf.py -B "$build_dir" -D "SDKCONFIG=$generated_config" \
    -D "PROJECT_VER=$version" build
  python3 scripts/check-build-semantics.py \
    --target "$target" --sdkconfig "$generated_config" \
    --compile-commands "$build_dir/compile_commands.json" --source-root "$repo_root"
  cp "$build_dir/tesla-key-esp32.bin" "$work_root/app-$pass.bin"
  cp "$build_dir/tesla-key-esp32.elf" "$work_root/app-$pass.elf"
}

if [[ -n "$baseline_app" ]]; then
  cp "$baseline_app" "$work_root/app-a.bin"
  cp "$baseline_elf" "$work_root/app-a.elf"
else
  build_once a
fi
build_once b
cmp "$work_root/app-a.bin" "$work_root/app-b.bin"
cmp "$work_root/app-a.elf" "$work_root/app-b.elf"
sha256sum "$work_root/app-b.bin" "$work_root/app-b.elf"
echo "reproducible-build $target: PASS (unsigned app + ELF are byte-identical)"
