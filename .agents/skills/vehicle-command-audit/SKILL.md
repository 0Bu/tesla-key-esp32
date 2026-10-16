---
name: vehicle-command-audit
description: Read-only three-way conformance audit of tesla-key-esp32 against teslamotors/vehicle-command and the capabilities of the pinned yoziru/tesla-ble. Report upstream/local/feasibility findings; do not edit code/docs/dependencies, publish, or contact a vehicle unless the user separately authorizes implementation.
---

Invoke `$vehicle-command-audit`. Read [AGENTS.md](../../../AGENTS.md) first; this workflow grants no
additional authorization. Canonical skills live under `.agents/skills/`; hooks enforce local policy.

# vehicle-command-audit — upstream conformance audit against teslamotors/vehicle-command

This firmware is a **BLE↔HTTP proxy for a Tesla**, built on **yoziru/tesla-ble** (a C++ port),
which itself mirrors Tesla's official Go SDK **teslamotors/vehicle-command**. `$vehicle-command-audit`
checks the firmware against that authoritative upstream reference and reports drift.

> **Read-only boundary.** An audit request authorizes source retrieval, repository inspection, and a
> report only. Do not edit code/docs/dependencies, create tracking items, commit, push, merge,
> release, flash, OTA, or contact/wake/command a vehicle. Any accepted fix is a separately
> authorized implementation task.

It is the *external-conformance* counterpart to [`$project-review`](../project-review/SKILL.md)
(internal coherence). project-review asks "do our code/docs/config/build agree with **each
other**?"; this skill asks "do they agree with **Tesla's protocol**, and is each proposed fix
**physically possible** with the library we link?"

## The one rule that makes this skill different — the **tesla-ble feasibility gate**

`teslamotors/vehicle-command` is the **truth**, but it is NOT what we run. We run
**yoziru/tesla-ble** (pinned in [`main/idf_component.yml`](../../../main/idf_component.yml) — **read
the pin first**, currently `v6.1.1`). The Go SDK exposes commands, builders, fields and enum
values the pinned C++ port may **not** have. So every comparison is **three-way**, and a
divergence from upstream does **not** automatically mean "change the code":

```
1. UPSTREAM truth  — does teslamotors/vehicle-command say/do X?
2. LOCAL behaviour — does the firmware do X?
3. FEASIBILITY     — CAN tesla-ble <pin> even do X? (builder exists? enum value exists? API supports it?)
```

Decision table:

| upstream says X | we do X | tesla-ble can do X | → correct action |
|---|---|---|---|
| yes | no | **yes** | **recommend a code fix** to match upstream, including every cross-cutting sink |
| yes | no | **no** | **do not recommend local protocol code**; report a doc-note proposal and/or separately authorized library-bump tracking item. A missing builder cannot be called safely. |
| yes | yes | yes | conformant — verify the docs *describe* it correctly |
| no  | yes | — | we do *more* than upstream (e.g. a clamp). Fine if safe + documented; flag only if it hides an error the caller should see |

**Second reality gate — the Charging-Manager role.** This device enrols a **Charging-Manager
key only** (charging + wake). Many commands the firmware *sends* are **rejected by the car** for
that role. "tesla-ble has the builder" and "upstream lists the command" do **not** mean the
command *works here*. A doc that implies a role-denied command works is a real finding even when
the code and the builder are correct.

Recommend code changes only after upstream, local feasibility and role checks agree.
Never hand-edit managed components; propose an authorized pin/patch migration instead.

## Sources — what to read, and how to fetch it

### Upstream truth — `teslamotors/vehicle-command`
Fetch **raw** files (base `https://raw.githubusercontent.com/teslamotors/vehicle-command/main`).
Verify these paths at the reviewed upstream revision:

| Dimension | Upstream file(s) |
|---|---|
| BLE transport (UUIDs, name, framing, MTU, max-conns, ATT write/subscription type) | `pkg/connector/ble/ble.go`, `pkg/protocol/protocol.md` (characteristics, "write with response"), go-ble at the version pinned in `go.mod` (`linux/gatt/client.go`, `linux/att/client.go`) |
| Session / signing / anti-replay / clock | `pkg/protocol/protocol.md`, `internal/authentication/signer.go`, `pkg/protocol/protobuf/signatures.proto` |
| Roles | `pkg/protocol/protobuf/keys.proto` (Role enum), `pkg/protocol/protocol.md` (role *scope* prose) |
| Wake / sleep / body controller | `pkg/vehicle/vcsec.go`, `pkg/vehicle/state.go`, `pkg/vehicle/vehicle.go`, `pkg/protocol/protobuf/vcsec.proto` |
| Commands + params/ranges | `pkg/vehicle/charge.go` (**note: `charge.go`, not `charging.go`**), `pkg/vehicle/climate.go`, `cmd/tesla-control/commands.go` |
| Error / fault taxonomy | `pkg/protocol/protobuf/universal_message.proto` (`MessageFault_E`, `OperationStatus_E`), `pkg/protocol/protobuf/car_server.proto` |

### Feasibility — `yoziru/tesla-ble` at the pinned tag
Fetch raw at the **pin** (base `https://raw.githubusercontent.com/yoziru/tesla-ble/<pin>`; confirm
`<pin>` from `idf_component.yml`). Layout at v6.1.1:
`include/{vehicle.h, client.h, command_error.h, message_builders.h, peer.h, vin_utils.h, errors.h, …}`
and `src/{vehicle.cpp, client.cpp, peer.cpp, message_builders.cpp, message_processor.cpp, crypto_context.cpp, vin_utils.cpp, errors.cpp, …}`.
- **Does a command builder exist?** → `src/message_builders.cpp`. v6.1.1 registers both
  `scheduledChargingAction` and `scheduledDepartureAction`; the firmware deliberately does not
  expose scheduled departure. Builder availability alone is not implementation, role or hardware acceptance.
- **Enum / API values** (roles, form factors) → `include/client.h`; firmware-level `SleepState`, `WakePolicy` → `main/logic/command_runner.hpp`.
- **How a fault becomes a string** the firmware matches → `include/command_error.h`, `main/logic/command_runner.hpp`, `main/vehicle_telemetry.cpp`.
- **VIN→BLE-name / matching** → `include/vin_utils.h`, `src/vin_utils.cpp` (the firmware delegates here; it does **not** build the name itself).

### Local — firmware and owning documents
Code: `main/ble_client.{cpp,hpp}`, `main/vehicle_ctrl.{cpp,hpp}` (+ `vehicle_commands.cpp`,
`vehicle_telemetry.cpp`, `vehicle_pairing.cpp`), `main/http_server.cpp` (+ `http_api.cpp`,
`http_status.cpp`, `http_ota.cpp`, `http_config.cpp`), `main/command_exec.cpp`, `main/mqtt_ha.cpp`,
`main/ota_update.cpp`, `main/Kconfig.projbuild`, `partitions.csv`.
Docs to hold accountable: [`README.md`](../../../README.md) and
[`docs/SECURITY.md`](../../../docs/SECURITY.md).
[`docs/API.md`](../../../docs/API.md) owns the HTTP command catalog.
[`docs/ARCHITECTURE.md`](../../../docs/ARCHITECTURE.md) owns link-state and pairing semantics.
[`docs/MCP.md`](../../../docs/MCP.md) owns MCP tools.
`AGENTS.md` owns only runner policy and safety boundaries.

### Source retrieval

Record the upstream commit and local dependency pin; retrieve source at those revisions.
Use raw source or an authorized GitHub read when an HTML/private endpoint fails. A 403/404 alone
is not evidence that a protocol feature is absent. Role scope is in protocol prose, not enum names.

## Dimensions to compare

Verify every dimension against the reviewed revisions; previous audit results are not evidence.

1. **BLE transport** — service `00000211…`, write `…0212`, notify `…0213`; VIN→name `S%02xC` over
   the **first 8 bytes** of `sha1(VIN)` (⇒ `S` + 16 hex + `C`); 2-byte big-endian length prefix;
   block `min(txMtu,maxBLEMessageSize)-3`; `ErrMaxConnectionsExceeded` keyed off the advert
   **`Connectable`** flag (not the connect error). (Writes use 20-byte
   chunks: this build starts/accepts no MTU exchange, so MTU is 23.
   Check ATT Write Command versus reference Write Request, CCCD 0x0001 notifications versus Linux
   reference indications, no ATT server and disabled SMP against every ADR-0005 departure. The
   `min(MTU-3, 244)` path is compiled but unreachable; do not call it negotiated field behavior.)
2. **Roles / Charging-Manager scope** — charging commands + wake only; everything else
   (lights/horn/sentry/**climate**/locks) is role-rejected.
3. **Session / signing / clock** — `expires_at` = **vehicle's `SessionInfo.ClockTime` + a
   monotonic `steady_clock` delta** (`src/peer.cpp` `generate_expires_at`), per-domain counter +
   16-byte epoch, separate VCSEC/Infotainment sessions. **The device wall clock does NOT enter
   per-command signing/expiry** — a wrong RTC cannot make an already-loaded command stale or
   replayable. It **does** gate persisted-session loading: `load_nvs_sessions_()` (inheriting
   upstream `vehicle.cpp`'s `system_clock - SessionInfo.ClockTime` check, see ADR-0005 §2). Because
   `SessionInfo.ClockTime` counts seconds in vehicle epoch rather than Unix epoch, restoring the
   clock successfully to Unix time rejects stored sessions (> 3600 s), rather than implementing a
   real-time 1-hour reuse window. Restore is attempted before controller init; absent/unreadable
   `last_time` or a failed clock set can leave the inherited negative-age acceptance path. Do not
   claim guaranteed expiry from this restore.
4. **Pairing / whitelist** — add-key carries role + `KEY_FORM_FACTOR_CLOUD_KEY`, no key name (car
   shows "Unknown key"), requires an **NFC card on the console reader**, verify via a SessionInfo
   probe. (Note: the **"3"** is the simultaneous-BLE-**connection** limit; a
   car stores up to **19 keys** — don't call it a 3-key cap.)
5. **Command params / ranges** — amps **0–48** (faithful to real chargers; upstream has no cap),
   charge-limit **50–100**, scheduled-charging `start_minutes` **0–1439** after local midnight.
   REST and MCP reject supplied fractional/out-of-range values rather than silently executing a
   different value.
6. **Wake / sleep** — wake is a **VCSEC-domain** `RKE_ACTION_WAKE_VEHICLE` (not infotainment);
   `vehicleSleepStatus`/`vehicleLockState`/`userPresence` string values match the upstream enums;
   the "trust debounced VCSEC ASLEEP, never VCSEC AWAKE" asymmetry is a correct reading.
7. **Error / fault taxonomy + revocation heuristic** — `OperationStatus`, `MessageFault`
   (`UNKNOWN_KEY_ID=3`, `INACTIVE_KEY=4`, `INSUFFICIENT_PRIVILEGES=7`, `INVALID_KEY_HANDLE=27`),
   `SESSION_INFO_STATUS_KEY_NOT_ON_WHITELIST`. Confirm the firmware's three detectors stay sound:
   the `set_message_callback` fault match (the path that actually fires on a **cached** session),
   the `"whitelist"` substring (session-info handshake only), and the two-strike `"authentication
   failed"` **gated to the health probe** so a role-denied user command can't destroy a pairing.
8. **Library-version claims** — every command the firmware calls resolves to a real builder at the
   pin; distinguish the firmware registry's no-exposure policy (scheduled departure) from builder
   availability in `message_builders.cpp` and from role/hardware acceptance.
9. **evcc / TeslaBleHttpProxy HTTP shape** — `/api/.../command/{name}` names, `vehicle_data` =
   `.response.response.charge_state.*` with **`charge_amps`** (not `charging_amps`), doubled
   `response`, **miles/mph on the `/api` path** (metric is MQTT-only), `charging_state` strings
   `Charging/Disconnected/Complete/Stopped/NoPower/Starting`. Verify selectors, cache freshness,
   energy/preconditioning/odometer presence and intentional proxy departures in
   [API](../../../docs/API.md#vehicle-data) against all emitters and consumers.
9a. **Response-counter anti-replay and patch series** — response-counter anti-replay and Request-UUID routing
    are enforced natively by `main/logic/ble_dispatcher.hpp` and `Peer::validate_response_counter()`, superseding
    patch 0001 and ADR-0003. Verify the current 4 repository patches under `patches/tesla-ble/` (0004 parental
    controls trim, 0005 session counter replay, 0006 PSA crypto-binding port and 0007 request RNG failure) apply lexically/idempotently
    through root CMake to the managed dependency tree, and are rebased explicitly on every pin bump; 0006 must
    keep the V1 vectors in `test/test_tesla_ble_harness.cpp` byte-identical and keep verifying response tags
    against response metadata bound to the counter the response carries (`Signer.Decrypt`), never the
    request counter (v6.1.1 now verifies/passes it upstream; PSA preserves that behavior): every `Peer::decrypt_response` call site passes `AES_GCM_Response_data.counter`.
    Encrypted VCSEC additionally requires an initialized peer and non-empty domain request hash;
    missing context, failed tag or repeated response counter (including zero) drops before decoding,
    sleep/status updates and callbacks. Plaintext passive status is a separate path. Fresh sessions
    need a shared secret, request hashes are domain-isolated, and native retries re-sign rather than
    importing upstream Vehicle's identical-payload retry policy.
9b. **ADRs and cryptographic / protocol boundary claims** — when an ADR or architecture doc makes
    claims about protocol vulnerabilities, replays, or countermeasures:
    - Cryptographically verify the claims against `teslamotors/vehicle-command` Go reference sources
      (`internal/authentication/signer.go`, `internal/dispatcher/dispatcher.go`, `pkg/protocol/protocol.md`).
    - Distinguish AEAD Associated Data binding (which cryptographically binds the request hash to
      authenticated responses, preventing cross-command injection between different requests) from
      link-layer duplicate frame delivery (caused by RX buffer recovery re-emitting the same response)
      and unauthenticated plaintext responses (where `response_counter = 0` requires Request-UUID matching).
    - Do not assert application-layer replay vulnerability where AEAD authentication already guarantees
      message-to-request integrity.
10. **Docs internal coherence vs code** — `/status.link` and MQTT `sleep_status` enum value sets,
    endpoint/CONFIG/partition/version drift across owning documents.

## How to run

1. **Confirm the pin** (`idf_component.yml`) — every feasibility check is against *that* tesla-ble
   version, not "latest". If the pin differs from the listed layout, re-verify dimension 8 first.
2. **For each dimension:** fetch the upstream truth (raw URL), fetch the tesla-ble capability at the
   pin, read the local code + the doc that describes it. Apply the **three-way gate** + the
   **role gate**. Classify each divergence: `technical` (mis-implements the protocol) /
   `logical` (internally wrong) / `documentation` (a doc claim that's false/misleading/drifted) /
   `gap` (something expected is missing/unhandled).
3. **Verify before asserting (refute by default).** Firmware is easy to mis-diagnose and **the Go
   SDK is not the code we run** — confirm a claim against **tesla-ble at the pin**, not just
   vehicle-command. Separate **confirmed** from **suspected**. A "we diverge from upstream" claim is
   only real after gate 3 says the divergence is even fixable here.
4. **Recommend the smallest safe correction.** Prefer a documentation proposal where the code is
   conformant. Recommend code only when all three gates clear, and enumerate every cross-cutting
   sink from `$project-review`. Never edit generated dependency trees during the audit.
5. **Write the report** (structure below). State explicitly that no fixes were applied.

## Report structure

```
# vehicle-command audit — tesla-key-esp32 (<date>)  ·  tesla-ble pin: <pin>

## Summary
<reviewed SHAs/pin, scope, conformance and findings by severity; no presumed favorable verdict.>

## Findings   (priority order)
### [CATEGORY/SEV] <title>
- Upstream truth: <fact> — `<vehicle-command path>` (quote)
- tesla-ble <pin>: <can / cannot do it> — `<tesla-ble path>` (the feasibility gate result)
- Local: `<file:line>` (code) and/or `<doc:line>` (the claim)
- Verdict: technical | logical | documentation | gap · confirmed | suspected
- Proposed fix: <doc edit | code edit only if all gates clear | library-bump tracking item>, including every place that must move together

## Conformant (re-verified, no change)
<the dimensions that match upstream — name them, so "no finding" is an asserted result, not a gap in coverage.>

## Blocked by tesla-ble <pin> (cannot fix in code)
<divergences from upstream that the pinned library can't express — documented as such, optionally a pin-bump item.>

## Prioritized actions
1. <must-fix> … 2. <should-fix> … 3. <nice-to-have> …

## PR gate record
When clean, provide this record for a separately authorized PR-body update:
- [x] `$vehicle-command-audit` clean — merge gate @ <full-40-hex-sha>
```

Order by user impact: a role/protocol mismatch that misleads a user or breaks evcc outranks a
comment nit. Keep findings tight and actionable — someone should be able to fix the project from
the report without re-deriving the upstream context.
