---
name: ota-release-verify
description: Verify the ALREADY-PUBLISHED OTA release channel is internally coherent so real devices can actually update — fetch the GitHub Pages manifest, assert version/build/offset coherence, and confirm every per-target image URL the firmware pulls is reachable (read-only — gh + curl + jq, plus one optional live-device GET). Use when asked to "verify the OTA channel", "is OTA healthy", "can devices update", "check the OTA manifest", "check the release channel", after a release lands on main, when a device reports "signature bad" or an OTA image 404s, or to confirm the manifest version / per-target images / embedded version stamp all agree. Scope is the published channel only — for a build + USB-flash use the flash-esp32 skill, for merge → CI → flash use the ship skill.
---

# ota-release-verify — is the published OTA channel actually flashable?

Verify that the **whole published OTA chain** — the GitHub Pages `manifest.json`, the per-target
images it points at, and the version stamped into them — is internally consistent, so a real
device on the LAN can pull an update and boot it. This is a health check of the *release channel
itself*, independent of any one board.

This subsystem has broken in three distinct ways, each of which this check catches:

- **Image rename 404** — v1.4.6 renamed the OTA image `tesla-key-esp32.bin` → `-<target>.bin`;
  the URL the device builds must exist same-origin or every OTA `"could not start download"`.
- **TOFU signing-key mismatch** — a device whose running image was signed with a key ≠ the current
  CI `OTA_SIGNING_KEY` rejects every published image as `"downloaded image is invalid"`
  (`signature bad`), because the signature proves authenticity (TOFU), not that the *channel* is
  wrong.
- **Floor-vs-stamped version drift** — [`version.txt`](../../../version.txt) is a committed
  **floor** (`1.4.0`); CI stamps the *real* release version into the binary and the manifest. If
  those disagree, devices loop on "update available" or silently no-op.

> **Read-only by default.** Steps 1–3 are `gh` + `curl` + `jq` against the public release/Pages —
> they touch no device. Step 4 is an *optional* live-board `GET` the user opts into.

## Scope boundary — what this skill does NOT do

| Skill | Job |
|-------|-----|
| [`flash-esp32`](../flash-esp32/SKILL.md) | Build (Docker) + USB-flash the **local tree** to a board. |
| [`ship`](../ship/SKILL.md) | Take a **specific PR** to the board: squash-merge → watch post-merge CI → download the signed artifact → USB-flash or OTA → verify version. |
| **this skill** | Verify the **already-published** release channel (Pages manifest + the per-target images it points at + version coherence) is internally consistent — no build, no flash, no single PR. |

Do not duplicate their steps. If the manifest is coherent but a *particular device* still won't
update, that is a per-board TOFU/anchor issue → see the interpretation table and
[`docs/SECURITY.md`](../../../docs/SECURITY.md), not this channel check.

## Ground truth (confirm by reading these files)

The device fetches, at runtime, from [`main/Kconfig.projbuild`](../../../main/Kconfig.projbuild)
defaults and [`main/ota_update.cpp`](../../../main/ota_update.cpp):

- `CONFIG_TESLA_OTA_MANIFEST_URL` = `https://0bu.github.io/tesla-key-esp32/manifest.json`.
- `CONFIG_TESLA_OTA_FIRMWARE_BASE_URL` = `https://0bu.github.io/tesla-key-esp32/`.
- Image URL the device builds: `FIRMWARE_BASE_URL` + `"tesla-key-esp32"` + `<suffix>` + `".bin"`
  (`ota_update.cpp` line ~232). **The step-3 URLs below are exactly this string** — a 404 here is
  the 404 a real device would hit.

Per-target image **suffix** (must agree across three places —
[`main/ota_update.cpp`](../../../main/ota_update.cpp) `TESLA_OTA_IMG_SUFFIX` /
[`main/logic/target.hpp`](../../../main/logic/target.hpp) `tk::image_suffix()`,
[`scripts/ci-build-all.sh`](../../../scripts/ci-build-all.sh) and
[`scripts/build-pages.sh`](../../../scripts/build-pages.sh) `image_suffix()`):

| target | suffix | chipFamily | bootloader offset |
|--------|--------|------------|-------------------|
| esp32   | `""`  | `ESP32`    | 4096 (`0x1000`)  |
| esp32s3 | `-s3` | `ESP32-S3` | 0                |
| esp32c3 | `-c3` | `ESP32-C3` | 0                |
| esp32c6 | `-c6` | `ESP32-C6` | 0                |
| esp32c5 | `-c5` | `ESP32-C5` | 8192 (`0x2000`)  |

`manifest.json` is written by [`scripts/build-pages.sh`](../../../scripts/build-pages.sh):
`{name:"tesla-key-esp32", version, new_install_prompt_erase:true, builds:[…]}` — **five** builds,
one per chipFamily. Each `build.parts` is exactly three parts:
`[ {bootloader-<t>.bin, offset per-target}, {partition-table-<t>.bin, offset 32768}, {tesla-key-esp32<sfx>.bin, offset 131072} ]`.
(`131072 = 0x20000` = the app slot; `32768 = 0x8000` = partition table.)

Images are built **and signed** by [`scripts/ci-build-all.sh`](../../../scripts/ci-build-all.sh)
(`espsecure.py sign_data --version 2`, Secure Boot v2 RSA-3072; key from CI secret
`OTA_SIGNING_KEY` → gitignored `ota_signing_key.pem`; unsigned only when no key). Signing is
**authenticity (TOFU), not freshness**.

Version coherence: [`scripts/next-version.sh`](../../../scripts/next-version.sh) auto-increments
the patch above the latest `v*` tag (floor = `version.txt`); the
[`.github/workflows/build.yml`](../../../.github/workflows/build.yml) "Stamp firmware version" step
overwrites `version.txt` in the workspace (**not** committed) with the version being published, so
`esp_app_get_description()->version`, the release tag, and the manifest `version` all agree. The
main Pages manifest carries `steps.stamp.outputs.disp` = the release version.

Device-side **downgrade gate** ([`ota_update.cpp`](../../../main/ota_update.cpp) ~266-284): before
the bulk download, `ota_task` reads the incoming image's own version via
`esp_https_ota_get_img_desc` and refuses anything not strictly newer than the running firmware
(`ver_newer`) — software anti-rollback, no eFuses.

## The check

### 1. Which version *should* be live

```bash
# Newest post-merge main 'build' run (the one that publishes Pages + the release):
gh run list --workflow build.yml --branch main --limit 5 \
  --json databaseId,headSha,conclusion,displayTitle,createdAt \
  --jq '.[] | "\(.createdAt)  \(.conclusion)  \(.displayTitle)  (run \(.databaseId))"'

# Latest release = newest v* tag (== next-version.sh's `latest`). gh release list agrees.
git fetch --tags -q && git tag -l 'v*' --sort=-v:refname | head -1
gh release list --limit 5
```

The `v*` tag (minus its `v`) is the version every device and the manifest should report.
`version.txt` = `1.4.0` here is only the **floor** — do not expect it to equal the live version;
CI stamps the real one uncommitted.

### 2. Fetch the manifest and validate it

```bash
BASE=https://0bu.github.io/tesla-key-esp32
REL=$(git tag -l 'v*' --sort=-v:refname | head -1 | sed 's/^v//')   # from step 1
curl -fsS "$BASE/manifest.json" -o /tmp/manifest.json && jq . /tmp/manifest.json

# a) version == latest release
jq -e --arg v "$REL" '.version == $v' /tmp/manifest.json \
  && echo "version OK ($REL)" || echo "MISMATCH: manifest $(jq -r .version /tmp/manifest.json) != release $REL"

# b) exactly 5 builds, one per chipFamily (set match)
jq -e '.builds | length == 5' /tmp/manifest.json && echo "5 builds OK"
jq -r '[.builds[].chipFamily] | sort | @csv' /tmp/manifest.json
#   expect: "ESP32","ESP32-C3","ESP32-C5","ESP32-C6","ESP32-S3"

# c) parts offsets per build — bootloader per-target, partition-table 32768, app 131072
jq -r '.builds[] | .chipFamily + ": " + ([.parts[] | "\(.path)@\(.offset)"] | join("  "))' \
  /tmp/manifest.json
#   ESP32:    bootloader-esp32.bin@4096      partition-table-esp32.bin@32768   tesla-key-esp32.bin@131072
#   ESP32-C5: bootloader-esp32c5.bin@8192    partition-table-esp32c5.bin@32768 tesla-key-esp32-c5.bin@131072
#   S3/C3/C6: bootloader-<t>.bin@0           partition-table-<t>.bin@32768     tesla-key-esp32<sfx>.bin@131072
```

`new_install_prompt_erase:true` is expected (it wipes NVS on a **fresh USB install** via
esp-web-tools — it does **not** affect OTA, which never rewrites `nvs@0x9000`).

### 3. Every per-target image must be reachable same-origin (the 404 test)

This is exactly the URL the firmware builds (§ ground truth). A 404 here = the pre-1.4.6-style
migration break — a device would fail with `"could not start download"`.

```bash
BASE=https://0bu.github.io/tesla-key-esp32
for sfx in "" -s3 -c3 -c6 -c5; do
  code=$(curl -s -o /dev/null -w '%{http_code}' -I "$BASE/tesla-key-esp32$sfx.bin")
  printf 'tesla-key-esp32%s.bin -> HTTP %s\n' "$sfx" "$code"
done
# every line must be 200
```

### 4. (Optional — live board) confirm a real device agrees

Only with the user's OK and a device on the trusted LAN (no auth). `IP` = the board's address or
`tesla-key-esp32.local`.

```bash
IP=tesla-key-esp32.local
# running version + chip. proxy .version carries the fixed "-esp32" suffix for ALL targets;
# the real chip is in .platform (ESP32/ESP32-S3/ESP32-C3/ESP32-C6/ESP32-C5).
curl -s "http://$IP/api/proxy/1/version"           # {"version":"X.Y.Z-esp32","platform":"ESP32-C5"}
curl -s "http://$IP/status" | jq -r .version       # X.Y.Z  (no "-esp32"; must match the release)

# non-blocking manifest check → poll status (ms = browser-clock NTP fallback for TLS)
curl -s "http://$IP/ota/check?ms=$(date +%s000)"
sleep 3
curl -s "http://$IP/ota/status" | jq
#   {state, progress, message, available, update_available, current}
```

`update_available:true` + `available` == the release means the device sees the channel and will
take it on `POST /ota/update`. A non-empty `message` on `state:"error"` is the diagnosis — read the
table below before retrying. (Endpoints: [`main/http_ota.cpp`](../../../main/http_ota.cpp),
[`main/http_api.cpp`](../../../main/http_api.cpp).)

## Interpreting failures — keyed on the known break modes

| Symptom | Where it shows | What it means | Fix |
|---------|----------------|---------------|-----|
| A `tesla-key-esp32<sfx>.bin` returns **≠ 200** | step 3 (or device `"could not start download"`) | image rename / missing Pages copy — the URL the device builds does not exist | re-run the main `build` publish; confirm `build-pages.sh` staged all 5 (pre-1.4.6 alias break) |
| `/ota/status` `message:"downloaded image is invalid"` (serial: `image valid, signature bad`) | step 4, `state:"error"` | **TOFU key mismatch** — the running image's trust anchor ≠ the current `OTA_SIGNING_KEY`; the channel is fine, the *device* can't accept it | USB-reflash the published signed `.bin` to `0x20000` + erase otadata (keeps NVS) — see [`usb-recovery`](../usb-recovery/SKILL.md) and [`docs/SECURITY.md`](../../../docs/SECURITY.md) "Trust anchor (trust-on-first-use)" |
| `/ota/status` `message:"no newer version available"` | step 4, `state:"error"` | **downgrade gate** — incoming image is not strictly newer than what's running (expected when already current, or a stale manifest) | benign if the device already runs the release; else the manifest/version stamp is behind → check step 2a |
| manifest `version` ≠ latest `v*` tag, or a device loops on "update available" with no version change | step 2a / step 4 `current` vs `available` | **floor-vs-stamped drift** — the published binary/manifest froze at the `version.txt` floor instead of the stamped release | inspect the "Stamp firmware version" step in [`.github/workflows/build.yml`](../../../.github/workflows/build.yml); the manifest must be built with `steps.stamp.outputs.disp` |
| fewer than 5 builds / wrong chipFamily set / wrong part offset | step 2b/2c | a target failed to stage, or the suffix/offset maps drifted | reconcile `image_suffix()`/`boot_offset()` across `ci-build-all.sh`, `build-pages.sh`, `ota_update.cpp`, `target.hpp` |

## See also

- [`ship`](../ship/SKILL.md) — cut a release and put it on the board (this skill verifies the
  channel that leaves behind).
- [`usb-recovery`](../usb-recovery/SKILL.md) — no-build USB reflash of the published signed image
  + otadata erase, the recovery a "signature bad" device needs.
- [`docs/SECURITY.md`](../../../docs/SECURITY.md) — signing key lifecycle, TOFU trust anchor, and
  the USB-reflash recovery for a device off the current key.

