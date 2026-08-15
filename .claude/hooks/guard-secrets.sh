#!/usr/bin/env bash
# PreToolUse guard: stop private key material from being read into the transcript, written to a
# tracked path, or printed by a shell command.
#
# WHAT THIS DEVICE HOLDS. Two secrets, and losing either is not recoverable by re-flashing:
#
#   * `ota_signing_key.pem` — the OFFLINE Secure Boot v2 RSA-3072 key that signs every OTA image.
#     Trust is TOFU from the running app's signature block, so a device on a signed build accepts
#     updates signed with THIS key and no other. If it leaks, anyone can push firmware to every
#     device in the field; if it is committed, it leaks permanently (git history). It is gitignored
#     precisely because CI writes it from a secret at build time — see docs/SECURITY.md.
#   * The vehicle's EC private key, stored unencrypted in the `tesla_ble` NVS namespace. It is the
#     device's sole authenticator to the car.
#
# WHY A HOOK AND NOT A .gitignore ENTRY. .gitignore stops `git add`. It does not stop a key being
# READ into a transcript that is then pasted into an issue, echoed by a debug command, or copied to
# an un-ignored path while chasing an unrelated bug. Those are the realistic leaks, and they all
# look like ordinary work at the moment they happen.
#
# ASKS rather than blocks: signing IS a legitimate operation (scripts/ci-sign-artifacts.sh and the
# explicitly documented local flash path), so the point is to make a key touch deliberate and
# visible, not impossible.
# Fails open. Reads the PreToolUse JSON payload on stdin (matcher: Read|Edit|Write|Bash).
set -u

payload="$(cat)"
tool="$(printf '%s' "$payload" | jq -r '.tool_name // ""' 2>/dev/null)"
file="$(printf '%s' "$payload" | jq -r '.tool_input.file_path // ""' 2>/dev/null)"
cmd="$(printf  '%s' "$payload" | jq -r '.tool_input.command  // ""' 2>/dev/null)"

ask() {
    printf '{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"ask","permissionDecisionReason":%s}}\n' \
        "$(printf '%s' "$1" | jq -Rs . 2>/dev/null || printf '"key material"')"
    exit 0
}

# A path that IS key material, whatever it is called on this machine.
case "$file" in
    *ota_signing_key*|*.pem|*private_key*|*secure_boot_signing_key*)
        ask "This path is private key material (the OTA signing key or a device private key). Reading it puts the key in the transcript; writing it risks committing it. The signing key must stay OFFLINE and out of git — docs/SECURITY.md. Confirm this is a deliberate, necessary key operation."
        ;;
esac

# A shell command that would PRINT key material. Deliberately narrow: `espsecure sign_data` and
# `openssl` reading a key are normal operations and are not matched, because they consume the key
# rather than emitting it. What is matched is the shape that puts bytes on stdout.
if [ "$tool" = "Bash" ] && [ -n "$cmd" ]; then
    if printf '%s' "$cmd" | grep -Eq '(cat|head|tail|less|more|xxd|od|base64|strings)[[:space:]]+[^|;&]*(\.pem|ota_signing_key|private_key)'; then
        ask "This command would print private key material to the transcript (the OTA signing key or a device private key). If you only need to know whether the file EXISTS or its size, use \`ls -l\` or \`test -f\` instead — those answer the question without emitting the key. docs/SECURITY.md."
    fi
fi

exit 0
