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

# The host-side mock build is the ONE real verify loop that works even here (no IDF/Docker/
# board needed): it compiles + runs the pure logic in main/logic/ with the system toolchain.
# CI gates the firmware build on it (logic-test job). A cloud session should reach for this
# instead of assuming it can only reason about a change.
if have cmake || have g++ || have clang++; then
  verify_note="host LOGIC TESTS available — VERIFY pure logic in seconds: scripts/run-mock-tests.sh"
else
  verify_note="host logic tests UNAVAILABLE here (no cmake/g++/clang++)"
fi

cat <<EOF
[tesla-key-esp32 capabilities]
- docker: ${docker_state}
- esptool: ${esptool_state} | jq: ${jq_state}
- ${build_note}
- ${verify_note}
- ${flash_note}
- Two gates block a PR until they pass for the current tree: /project-review gates the MERGE
  into main (whole-firmware coherence); /skill-audit gates OPENING a PR and every PUSH to it
  (every skill+agent still matches the project). A full /project-review also clears the
  skill-audit gate (it audits the skills too).
EOF

exit 0
