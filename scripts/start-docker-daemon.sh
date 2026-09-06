#!/usr/bin/env bash
# Provision the pinned-image build boundary inside a cloud/remote session.
#
# In Claude Code cloud sessions (and comparable remote runners) the `docker` CLI
# is installed but no Docker engine is running: there is no /var/run/docker.sock
# and `docker info` fails. Because there is no local, unpinned ESP-IDF on purpose
# (see scripts/idf-docker.sh), scripts/idf-docker.sh is the ONLY firmware build
# path, and it needs an engine — so without one no target can be built at all.
#
# This script starts a Docker engine in the session so the EXISTING pinned-image
# build path works unchanged. It never installs or unpins a toolchain: builds
# keep running the exact espressif/idf digest from esp-idf-toolchain.txt, so CI
# parity and byte-for-byte reproducibility are preserved. This is the sanctioned
# way to satisfy AGENTS.md "do not bypass a missing boundary with an unpinned
# toolchain" while still enabling remote builds.
#
# Egress note: some environments' network policy allows Docker Hub's manifest
# host (registry-1.docker.io) but blocks its blob CDN
# (production.cloudfront.docker.com). Google's Docker Hub pull-through mirror
# (mirror.gcr.io, whose blobs come from storage.googleapis.com) is commonly
# reachable where the CDN is not. Pulling BY DIGEST through a mirror is
# byte-identical and containerd verifies the sha256, so the pin is unchanged.
# Override the mirror with IDF_DOCKER_REGISTRY_MIRROR (set it empty to disable).
# If a build still cannot pull, the blocker is the environment's network policy,
# not this repo — report the blocked host rather than unpinning the toolchain.
#
# Usage:
#   scripts/start-docker-daemon.sh            # start the engine if not already up
#   scripts/start-docker-daemon.sh --pull     # also pre-pull the pinned image
#   IDF_DOCKER_REGISTRY_MIRROR= scripts/start-docker-daemon.sh   # no mirror
#
# Idempotent: exits 0 immediately when an engine is already reachable, and never
# touches an existing daemon or its /etc/docker/daemon.json registry-mirrors.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
log() { printf 'start-docker-daemon: %s\n' "$*" >&2; }

pull_image=0
for arg in "$@"; do
  case "$arg" in
    --pull) pull_image=1 ;;
    -h|--help) sed -n '2,33p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) log "unknown argument: $arg (see --help)"; exit 2 ;;
  esac
done

# Configure a Docker Hub pull-through mirror unless one is already set or the
# caller disabled it. Merge-safe: preserves any other daemon.json keys and never
# overrides an existing registry-mirrors list.
ensure_mirror_config() {
  local mirror="${IDF_DOCKER_REGISTRY_MIRROR-https://mirror.gcr.io}"
  local cfg=/etc/docker/daemon.json
  if [ -z "$mirror" ]; then
    log "registry mirror disabled (IDF_DOCKER_REGISTRY_MIRROR is empty)"
    return 0
  fi
  if [ -f "$cfg" ] && grep -q '"registry-mirrors"' "$cfg" 2>/dev/null; then
    log "keeping the registry-mirrors already set in $cfg"
    return 0
  fi
  mkdir -p /etc/docker
  python3 - "$cfg" "$mirror" <<'PY'
import json, os, sys
cfg, mirror = sys.argv[1], sys.argv[2]
data = {}
if os.path.exists(cfg):
    try:
        with open(cfg) as handle:
            data = json.load(handle)
    except Exception:
        data = {}
mirrors = data.get("registry-mirrors") or []
if mirror not in mirrors:
    mirrors.append(mirror)
data["registry-mirrors"] = mirrors
with open(cfg, "w") as handle:
    json.dump(data, handle, indent=2)
    handle.write("\n")
PY
  log "configured registry mirror $mirror in $cfg"
}

start_daemon() {
  command -v docker  >/dev/null 2>&1 || { log "ERROR: docker CLI not found in PATH"; exit 3; }
  command -v dockerd >/dev/null 2>&1 || { log "ERROR: dockerd not found; this session cannot host an engine"; exit 3; }
  [ "$(id -u)" -eq 0 ] || { log "ERROR: must run as root to start dockerd"; exit 3; }

  ensure_mirror_config

  local logf=/var/log/idf-dockerd.log
  # The daemon (Go) does the network itself, so it — not just the client — must
  # see the proxy and trust its MITM CA. Go honors SSL_CERT_FILE for the CA.
  local env_args=()
  [ -n "${HTTPS_PROXY:-}" ] && env_args+=("HTTPS_PROXY=${HTTPS_PROXY}")
  [ -n "${https_proxy:-}" ] && env_args+=("https_proxy=${https_proxy}")
  [ -n "${NO_PROXY:-}" ]    && env_args+=("NO_PROXY=${NO_PROXY}")
  [ -n "${no_proxy:-}" ]    && env_args+=("no_proxy=${no_proxy}")
  [ -n "${SSL_CERT_FILE:-}" ] && [ -f "${SSL_CERT_FILE:-}" ] && env_args+=("SSL_CERT_FILE=${SSL_CERT_FILE}")

  rm -f /var/run/docker.sock
  log "starting dockerd (log: $logf) ..."
  # setsid + </dev/null detaches the engine so it survives this script exiting.
  setsid env "${env_args[@]}" dockerd \
    --host=unix:///var/run/docker.sock \
    --data-root=/var/lib/docker \
    >"$logf" 2>&1 </dev/null &
  disown 2>/dev/null || true

  local ready=0 i
  for i in $(seq 1 120); do
    if docker info >/dev/null 2>&1; then ready=1; break; fi
    pgrep -x dockerd >/dev/null 2>&1 || break   # daemon died: stop waiting
    sleep 0.5
  done
  if [ "$ready" -ne 1 ]; then
    log "ERROR: dockerd did not become ready within ~60s. Last log lines:"
    tail -n 30 "$logf" >&2 2>/dev/null || true
    exit 4
  fi
}

if docker info >/dev/null 2>&1; then
  log "Docker engine already reachable; nothing to start."
else
  start_daemon
fi

log "engine ready: $(docker info --format 'server={{.ServerVersion}} driver={{.Driver}} mirrors={{json .RegistryConfig.Mirrors}}' 2>&1)"

if [ "$pull_image" -eq 1 ]; then
  img="$("$repo_root/scripts/idf-version.sh" --image)"
  log "pulling pinned image $img ..."
  docker pull "$img"
  log "pull complete: $(docker image inspect "$img" --format '{{index .RepoDigests 0}}' 2>/dev/null || echo "$img")"
fi

log "ready. Build the firmware with the usual pinned entry point, e.g.:"
log "  scripts/idf-docker.sh ./scripts/ci-build-verify.sh \"\$(cat version.txt)\" \"\$(git rev-parse HEAD)\""
