---
name: deploy-bench
description: Identify the intended physical ESP32 bench among multiple USB boards, including benches currently running Tesla or Daikin firmware, delegate signed app-only USB flashing to flash-esp32 (or usb-recovery's steps for a published image), then test the implementation on that same board. Does not publish or merge repository changes.
---

Invoke `$deploy-bench`. Read [AGENTS.md](../../../AGENTS.md) first. The user's explicit invocation
authorizes the workflow below; automatic selection by an agent does not. Canonical skills live
under `.agents/skills/`.

# deploy-bench — identify, USB-flash and test the bench

Test the implementation on the intended physical bench, even when its current firmware belongs to
`daikin-altherma-esp32`. For signed-image verification and the USB write, use
[$flash-esp32](../flash-esp32/SKILL.md) for a signed PR preview or development-key image, or
[$usb-recovery](../usb-recovery/SKILL.md) steps 1 and 3 for an already-published image. [$deploy](../deploy/SKILL.md) owns commit, push, PR and merge; this skill does
not perform those steps or deploy to a production device.

The user's own `$deploy-bench` invocation is explicit user authorization to identify the intended bench,
reset/query that board over USB, read its bootloader and partition table without NVS, verify and
flash the eligible signed app at `0x20000`, activate it by erasing `otadata@0xf000/0x2000`, and run
same-board passive acceptance. Acceptance includes bounded serial logs and cache-only
`GET /status`, `GET /diag?redact=1` and `GET /api/proxy/1/version`. It also covers a clean reset
and timed boot check when serial release delays startup after a successful write/activation.
Carry this scope into `$flash-esp32` or the delegated `$usb-recovery` steps; do not ask for USB or
HTTP approval again after resolving the board, port, target and artifact. Ask for missing identity
information only when selection remains ambiguous. Vehicle commands, pairing, charge toggles,
cluster access, OTA, NVS/identity changes and broader recovery need separate scope.
Run multi-line shell blocks, here and in the delegated skills, through a scratch script as
[Development](../../../docs/DEVELOPMENT.md#running-workflow-shell-blocks) describes.

## 1. Bind the implementation and test plan

Identify the source commit or local patch, affected behavior and expected positive and rejection
cases. Define the tests before flashing, including what an isolated unpaired bench can exercise.
Record the expected image version, target and provenance. A test of an older artifact does not
accept uncommitted changes.

When testing changes to this skill itself with unchanged firmware, record the skill revision and
the explicitly selected signed CI firmware revision separately. Exercise discovery, flash and
acceptance using that known firmware; do not claim it contains unrelated local firmware changes.
The CI firmware-relevance classifier may correctly produce no new image for agent-only changes.

Select exactly one signed image source:

- the exact-head signed PR preview, or a local tree signed with an explicitly supplied development
  key and its approved trust lineage, both through `$flash-esp32`;
- an already-published main-build or Release image, selected and verified by
  [$usb-recovery](../usb-recovery/SKILL.md) step 1 (Option A or B, including the production
  public-key pin), then written by its step-3 port probe, bootloader-compatibility check and
  app-only write under this skill's bench approval.

A compile-only local build is not flashable. Never access the offline production key. If no
eligible signed image exists, report that prerequisite; do not silently publish, merge, use an
unsigned app or substitute a different source revision.

## 2. Find the physical bench without probing every board

Enumerate serial ports and their OS USB parent, serial number and physical location without opening
or resetting them. On macOS inspect USB devices and serial clients; use the IOService subtree to
retain the parent-to-interface mapping that the separate listings alone can omit:

```bash
ioreg -p IOUSB -l -w 0
ioreg -r -c IOSerialBSDClient -l -w 0
ioreg -p IOService -r -c IOUSBHostDevice -l -w 0
```

On Linux correlate `/dev/serial/by-id`, `/dev/serial/by-path` and the corresponding sysfs USB parent.
Group multiple interfaces belonging to one physical board. Native USB and UART bridges can both
be valid; a preferred bridge name does not identify the intended board.

Match the intended bench against an already established session identity or the operator's private
hardware inventory: a unique USB serial/parent mapping or a verified physical attachment, with the
expected hardware MAC when available. Keep real identifiers outside the repository. Port names and
USB location alone can change or be reused; recheck attachment before mutation.

The running project, hostname, IP, first serial responder, chip family or product label alone must
not select a board. A Tesla bench can currently run Daikin firmware and vice versa. Do not probe,
reset or contact other candidates to discover which one responds. If selection is missing or
ambiguous, obtain the bench's USB identity or a physical unplug/replug identification, then repeat
the passive inventory. Never choose the first port.

Only after identifying the physical bench, bind its current explicit `PORT` and perform the scoped
chip/MAC check on that port. These serial operations may reset it. Match the detected chip and
hardware identity to the selected board and signed image; a mismatch stops the flow. Re-enumeration
must resolve to that same physical board, not to whichever device inherits its old port name.

## 3. Check the installed layout and preserve identity

Use collected evidence, a bounded serial boot log or explicitly authorized passive device reads to
identify current firmware. Current Tesla/Daikin labels are observations, not physical identity or
layout proof. Resolve any post-flash HTTP address to the selected board before contacting it; never
fall back to a production hostname or a cached address belonging to another board.

Both app-only paths require a compatible installed bootloader and partition table.
Verify these on the selected bench before writing, especially for Daikin, unknown firmware or stale
layout evidence. The user's workflow instruction covers the necessary bounded bootloader/table
reads; they must exclude NVS.
Check actual table integrity, chip/bootloader compatibility, app capacity and the exact permitted
app/otadata ranges. An unfamiliar project is not permission to rewrite its bootloader/table, erase
NVS or interpret its stored data as Tesla pairing. If compatibility cannot be established, stop for
an explicit project-conversion/recovery plan; do not silently change the flash procedure.

## 4. Execute the flash procedure on that board

Read and execute the procedure for the selected source — [$flash-esp32](../flash-esp32/SKILL.md)
for a PR preview or development-key image, [$usb-recovery](../usb-recovery/SKILL.md) steps 1 and 3
for a published image — with the resolved `PORT`, detected `TARGET` and selected signed app. Pass
the explicit port through every operation; do not use fallback port detection. Retain the
exact-source, version, chip, size and signature/key checks. CI/preview images must match the
production public-key pin.

The only ordinary writes are the verified signed app at `0x20000`, then activation by erasing
`otadata@0xf000/0x2000` last. Preserve NVS, the existing bootloader and partition table. Recheck
attachment and app hash immediately before writing. A failed write or activation stops the flow;
no automatic retry, alternate board or OTA fallback. Release serial handles before acceptance.

## 5. Accept the implementation on the same bench

### 5a. Targeted verification

Within 60 seconds, require the exact version/platform from the flashed artifact and the same
physical board. Compare pre/post identity and retained configuration fingerprints without exposing
secrets. Then run the planned tests for the changed behavior: success and rejection cases, relevant
HTTP schemas/errors, offline fault injection, display preview or MQTT observations as applicable.
Use bounded tests and only their authorized effects. An unpaired bench cannot prove vehicle pairing,
key reuse, sleep/wake, charge commands or production behavior; record these as **not run**.

### 5b. Health verification

Under the user's `$deploy-bench` authorization for these passive endpoints, set `DEVICE_IP` to the
address verified for this physical bench. Keep it for all requests; no default production address. Observe stable version,
uptime, safe mode/crash state, INTERNAL largest contiguous block and task stack margins. Review
warnings in context, distinguishing expected unpaired bench state from a regression.

```bash
: "${DEVICE_IP:?set the verified address of the selected physical bench}"
curl --connect-timeout 3 --max-time 5 -fsS "http://$DEVICE_IP/status" | jq '{version, paired, heap: .sys.free_heap, min_heap: .sys.min_free_heap, largest_block: .sys.largest_block, uptime: .sys.uptime_s}'
curl --connect-timeout 3 --max-time 5 -fsS "http://$DEVICE_IP/api/proxy/1/version" | jq .
BENCH_DIAG=$(curl --connect-timeout 3 --max-time 5 -fsS "http://$DEVICE_IP/diag?redact=1") || exit 1
printf '%s\n' "$BENCH_DIAG" | grep -iE 'error|warn|fail|panic' || true
```

USB boot acceptance is distinct from OTA probation. Neither successful flashing nor a clean health
snapshot proves the changed behavior; retain the targeted test results as separate evidence.

### 5c. Optional integration verification

Only when separately authorized for this bench, the cluster and vehicle-read scope, use the global
`$tesla-key-e2e-evcc` workflow. Keep all command/toggle modes disabled unless separately requested:

```bash
: "${DEVICE_IP:?retain the verified address of the selected physical bench}"
RUN_COMMANDS=0 ALLOW_CHARGE_TOGGLE=0 RUN_ALL_COMMANDS=0 \
  ESP32_URL="http://$DEVICE_IP" bash scripts/e2e_evcc.sh
```

Report source/artifact identity, selected board evidence, USB result, targeted positive/negative
results, health observations and unexercised boundaries. Keep private identifiers local. Hand the
accepted exact-candidate evidence back to `$deploy` when a merge is waiting; do not merge here.
