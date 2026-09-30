#!/usr/bin/env bash
# Run any command in the SAME ESP-IDF Docker image the CI uses — so local
# build/debug never drifts from CI. The tag and immutable manifest-list digest are read at runtime
# from esp-idf-toolchain.txt. When Renovate updates that contract, both paths move together.
#
# There is no local ESP-IDF install on this machine (removed on purpose) — this
# wrapper is the only build path. Flashing still happens on the HOST with
# `esptool` (Docker Desktop on macOS has no USB passthrough).
#
# Usage:
#   scripts/idf-docker.sh idf.py set-target esp32s3 build
#   scripts/idf-docker.sh idf.py menuconfig            # interactive (-it auto)
#   scripts/idf-docker.sh sh -c 'if [ -f sdkconfig ]; then idf.py build; \
#                                else idf.py set-target esp32s3 build; fi'
#
# Acceleration options:
#   IDF_FAST_BUILD=1 scripts/idf-docker.sh idf.py build
#   scripts/idf-docker.sh daemon start
#   scripts/idf-docker.sh daemon stop
#   scripts/idf-docker.sh daemon status
#   scripts/idf-docker.sh --self-test        # offline check of the daemon-reuse contract
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
image="$("$repo_root/scripts/idf-version.sh" --image)"
echo "idf-docker: using ${image} (from esp-idf-toolchain.txt)" >&2

# Interactive TTY only when actually attached, so `menuconfig` works from a
# terminal but piped/automated runs (e.g. `... | tail`) don't break.
tty_flags=()
if [ -t 0 ] && [ -t 1 ]; then tty_flags=(-it); fi

mount_flags=(-v "$repo_root":/project)
git_common="$(git -C "$repo_root" rev-parse --git-common-dir 2>/dev/null || true)"
if [ -n "$git_common" ]; then
  git_common_abs="$(cd "$repo_root" && cd "$git_common" && pwd -P 2>/dev/null || true)"
  if [ -n "$git_common_abs" ]; then
    case "$git_common_abs" in
      "$repo_root"/*) ;;
      *)
        git_parent="$(dirname "$git_common_abs")"
        mount_flags+=(-v "$git_parent":"$git_parent")
        ;;
    esac
  fi
fi

repo_hash="$(printf '%s' "$repo_root" | shasum 2>/dev/null | cut -c1-8 || md5 -qs "$repo_root" 2>/dev/null | cut -c1-8 || echo "default")"
daemon_name="idf-daemon-${repo_hash}"
build_vol_name="idf-build-${repo_hash}"

# The daemon name depends only on the checkout path, so a daemon created before esp-idf-toolchain.txt
# moved would keep serving builds from the PREVIOUS toolchain while this wrapper prints the new pin.
# A running daemon is therefore reusable only if it was created from exactly the current image
# reference (tag AND immutable digest) with the reviewed resource ceiling; anything else is recreated.
IDF_DAEMON_NANO_CPUS=1500000000       # --cpus 1.5
IDF_DAEMON_MEMORY_BYTES=1887436800    # --memory 1800m

daemon_matches_contract() {
  local daemon_image nano_cpus memory
  daemon_image="$(docker inspect -f '{{.Config.Image}}' "$daemon_name" 2>/dev/null)" || return 1
  nano_cpus="$(docker inspect -f '{{.HostConfig.NanoCpus}}' "$daemon_name" 2>/dev/null)" || return 1
  memory="$(docker inspect -f '{{.HostConfig.Memory}}' "$daemon_name" 2>/dev/null)" || return 1
  [ "$daemon_image" = "$image" ] \
    && [ "$nano_cpus" = "$IDF_DAEMON_NANO_CPUS" ] \
    && [ "$memory" = "$IDF_DAEMON_MEMORY_BYTES" ]
}

idf_docker_self_test() {
  local fake_image="espressif/idf:v0.0@sha256:0000000000000000000000000000000000000000000000000000000000000000"
  local stale_image="espressif/idf:v0.0@sha256:1111111111111111111111111111111111111111111111111111111111111111"
  local fake_state
  image="$fake_image"
  daemon_name="idf-daemon-selftest"
  # Shadow the docker CLI with a deterministic inspect; nothing here can reach a real daemon.
  docker() {
    [ "$1" = inspect ] && [ "$2" = -f ] && [ "$4" = "$daemon_name" ] || return 1
    case "$fake_state:$3" in
      absent:*) return 1 ;;
      *:'{{.Config.Image}}') printf '%s\n' "${fake_image_value}" ;;
      *:'{{.HostConfig.NanoCpus}}') printf '%s\n' "${fake_cpus_value}" ;;
      *:'{{.HostConfig.Memory}}') printf '%s\n' "${fake_memory_value}" ;;
      *) return 1 ;;
    esac
  }
  local fake_image_value fake_cpus_value fake_memory_value
  expect() {
    local want="$1" label="$2" rc=0
    daemon_matches_contract || rc=$?
    if { [ "$want" = match ] && [ "$rc" -ne 0 ]; } || { [ "$want" = reject ] && [ "$rc" -eq 0 ]; }; then
      echo "idf-docker self-test FAIL: $label" >&2
      exit 1
    fi
  }
  fake_state=present
  fake_image_value="$fake_image"; fake_cpus_value="$IDF_DAEMON_NANO_CPUS"; fake_memory_value="$IDF_DAEMON_MEMORY_BYTES"
  expect match "a daemon created from the current pin with the reviewed limits must be reused"
  fake_image_value="$stale_image"
  expect reject "a daemon created from the previous toolchain digest must not be reused"
  fake_image_value="${fake_image%%@*}"
  expect reject "a tag-only daemon without the immutable digest must not be reused"
  fake_image_value="$fake_image"; fake_cpus_value=4000000000
  expect reject "a daemon with a raised CPU ceiling must not be reused"
  fake_cpus_value="$IDF_DAEMON_NANO_CPUS"; fake_memory_value=0
  expect reject "a daemon without the memory ceiling must not be reused"
  fake_memory_value="$IDF_DAEMON_MEMORY_BYTES"; fake_state=absent
  expect reject "a missing daemon must not count as matching"
  echo "idf-docker self-test: PASS"
}

if [ "${1:-}" = "--self-test" ]; then
  idf_docker_self_test
  exit 0
fi

mkdir -p "$repo_root/.ccache"
mount_flags+=(-v "$repo_root/.ccache":/project/.ccache)

extra_env=()
if [ -n "${IDF_PY_BUILD_JOBS:-}" ]; then
  extra_env+=(-e "IDF_PY_BUILD_JOBS=$IDF_PY_BUILD_JOBS")
fi
if [ -n "${CCACHE_DISABLE:-}" ]; then
  extra_env+=(-e "CCACHE_DISABLE=$CCACHE_DISABLE")
fi

# Authoritative gate builds (CI, reproducibility, firmware size) forbid caller-provided CCACHE_* variables
is_authoritative_gate=0
for arg in "$@"; do
  if [[ "$arg" == *"ci-build"* ]] || [[ "$arg" == *"check-reproducible"* ]] || [[ "$arg" == *"check-firmware-size"* ]]; then
    is_authoritative_gate=1
    break
  fi
done

ccache_env=()
if [ "$is_authoritative_gate" -eq 0 ] && [ "${CCACHE_DISABLE:-0}" != "1" ]; then
  ccache_env=(
    -e IDF_CCACHE_ENABLE=1
    -e CCACHE_DIR=/project/.ccache
    -e CCACHE_BASEDIR=/project
    -e CCACHE_SLOPPINESS=time_macros
    -e CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-2G}"
  )
fi

# Fast-Build: mount a dedicated Docker volume at /build_cache to avoid macOS VirtioFS I/O latency.
vol_flags=()
if [ "${IDF_FAST_BUILD:-0}" = "1" ] || [ "${1:-}" = "daemon" ]; then
  if ! docker volume inspect "$build_vol_name" >/dev/null 2>&1; then
    docker volume create "$build_vol_name" >/dev/null
    docker run --rm -v "$build_vol_name":/b alpine chmod 777 /b >/dev/null 2>&1 || true
  fi
  vol_flags=(-v "$build_vol_name":/build_cache)
fi

# Daemon management commands:
#   scripts/idf-docker.sh daemon start
#   scripts/idf-docker.sh daemon stop
#   scripts/idf-docker.sh daemon status
if [ "${1:-}" = "daemon" ]; then
  subcmd="${2:-status}"
  case "$subcmd" in
    start)
      if docker inspect -f '{{.State.Running}}' "$daemon_name" 2>/dev/null | grep -q true; then
        if daemon_matches_contract; then
          echo "idf-docker: daemon is already running ($daemon_name)"
          exit 0
        fi
        echo "idf-docker: running daemon ($daemon_name) does not match the current toolchain pin or resource limits — recreating it" >&2
      fi
      docker rm -f "$daemon_name" >/dev/null 2>&1 || true
      echo "idf-docker: starting background daemon ($daemon_name)..."
      docker run -d --name "$daemon_name" \
        --cpus 1.5 --memory 1800m \
        ${vol_flags[@]+"${vol_flags[@]}"} \
        "${mount_flags[@]}" -w /project \
        -u "$(id -u):$(id -g)" -e HOME=/tmp \
        -e GIT_CONFIG_COUNT=1 -e GIT_CONFIG_KEY_0=safe.directory -e GIT_CONFIG_VALUE_0='*' \
        -e IDF_CCACHE_ENABLE=1 \
        -e CCACHE_DIR=/project/.ccache \
        -e CCACHE_BASEDIR=/project \
        -e CCACHE_SLOPPINESS=time_macros \
        -e CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-2G}" \
        -e NINJA_STATUS="${NINJA_STATUS:-[%f/%t (%r running)] }" \
        ${extra_env[@]+"${extra_env[@]}"} \
        "$image" bash -c '. /opt/esp/idf/export.sh >/dev/null 2>&1 && export -p > /tmp/esp_env.sh && sleep infinity' >/dev/null
      echo "idf-docker: daemon started. Future commands will use 'docker exec' with zero startup latency."
      exit 0
      ;;
    stop)
      if docker rm -f "$daemon_name" >/dev/null 2>&1; then
        echo "idf-docker: daemon stopped ($daemon_name)."
      else
        echo "idf-docker: daemon was not running."
      fi
      exit 0
      ;;
    status)
      st="$(docker inspect -f '{{.State.Status}}' "$daemon_name" 2>/dev/null || echo 'not running')"
      if [ "$st" = running ] && ! daemon_matches_contract; then
        st="running, STALE (differs from the current toolchain pin or resource limits; the next command recreates it)"
      fi
      echo "idf-docker: daemon ($daemon_name) status: $st"
      exit 0
      ;;
    *)
      echo "Usage: $0 daemon {start|stop|status}" >&2
      exit 1
      ;;
  esac
fi

# Determine if we should wrap with Fast-Build artifact sync
run_cmd=("$@")
if [ "${IDF_FAST_BUILD:-0}" = "1" ] && [ "${1:-}" = "idf.py" ]; then
  # Inject -B /build_cache if not explicitly provided
  has_b=0
  for arg in "$@"; do
    if [ "$arg" = "-B" ] || [[ "$arg" == --build-dir* ]]; then has_b=1; break; fi
  done
  if [ "$has_b" -eq 0 ]; then
    shift
    sync_script='mkdir -p /project/build/bootloader /project/build/partition_table && cp -f /build_cache/*.bin /build_cache/*flash* /build_cache/*.elf /build_cache/*.map /build_cache/project_description.json /project/build/ 2>/dev/null || true; cp -f /build_cache/bootloader/*.bin /project/build/bootloader/ 2>/dev/null || true; cp -f /build_cache/partition_table/*.bin /project/build/partition_table/ 2>/dev/null || true'
    run_cmd=(bash -c "idf.py -B /build_cache \"\$@\" && ($sync_script)" -- "$@")
  fi
fi

# Check if daemon is active or requested (authoritative gate builds always use an ephemeral container)
is_daemon_running="$(docker inspect -f '{{.State.Running}}' "$daemon_name" 2>/dev/null || echo false)"
if [ "$is_authoritative_gate" -eq 0 ] && [ "$is_daemon_running" = "true" ] && ! daemon_matches_contract; then
  # Never run a build on a daemon from another toolchain pin. Drop it; with IDF_DOCKER_DAEMON=1 the
  # block below starts a fresh one, otherwise this command runs in an ordinary ephemeral container.
  echo "idf-docker: daemon ($daemon_name) does not match the current toolchain pin or resource limits — recreating it" >&2
  docker rm -f "$daemon_name" >/dev/null 2>&1 || true
  is_daemon_running=false
fi
if [ "$is_authoritative_gate" -eq 0 ] && ([ "${IDF_DOCKER_DAEMON:-0}" = "1" ] || [ "$is_daemon_running" = "true" ]); then
  if [ "$is_daemon_running" != "true" ]; then
    echo "idf-docker: auto-starting daemon ($daemon_name)..." >&2
    "$0" daemon start >&2
  fi
  exec docker exec ${tty_flags[@]+"${tty_flags[@]}"} \
    ${extra_env[@]+"${extra_env[@]}"} \
    "$daemon_name" \
    bash -c 'if [ -f /tmp/esp_env.sh ]; then . /tmp/esp_env.sh; else . /opt/esp/entrypoint.sh true; fi; exec "$@"' -- "${run_cmd[@]}"
fi

# Hard limits keep a local ESP-IDF build from starving co-resident services. They are explicit
# here rather than inherited from a Docker daemon/shim default, so standard Docker and the k3s
# shim enforce the same ceiling. The complete S3 build is proven below both limits.
#
# -u maps to the host user so build/ artifacts aren't root-owned; HOME=/tmp gives that non-root
# user a writable home; GIT_CONFIG safe.directory='*' avoids git "dubious ownership" on the
# mounted repo and on /opt/esp/idf.
exec docker run --rm --cpus 1.5 --memory 1800m ${tty_flags[@]+"${tty_flags[@]}"} \
  ${vol_flags[@]+"${vol_flags[@]}"} \
  "${mount_flags[@]}" -w /project \
  -u "$(id -u):$(id -g)" -e HOME=/tmp \
  -e GIT_CONFIG_COUNT=1 -e GIT_CONFIG_KEY_0=safe.directory -e GIT_CONFIG_VALUE_0='*' \
  -e NINJA_STATUS="${NINJA_STATUS:-[%f/%t (%r running)] }" \
  ${ccache_env[@]+"${ccache_env[@]}"} \
  ${extra_env[@]+"${extra_env[@]}"} \
  "$image" "${run_cmd[@]}"
