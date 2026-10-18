---
name: ota-release-verify
description: "Read-only verification of the already-published OTA channel against the latest GitHub Release: bind manifest.sourceSha to the Release tag commit, hash-check all 16 manifest parts, and verify every app's embedded version and chip family without publishing, releasing, flashing, OTA, or local ref mutation. Optional live-board HTTP requests require separate explicit user approval, including the state-changing POST /ota/check."
---

Invoke `$ota-release-verify`. Read [AGENTS.md](../../../AGENTS.md) first; this workflow grants no
additional authorization. Canonical skills live under `.agents/skills/`; hooks enforce local policy.

# ota-release-verify — is the published OTA channel actually flashable?

Verify that the **whole published OTA chain** — the GitHub Pages `manifest.json`, the per-target
images it points at, and the version stamped into them — is internally consistent, so a real
device on the LAN can pull an update and boot it. This is a health check of the *release channel
itself*, independent of any one board.

Check fixed filenames, full source SHA, all manifest parts, Release-byte identity and descriptors.
Channel validity does not prove a particular device's installed signing authority or hardware state.

> **Read-only by default.** Steps 1–3 use `git`, `gh`, `curl`, Python and `esptool image-info`
> against public Release/Pages bytes; they do not update Git refs, publish, sign, flash, start OTA,
> or touch a device. Step 4 contains optional live-board GET reads and a state-changing
> `POST /ota/check`; run them only after explicit approval for those methods and endpoints.
> It never sends `POST /ota/update`.

## Scope boundary — what this skill does NOT do

| Skill | Job |
|-------|-----|
| [`$flash-esp32`](../flash-esp32/SKILL.md) | Build (Docker) + USB-flash the **local tree** to a board. |
| [`$ship`](../ship/SKILL.md) | Take a **specific PR** to the board: squash-merge → watch post-merge CI → download the signed artifact → USB-flash or OTA → verify version. |
| **this skill** | Verify the **already-published** release channel (Pages manifest + the per-target images it points at + version coherence) is internally consistent — no build, no flash, no single PR. |

Do not duplicate their steps. If the manifest is coherent but a *particular device* still won't
update, that is a per-board TOFU/anchor issue → see the interpretation table and
[`docs/SECURITY.md`](../../../docs/SECURITY.md), not this channel check.

## Ground truth

Read [Security](../../../docs/SECURITY.md#release-pipeline-and-trust-boundaries),
`main/Kconfig.projbuild`, `scripts/build-pages.sh`, `scripts/ci-sign-artifacts.sh`,
`main/logic/target.hpp`, `main/ota_update.cpp`, and `scripts/check-release-pages-bytes.py`.
Verify their current four-target suffix/offset map and the fixed publication URLs before continuing.
Root Pages is stable Release; Dev and previews are separate. A test-mode dispatch does not publish.
The full two-snapshot byte-verification procedure below is mandatory.

## The check

### 1. Select the exact published Release and its tag commit

```bash
set -euo pipefail

# Show recent publishing runs for context. Selection below comes from the published GitHub Release,
# never from the first/last row of this display.
gh run list --workflow build.yml --branch main --limit 5 \
  --json databaseId,headSha,conclusion,displayTitle,createdAt \
  --jq '.[] | "\(.createdAt)  \(.conclusion)  \(.displayTitle)  (run \(.databaseId))"'

# The API's latest immutable, non-draft, non-prerelease Release is the authority. Missing or false
# `immutable` fails closed. Its dereferenced tag commit is the source SHA the manifest must carry.
RELEASE_JSON=$(gh api repos/:owner/:repo/releases/latest)
RELEASE_TAG=$(printf '%s' "$RELEASE_JSON" | jq -er \
  'select(.draft == false and .prerelease == false and .immutable == true) | .tag_name')
[[ "$RELEASE_TAG" =~ ^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]] || {
  echo "REFUSING: latest Release tag is not canonical stable vX.Y.Z: $RELEASE_TAG" >&2; exit 1;
}
REL=${RELEASE_TAG#v}
(( ${#REL} <= 31 )) || {
  echo "REFUSING: Release version exceeds the 31-byte app descriptor" >&2; exit 1;
}
SOURCE_SHA=$(gh api "repos/:owner/:repo/commits/$RELEASE_TAG" --jq .sha)
[[ "$SOURCE_SHA" =~ ^[0-9a-f]{40}$ ]] || {
  echo "REFUSING: Release tag did not resolve to a full commit SHA" >&2; exit 1;
}
printf 'Release %s -> %s\n' "$RELEASE_TAG" "$SOURCE_SHA"
```

`REL` is the version the manifest and every embedded app descriptor must report; `SOURCE_SHA` is
the only acceptable `manifest.sourceSha`. A merely well-formed but different SHA is stale or
unreconciled channel state and fails closed. `version.txt` = `1.5.0` here is only the **floor** —
do not expect it to equal the live version; CI stamps the real one uncommitted.

`build.yml` keeps this invariant structurally: a firmware-relevant `main` push enters the
sign/deploy jobs in `dev` mode and publishes only `gh-pages:/dev/`, while the stable Release and
root Pages are produced only by an explicitly dispatched `workflow_dispatch` run with
`release: true` on current `main`; the Release tag is targeted explicitly at that run's SHA. A
`workflow_dispatch` without `release: true` runs in `test` mode, an unprivileged build/test that
cannot sign or replace Pages. Therefore a different live root `sourceSha` is actual
stale/unreconciled channel state, not a supported republish mode. The publish jobs refetch and compare `origin/main` immediately before signing, signed-artifact
upload and Release/Pages mutation, and require the exact source-bound tag to remain newest;
Pages steps additionally require the matching latest GitHub Release object to report
`immutable: true` and expose four unique,
nonempty, SHA-256-digest-bound merged assets. Do not waive a failure before device OTA.

### 2. Download the complete published snapshot and bind all 16 parts to Release assets

```bash
set -euo pipefail
ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"
for tool in git gh curl jq python3 esptool; do
  command -v "$tool" >/dev/null || { echo "REFUSING: missing required tool: $tool" >&2; exit 1; }
done

RELEASE_JSON=$(gh api repos/:owner/:repo/releases/latest)
RELEASE_TAG=$(printf '%s' "$RELEASE_JSON" | jq -er \
  'select(.draft == false and .prerelease == false and .immutable == true) | .tag_name')
[[ "$RELEASE_TAG" =~ ^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]] || {
  echo "REFUSING: latest Release tag is not canonical stable vX.Y.Z: $RELEASE_TAG" >&2; exit 1;
}
REL=${RELEASE_TAG#v}
(( ${#REL} <= 31 )) || {
  echo "REFUSING: Release version exceeds the 31-byte app descriptor" >&2; exit 1;
}
SOURCE_SHA=$(gh api "repos/:owner/:repo/commits/$RELEASE_TAG" --jq .sha)
[[ "$SOURCE_SHA" =~ ^[0-9a-f]{40}$ ]] || {
  echo "REFUSING: Release tag did not resolve to a full commit SHA" >&2; exit 1;
}

SNAPSHOT=$(mktemp -d "${TMPDIR:-/tmp}/tesla-ota-snapshot.XXXXXX")
SITE="$SNAPSHOT/pages"
RELEASE="$SNAPSHOT/release"
mkdir -p "$SITE" "$RELEASE"
trap 'rm -rf -- "$SNAPSHOT"' EXIT HUP INT TERM
PAGES_JSON="$SNAPSHOT/pages-api.json"
gh api repos/:owner/:repo/pages > "$PAGES_JSON"
BASE=$(python3 scripts/check-pages-source.py "$PAGES_JSON" --print-url)
BASE=${BASE%/}
curl -fsSL --retry 2 --retry-all-errors "$BASE/manifest.json" -o "$SITE/manifest.json"

# Exact layoutVersion-2 contract: four ordered builds x four ordered parts. Hard-coded safe
# basenames prevent a remotely supplied manifest path from becoming a curl destination.
FILES=(
  bootloader-esp32.bin partition-table-esp32.bin tesla-key-esp32.bin ota_data_initial-esp32.bin
  bootloader-esp32s3.bin partition-table-esp32s3.bin tesla-key-esp32-s3.bin ota_data_initial-esp32s3.bin
  bootloader-esp32c3.bin partition-table-esp32c3.bin tesla-key-esp32-c3.bin ota_data_initial-esp32c3.bin
  bootloader-esp32c6.bin partition-table-esp32c6.bin tesla-key-esp32-c6.bin ota_data_initial-esp32c6.bin
)
for file in "${FILES[@]}"; do
  curl -fsSL --retry 2 --retry-all-errors "$BASE/$file" -o "$SITE/$file"
done

# Download the four exact versioned merged images by immutable asset ID from this Release object.
# Each contains the four Pages parts at their flash offsets, giving all 16 files a byte-for-byte
# Release counterpart without trusting a same-version manifest that an independent Pages writer
# could have regenerated around different bytes.
RELEASE_ID=$(printf '%s' "$RELEASE_JSON" | jq -er '.id | select(type == "number")')
release_asset_set() {
  jq -ecS --arg v "$REL" '
    ["tesla-key-esp32-" + $v + "-merged.bin",
     "tesla-key-esp32-s3-" + $v + "-merged.bin",
     "tesla-key-esp32-c3-" + $v + "-merged.bin",
     "tesla-key-esp32-c6-" + $v + "-merged.bin"] as $expected |
    [.assets[] | select(.name as $name | $expected | index($name)) |
      {id, name, size, digest}] | sort_by(.name) |
    if length == 4 and ([.[].name] | unique | length) == 4 and all(.[];
      (.id | type == "number") and (.size | type == "number" and . > 0) and
      (.digest | type == "string" and test("^sha256:[0-9a-f]{64}$")))
    then . else error("incomplete/unsafe merged Release asset set") end
  '
}
START_ASSET_SET=$(printf '%s' "$RELEASE_JSON" | release_asset_set) || {
  echo "REFUSING: Release lacks four unique digest-bound merged assets" >&2; exit 1;
}
file_sha256() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}'
  else shasum -a 256 "$1" | awk '{print $1}'; fi
}
download_release_asset() {
  local name="$1" row endpoint digest actual
  row=$(printf '%s' "$RELEASE_JSON" | jq -ec --arg name "$name" '
    [.assets[] | select(.name == $name)] |
    if length == 1 and .[0].size > 0 and
       (.[] | .digest | type == "string" and test("^sha256:[0-9a-f]{64}$")) then .[0]
    else error("missing, duplicate or empty Release asset: " + $name) end
  ') || { echo "REFUSING: invalid Release asset set for $name" >&2; exit 1; }
  endpoint=$(printf '%s' "$row" | jq -er \
    '.url | sub("^https://api.github.com/"; "") | select(length > 0)')
  digest=$(printf '%s' "$row" | jq -r '.digest // ""')
  gh api -H 'Accept: application/octet-stream' "$endpoint" > "$RELEASE/$name"
  [[ -f "$RELEASE/$name" && ! -L "$RELEASE/$name" ]] || {
    echo "REFUSING: unsafe downloaded Release asset: $name" >&2; exit 1;
  }
  actual="sha256:$(file_sha256 "$RELEASE/$name")"
  [[ "$actual" == "$digest" ]] || {
    echo "REFUSING: GitHub asset digest mismatch for $name" >&2; exit 1;
  }
}
for asset in \
  "tesla-key-esp32-$REL-merged.bin" \
  "tesla-key-esp32-s3-$REL-merged.bin" \
  "tesla-key-esp32-c3-$REL-merged.bin" \
  "tesla-key-esp32-c6-$REL-merged.bin"; do
  download_release_asset "$asset"
done

# This is the same fail-closed validator used by build-pages.sh. It checks the exact schema/order,
# offsets and bounds; binds sourceSha/version; then compares the declared length and SHA-256 of all
# 16 regular, non-symlink files and rejects missing or unreferenced .bin files.
python3 scripts/check-pages-manifest.py "$SITE" \
  --source-sha "$SOURCE_SHA" --version "$REL"
python3 scripts/check-release-pages-bytes.py "$SITE" "$RELEASE" --version "$REL"
jq -r '.builds[] | .chipFamily + ": " +
  ([.parts[] | "\(.path)@\(.offset) \(.size)B \(.sha256)"] | join("  "))' \
  "$SITE/manifest.json"

# Validate each downloaded app cryptographically against the reviewed production authority, then
# independently bind its ESP descriptor to the exact Release version and manifest chip family.
while IFS=: read -r target family boot app; do
  python3 scripts/check-firmware-artifacts.py \
    --target "$target" --version "$REL" --signed-app \
    --bootloader "$SITE/$boot" --app "$SITE/$app" \
    --expected-public-key-digest scripts/ota-signing-public-key.sha256
  INFO=$(esptool image-info "$SITE/$app") || {
    echo "REFUSING: $app is not a valid ESP image" >&2; exit 1;
  }
  printf '%s\n' "$INFO" | grep -Fqx "Detected image type: $family" \
    && printf '%s\n' "$INFO" | grep -Fqx "App version: $REL" || {
      echo "REFUSING: $app target/version does not match $family/$REL" >&2; exit 1;
    }
  printf '%s -> %s / %s\n' "$app" "$family" "$REL"
done <<'APP_IMAGES'
esp32:ESP32:bootloader-esp32.bin:tesla-key-esp32.bin
esp32s3:ESP32-S3:bootloader-esp32s3.bin:tesla-key-esp32-s3.bin
esp32c3:ESP32-C3:bootloader-esp32c3.bin:tesla-key-esp32-c3.bin
esp32c6:ESP32-C6:bootloader-esp32c6.bin:tesla-key-esp32-c6.bin
APP_IMAGES

# Refuse a race in which a newer Release became authoritative or any selected asset was replaced
# inside the same Release while this snapshot was downloaded/checked.
LATEST_RELEASE_JSON=$(gh api repos/:owner/:repo/releases/latest)
LATEST_RELEASE_ID=$(printf '%s' "$LATEST_RELEASE_JSON" | jq -er \
  'select(.draft == false and .prerelease == false and .immutable == true) | .id')
END_ASSET_SET=$(printf '%s' "$LATEST_RELEASE_JSON" | release_asset_set) || {
  echo "REFUSING: latest Release asset set became incomplete" >&2; exit 1;
}
[[ "$LATEST_RELEASE_ID" == "$RELEASE_ID" ]] || {
  echo "REFUSING: latest GitHub Release changed during verification" >&2; exit 1;
}
[[ "$END_ASSET_SET" == "$START_ASSET_SET" ]] || {
  echo "REFUSING: GitHub Release assets changed during verification" >&2; exit 1;
}
```

`new_install_prompt_erase:true` is expected (it wipes NVS on a **fresh USB install** via
esp-web-tools — it does **not** affect OTA, which never rewrites `nvs@0x9000`).

### 3. Verify every app's embedded version and chip family

The final loop in step 2 performs this check while its private snapshot still exists.
`esptool image-info` validates the bytes as ESP images; exact line matches prevent a stale
cross-target or same-filename/old-version app from passing. The fail-closed downloads prove all 16
exact same-origin URLs return successfully, including the four app URLs firmware uses; the
Release-byte validator additionally proves every one is the exact slice at its declared offset in
the corresponding versioned merged asset attached to the bound GitHub Release.

### 4. (Optional — live board) confirm a real device agrees

Only with explicit user approval to contact a device on the trusted LAN (no auth). `IP` = the
board's address or `tesla-key-esp32.local`. `POST /ota/check` starts a channel check and changes the
reported OTA-check state, so it is outside the default public-channel read and still never
authorizes `POST /ota/update`.

```bash
IP=tesla-key-esp32.local
# running version + chip. proxy .version carries the fixed "-esp32" suffix for ALL targets;
# the real chip is in .platform (ESP32/ESP32-S3/ESP32-C3/ESP32-C6).
curl -s "http://$IP/api/proxy/1/version"           # {"version":"X.Y.Z-esp32","platform":"ESP32-S3"}
curl -s "http://$IP/status" | jq -r .version       # X.Y.Z  (no "-esp32"; must match the release)

# non-blocking manifest check → poll status (ms = browser-clock NTP fallback for TLS)
curl -s -X POST "http://$IP/ota/check?ms=$(date +%s000)"
sleep 3
curl -s "http://$IP/ota/status" | jq
#   {state, progress, message, available, update_available, current}
```

`update_available:true` + `available` == the release reports a channel offer. Download, image
validation and update admission still need to succeed. A non-empty `message` on `state:"error"`
describes the failure symptom; use the table below to investigate before retrying.
(Endpoints: [`main/http_ota.cpp`](../../../main/http_ota.cpp),
[`main/http_api.cpp`](../../../main/http_api.cpp).)

## Interpreting failures — keyed on the known break modes

| Symptom | Where it shows | What it means | Fix |
|---------|----------------|---------------|-----|
| Pages API is not `legacy` + `gh-pages:/` | step 2 before download | the repository no longer has the single branch-backed root + `PR/<N>/` serving authority | do not OTA; restore the repository Pages source contract and rerun from a fresh snapshot |
| Any of the 16 fixed Pages files returns **≠ 200** | step 2 (or device `"could not start download"` for an app) | incomplete/old Pages snapshot — a manifest-bound URL does not exist | reconcile Pages from the exact Release commit; confirm `build-pages.sh` copied and manifest-bound all four parts for every target |
| manifest lacks layout v2 fields, or `sourceSha` ≠ the Release tag commit | step 2 | legacy, stale, or non-Release republish; regex-valid provenance alone is insufficient | do not OTA; reconcile Pages from the exact Release commit, then rerun this check |
| manifest length/SHA-256 differs from any downloaded part | step 2 | partial branch publication, cache race, or bytes changed underneath the manifest | do not OTA; let branch-backed Pages settle or republish, then rerun from a fresh snapshot |
| Pages part differs from its byte range in the Release merged asset | step 2 | same-version/source manifest was regenerated around bytes that were not attached to the bound GitHub Release | do not OTA; republish Pages exclusively from the signed Release staging tree and rerun |
| an app's embedded target/version differs from its manifest family/Release | step 2/3 | stale or cross-target app was published under the expected basename | do not OTA; rebuild/sign/publish the exact Release and verify all four descriptors |
| `/ota/status` `message:"downloaded image is invalid"` | step 4, `state:"error"` | image validation failed; a TOFU trust-anchor mismatch is one possible cause, not proven by this message | verify candidate chip/revision, integrity and signature evidence; only after establishing the cause, propose separately authorized [`$usb-recovery`](../usb-recovery/SKILL.md) if appropriate; see [`docs/SECURITY.md`](../../../docs/SECURITY.md) "Trust anchor (trust-on-first-use)" |
| `/ota/status` `message:"no newer version available"` | step 4, `state:"error"` | **version eligibility gate** — the candidate fails the selected channel/PR policy (for ordinary stable updates, an equal or older version; intentional return/preview exceptions still apply) | benign if the device already runs the release; otherwise verify candidate identity and the selected channel/PR → check step 2 |
| manifest `version` ≠ latest Release tag, or repeated "update available" with no version change | step 2 / step 4 `current` vs `available` | verify Release/Pages/candidate identity; floor-vs-stamped drift is one possible cause of a version mismatch. Repeated checks only report an offer and do not prove a stamping defect or perform an update | compare the selected Release, manifest and embedded version; if stamping drift is confirmed, inspect "Stamp firmware version" in [`.github/workflows/build.yml`](../../../.github/workflows/build.yml) and `steps.stamp.outputs.disp` |
| fewer than 4 builds / wrong chipFamily set / wrong part offset | step 2 | a target failed to stage, or the suffix/offset maps drifted | reconcile `image_suffix()`/`boot_offset()` across `ci-sign-artifacts.sh`, `build-pages.sh`, `ota_update.cpp`, `target.hpp` |

## See also

- [`$ship`](../ship/SKILL.md) — cut a release and put it on the board (this skill verifies the
  channel that leaves behind).
- [`$usb-recovery`](../usb-recovery/SKILL.md) — no-build USB reflash of the published signed image
  + otadata erase, the recovery a "signature bad" device needs.
- [`docs/SECURITY.md`](../../../docs/SECURITY.md) — signing key lifecycle, TOFU trust anchor, and
  the USB-reflash recovery for a device off the current key.

The runtime descriptor must equal the manifest and pass channel-aware eligibility in
`logic/ota_contract.hpp`: stable advances normally; intentional same-core PR/Dev transitions and
Dev/PR/pre-release return to Release are exceptions, with older stable cores allowed from Dev.
This is not a universal downgrade barrier. Source floor is not the CI display version. Optional
live checks use separately authorized POST /ota/check, plus independently approved cache GETs.
