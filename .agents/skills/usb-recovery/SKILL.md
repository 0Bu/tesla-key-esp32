---
name: usb-recovery
description: Emergency no-build USB recovery using an already-published, provenance-verified signed CI image. Run only after explicit user approval for the exact board, port, target, app write, and otadata erase. Live HTTP verification is a separate approval boundary. The minimal flow preserves secret NVS at 0x9000/0x6000; NVS wipe, merged images, and broader bootloader/partition recovery are outside this authorization unless separately and explicitly approved.
---

> **Canonical runner-neutral skill.** Read [`AGENTS.md`](../../../AGENTS.md) before acting.
> Project skills are canonical under [`.agents/skills/`](../), and lifecycle/PR policy is
> enforced by the runner-neutral core under [`tools/agent-hooks/`](../../../tools/agent-hooks/).
> This skill does not grant permissions beyond the user's explicit request.
> Invoke this workflow canonically as `$usb-recovery`.

# usb-recovery — no-build emergency reflash of a bricked / OTA-refusing board

Last-resort recovery for a board that **won't OTA and won't come back from a normal reflash**.
It builds **nothing** — it USB-writes an *already-published, signed* CI image to the app slot
and erases `otadata`, deliberately leaving `nvs@0x9000` untouched so the pairing, private key,
VIN and WiFi survive (no NFC re-enrol).

> **Destructive recovery boundary.** Diagnosis or a request to review recovery does not authorize a
> USB write. Before any mutation, identify the exact physical board/port/chip, target, signed image
> provenance and the minimal write set (`app@0x20000` plus `otadata@0xf000/0x2000` erase), then
> obtain explicit user approval. Never write, erase, dump, print, archive, or upload
> `nvs@0x9000/0x6000`. NVS contains WiFi, VIN, MQTT configuration, the vehicle private key, and BLE
> sessions and is secret material. Bootloader/partition-table recovery is a broader destructive
> scope requiring a second explicit approval after evidence that those regions are damaged.
> The USB-write approval does not authorize live verification.
> Before any HTTP request, obtain separate explicit user approval for the exact recovered device/IP and the named GET endpoints.
> `GET /ota/check` is state-changing and must be named explicitly in that live approval.

Two failure modes dominate:

- **(a) OTA reaches 100 %, then `/ota/status` → `state:error, message:"downloaded image is
  invalid"`** (serial: `image valid, signature bad` / `ESP_ERR_OTA_VALIDATE_FAILED`). The
  running app verifies each OTA's RSA signature against the public key embedded in *itself*
  (TOFU, `CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`). If the running image was signed with
  a key ≠ the current CI `OTA_SIGNING_KEY` (classic case: a **local build** whose reported
  version is the `version.txt` floor, e.g. `1.4.0`), every CI-signed OTA fails at the final
  validate. Fixed live 2026-07-08 on a test board → recovered to `1.4.34`, OTA "up to date".
- **(b) A boot / reboot loop after flashing an UNSIGNED local build.** An unsigned app
  `abort()`s in ESP-IDF's `check_signature_on_update_check()` during core init, **before
  `app_main`**, on *any* target — it does not boot-and-TOFU. Local `scripts/idf-docker.sh`
  builds come out unsigned (`CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=n`; the RSA-3072 key is a
  CI-only secret), so flashing one directly bricks the boot.

Both are cured the same way: **USB-flash the published, signed image + erase `otadata`.**

> ### Which path do I need?
> - **OTA still works** — `/ota/check` → `update_available`, `/ota/update` installs and reboots
>   onto the new version → **not this skill**; just OTA (or use [`$ship`](../ship/SKILL.md)).
> - **Board boots but reports the wrong / old version and OTA is refused** ("signature bad" /
>   "downloaded image is invalid") → **this skill**.
> - **Board is dead / bootlooping** (e.g. after an unsigned local flash) → **this skill**.
> - **You want to wipe pairing too** (start completely clean) → *do not use this skill*. Stop and
>   hand off to the installed global `$nvs-backup`/`$nvs-delete` workflow with separate explicit
>   authorization. Do not create a project copy and do not provide a whole-flash erase shortcut.

## Scope vs. the neighbouring skills

- [`$flash-esp32`](../flash-esp32/SKILL.md) is the **normal** path — build the local tree in
  Docker and USB-flash it; its "flashing a specific PR" section downloads the signed CI artifact.
- [`$ship`](../ship/SKILL.md) is the **release** path — squash-merge the PR, ride post-merge CI,
  then flash the signed artifact (or OTA).
- **This skill** is the **no-build EMERGENCY reflash** of an *already-published* signed image
  plus an `otadata` erase, for the bootloop / "signature bad" cases where OTA and a plain
  reflash can't recover the board. It reuses flash-esp32's port detection and signed-artifact
  facts and does **not** re-teach the build.

## Host prerequisites

`esptool` runs on the **host** (`brew install esptool`) — Docker on macOS has no USB
passthrough. There is no `timeout` on macOS. Native-USB targets (s3/c3/c6) enumerate as
`/dev/cu.usbmodem*`, the classic esp32's UART bridge as `/dev/cu.usbserial-*` — but **don't
just pick a node and assume the chip**; step 3 below probes for the port and the chip identity
together, which matters more here than in a normal flash since a wrong guess writes firmware.

## Partition map (from [`partitions.csv`](../../../partitions.csv)) — what recovery touches

| Region          | Offset                         | Size      | Recovery action                                   |
|-----------------|--------------------------------|-----------|---------------------------------------------------|
| bootloader      | `0x1000` esp32 / `0x0` s3·c3·c6 | —         | leave alone (never signature-checked); rewrite only if also damaged |
| partition-table | `0x8000`                       | —         | leave alone; rewrite only if also damaged         |
| **nvs**         | `0x9000`                       | `0x6000`  | **NEVER touch** — holds pairing / key / VIN / WiFi |
| **otadata**     | `0xf000`                       | `0x2000`  | **ERASE** — a blank `otadata` cleanly boots `ota_0` |
| phy_init        | `0x11000`                      | `0x1000`  | leave alone                                       |
| **ota_0 (app)** | `0x20000`                      | `0x1f0000`| **write the signed app here**                     |
| ota_1           | `0x210000`                     | `0x1f0000`| leave alone                                       |

The minimal, pairing-preserving recovery is exactly two writes: **signed app → `0x20000`** and
**erase `otadata`**. Because `nvs@0x9000` is never in the write set, pairing survives.

> ⚠️ **Never flash `tesla-key-esp32<sfx>-<ver>-merged.bin` for a recovery-with-pairing.** The
> merged image is a single full-flash blob that spans `0x0` and **overwrites `nvs@0x9000`**,
> forcing a full NFC re-pair. It's for a deliberate clean install only.

## 1. Get the published, SIGNED image (never a local `build/*.bin`)

Pick the target and its image suffix (`""` / `-s3` / `-c3` / `-c6`, matching
`image_suffix()` in [`ci-sign-artifacts.sh`](../../../scripts/ci-sign-artifacts.sh) and
`TESLA_OTA_IMG_SUFFIX` in [`main/ota_update.cpp`](../../../main/ota_update.cpp)):

```bash
set -euo pipefail
TARGET=esp32s3     # esp32 | esp32s3 | esp32c3 | esp32c6
case "$TARGET" in
  esp32)   SFX=""; FAMILY=ESP32 ;;  esp32s3) SFX=-s3; FAMILY=ESP32-S3 ;;
  esp32c3) SFX=-c3; FAMILY=ESP32-C3 ;; esp32c6) SFX=-c6; FAMILY=ESP32-C6 ;;
  *) echo "REFUSING: unsupported TARGET=$TARGET" >&2; exit 1 ;;
esac
```

**Option A — exact GitHub Release asset, cross-checked against its signed main artifact.** Require
an explicit release tag; bind it to the one successful main push build, select exactly the
versioned target asset, verify GitHub's recorded length/SHA-256, then require byte identity with
the source-SHA-bound signed Actions artifact. Both create and verified reuse runs produce that
artifact; reuse performs no signing or Release mutation/re-upload and accepts only RSA-PSS-valid
apps matching the production-authority pin. A mutable Release asset alone is not provenance:

```bash
set -euo pipefail
: "${TARGET:?run the target-selection block first}"
: "${FAMILY:?run the target-selection block first}"
[ "${SFX+x}" = x ] || { echo "REFUSING: image suffix is unset" >&2; exit 1; }
: "${RELEASE_TAG:?set RELEASE_TAG explicitly, for example v1.4.74}"
[[ "$RELEASE_TAG" =~ ^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]] || {
  echo "REFUSING: release tag is not canonical stable vX.Y.Z" >&2; exit 1;
}
VERSION=${RELEASE_TAG#v}
(( ${#VERSION} <= 31 )) || {
  echo "REFUSING: release version exceeds the 31-byte app descriptor" >&2; exit 1;
}
git fetch --tags -q
SOURCE_SHA=$(git rev-parse "$RELEASE_TAG^{commit}")
[[ "$SOURCE_SHA" =~ ^[0-9a-f]{40}$ ]] || {
  echo "REFUSING: release source SHA is unavailable" >&2; exit 1;
}
RUN_IDS=$(gh run list --workflow build --branch main --commit "$SOURCE_SHA" --limit 20 \
  --json databaseId,headSha,event,conclusion \
  --jq ".[] | select(.headSha == \"$SOURCE_SHA\" and .event == \"push\" and .conclusion == \"success\") | .databaseId")
[ "$(printf '%s\n' "$RUN_IDS" | awk 'NF {n++} END {print n+0}')" -eq 1 ] || {
  echo "REFUSING: release tag does not resolve to exactly one successful main push build" >&2; exit 1;
}
RUNID=$(printf '%s\n' "$RUN_IDS" | awk 'NF {print}')
RUN_SHA="$SOURCE_SHA"
RELEASE_JSON=$(gh api "repos/:owner/:repo/releases/tags/$RELEASE_TAG")
[ "$(printf '%s' "$RELEASE_JSON" | jq -r .tag_name)" = "$RELEASE_TAG" ] \
  && [ "$(printf '%s' "$RELEASE_JSON" | jq -r .draft)" = false ] || {
  echo "REFUSING: release is missing, draft or has the wrong tag" >&2; exit 1;
}
ASSET="tesla-key-esp32$SFX-$VERSION.bin"
ASSET_ROWS=$(printf '%s' "$RELEASE_JSON" | jq -r --arg n "$ASSET" \
  '.assets[] | select(.name == $n) | [.name, .size, (.digest // "")] | @tsv')
[ "$(printf '%s\n' "$ASSET_ROWS" | awk 'NF {n++} END {print n+0}')" -eq 1 ] || {
  echo "REFUSING: expected exactly one release asset named $ASSET" >&2; exit 1;
}
IFS=$'\t' read -r _ EXPECTED_SIZE EXPECTED_DIGEST <<< "$ASSET_ROWS"
[[ "$EXPECTED_SIZE" =~ ^[1-9][0-9]*$ && "$EXPECTED_DIGEST" =~ ^sha256:[0-9a-f]{64}$ ]] || {
  echo "REFUSING: release asset lacks an exact size/SHA-256 digest" >&2; exit 1;
}
(( EXPECTED_SIZE <= 0x1e8000 )) || {
  echo "REFUSING: release app exceeds the signed-app policy limit" >&2; exit 1;
}
CI_DIR=$(mktemp -d "${TMPDIR:-/tmp}/tesla-recovery.XXXXXX")
gh release download "$RELEASE_TAG" -p "$ASSET" -D "$CI_DIR"
APP="$CI_DIR/$ASSET"
[ -f "$APP" ] && [ ! -L "$APP" ] || { echo "REFUSING: release app missing/unsafe" >&2; exit 1; }
[ "$(wc -c < "$APP" | tr -d ' ')" = "$EXPECTED_SIZE" ] || {
  echo "REFUSING: release app length mismatch" >&2; exit 1;
}
if command -v sha256sum >/dev/null 2>&1; then ACTUAL_DIGEST=sha256:$(sha256sum "$APP" | awk '{print $1}')
else ACTUAL_DIGEST=sha256:$(shasum -a 256 "$APP" | awk '{print $1}'); fi
[ "$ACTUAL_DIGEST" = "$EXPECTED_DIGEST" ] || {
  echo "REFUSING: release app SHA-256 mismatch" >&2; exit 1;
}

# A Release asset can be replaced. Bind these bytes to the unique build run and its metadata by
# comparing them with the exact signed artifact produced by that run. Expiry is a hard stop; a
# durable standalone Release path needs a future signed provenance manifest published with it.
SIGNED_ART="tesla-key-esp32-$VERSION-$SOURCE_SHA"
SIGNED_ROWS=$(gh api "repos/:owner/:repo/actions/runs/$RUNID/artifacts" \
  --jq ".artifacts[] | select(.expired == false and .name == \"$SIGNED_ART\") | .name")
[ "$(printf '%s\n' "$SIGNED_ROWS" | awk 'NF {n++} END {print n+0}')" -eq 1 ] || {
  echo "REFUSING: source run lacks exactly one unexpired $SIGNED_ART artifact" >&2; exit 1;
}
PROVENANCE_DIR=$(mktemp -d "${TMPDIR:-/tmp}/tesla-release-provenance.XXXXXX")
gh run download "$RUNID" -n "$SIGNED_ART" -D "$PROVENANCE_DIR"
PROVENANCE_META="$PROVENANCE_DIR/dist/build-metadata.txt"
[ -f "$PROVENANCE_META" ] && [ ! -L "$PROVENANCE_META" ] \
  && [ "$(grep -c '^head_sha=' "$PROVENANCE_META")" -eq 1 ] \
  && [ "$(sed -n 's/^head_sha=//p' "$PROVENANCE_META")" = "$SOURCE_SHA" ] \
  && [ "$(grep -c '^display_version=' "$PROVENANCE_META")" -eq 1 ] \
  && [ "$(sed -n 's/^display_version=//p' "$PROVENANCE_META")" = "$VERSION" ] || {
  echo "REFUSING: release source artifact metadata does not match tag SHA/version" >&2; exit 1;
}
PROVENANCE_APP="$PROVENANCE_DIR/tesla-key-esp32$SFX.bin"
[ -f "$PROVENANCE_APP" ] && [ ! -L "$PROVENANCE_APP" ] || {
  echo "REFUSING: source-SHA-bound target app is missing/unsafe" >&2; exit 1;
}
if command -v sha256sum >/dev/null 2>&1; then
  PROVENANCE_DIGEST=sha256:$(sha256sum "$PROVENANCE_APP" | awk '{print $1}')
else
  PROVENANCE_DIGEST=sha256:$(shasum -a 256 "$PROVENANCE_APP" | awk '{print $1}')
fi
[ "$PROVENANCE_DIGEST" = "$ACTUAL_DIGEST" ] || {
  echo "REFUSING: Release bytes differ from the source-SHA-bound signed artifact" >&2; exit 1;
}
python3 scripts/check-firmware-artifacts.py --app-only --target "$TARGET" --version "$VERSION" --app "$APP" --signed-app --expected-public-key-digest scripts/ota-signing-public-key.sha256
APP_INFO=$(esptool image-info "$APP")
printf '%s\n' "$APP_INFO"
printf '%s\n' "$APP_INFO" | grep -qx "Detected image type: $FAMILY" \
  && printf '%s\n' "$APP_INFO" | grep -qx "App version: $VERSION" || {
  echo "REFUSING: release app target/version does not match selection" >&2; exit 1;
}
```

**Option B — exact signed artifact from a specific successful main `build` run.** The run also
contains unsigned and independent-rebuild evidence. Pages is served from branch-backed
`gh-pages:/`, not a run artifact; selecting the first Actions artifact is forbidden:

```bash
set -euo pipefail
: "${TARGET:?run the target-selection block first}"
: "${FAMILY:?run the target-selection block first}"
[ "${SFX+x}" = x ] || { echo "REFUSING: image suffix is unset" >&2; exit 1; }
: "${RUNID:?set RUNID to a specific successful main build run}"
[[ "$RUNID" =~ ^[1-9][0-9]*$ ]] || { echo "REFUSING: RUNID must be numeric" >&2; exit 1; }
RUN_SHA=$(gh run view "$RUNID" --json headSha --jq .headSha)
[[ "$RUN_SHA" =~ ^[0-9a-f]{40}$ ]] || { echo "REFUSING: malformed run SHA" >&2; exit 1; }
[ "$(gh run view "$RUNID" --json workflowName --jq .workflowName)" = build ] \
  && [ "$(gh run view "$RUNID" --json headBranch --jq .headBranch)" = main ] \
  && [ "$(gh run view "$RUNID" --json conclusion --jq .conclusion)" = success ] \
  && [[ "$(gh run view "$RUNID" --json event --jq .event)" =~ ^(push|workflow_dispatch)$ ]] || {
  echo "REFUSING: selected run is not a successful main build" >&2; exit 1;
}
ARTS=$(gh api "repos/:owner/:repo/actions/runs/$RUNID/artifacts" \
  --jq '.artifacts[] | select(.expired == false) | .name' \
  | grep -E "^tesla-key-esp32-(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z.-]+)?-${RUN_SHA}$" || true)
[ "$(printf '%s\n' "$ARTS" | awk 'NF {n++} END {print n+0}')" -eq 1 ] || {
  echo "REFUSING: expected exactly one unexpired signed main artifact" >&2; exit 1;
}
ART=$(printf '%s\n' "$ARTS" | awk 'NF {print}')
CI_DIR=$(mktemp -d "${TMPDIR:-/tmp}/tesla-recovery.XXXXXX")
gh run download "$RUNID" -n "$ART" -D "$CI_DIR"
META="$CI_DIR/dist/build-metadata.txt"
[ -f "$META" ] && [ ! -L "$META" ] \
  && [ "$(grep -c '^head_sha=' "$META")" -eq 1 ] \
  && [ "$(sed -n 's/^head_sha=//p' "$META")" = "$RUN_SHA" ] \
  && [ "$(grep -c '^display_version=' "$META")" -eq 1 ] || {
  echo "REFUSING: signed artifact metadata does not match the selected main run SHA" >&2; exit 1;
}
VERSION=$(sed -n 's/^display_version=//p' "$META")
[[ "$VERSION" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z.-]+)?$ ]] \
  && (( ${#VERSION} <= 31 )) \
  && [ "$ART" = "tesla-key-esp32-$VERSION-$RUN_SHA" ] || {
  echo "REFUSING: signed artifact name is not bound to metadata version and run SHA" >&2; exit 1;
}
APP="$CI_DIR/tesla-key-esp32$SFX.bin"
[ -f "$APP" ] && [ ! -L "$APP" ] || {
  echo "REFUSING: signed target app is missing or unsafe" >&2; exit 1;
}
[ "$(wc -c < "$APP" | tr -d ' ')" -le $((0x1e8000)) ] || {
  echo "REFUSING: signed target app exceeds the policy limit" >&2; exit 1;
}
python3 scripts/check-firmware-artifacts.py --app-only --target "$TARGET" --version "$VERSION" --app "$APP" --signed-app --expected-public-key-digest scripts/ota-signing-public-key.sha256
APP_INFO=$(esptool image-info "$APP")
printf '%s\n' "$APP_INFO"
printf '%s\n' "$APP_INFO" | grep -qx "Detected image type: $FAMILY" \
  && printf '%s\n' "$APP_INFO" | grep -qx "App version: $VERSION" || {
  echo "REFUSING: signed artifact target/version does not match metadata" >&2; exit 1;
}
```

> The image **must** be the published CI-signed one — flashing an unsigned local build re-creates
> failure mode (b) (bootloop), while a development-key signature creates a trust anchor no public
> image matches. Recovery must not search for or expose the real CI key locally. There is
> deliberately **no Pages fallback** here: without hardware Secure Boot, a USB write cannot
> authenticate the signer and would trust the selected Pages origin. If neither an exact Release
> asset nor an exact signed main artifact passes every check above, stop. See
> [`docs/SECURITY.md`](../../../docs/SECURITY.md).

## 2. (Only with separate explicit approval if bootloader / partition-table are ALSO damaged)

The two writes in step 3 fix the app + boot-select and are enough for the signature-bad /
unsigned-app cases. If serial shows the *bootloader itself* failing (not the app signature
check), restore those regions too — but only from the exact `firmware-unsigned` support artifact
of the **same validated main run**. A local build or Pages copy is not source-SHA-bound recovery
material. If the support artifact expired, stop.

Do not infer permission for these extra regions from approval of the minimal app/otadata recovery.
Present the evidence of bootloader/partition damage and the exact additional offsets, then obtain a
second explicit user approval before proceeding:

```bash
set -euo pipefail
: "${RUNID:?select and validate Option A or B first}"
: "${RUN_SHA:?select and validate Option A or B first}"
: "${VERSION:?select and validate Option A or B first}"
SUPPORT_ROWS=$(gh api "repos/:owner/:repo/actions/runs/$RUNID/artifacts" \
  --jq '.artifacts[] | select(.expired == false and .name == "firmware-unsigned") | .name')
[ "$(printf '%s\n' "$SUPPORT_ROWS" | awk 'NF {n++} END {print n+0}')" -eq 1 ] || {
  echo "REFUSING: exact same-run firmware-unsigned support artifact unavailable" >&2; exit 1;
}
SUPPORT_DIR=$(mktemp -d "${TMPDIR:-/tmp}/tesla-recovery-support.XXXXXX")
gh run download "$RUNID" -n firmware-unsigned -D "$SUPPORT_DIR"
SUPPORT_META="$SUPPORT_DIR/dist/build-metadata.txt"
[ -f "$SUPPORT_META" ] && [ ! -L "$SUPPORT_META" ] \
  && [ "$(grep -c '^head_sha=' "$SUPPORT_META")" -eq 1 ] \
  && [ "$(sed -n 's/^head_sha=//p' "$SUPPORT_META")" = "$RUN_SHA" ] \
  && [ "$(grep -c '^display_version=' "$SUPPORT_META")" -eq 1 ] \
  && [ "$(sed -n 's/^display_version=//p' "$SUPPORT_META")" = "$VERSION" ] || {
  echo "REFUSING: support artifact metadata does not match selected source/version" >&2; exit 1;
}
BOOTLOADER="$SUPPORT_DIR/_unsigned/$TARGET/bootloader/bootloader.bin"
PARTITION_TABLE="$SUPPORT_DIR/_unsigned/$TARGET/partition_table/partition-table.bin"
[ -f "$BOOTLOADER" ] && [ ! -L "$BOOTLOADER" ] \
  && [ -f "$PARTITION_TABLE" ] && [ ! -L "$PARTITION_TABLE" ] || {
  echo "REFUSING: target support binaries are missing/unsafe" >&2; exit 1;
}
BOOT_OFFSET=0x0; BOOT_MAX=$((0x8000))
[ "$TARGET" != esp32 ] || { BOOT_OFFSET=0x1000; BOOT_MAX=$((0x7000)); }
[ "$(wc -c < "$BOOTLOADER" | tr -d ' ')" -le "$BOOT_MAX" ] \
  && [ "$(wc -c < "$PARTITION_TABLE" | tr -d ' ')" -le $((0x1000)) ] || {
  echo "REFUSING: support binary exceeds its partition boundary" >&2; exit 1;
}
BOOT_INFO=$(esptool image-info "$BOOTLOADER")
printf '%s\n' "$BOOT_INFO" | grep -qx "Detected image type: $FAMILY" || {
  echo "REFUSING: support bootloader target mismatch" >&2; exit 1;
}
```

These variables add only the exact bootloader and partition table at their trusted offsets in the
step-3 command below — **still never `nvs@0x9000`**.

## 3. Recovery flash — signed app + erase otadata (NVS preserved)

Identify the intended physical board and set its **exact** port. Never pick the first enumerated
node: two same-target boards cannot be distinguished by chip family. Unplug all other boards (or
map the USB serial/product to the intended board), then probe only that explicit character device
and cross-check the detected chip against `$TARGET`.

Immediately before the commands, reconfirm that the user approved this exact board, port, target,
signed app, `0x20000` app write and `0xf000/0x2000` otadata erase. Any ambiguity is a hard stop:

```bash
# esptool v5 renamed chip_id → chip-id (v5 still accepts the old spelling; v4 only has it).
set -euo pipefail
: "${PORT:?set PORT to the explicitly identified /dev device; never auto-select}"
[ -c "$PORT" ] || { echo "REFUSING: PORT is not a character device: $PORT" >&2; exit 1; }
esptool chip-id --help >/dev/null 2>&1 && CHIP_CMD=chip-id || CHIP_CMD=chip_id
CHIP_RAW=$(esptool -p "$PORT" "$CHIP_CMD" 2>&1 \
  | grep -m1 -oE '(Chip is|Chip type:)[[:space:]]*[A-Za-z0-9()+/. -]+' \
  | sed -E 's/^(Chip is|Chip type:)[[:space:]]*//' || true)
if [ -z "$CHIP_RAW" ]; then
  CHIP_RAW=$(esptool -p "$PORT" --before no-reset "$CHIP_CMD" 2>&1 \
    | grep -m1 -oE '(Chip is|Chip type:)[[:space:]]*[A-Za-z0-9()+/. -]+' \
    | sed -E 's/^(Chip is|Chip type:)[[:space:]]*//' || true)
fi
[ -n "$CHIP_RAW" ] || {
  echo "REFUSING: no board answered on explicit PORT=$PORT; check cable or hold BOOT" >&2; exit 1;
}

case "$CHIP_RAW" in
  ESP32-S3*)                   DETECTED=esp32s3 ;;
  ESP32-C3*)                   DETECTED=esp32c3 ;;
  ESP32-C6*)                   DETECTED=esp32c6 ;;
  ESP32-D0WD*|ESP32|"ESP32 "*) DETECTED=esp32 ;;
  *) echo "REFUSING: could not identify connected chip (esptool said: '$CHIP_RAW')"; exit 1 ;;
esac
echo "Connected: $DETECTED on explicit $PORT (recovering as TARGET=$TARGET)"

if [ "$DETECTED" != "$TARGET" ]; then
  echo "REFUSING: connected board is $DETECTED but TARGET=$TARGET" \
    "(set in step 1) — wrong board/port. Unplug other boards and identify the intended port." >&2
  exit 1
fi
```

This explicit-port cross-check is not optional. With two boards on USB, a naive first-responder
pick can select the wrong device even when both share the same chip family.

**s3 / c3 / c6 / classic esp32** (auto-reset works):

```bash
set -euo pipefail
EXTRA_FLASH_ARGS=()
if [ -n "${BOOTLOADER:-}" ] || [ -n "${PARTITION_TABLE:-}" ]; then
  [ -n "${BOOTLOADER:-}" ] && [ -n "${PARTITION_TABLE:-}" ] && [ -n "${BOOT_OFFSET:-}" ] || {
    echo "REFUSING: partial bootloader/partition recovery selection" >&2; exit 1;
  }
  EXTRA_FLASH_ARGS=("$BOOT_OFFSET" "$BOOTLOADER" 0x8000 "$PARTITION_TABLE")
fi
if ! esptool --chip "$TARGET" -p "$PORT" write_flash \
    "${EXTRA_FLASH_ARGS[@]}" 0x20000 "$APP"; then
  echo "REFUSING: recovery write failed; otadata was not activated" >&2; exit 1
fi
if ! esptool --chip "$TARGET" -p "$PORT" erase_region 0xf000 0x2000; then
  echo "RECOVERY INCOMPLETE: app wrote but otadata activation erase failed" >&2; exit 1
fi
```

**A board with no auto-reset** — hold BOOT continuously through the probe above and both
commands below, and add `--before no-reset --after no-reset` to each. The download-mode port
node can differ from the app node, which is why the probe re-detects rather than reusing an
earlier port. Release BOOT and replug afterwards.

```bash
set -euo pipefail
EXTRA_FLASH_ARGS=()
if [ -n "${BOOTLOADER:-}" ] || [ -n "${PARTITION_TABLE:-}" ]; then
  [ -n "${BOOTLOADER:-}" ] && [ -n "${PARTITION_TABLE:-}" ] && [ -n "${BOOT_OFFSET:-}" ] || {
    echo "REFUSING: partial bootloader/partition recovery selection" >&2; exit 1;
  }
  EXTRA_FLASH_ARGS=("$BOOT_OFFSET" "$BOOTLOADER" 0x8000 "$PARTITION_TABLE")
fi
if ! esptool --chip "$TARGET" -p "$PORT" --before no-reset --after no-reset \
    write_flash "${EXTRA_FLASH_ARGS[@]}" 0x20000 "$APP"; then
  echo "REFUSING: recovery write failed; otadata was not activated" >&2; exit 1
fi
if ! esptool --chip "$TARGET" -p "$PORT" --before no-reset --after no-reset \
    erase_region 0xf000 0x2000; then
  echo "RECOVERY INCOMPLETE: app wrote but otadata activation erase failed" >&2; exit 1
fi
```

`Hash of data verified.` on the `write_flash` and a clean `erase_region` mean success. The blank
`otadata` makes the bootloader fall back to the freshly written `ota_0`.

## 4. Verify — the board is recovered only when it says so

> **Separate live-device boundary.**
> Do not run this section merely because the USB recovery was approved.
> First obtain explicit user approval to contact the exact recovered device/IP and list the intended GET endpoints.
> If that approval is absent, stop after the verified USB write and report that live recovery acceptance remains pending.
> `GET /ota/check` is state-changing and must be named explicitly in that live approval.

After that separate approval, and after the board reboots and rejoins WiFi:

```bash
set -euo pipefail
: "${DEVICE_IP:?set DEVICE_IP to the recovered board address}"
recovery_matches() {
  local version_json status_json
  version_json=$(curl --connect-timeout 3 --max-time 5 -fsS \
    "http://$DEVICE_IP/api/proxy/1/version") || return 1
  status_json=$(curl --connect-timeout 3 --max-time 5 -fsS \
    "http://$DEVICE_IP/status") || return 1
  printf '%s' "$version_json" | jq -e --arg v "$VERSION" --arg p "$FAMILY" \
    '.version == ($v + "-esp32") and .platform == $p' >/dev/null \
    && printf '%s' "$status_json" | jq -e --arg v "$VERSION" \
         '.version == $v and .paired == true' >/dev/null
}
RECOVERY_VERIFIED=0
RECOVERY_DEADLINE=$((SECONDS + 60))
while (( SECONDS < RECOVERY_DEADLINE )); do
  if recovery_matches; then RECOVERY_VERIFIED=1; break; fi
  sleep 2
done
[ "$RECOVERY_VERIFIED" -eq 1 ] || {
  echo "RECOVERY INCOMPLETE: exact version/platform/paired state not reachable within 60 seconds" >&2
  exit 1
}
curl -fsS "http://$DEVICE_IP/ota/check" >/dev/null
sleep 2
curl -fsS "http://$DEVICE_IP/ota/status" | jq .
```

- `version` / `platform` should read the just-flashed release (not the old `1.4.0` floor).
- `paired:true` confirms `nvs@0x9000` was preserved — no NFC re-enrol needed.
- `/ota/status` should now settle to up-to-date / no `available` update, and future OTAs verify
  because the running app's signature block (its trust anchor) is once again the CI key — no more
  "signature bad". A live *successful* OTA can't be demonstrated when you've just flashed the
  newest release (the downgrade gate refuses a same-version image before the signature check).

To confirm the live device is otherwise healthy (BLE up, no evcc timeouts), propose
global `$tesla-key-e2e-evcc` skill and obtain its separate explicit live-test approval.
