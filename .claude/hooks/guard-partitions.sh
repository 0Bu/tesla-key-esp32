#!/usr/bin/env bash
# PreToolUse guard: route any edit of partitions.csv to the user's confirmation prompt.
#
# WHY THIS FILE AND NOT ANY OTHER. A partition table is the ONE artifact in this repo that an OTA
# cannot deliver: the image is written into an OTA slot, the table is not part of it. So an edit
# here splits the fleet — already-deployed devices keep the OLD table forever, only a USB/web-
# installer full flash gets the new one — and two specific mistakes are unrecoverable in the field:
#
#   * MOVING nvs (0x9000) or changing its size. OTA leaves nvs untouched only as long as its
#     offset and size do not move. Shift them and the next OTA silently wipes every device's
#     configuration: WiFi credentials, the VIN, and the tesla_ble private key + session — i.e. the
#     pairing with the car, which then has to be re-enrolled by hand at the vehicle.
#   * MOVING ota_0/ota_1. The INSTALLED table governs where an OTA writes, so a device would flash
#     one address and boot another. ota_0 stays at 0x20000 for exactly this reason.
#
# Growing a data partition into the free 0x12000..0x20000 gap (where `coredump` lives) is the safe
# shape of a change here, because it moves nothing.
#
# NOT a hard block — it asks, so the change is a deliberate one rather than a side effect. Reads
# the tool payload as JSON on stdin (matcher: Edit|Write). Fails open.
set -u

payload="$(cat)"
file="$(printf '%s' "$payload" | jq -r '.tool_input.file_path // ""' 2>/dev/null)"
[ -n "$file" ] || exit 0

case "$file" in
    */partitions.csv|partitions.csv)
        reason="partitions.csv edit. A partition table CANNOT be delivered by OTA, so this only reaches devices that are fully re-flashed. Confirm that: (1) nvs stays at 0x9000 with an unchanged size — moving it wipes every device's WiFi, VIN and tesla_ble private key/pairing on the next OTA; (2) ota_0 stays at 0x20000 and ota_1's offset is unchanged — the INSTALLED table governs OTA writes, so a moved slot flashes one address and boots another; (3) the ci-build-all.sh size gate still matches the slot size."
        printf '{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"ask","permissionDecisionReason":%s}}\n' \
            "$(printf '%s' "$reason" | jq -Rs . 2>/dev/null || printf '"partitions.csv: keep nvs@0x9000 and the OTA slot offsets stable"')"
        ;;
esac

exit 0
