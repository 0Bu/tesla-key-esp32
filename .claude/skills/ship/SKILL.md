---
name: ship
description: Take the current PR all the way to the board in one step — merge (squash), bind the exact post-merge main run and signed artifact to its merge SHA, USB-flash the per-target app (NVS/pairing preserved) and verify the device reports the new version; or trigger OTA instead when no cable is attached. Use when asked to "ship", "ship it", "merge und flash", "merge and flash", "release this to the board", or after a PR is approved and the change should end up running on the device. For a local compile or protected signed-PR-preview flash WITHOUT merging, use the flash-esp32 skill instead.
model: haiku
---

# ship — merge the PR, ride CI, put the signed build on the board

The observed manual loop is three messages: "commit und push" → "merge the PR" → "flashe".
This skill is that loop as one deterministic pipeline. It deploys the **signed CI artifact**
(never a local build — the CI image carries the real `OTA_SIGNING_KEY` signature and the
CI-stamped release version), so the device's OTA trust anchor stays intact.

## 0. Preconditions (check, don't assume)

```bash
set -euo pipefail
: "${PR:?set PR to the pull-request number}"
[[ "$PR" =~ ^[1-9][0-9]*$ ]] || { echo "REFUSING: PR must be numeric" >&2; exit 1; }
gh pr view "$PR" --json number,state,mergeable,headRefOid,body
```

- A PR exists for the current branch and is `MERGEABLE`.
- The branch is pushed (`git status -sb` shows no ahead-count).
- **Merge gate:** `require-project-review.sh` blocks `gh pr merge` inside Claude Code unless
  the PR body's `- [x] /project-review clean — merge gate @ <sha>` box is ticked and the
  stamp matches the head commit and the command carries that same full SHA in
  `--match-head-commit`. If it is stale, run the full `/project-review` first. After
  an actually clean review of that exact head, stamp both the `/project-review` and
  `/skill-audit` boxes yourself; never tick either as a bypass.

## 1. Merge (squash — repo convention)

```bash
set -euo pipefail
PR_HEAD=$(gh pr view "$PR" --json headRefOid --jq .headRefOid)
[[ "$PR_HEAD" =~ ^[0-9a-f]{40}$ ]] || {
  echo "REFUSING: PR head SHA is unavailable or malformed" >&2; exit 1;
}
gh pr merge "$PR" --match-head-commit "$PR_HEAD" --squash --delete-branch
MERGE_SHA=$(gh pr view "$PR" --json mergeCommit --jq '.mergeCommit.oid')
[[ "$MERGE_SHA" =~ ^[0-9a-f]{40}$ ]] || {
  echo "REFUSING: merge commit SHA is unavailable or malformed" >&2; exit 1;
}
```

## 2. Watch the post-merge build on main — `gh run watch`, never sleep-polling

```bash
set -euo pipefail
# Select by the exact merge SHA. Absence or multiple matching runs is not permission to guess;
# wait for Actions to enqueue the run, or set an explicitly reviewed run id and repeat the checks.
RUN_IDS=$(gh run list --workflow build --branch main --commit "$MERGE_SHA" --event push --limit 20 \
  --json databaseId,headSha,event \
  --jq ".[] | select(.headSha == \"$MERGE_SHA\" and .event == \"push\") | .databaseId")
[ "$(printf '%s\n' "$RUN_IDS" | awk 'NF {n++} END {print n+0}')" -eq 1 ] || {
  echo "REFUSING: expected exactly one build run for merge SHA $MERGE_SHA" >&2; exit 1;
}
run_id=$(printf '%s\n' "$RUN_IDS" | awk 'NF {print}')
gh run watch "$run_id" --exit-status    # blocks until done, fails on a red run (~4 min typical)
```

A red run: stop, report the failing job (`gh run view "$run_id" --log-failed | tail -40`).

**Release is conditional:** CI cuts a release/new version only when firmware-relevant files
changed (`Detect firmware-relevant changes` step). A docs/config-only merge may still rebuild a
signed artifact at the existing version, but it does not create new firmware to deploy; say so
and stop instead of flashing an unchanged rebuild.

## 3. Download the signed image

The main run contains several artifacts: `firmware-unsigned`, the signed
`tesla-key-esp32-<version>` artifact, and usually `github-pages`. Never download all of them and
infer a path. Select exactly one signed main artifact by name, then bind its metadata to the run SHA:

```bash
set -euo pipefail
RUN_SHA=$(gh run view "$run_id" --json headSha --jq .headSha)
[ "$RUN_SHA" = "$MERGE_SHA" ] || {
  echo "REFUSING: selected run SHA is not the merged commit" >&2; exit 1;
}
[ "$(gh run view "$run_id" --json workflowName --jq .workflowName)" = build ] \
  && [ "$(gh run view "$run_id" --json headBranch --jq .headBranch)" = main ] \
  && [ "$(gh run view "$run_id" --json conclusion --jq .conclusion)" = success ] || {
  echo "REFUSING: selected run is not a successful main build" >&2; exit 1;
}
ARTS=$(gh api "repos/:owner/:repo/actions/runs/$run_id/artifacts" \
  --jq '.artifacts[] | select(.expired == false) | .name' \
  | grep -E '^tesla-key-esp32-[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?$' || true)
[ "$(printf '%s\n' "$ARTS" | awk 'NF {n++} END {print n+0}')" -eq 1 ] || {
  echo "REFUSING: expected exactly one unexpired signed main artifact" >&2; exit 1;
}
ART=$(printf '%s\n' "$ARTS" | awk 'NF {print}')
VERSION="${ART#tesla-key-esp32-}"
SHIP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/tesla-main-artifact.XXXXXX")
gh run download "$run_id" -n "$ART" -D "$SHIP_DIR"
META="$SHIP_DIR/dist/build-metadata.txt"
[ -f "$META" ] && [ ! -L "$META" ] \
  && [ "$(grep -c '^head_sha=' "$META")" -eq 1 ] \
  && [ "$(sed -n 's/^head_sha=//p' "$META")" = "$RUN_SHA" ] \
  && [ "$(grep -c '^display_version=' "$META")" -eq 1 ] \
  && [ "$(sed -n 's/^display_version=//p' "$META")" = "$VERSION" ] || {
  echo "REFUSING: signed artifact metadata does not match the main run" >&2; exit 1;
}
```

Per target use `tesla-key-esp32<sfx>.bin` — suffix `""` (esp32) / `-s3` / `-c3` / `-c6`.
**Never flash `tesla-key-esp32<sfx>-<version>-merged.bin`** — the merged image rewrites the
whole flash including `nvs@0x9000` and wipes pairing/WiFi/VIN.

## 4. Select target, then USB-flash or OTA

Target selection is common to both branches; verification must not inherit it only from a skipped
USB block:

```bash
set -euo pipefail
TARGET=esp32s3  # choose esp32 | esp32s3 | esp32c3 | esp32c6
case "$TARGET" in esp32) SFX=""; FAMILY=ESP32 ;; esp32s3) SFX=-s3; FAMILY=ESP32-S3 ;;
  esp32c3) SFX=-c3; FAMILY=ESP32-C3 ;; esp32c6) SFX=-c6; FAMILY=ESP32-C6 ;;
  *) echo "REFUSING: unsupported target" >&2; exit 1 ;; esac
```

### USB (app slot only — NVS and pairing survive)

Write the app to `ota_0` and erase `otadata` so the bootloader boots the freshly written
slot (a device that previously OTA'd may be running from `ota_1`):

```bash
set -euo pipefail
: "${PORT:?set PORT to the explicitly identified serial device}"
[ -c "$PORT" ] || { echo "REFUSING: PORT is not a character device: $PORT" >&2; exit 1; }
APP="$SHIP_DIR/tesla-key-esp32$SFX.bin"
[ -f "$APP" ] && [ ! -L "$APP" ] || { echo "REFUSING: signed target app missing/unsafe" >&2; exit 1; }
[ "$(wc -c < "$APP" | tr -d ' ')" -le $((0x1e8000)) ] || {
  echo "REFUSING: signed main app exceeds the policy limit" >&2; exit 1;
}
APP_INFO=$(esptool image-info "$APP")
printf '%s\n' "$APP_INFO"
printf '%s\n' "$APP_INFO" | grep -qx "Detected image type: $FAMILY" \
  && printf '%s\n' "$APP_INFO" | grep -qx "App version: $VERSION" || {
  echo "REFUSING: signed main app target/version does not match metadata" >&2; exit 1;
}
esptool chip-id --help >/dev/null 2>&1 && CHIP_CMD=chip-id || CHIP_CMD=chip_id
CHIP_RAW=$(esptool -p "$PORT" "$CHIP_CMD" 2>&1 \
  | grep -m1 -oE '(Chip is|Chip type:)[[:space:]]*[A-Za-z0-9()+/. -]+' \
  | sed -E 's/^(Chip is|Chip type:)[[:space:]]*//' || true)
case "$CHIP_RAW" in ESP32-S3*) DETECTED=esp32s3 ;; ESP32-C3*) DETECTED=esp32c3 ;;
  ESP32-C6*) DETECTED=esp32c6 ;; ESP32-D0WD*|ESP32|"ESP32 "*) DETECTED=esp32 ;;
  *) echo "REFUSING: could not identify chip on $PORT" >&2; exit 1 ;; esac
[ "$DETECTED" = "$TARGET" ] || {
  echo "REFUSING: selected port is $DETECTED, requested target is $TARGET" >&2; exit 1;
}
if ! esptool --chip "$TARGET" -p "$PORT" write_flash 0x20000 "$APP"; then
  echo "REFUSING: signed app write failed; otadata was not activated" >&2; exit 1
fi
if ! esptool --chip "$TARGET" -p "$PORT" erase_region 0xf000 0x2000; then
  echo "DEPLOYMENT INCOMPLETE: app wrote but otadata activation erase failed" >&2; exit 1
fi
DELIVERY=usb
```

`nvs@0x9000` is untouched → pairing/key/VIN/WiFi survive. Download-mode gotcha: a
board with no auto-reset needs BOOT held + `--before no-reset`. Port selection: see the
**flash-esp32** skill — same host mechanics.

### No cable? OTA instead

The same CI run publishes the Pages manifest. The check is asynchronous, so bind its completed
status to the artifact's exact `VERSION` before authorizing the POST. A stale/different manifest,
an error, an unexpected state or a timeout is a hard stop:

```bash
set -euo pipefail
: "${DEVICE_IP:?set DEVICE_IP to the deployed board address}"
CHECK_JSON=$(curl --connect-timeout 5 --max-time 10 -fsS \
  "http://$DEVICE_IP/ota/check?ms=$(date +%s000)")
printf '%s' "$CHECK_JSON" | jq -e '.started == true' >/dev/null || {
  echo "REFUSING: OTA check did not start" >&2; exit 1;
}

OTA_READY=0
CHECK_DEADLINE=$((SECONDS + 60))
while (( SECONDS < CHECK_DEADLINE )); do
  OTA_JSON=$(curl --connect-timeout 5 --max-time 10 -fsS \
    "http://$DEVICE_IP/ota/status") || { sleep 2; continue; }
  printf '%s' "$OTA_JSON" | jq -e '
    (.state == "idle" or .state == "checking" or .state == "downloading" or
     .state == "done" or .state == "error") and
    (.available | type == "string") and (.current | type == "string") and
    (.message | type == "string") and (.update_available | type == "boolean")
  ' >/dev/null || { echo "REFUSING: invalid OTA status schema" >&2; exit 1; }
  OTA_STATE=$(printf '%s' "$OTA_JSON" | jq -r .state)
  case "$OTA_STATE" in
    checking) sleep 2; continue ;;
    error)
      echo "REFUSING: OTA check failed: $(printf '%s' "$OTA_JSON" | jq -r .message)" >&2
      exit 1 ;;
    idle)
      printf '%s' "$OTA_JSON" | jq -e --arg v "$VERSION" \
        '.update_available == true and .available == $v' >/dev/null || {
        echo "REFUSING: OTA manifest is not an available update for exact version $VERSION" >&2
        exit 1
      }
      OTA_READY=1
      break ;;
    *) echo "REFUSING: OTA check entered unexpected state $OTA_STATE" >&2; exit 1 ;;
  esac
done
[ "$OTA_READY" -eq 1 ] || { echo "REFUSING: OTA check timed out" >&2; exit 1; }

UPDATE_JSON=$(curl --connect-timeout 5 --max-time 10 -fsS -X POST \
  "http://$DEVICE_IP/ota/update")
printf '%s' "$UPDATE_JSON" | jq -e '.result == true' >/dev/null || {
  echo "REFUSING: OTA update did not start" >&2; exit 1;
}
DELIVERY=ota
```

The device refuses non-newer or wrongly-signed images on its own (downgrade gate + RSA
signature check), and rollback stays armed until the image has both run 90 s and PROVEN a link
(`logic/health_gate.hpp`) — so a flashed image that never reaches the LAN is not committed, and
after 600 s it is left pending for the next reboot to roll back.

## 5. Verify — the loop is closed only when the device says so

```bash
set -euo pipefail
: "${DEVICE_IP:?set DEVICE_IP to the deployed board address}"
: "${DELIVERY:?run exactly one delivery branch (usb or ota) first}"
case "$DELIVERY" in usb|ota) ;; *) echo "REFUSING: invalid delivery mode" >&2; exit 1 ;; esac

live_matches_artifact() {
  LIVE_REACHABLE=0
  LIVE_STATUS_JSON=$(curl --connect-timeout 3 --max-time 5 -fsS \
    "http://$DEVICE_IP/status") || return 1
  LIVE_STATUS_WALL=$SECONDS
  LIVE_VERSION_JSON=$(curl --connect-timeout 3 --max-time 5 -fsS \
    "http://$DEVICE_IP/api/proxy/1/version") || return 1
  LIVE_REACHABLE=1
  printf '%s' "$LIVE_STATUS_JSON" | jq -e --arg v "$VERSION" '.version == $v' >/dev/null \
    && printf '%s' "$LIVE_VERSION_JSON" | jq -e --arg v "$VERSION" --arg p "$FAMILY" \
         '.version == ($v + "-esp32") and .platform == $p' >/dev/null
}

if [ "$DELIVERY" = usb ]; then
  # USB flashing returns while the board is still booting/rejoining WiFi. Retry reachability and
  # exact identity briefly, but do not impose the OTA-only 100-s rollback probation.
  USB_VERIFIED=0
  USB_DEADLINE=$((SECONDS + 60))
  while (( SECONDS < USB_DEADLINE )); do
    if live_matches_artifact; then USB_VERIFIED=1; break; fi
    sleep 2
  done
  [ "$USB_VERIFIED" -eq 1 ] || {
    echo "DEPLOYMENT INCOMPLETE: USB-flashed $VERSION/$FAMILY not reachable within 60 seconds" >&2
    exit 1
  }
else
  # OTA is asynchronous and deliberately reboots. Bound the whole download/reboot/probe window;
  # transient connection failures are expected, but a reported OTA error is terminal.
  VERIFIED=0
  UPDATE_DEADLINE=$((SECONDS + 600))
  while (( SECONDS < UPDATE_DEADLINE )); do
    if live_matches_artifact; then VERIFIED=1; break; fi
    if OTA_JSON=$(curl --connect-timeout 3 --max-time 5 -fsS \
        "http://$DEVICE_IP/ota/status"); then
      printf '%s' "$OTA_JSON" | jq -e '
        (.state == "idle" or .state == "checking" or .state == "downloading" or
         .state == "done" or .state == "error") and (.message | type == "string")
      ' >/dev/null || { echo "DEPLOYMENT INCOMPLETE: invalid OTA status schema" >&2; exit 1; }
      if [ "$(printf '%s' "$OTA_JSON" | jq -r .state)" = error ]; then
        echo "DEPLOYMENT INCOMPLETE: OTA failed: $(printf '%s' "$OTA_JSON" | jq -r .message)" >&2
        exit 1
      fi
    fi
    sleep 2
  done
  [ "$VERIFIED" -eq 1 ] || {
    echo "DEPLOYMENT INCOMPLETE: OTA did not boot exact $VERSION/$FAMILY within 600 seconds" >&2
    exit 1
  }

  # A first successful boot is still PENDING_VERIFY. The rollback health gate starts only after
  # the firmware services have proved a link, so absolute device uptime is not evidence that its
  # 90-s probation has elapsed. Bind a baseline to the first exact post-OTA observation instead.
  PROBATION_BASELINE_UPTIME=$(printf '%s' "$LIVE_STATUS_JSON" \
    | jq -er '.sys.uptime_s | select(type == "number" and . >= 0) | floor') || {
    echo "DEPLOYMENT INCOMPLETE: live status lacks numeric sys.uptime_s" >&2; exit 1;
  }
  PROBATION_BASELINE_WALL=$LIVE_STATUS_WALL
  LAST_UPTIME=$PROBATION_BASELINE_UPTIME
  PROBATION_VERIFIED=0
  PROBATION_DEADLINE=$((PROBATION_BASELINE_WALL + 180))
  while (( SECONDS < PROBATION_DEADLINE )); do
    if live_matches_artifact; then
      LIVE_UPTIME=$(printf '%s' "$LIVE_STATUS_JSON" \
        | jq -er '.sys.uptime_s | select(type == "number" and . >= 0) | floor') || {
        echo "DEPLOYMENT INCOMPLETE: live status lacks numeric sys.uptime_s" >&2; exit 1;
      }
      if (( LIVE_UPTIME < LAST_UPTIME )); then
        echo "DEPLOYMENT INCOMPLETE: device rebooted during OTA probation" >&2
        exit 1
      fi
      LAST_UPTIME=$LIVE_UPTIME
      OBSERVED_WALL=$((LIVE_STATUS_WALL - PROBATION_BASELINE_WALL))
      OBSERVED_UPTIME=$((LIVE_UPTIME - PROBATION_BASELINE_UPTIME))
      CLOCK_SKEW=$((OBSERVED_UPTIME - OBSERVED_WALL))
      if (( CLOCK_SKEW < -5 || CLOCK_SKEW > 5 )); then
        echo "DEPLOYMENT INCOMPLETE: uptime/wall-clock drift suggests a hidden reboot" >&2
        exit 1
      fi
      if (( OBSERVED_WALL >= 100 && OBSERVED_UPTIME >= 100 )); then
        PROBATION_VERIFIED=1
        break
      fi
    elif [ "${LIVE_REACHABLE:-0}" -eq 1 ]; then
      echo "DEPLOYMENT INCOMPLETE: device changed version/platform during OTA probation" >&2
      exit 1
    fi
    sleep 2
  done
  [ "$PROBATION_VERIFIED" -eq 1 ] || {
    echo "DEPLOYMENT INCOMPLETE: exact image was not stable for 100 seconds after first live observation" >&2
    exit 1
  }

  # Time and identity prove a stable boot, but not that ESP-IDF accepted the irreversible
  # mark-valid call. This boot-local diagnostic line is emitted only after that API returns
  # ESP_OK; its absence (including a wrapped/unavailable diagnostic ring) must remain inconclusive.
  OTA_DIAG=$(curl --connect-timeout 3 --max-time 5 -fsS \
    "http://$DEVICE_IP/diag?redact=1") || {
    echo "DEPLOYMENT INCOMPLETE: cannot verify OTA mark-valid result" >&2
    exit 1
  }
  printf '%s' "$OTA_DIAG" | grep -F 'OTA image healthy after ' \
    | grep -F 'marked valid (rollback cancelled' >/dev/null || {
    echo "DEPLOYMENT INCOMPLETE: firmware did not confirm OTA rollback cancellation" >&2
    exit 1
  }
fi
```

Report: merged PR, release version, target(s) flashed, device-confirmed version and — for OTA —
the exact version/platform observed for at least 100 seconds from the first post-OTA live
baseline, with monotonic uptime advancing by the same minimum and tracking wall-clock time
within five seconds, plus the boot-local `/diag?redact=1` confirmation that ESP-IDF accepted
rollback cancellation. If the
device still reports the old version after an OTA, check `/ota/status` `message` before
retrying — a "signature bad" there means the running image's trust anchor doesn't match the
published key (see the USB-recovery note in the project memory/docs).
