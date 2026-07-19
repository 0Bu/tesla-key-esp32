---
name: usb-recovery
description: Emergency, no-build USB recovery of a tesla-key-esp32 board that is bootlooping or refuses every OTA — reflash the already-published, SIGNED CI image to the app slot and erase otadata WITHOUT wiping NVS (pairing / key / VIN / WiFi survive). Use when asked to "recover", "unbrick", "de-brick", "rescue" or "resurrect" a board, when OTA reaches 100% then fails "signature bad" / "downloaded image is invalid" (the running image's TOFU trust anchor was signed with a key ≠ the published images), when the device is stuck in a boot/reboot loop after flashing an unsigned local build, when a normal reflash or OTA won't bring it back, or when the board reports the wrong/old version and refuses every update. User-only — it flashes physical hardware. For the normal build-and-flash path use flash-esp32; for merge → CI → flash use ship.
disable-model-invocation: true
---

# usb-recovery — no-build emergency reflash of a bricked / OTA-refusing board

Last-resort recovery for a board that **won't OTA and won't come back from a normal reflash**.
It builds **nothing** — it USB-writes an *already-published, signed* CI image to the app slot
and erases `otadata`, deliberately leaving `nvs@0x9000` untouched so the pairing, private key,
VIN and WiFi survive (no NFC re-enrol). Two failure modes dominate:

- **(a) OTA reaches 100 %, then `/ota/status` → `state:error, message:"downloaded image is
  invalid"`** (serial: `image valid, signature bad` / `ESP_ERR_OTA_VALIDATE_FAILED`). The
  running app verifies each OTA's RSA signature against the public key embedded in *itself*
  (TOFU, `CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`). If the running image was signed with
  a key ≠ the current CI `OTA_SIGNING_KEY` (classic case: a **local build** whose reported
  version is the `version.txt` floor, e.g. `1.4.0`), every CI-signed OTA fails at the final
  validate. Fixed live 2026-07-08 on the C5 `.196` → recovered to `1.4.34`, OTA "up to date".
- **(b) A boot / reboot loop after flashing an UNSIGNED local build.** An unsigned app
  `abort()`s in ESP-IDF's `check_signature_on_update_check()` during core init, **before
  `app_main`**, on *any* target — it does not boot-and-TOFU. Local `scripts/idf-docker.sh`
  builds come out unsigned (`CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=n`; the RSA-3072 key is a
  CI-only secret), so flashing one directly bricks the boot.

Both are cured the same way: **USB-flash the published, signed image + erase `otadata`.**

> ### Which path do I need?
> - **OTA still works** — `/ota/check` → `update_available`, `/ota/update` installs and reboots
>   onto the new version → **not this skill**; just OTA (or use [`ship`](../ship/SKILL.md)).
> - **Board boots but reports the wrong / old version and OTA is refused** ("signature bad" /
>   "downloaded image is invalid") → **this skill**.
> - **Board is dead / bootlooping** (e.g. after an unsigned local flash) → **this skill**.
> - **You WANT to wipe pairing too** (start completely clean) → *don't* use this; run
>   `esptool --chip <TARGET> -p <PORT> erase_flash`, then flash and re-pair (full NFC enrol).

## Scope vs. the neighbouring skills

- [`flash-esp32`](../flash-esp32/SKILL.md) is the **normal** path — build the local tree in
  Docker and USB-flash it; its "flashing a specific PR" section downloads the signed CI artifact.
- [`ship`](../ship/SKILL.md) is the **release** path — squash-merge the PR, ride post-merge CI,
  then flash the signed artifact (or OTA).
- **This skill** is the **no-build EMERGENCY reflash** of an *already-published* signed image
  plus an `otadata` erase, for the bootloop / "signature bad" cases where OTA and a plain
  reflash can't recover the board. It reuses flash-esp32's port detection and signed-artifact
  facts and does **not** re-teach the build.

## Host prerequisites

`esptool` runs on the **host** (`brew install esptool`) — Docker on macOS has no USB
passthrough. There is no `timeout` on macOS. Native-USB targets (s3/c3/c6/c5) enumerate as
`/dev/cu.usbmodem*`, the classic esp32's UART bridge as `/dev/cu.usbserial-*` — but **don't
just pick a node and assume the chip**; step 3 below probes for the port and the chip identity
together, which matters more here than in a normal flash since a wrong guess writes firmware.

## Partition map (from [`partitions.csv`](../../../partitions.csv)) — what recovery touches

| Region          | Offset                         | Size      | Recovery action                                   |
|-----------------|--------------------------------|-----------|---------------------------------------------------|
| bootloader      | `0x1000` esp32 / `0x2000` c5 / `0x0` s3·c3·c6 | —         | leave alone (never signature-checked); rewrite only if also damaged |
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

Pick the target and its image suffix (`""` / `-s3` / `-c3` / `-c6` / `-c5`, matching
`image_suffix()` in [`ci-build-all.sh`](../../../scripts/ci-build-all.sh) and
`TESLA_OTA_IMG_SUFFIX` in [`main/ota_update.cpp`](../../../main/ota_update.cpp)):

```bash
TARGET=esp32c5     # esp32 | esp32s3 | esp32c3 | esp32c6 | esp32c5
case "$TARGET" in
  esp32)   SFX="" ;;  esp32s3) SFX=-s3 ;;  esp32c3) SFX=-c3 ;;
  esp32c6) SFX=-c6 ;; esp32c5) SFX=-c5 ;;
esac
```

**Simplest (verified live) — pull the stable image straight off GitHub Pages** (same file CI
publishes; from [`build-pages.sh`](../../../scripts/build-pages.sh), base
`CONFIG_TESLA_OTA_FIRMWARE_BASE_URL`):

```bash
mkdir -p _ci
curl -fsSL -o "_ci/tesla-key-esp32$SFX.bin" \
  "https://0bu.github.io/tesla-key-esp32/tesla-key-esp32$SFX.bin"
esptool image-info "_ci/tesla-key-esp32$SFX.bin"   # sanity: Chip ID matches, App version, hash valid
```

**Or download the artifact from a specific green `build` run** (artifact name varies by version):

```bash
RUNID=$(gh run list --branch main --workflow build.yml --status success \
        --limit 1 --json databaseId --jq '.[0].databaseId')
ART=$(gh api repos/:owner/:repo/actions/runs/$RUNID/artifacts --jq '.artifacts[].name' | head -1)
gh run download "$RUNID" -n "$ART" -D _ci      # → _ci/tesla-key-esp32$SFX.bin (+ a versioned copy)
```

> The image **must** be the CI-signed one — flashing an unsigned local build re-creates failure
> mode (b) (bootloop) and resets the OTA trust anchor to a key no published image matches. Only
> sign a local build yourself if `ota_signing_key.pem` (the real CI key) is provably present in
> the repo root: `espsecure.py sign_data --version 2 --keyfile ota_signing_key.pem --output
> app.bin.signed app.bin`. The published image is the safe default. See
> [`docs/SECURITY.md`](../../../docs/SECURITY.md).

## 2. (Only if the bootloader / partition-table are ALSO damaged)

The two writes in step 3 fix the app + boot-select and are enough for the signature-bad /
unsigned-app cases. If serial shows the *bootloader itself* failing (not the app signature
check), restore those regions too — they are **never signature-checked**, so an unsigned local
build is fine for them. Get them either from a local `build/` of the SAME target
(`build/bootloader/bootloader.bin`, `build/partition_table/partition-table.bin`) or from Pages
(`bootloader-<TARGET>.bin`, `partition-table-<TARGET>.bin`, published by
[`build-pages.sh`](../../../scripts/build-pages.sh)), and add them at their offsets from the
table above — **still never `nvs@0x9000`**.

## 3. Recovery flash — signed app + erase otadata (NVS preserved)

Find the port **and confirm it's actually the board you mean to recover** — never take the
first port node you see, and never trust `$TARGET` from step 1 unchecked. Probe every
candidate node with a chip-only esptool call (no `--chip`, so esptool must detect it) and keep
the first that answers; this settles the port *and* gives you the board's real identity to
cross-check against `$TARGET`:

```bash
# esptool v5 renamed chip_id → chip-id (v5 still accepts the old spelling; v4 only has it).
esptool chip-id --help >/dev/null 2>&1 && CHIP_CMD=chip-id || CHIP_CMD=chip_id

# `find` (not a glob): under zsh an unmatched glob aborts the whole command.
PORT=""; CHIP_RAW=""
for p in $(find /dev -maxdepth 1 \( -name 'cu.usbmodem*' -o -name 'cu.usbserial-*' \
                                 -o -name 'ttyACM*' -o -name 'ttyUSB*' \) 2>/dev/null); do
  for extra in "" "--before no-reset"; do
    raw=$(esptool -p "$p" $extra "$CHIP_CMD" 2>&1 \
          | grep -m1 -oE '(Chip is|Chip type:)[[:space:]]*[A-Za-z0-9()+/. -]+' \
          | sed -E 's/^(Chip is|Chip type:)[[:space:]]*//')
    if [ -n "$raw" ]; then PORT="$p"; CHIP_RAW="$raw"; break 2; fi
  done
done
[ -z "$PORT" ] && { echo "No board answered on USB — check the cable/port (C5: hold BOOT)"; exit 1; }

case "$CHIP_RAW" in
  ESP32-S3*)                   DETECTED=esp32s3 ;;
  ESP32-C3*)                   DETECTED=esp32c3 ;;
  ESP32-C6*)                   DETECTED=esp32c6 ;;
  ESP32-C5*)                   DETECTED=esp32c5 ;;
  ESP32-D0WD*|ESP32|"ESP32 "*) DETECTED=esp32 ;;
  *) echo "REFUSING: could not identify connected chip (esptool said: '$CHIP_RAW')"; exit 1 ;;
esac
echo "Connected: $DETECTED on $PORT (recovering as TARGET=$TARGET)"

[ "$DETECTED" != "$TARGET" ] && { echo "REFUSING: connected board is $DETECTED but TARGET=$TARGET" \
  "(set in step 1) — wrong board, or another board answered first. With more than one board on" \
  "USB, unplug the others and re-run this probe."; exit 1; }
```

This cross-check is not optional here: with two boards on USB, a naive "first port that shows
up" pick has silently read the wrong board before (an S3 read as if it were the C5 target — see
project memory `esp32c5-support-local-patch`) — a bad pick in a *recovery* flash means writing
the wrong per-target image to the wrong board.

**s3 / c3 / c6 / classic esp32** (auto-reset works):

```bash
esptool --chip "$TARGET" -p "$PORT" write_flash 0x20000 "_ci/tesla-key-esp32$SFX.bin" \
  && esptool --chip "$TARGET" -p "$PORT" erase_region 0xf000 0x2000
```

**T-Dongle-C5** (esp32c5) — no auto-reset, so **hold BOOT (GPIO28) continuously through the
probe above and both commands below**. The `--before no-reset` fallback in the probe already
found the ROM download-mode node (it differs from the app node — app ≈ `usbmodem1101`, ROM ≈
`usbmodem2101` — which is exactly why the probe re-detects rather than reusing an earlier port):

```bash
esptool --chip esp32c5 -p "$PORT" --before no-reset --after no-reset \
  write_flash 0x20000 "_ci/tesla-key-esp32-c5.bin"
esptool --chip esp32c5 -p "$PORT" --before no-reset --after no-reset \
  erase_region 0xf000 0x2000
# then release BOOT and replug WITHOUT holding it (RTS reset is a no-op on this board).
```

`Hash of data verified.` on the `write_flash` and a clean `erase_region` mean success. The blank
`otadata` makes the bootloader fall back to the freshly written `ota_0`.

## 4. Verify — the board is recovered only when it says so

After it reboots and rejoins WiFi:

```bash
curl -s "http://<ip>/api/proxy/1/version"                 # {"version":"X.Y.Z-esp32","platform":"ESP32-C5"}
curl -s "http://<ip>/status" | jq '{version, paired}'      # paired:true — pairing survived (nvs untouched)
curl -s "http://<ip>/ota/check" && sleep 2 && curl -s "http://<ip>/ota/status"
```

- `version` / `platform` should read the just-flashed release (not the old `1.4.0` floor).
- `paired:true` confirms `nvs@0x9000` was preserved — no NFC re-enrol needed.
- `/ota/status` should now settle to up-to-date / no `available` update, and future OTAs verify
  because the running app's signature block (its trust anchor) is once again the CI key — no more
  "signature bad". A live *successful* OTA can't be demonstrated when you've just flashed the
  newest release (the downgrade gate refuses a same-version image before the signature check).

To confirm the live device is otherwise healthy (BLE up, no evcc timeouts), run
[`e2e-evcc`](../e2e-evcc/SKILL.md).
