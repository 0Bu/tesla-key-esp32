#!/usr/bin/env bash
# SessionStart hook: print a short toolchain-capability summary so a session (esp.
# Claude Code on the web / a remote sandbox) knows up front what it can and cannot do
# in THIS environment — instead of discovering mid-task that a firmware build or a
# USB flash is impossible here.
#
# This is purely informational: stdout becomes session context. It never blocks —
# it always exits 0, and every probe is best-effort (missing tools are reported, not
# fatal). Register it under "hooks.SessionStart" in .claude/settings.json.

have() { command -v "$1" >/dev/null 2>&1; }

docker_state="absent"
if have docker; then
  if docker info >/dev/null 2>&1; then
    docker_state="usable"
  else
    docker_state="present but daemon unreachable (no Docker-in-Docker)"
  fi
fi

esptool_state="absent"; have esptool && esptool_state="present"
jq_state="absent";      have jq      && jq_state="present"

# A firmware build needs a working Docker daemon (scripts/idf-docker.sh runs espressif/idf).
if [ "$docker_state" = "usable" ]; then
  build_note="firmware BUILD available (scripts/idf-docker.sh idf.py … build)"
else
  build_note="firmware BUILD UNAVAILABLE here (needs a working Docker daemon; edit/review code only)"
fi

# USB flashing requires esptool AND a physically attached board — never true in the cloud.
flash_note="USB FLASH UNAVAILABLE in a remote/cloud session (no USB passthrough); use the flash-esp32 skill on a host with the board attached"

cat <<EOF
[tesla-key-esp32 capabilities]
- docker: ${docker_state}
- esptool: ${esptool_state} | jq: ${jq_state}
- ${build_note}
- ${flash_note}
- Two gates block the MERGE of a PR into main until both pass for the current tree:
  /project-review (whole-firmware coherence) and /skill-audit (every skill+agent still matches
  the project). Opening a PR and pushing are NOT gated. A full /project-review also clears the
  skill-audit gate (it audits the skills too).
EOF

exit 0
