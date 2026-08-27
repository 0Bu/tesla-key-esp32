---
name: flash-esp32
description: Build tesla-key-esp32 firmware or, only with explicit user approval for the identified board and target, USB-flash a verified signed image (ESP32 / S3 / C3 / C6). Local builds are compile-only until an explicit development signing key is supplied; ordinary unsigned PR artifacts are never flash sources. Every flash preserves NVS and requires unambiguous port/chip verification.
---

> **Canonical runner-neutral skill.** Read [`AGENTS.md`](../../../AGENTS.md) before acting.
> Project skills are canonical under [`.agents/skills/`](../), and lifecycle/PR policy is
> enforced by the runner-neutral core under [`tools/agent-hooks/`](../../../tools/agent-hooks/).
> This skill does not grant permissions beyond the user's explicit request.
> Invoke this workflow canonically as `$flash-esp32`.

# flash-esp32 — build & USB-flash the firmware

Builds the ESP-IDF project (in Docker) and, only from an explicitly verified **signed** app and
after the user explicitly approves this physical-board mutation, flashes a connected ESP32 board (esp32 / esp32s3 / esp32c3 / esp32c6 — set `TARGET`, default
esp32s3) over USB from the host. NVS is left untouched, so the stored pairing, private key, VIN
and WiFi survive the flash (no re-pair needed). Use it only when the user asks for a build or
flash; editing anything under `main/` does not itself authorize hardware access —
including the embedded web UI (`main/www/` — `index.html` + `style.css` + `app.js`, spliced into
one page at build time), which is compiled into the app binary. A successful local compile is not
a flashable artifact: `CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=n` by design.

> This flashes over **USB**. For a remote, no-cable update use OTA (tap the firmware
> version in the web UI). USB flashing requires physical access and the board plugged in.

> **Authorization boundary.** A build request authorizes no signing-key use or USB write. A review,
> merge, release, or prior flash authorizes no new flash. Before `write-flash`/`write_flash` or
> `erase-region`/`erase_region`, identify the exact port, detected chip, image provenance, target,
> app offset `0x20000`, and activation erase `otadata@0xf000/0x2000`, then obtain explicit approval.
> Never touch `nvs@0x9000/0x6000`, never use a merged full-flash image, and never read or search for
> the real OTA signing key.

## Why two halves (Docker build, host flash)

There is **no local ESP-IDF install** — builds run via `scripts/idf-docker.sh`, which runs
the official `espressif/idf` Docker image pinned by the immutable tag+digest in
`esp-idf-toolchain.txt` (resolved by `scripts/idf-version.sh`, the same contract CI reads).
But **Docker Desktop on macOS has no USB passthrough**, so the *flash* step runs
on the **host** with `esptool` (`brew install esptool`). Build → produces `build/`, then
sign/verify the app explicitly or download a provenance-matched signed artifact, then flash from
the host.

## Local tree: compile first; signing is an explicit flash gate

Run from the **repo root** (where `CMakeLists.txt` lives). This command deliberately stops after
the compile; never append a flash of `build/tesla-key-esp32.bin`, because that file is unsigned:

```bash
set -euo pipefail
TARGET=${TARGET:-esp32s3}   # chip being built/flashed
case "$TARGET" in esp32|esp32s3|esp32c3|esp32c6) ;;
  *) echo "REFUSING: unsupported TARGET=$TARGET" >&2; exit 1 ;; esac
# Build via the digest-pinned ESP-IDF image (build/ stays host-owned). Root CMake applies every
# committed tesla-ble patch in lexical order. Reuse sdkconfig only when it names THIS target;
# changing TARGET must run set-target instead of silently rebuilding the previous chip.
if [ -f sdkconfig ] && grep -qx "CONFIG_IDF_TARGET=\"$TARGET\"" sdkconfig; then
  scripts/idf-docker.sh idf.py build 2>&1 | tail -15
else
  scripts/idf-docker.sh idf.py set-target "$TARGET" build 2>&1 | tail -15
fi
```

To flash that local tree, the operator must provide an existing RSA-3072 development key. The
skill must **not** generate one silently: changing the key changes the board's TOFU OTA trust
anchor, so public Pages OTA will fail until a real CI-signed image is USB-flashed again.

```bash
set -euo pipefail
: "${TARGET:?run the compile block for the intended target first}"
: "${DEV_SIGNING_KEY_FILE:?set DEV_SIGNING_KEY_FILE to an explicit RSA-3072 development key}"
[ -f "$DEV_SIGNING_KEY_FILE" ] && [ ! -L "$DEV_SIGNING_KEY_FILE" ] || {
  echo "REFUSING: signing key is missing, non-regular or a symlink" >&2; exit 1;
}
[ -f build/tesla-key-esp32.bin ] && [ ! -L build/tesla-key-esp32.bin ] || {
  echo "REFUSING: unsigned build input is missing or unsafe" >&2; exit 1;
}
SIGNED_APP=build/tesla-key-esp32.local-signed.bin
[ ! -L "$SIGNED_APP" ] || { echo "REFUSING: signed output path is a symlink" >&2; exit 1; }
espsecure.py sign_data --version 2 --keyfile "$DEV_SIGNING_KEY_FILE" \
  --output "$SIGNED_APP" build/tesla-key-esp32.bin
espsecure.py verify_signature --version 2 --keyfile "$DEV_SIGNING_KEY_FILE" "$SIGNED_APP" || exit 1
SIGNED_SIZE=$(wc -c < "$SIGNED_APP" | tr -d ' ')
(( SIGNED_SIZE <= 0x1e8000 )) || {
  echo "REFUSING: signed app is $SIGNED_SIZE bytes, over policy 0x1e8000" >&2; exit 1;
}
case "$TARGET" in esp32) FAMILY=ESP32 ;; esp32s3) FAMILY=ESP32-S3 ;;
  esp32c3) FAMILY=ESP32-C3 ;; esp32c6) FAMILY=ESP32-C6 ;;
  *) echo "REFUSING: unsupported TARGET=$TARGET" >&2; exit 1 ;; esac
SIGNED_INFO=$(esptool image-info "$SIGNED_APP")
printf '%s\n' "$SIGNED_INFO"
printf '%s\n' "$SIGNED_INFO" | grep -qx "Detected image type: $FAMILY" || {
  echo "REFUSING: signed app target does not match TARGET=$TARGET" >&2; exit 1;
}

# Flash from the HOST (Docker cannot reach USB). App-only write + otadata erase preserves the
# existing bootloader, partition table and nvs@0x9000; otadata is changed LAST as activation.
if [ -z "${PORT:-}" ]; then
  PORTS=$(ioreg -l -w 0 2>/dev/null | grep -iE '"USB Product Name"|"IOCalloutDevice"' \
         | grep -iA1 '"USB Single Serial"' | grep -o '/dev/cu\.usbmodem[^"]*' \
         | LC_ALL=C sort -u || true)
  [ "$(printf '%s\n' "$PORTS" | awk 'NF {n++} END {print n+0}')" -eq 1 ] || {
    echo "REFUSING: expected exactly one USB Single Serial port; set PORT explicitly after inspection" >&2
    printf '%s\n' "$PORTS" >&2; exit 1;
  }
  PORT=$(printf '%s\n' "$PORTS" | awk 'NF {print}')
fi
: "${PORT:?set PORT to the explicitly identified serial device}"
# Detect the chip on the chosen port and refuse a target mismatch before writing.
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
if ! esptool --chip "$TARGET" -p "$PORT" -b 460800 --before default-reset --after no-reset \
    write-flash 0x20000 "$SIGNED_APP"; then
  echo "REFUSING: signed app write failed; otadata was not activated" >&2; exit 1
fi
if ! esptool --chip "$TARGET" -p "$PORT" --before no-reset --after hard-reset \
    erase-region 0xf000 0x2000; then
  echo "FLASH INCOMPLETE: app wrote but otadata activation erase failed" >&2; exit 1
fi
```

> **Port detection above targets S3/C3/C6 boards** (native USB = `/dev/cu.usbmodem*`).
> The **classic esp32** has no native USB — it appears
> as a USB-UART bridge `/dev/cu.usbserial-*` (CP210x/CH340). List the candidates, identify the
> intended board and set its exact path (`PORT=/dev/cu.usbserial-...`); never pick the first entry.

> **Linux host** (e.g. Raspberry Pi): `ioreg` and `/dev/cu.*` are macOS-only. Ports appear as
> `/dev/ttyACM*` (native USB / WCH bridge) or `/dev/ttyUSB*` (CP210x/CH340). Inspect them and set
> exactly one `PORT=/dev/...`; ambiguity is a hard stop. Install esptool via `pipx install
> esptool` (no brew).

**Success looks like:** `Hash of data verified.` for each region, then
`Hard resetting via RTS pin...` → `Done`. The app image lands at `0x20000` (dual-OTA
layout). After reset the device rejoins WiFi in a few seconds; reload
`tesla-key-esp32.local` to see UI changes.

> ⚠️ **A local (unsigned) build crash-loops at boot — flash a _signed_ image.** Since the
> signed-OTA config landed (`CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT` in
> `sdkconfig.defaults`), the running app `abort()`s at startup when it carries no signature
> block. So a plain `idf.py build` image still flashes fine (`Hash of data verified.`) but then
> **reboot-loops before `app_main`** (`check_signature_on_update_check`, on every target).
> A normal PR `build` job uploads only `firmware-unsigned`; it is never a flash source. Use either
> the explicit local signing gate above or the opt-in, Environment-approved `signed-pr-preview`
> workflow below. A disposable development key becomes that board's OTA trust anchor until a real
> CI-signed image is USB-flashed again. Full detail: `docs/SECURITY.md`.

## Flashing a specific PR / branch onto a real board (use the *signed* CI artifact)

To test a PR/branch on a physical board, **do not USB-flash the local `scripts/idf-docker.sh`
build or the regular PR `firmware-unsigned` artifact.** The real-key path is opt-in: a maintainer
adds `signed-preview`, the default-branch `signed-pr-preview` workflow validates the same-repo PR,
waits for `firmware-signing` Environment approval, and uploads a signed artifact. If that workflow
did not complete successfully, **stop**; there is no signed PR image to flash.

```bash
set -euo pipefail
TARGET=esp32s3            # esp32 | esp32s3 | esp32c3 | esp32c6
: "${PR:?set PR to the pull-request number}"
: "${RUNID:?set RUNID to the successful signed-pr-preview run for this PR}"
[[ "$PR" =~ ^[1-9][0-9]*$ && "$RUNID" =~ ^[1-9][0-9]*$ ]] || {
  echo "REFUSING: PR and RUNID must be positive integers" >&2; exit 1;
}
PR_JSON=$(gh api "repos/:owner/:repo/pulls/$PR")
EXPECTED_SHA=$(printf '%s' "$PR_JSON" | jq -r .head.sha)
[[ "$EXPECTED_SHA" =~ ^[0-9a-f]{40}$ ]] || {
  echo "REFUSING: current PR head SHA is unavailable or malformed" >&2; exit 1;
}
[ "$(printf '%s' "$PR_JSON" | jq -r .state)" = open ] \
  && [ "$(printf '%s' "$PR_JSON" | jq -r .head.repo.full_name)" \
       = "$(printf '%s' "$PR_JSON" | jq -r .base.repo.full_name)" ] \
  && printf '%s' "$PR_JSON" | jq -e 'any(.labels[]; .name == "signed-preview")' >/dev/null || {
  echo "REFUSING: PR is closed, forked or no longer approved for signed preview" >&2; exit 1;
}
[ "$(gh run view "$RUNID" --json workflowName --jq .workflowName)" = signed-pr-preview ] \
  && [ "$(gh run view "$RUNID" --json conclusion --jq .conclusion)" = success ] || {
  echo "REFUSING: run is not a successful signed-pr-preview" >&2; exit 1;
}

# Select the one exact signed-preview artifact for this PR and current full head SHA; ambiguity or
# absence is a hard stop. The display version is metadata, not artifact-name authority.
EXPECTED_ART="tesla-key-esp32-pr${PR}-${EXPECTED_SHA}"
ARTS=$(gh api "repos/:owner/:repo/actions/runs/$RUNID/artifacts" \
  --jq ".artifacts[] | select(.expired == false and .name == \"$EXPECTED_ART\") | .name")
[ "$(printf '%s\n' "$ARTS" | awk 'NF {n++} END {print n+0}')" -eq 1 ] || {
  echo "REFUSING: expected exactly one unexpired signed artifact for current PR head $EXPECTED_SHA" >&2; exit 1;
}
ART=$(printf '%s\n' "$ARTS" | awk 'NF {print}')
CI_DIR=$(mktemp -d "${TMPDIR:-/tmp}/tesla-pr-artifact.XXXXXX")
gh run download "$RUNID" -n "$ART" -D "$CI_DIR"

# Bind downloaded bytes to the current PR head, not merely to a plausible artifact name.
META="$CI_DIR/_ci-input/dist/build-metadata.txt"
[ -f "$META" ] && [ ! -L "$META" ] || { echo "REFUSING: metadata missing/unsafe" >&2; exit 1; }
[ "$(grep -c '^head_sha=' "$META")" -eq 1 ] \
  && [ "$(sed -n 's/^head_sha=//p' "$META")" = "$EXPECTED_SHA" ] \
  && [ "$(grep -c '^display_version=' "$META")" -eq 1 ] || {
  echo "REFUSING: signed artifact provenance is not the current PR head" >&2; exit 1;
}
VERSION=$(sed -n 's/^display_version=//p' "$META")
[[ "$VERSION" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)-PR-([1-9][0-9]*)$ ]] \
  && (( ${#VERSION} <= 31 )) \
  && [ "${BASH_REMATCH[4]}" = "$PR" ] || {
  echo "REFUSING: signed artifact display version is not the exact PR preview identity" >&2; exit 1;
}
case "$TARGET" in esp32) SFX=""; FAMILY=ESP32 ;; esp32s3) SFX=-s3; FAMILY=ESP32-S3 ;;
  esp32c3) SFX=-c3; FAMILY=ESP32-C3 ;; esp32c6) SFX=-c6; FAMILY=ESP32-C6 ;;
  *) echo "REFUSING: unsupported TARGET=$TARGET" >&2; exit 1 ;; esac
APP="$CI_DIR/tesla-key-esp32$SFX.bin"
[ -f "$APP" ] && [ ! -L "$APP" ] || { echo "REFUSING: signed target app missing/unsafe" >&2; exit 1; }
[ "$(wc -c < "$APP" | tr -d ' ')" -le $((0x1e8000)) ] || {
  echo "REFUSING: signed preview app exceeds the policy limit" >&2; exit 1;
}
python3 scripts/check-firmware-artifacts.py --app-only --target "$TARGET" --version "$VERSION" --app "$APP" --signed-app --expected-public-key-digest scripts/ota-signing-public-key.sha256
APP_INFO=$(esptool image-info "$APP")
printf '%s\n' "$APP_INFO"
printf '%s\n' "$APP_INFO" | grep -qx "Detected image type: $FAMILY" \
  && printf '%s\n' "$APP_INFO" | grep -qx "App version: $VERSION" || {
  echo "REFUSING: signed preview target/version does not match metadata" >&2; exit 1;
}
# The suffix map is owned by scripts/ci-sign-artifacts.sh and mirrored by build-pages.sh,
# main/ota_update.cpp and main/logic/target.hpp.

# 2) Flash only the signed app, then erase otadata as the final activation step. This requires an
#    already-installed project bootloader/partition table; use the Web Serial installer for a
#    blank board. Bootloader, partition table and nvs@0x9000 remain untouched here.
: "${PORT:?detect and set the intended serial PORT first}"
# Detect the chip on the chosen port and refuse a target mismatch before writing.
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
esptool image-info "$APP" || exit 1
if ! esptool --chip "$TARGET" -p "$PORT" -b 460800 --before default-reset --after no-reset \
    write-flash 0x20000 "$APP"; then
  echo "REFUSING: signed preview app write failed; otadata was not activated" >&2; exit 1
fi
if ! esptool --chip "$TARGET" -p "$PORT" --before no-reset --after hard-reset \
    erase-region 0xf000 0x2000; then
  echo "FLASH INCOMPLETE: app wrote but otadata activation erase failed" >&2; exit 1
fi
```

- **Bonus:** the CI image carries the **CI-stamped version** (e.g. `1.4.22`), whereas a local
  build reports only the `version.txt` **floor** (e.g. `1.4.0`) — so the signed artifact makes
  the reported version match the release/manifest.
- **Never flash `tesla-key-esp32<sfx>-<ver>-merged.bin`** to preserve NVS — the merged image is a
  single full-flash blob spanning `0x0` and **wipes `nvs@0x9000`** (forces a full re-pair).
- After flashing, confirm the live device with the global `$tesla-key-e2e-evcc` skill.

## Picking the right serial port

This board exposes **two** USB serial interfaces — confirm before flashing:

| `/dev` node (example)        | USB product name              | What it is                     |
|------------------------------|-------------------------------|--------------------------------|
| `/dev/cu.usbmodem<SERIAL>`   | **USB Single Serial** (WCH)   | UART bridge — **use this one** |
| `/dev/cu.usbmodem<NNNN>`     | USB JTAG/serial debug unit    | S3/C3/C6 native USB (also works) |

The exact node name is device/cable-specific — **never hardcode it**; detect at runtime.
List both with their product names:

```bash
ioreg -l -w 0 2>/dev/null | grep -iE '"USB Product Name"|"IOCalloutDevice"' \
  | grep -iB1 usbmodem | sed -E 's/^[ |]+//'
```

Both interfaces can flash an S3. The local signed-flash command above targets the **WCH UART
bridge** ("USB Single Serial"), which is the conventional choice. If only the native
JTAG unit is present, target that node instead. If the listing finds no unambiguous intended
board, stop and identify its explicit port (check the cable/wake the board, then rerun the
listing). **Never drop `-p "$PORT"` or accept esptool's first auto-detected responder**: with
multiple compatible boards attached that can flash the wrong device.

## Notes & gotchas

- **No local IDF** — every `idf.py` step goes through `scripts/idf-docker.sh`, which uses the
  `espressif/idf` image pinned by `esp-idf-toolchain.txt` through `scripts/idf-version.sh`; CI
  reads that same contract. The mounted `build/` dir persists on the host, so
  Docker builds stay incremental. Run `idf.py` ad-hoc the same way, e.g.
  `scripts/idf-docker.sh idf.py size`.
- **Don't run a serial monitor in an automated session** — it never returns and hangs the
  turn. Flash without it. `idf.py monitor` isn't available on the host; for serial logs use a
  host terminal: `screen /dev/cu.usbmodemXXXX 115200` (exit `Ctrl-A` then `K`), or
  `pipx install esp-idf-monitor` → `esp-idf-monitor -p <PORT>`.
- **First build only** is slow (managed_components fetch + full compile). A `build/` dir
  already present means subsequent flashes are incremental and fast.
- **NVS is preserved** because the signed app write targets only `app@0x20000` and the activation
  step erases only `otadata@0xf000/0x2000`; neither touches `nvs@0x9000/0x6000`. This project skill
  never wipes NVS. If the user separately requests an NVS-destructive operation, stop and hand off
  to the installed global `$nvs-backup`/`$nvs-delete` workflow; do not create a project copy.
- **Artifacts are host-owned** thanks to `-u $(id -u):$(id -g)` — no root-owned files in the
  worktree. `build/`, `managed_components/`, `sdkconfig` are all gitignored.

## After flashing

To confirm the live device is healthy (paired, BLE up, no evcc timeouts), propose the
global `$tesla-key-e2e-evcc` skill and obtain its separate explicit live-test approval.
