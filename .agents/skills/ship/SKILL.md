---
name: ship
description: Take the current PR all the way to the board in one step — merge (squash), watch the post-merge CI build on main, download the SIGNED per-target image, USB-flash it (NVS/pairing preserved) and verify the device reports the new version; or trigger OTA instead when no cable is attached. Use when asked to "ship", "ship it", "merge und flash", "merge and flash", "release this to the board", or after a PR is approved and the change should end up running on the device. For a quick local-tree build+flash WITHOUT merging, use the flash-esp32 skill instead.
model: haiku
---

# ship — merge the PR, ride CI, put the signed build on the board

The observed manual loop is three messages: "commit und push" → "merge the PR" → "flashe".
This skill is that loop as one deterministic pipeline. It deploys the **signed CI artifact**
(never a local build — the CI image carries the real `OTA_SIGNING_KEY` signature and the
CI-stamped release version), so the device's OTA trust anchor stays intact.

## 0. Preconditions (check, don't assume)

```bash
gh pr view --json number,state,mergeable,headRefOid,body   # from the PR branch
```

- A PR exists for the current branch and is `MERGEABLE`.
- The branch is pushed (`git status -sb` shows no ahead-count).
- **Merge gate:** `require-project-review.sh` blocks `gh pr merge` inside Codex unless
  the PR body's `- [x] /project-review clean — merge gate @ <sha>` box is ticked and the
  stamp matches the head commit. If it isn't: stop and tell the user — run `/project-review`
  or have them tick the box / merge via the GitHub UI. Do NOT tick it yourself.

## 1. Merge (squash — repo convention)

```bash
gh pr merge <N> --squash --delete-branch
```

## 2. Watch the post-merge build on main — `gh run watch`, never sleep-polling

```bash
run_id=$(gh run list --workflow build --branch main --limit 1 --json databaseId --jq '.[0].databaseId')
gh run watch "$run_id" --exit-status    # blocks until done, fails on a red run (~4 min typical)
```

A red run: stop, report the failing job (`gh run view "$run_id" --log-failed | tail -40`).

**Release is conditional:** CI cuts a release/new version only when firmware-relevant files
changed (`Detect firmware-relevant changes` step). A docs/config-only merge builds but
releases nothing — then there is nothing to flash; say so and stop.

## 3. Download the signed image

Main-build artifact is named `tesla-key-esp32-<version>` (PR builds: `tesla-key-esp32-pr<N>-<version>`):

```bash
gh run download "$run_id" --dir dist    # single artifact → dist/tesla-key-esp32*.bin
```

Per target use `tesla-key-esp32<sfx>.bin` — suffix `""` (esp32) / `-s3` / `-c3` / `-c6` / `-c5`.
**Never flash `tesla-key-esp32<sfx>-<version>-merged.bin`** — the merged image rewrites the
whole flash including `nvs@0x9000` and wipes pairing/WiFi/VIN.

## 4. USB-flash (app slot only — NVS and pairing survive)

Write the app to `ota_0` and erase `otadata` so the bootloader boots the freshly written
slot (a device that previously OTA'd may be running from `ota_1`):

```bash
esptool --chip <target> -p <port> write_flash 0x20000 dist/tesla-key-esp32<sfx>.bin \
  && esptool --chip <target> -p <port> erase_region 0xf000 0x2000
```

`nvs@0x9000` is untouched → pairing/key/VIN/WiFi survive. Download-mode gotcha: the
T-Dongle-C5 needs BOOT held + `--before no-reset`. Port autodetect: see the
**flash-esp32** skill — same host mechanics.

### No cable? OTA instead

The same CI run publishes the Pages manifest. On the device (trusted LAN, no auth):

```bash
curl "http://<ip>/ota/check" && sleep 2 && curl "http://<ip>/ota/status"   # update_available?
curl -X POST "http://<ip>/ota/update"                                      # pull + reboot
```

The device refuses non-newer or wrongly-signed images on its own (downgrade gate + RSA
signature check), and rollback stays armed ~90 s after boot.

## 5. Verify — the loop is closed only when the device says so

```bash
curl -s "http://<ip>/status" | jq -r .version           # must equal the new release (vX.Y.Z w/o "v")
curl -s "http://<ip>/api/proxy/1/version"               # {"version":"X.Y.Z-esp32","platform":"ESP32-…"}
```

Report: merged PR, release version, target(s) flashed, device-confirmed version. If the
device still reports the old version after an OTA, check `/ota/status` `message` before
retrying — a "signature bad" there means the running image's trust anchor doesn't match the
published key (see the USB-recovery note in the project memory/docs).
