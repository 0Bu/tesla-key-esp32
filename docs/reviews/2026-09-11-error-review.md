# Error and coherence review — 2026-09-11

## Scope and result

This review records **15 finding groups: nine P2 and six P3** at source commit
`400cfa72066a437e5a76ec8f8217fe7064faf449`. The most important remaining issues are
premature deletion of the VIN recovery journal, incomplete RSA signature validation,
and a native pre-push hook that checks a different operation from the actual push.
All four firmware targets built successfully; the existing green tests do not cover
every failure path identified below.

The initial comparison was frozen at `d86fd483aef0c4306854c5c981d7f26a5e630658`.
Before preparing this report for publication, GitHub `main` was checked at
`ebf3cdae61d351164c200efd67b7b7a76107da09`. The only intervening change updates the
Renovate workflow action. It does not change the status of F01–F15. The report
branch starts from that newer main commit and adds this document only. An
independent publication recheck on 2026-09-12 found three additional issues,
recorded separately as A01–A03 below; these are not included in the historical
15-group count.

**Read the status column before scheduling fixes.** F04–F13 have corresponding
changes in the comparison tree. Those changes were inspected, but their firmware
was not rebuilt or exercised on hardware as part of this review. The build and
runtime-test evidence in this document belongs to `400cfa7`, not to the report
branch, the comparison commit, or a current remote CI run. Source permalinks below
deliberately point to the inspected historical commit.

| Findings | Status in the publication base |
|---|---|
| F01–F03 | Still present; prioritize remediation |
| F04–F13 | Addressed by subsequent code or documentation changes; fixes not independently runtime-validated here |
| F14 | Still present |
| F15 | Mixed: the wake endpoint, network-watchdog owner, and retired-manifest description remain stale |

P2 means the affected path should be corrected before technical acceptance. P3
identifies a lower-priority workflow or documentation defect. “Confirmed” refers
to the stated source path or local reproduction, not to an observed vehicle incident.

The original review was read-only and produced local test/report artifacts.
Preparing this publication adds documentation only. No firmware, policy, dependency,
partition, or test implementation is changed. No device, vehicle, NVS dump, pairing
key, live session, or production signing key was accessed. There was no flash,
OTA, release, or vehicle command. Local logs are not attached because raw logs can
contain workstation details; the relevant methods and outcomes are recorded here.

The review followed `$project-review`, including all 13 canonical project skills
and four read-only reviewer manifests. Specialist reviews were interrupted by
usage limits; preliminary messages were not treated as completed technical
certification. The final report retains the findings supported by source evidence
and the local checks described below.

## Findings

### F01 — P2 / bug: VIN recovery removes its retry journal after failed cleanup

**Sources:** [boot recovery](https://github.com/0Bu/tesla-key-esp32/blob/400cfa72066a437e5a76ec8f8217fe7064faf449/main/main.cpp#L574),
[correct key-rotation recovery ordering](https://github.com/0Bu/tesla-key-esp32/blob/400cfa72066a437e5a76ec8f8217fe7064faf449/main/vehicle_ctrl.cpp#L121).

The `CompleteNewIdentity` branch removes session, pairing, and MAC entries, then
removes `vin_txn` regardless of the earlier results. It checks the collected
results only afterward. If deleting the previous vehicle MAC fails, journal
deletion can nevertheless succeed. `boot_fatal()` stops that boot, but the next
reset no longer has the marker needed to repeat incomplete cleanup.

**Evidence:** The unchanged cleanup block was executed in a local host harness
whose MAC store fails deletion. The outcome was:

```text
halted=1; stale_mac_remains=1; retry_journal_remains=0
```

No real NVS data was involved. The separate `key_rotate` recovery path already
uses the correct pattern: it removes its marker only when `cleanup_ok` is true.
The demonstrated failure is loss of the VIN retry instruction during incomplete
cleanup; it does not prove loss of the private key.

**Recommendation:** Check every cleanup result first, and remove `vin_txn` only
after all cleanup succeeds. Exercise failure of each individual erase and the
following boot, checking that the journal survives until recovery completes.

**Confidence/status:** Confirmed by an executed failure path. Still present in the
comparison tree at `main/main.cpp:590–600` and unchanged in the publication base.

### F02 — P2 / bug: RSA verification accepts an out-of-range signature integer

**Source:** [artifact validator](https://github.com/0Bu/tesla-key-esp32/blob/400cfa72066a437e5a76ec8f8217fe7064faf449/scripts/check-firmware-artifacts.py#L119).

`verify_rsa_pss_sha256()` checks length and padding, but does not check `s < n`
before evaluating `pow(s, e, n)`. A valid signature integer `s` can therefore be
replaced with `s+n` when that sum still fits in 384 bytes. Modular exponentiation
then produces the same result. RSAVP1 requires the signature representative to be
in the range zero through `n-1` before that operation.
[RFC 8017, section 5.2.2](https://www.rfc-editor.org/rfc/rfc8017.html#section-5.2.2).

**Evidence:** Only the repository's public `TEST_RSA` fixture was used. The
signature bytes were modified and the signature-block CRC recalculated. OpenSSL
accepted the original image signature and rejected the modified signature. The
complete project image parser accepted the modified image, including when given
the original public-key digest as its expected pin:

```text
public_fixture_version=1.2.1
original_strict=accepted
signature_ge_modulus=True
project_parser=accepted
strict_openssl=rejected
project_parser_with_original_public_key_pin=accepted
```

**Impact and limits:** The validator's “RSA-PSS verified” result is incorrect for
this input class. This demonstrates a verification discrepancy, not an ability
to sign arbitrary new firmware. Actual boot or OTA acceptance was not tested.
Independent provenance, digest, and byte-identity checks remain additional controls.

**Recommendation:** Reject a signature integer outside `0 <= s < n` before `pow()`.
Add a negative image fixture with `s >= n` and compare its outcome against a strict
RSA implementation.

**Confidence/status:** Confirmed by local differential verification. The validator
is unchanged in the comparison and publication bases.

### F03 — P2 / inconsistency: the native pre-push hook audits a fabricated push target

**Source:** [native pre-push hook](https://github.com/0Bu/tesla-key-esp32/blob/400cfa72066a437e5a76ec8f8217fe7064faf449/.githooks/pre-push#L15),
especially lines 26–47 and 53–70.

Git supplies the actual remote and source/destination refs to the hook. The hook
instead checks whether the checked-out branch is named `main`, and sends the
policy implementation `git push origin <current-branch>` in every case. A push
from a feature branch to `refs/heads/main`, another remote, or another source ref
is therefore checked as the wrong operation. In addition, `git diff ... || true`
turns a failed diff calculation into an empty changed-file list, which can skip
the firmware-size check.

**Evidence:** The real hook was executed in an isolated fixture using fake Git
and a gate that succeeds for the claimed operation. The supplied remote was
`foreign` and the supplied destination was `refs/heads/main`. The generated audit
command was `git push origin feature/review`; the hook returned zero. No push was
executed. This proves the adapter mismatch, not a bypass of unknown server-side
branch protection.

**Recommendation:** Validate the actual remote and every source/destination ref
and SHA. Reject direct writes to `main` independently of the checked-out branch,
and fail closed when the diff cannot be determined. Do not stop examining later
ref updates after finding the first firmware change. The native hook and runner
gates should enforce the same remote/ref policy.

**Confidence/status:** Confirmed by the isolated adapter reproduction. Still
present; the hook is unchanged in both comparison bases.

### F04 — P2 / bug: key regeneration can bypass confirmation before the first status response

**Sources:** [UI handler](https://github.com/0Bu/tesla-key-esp32/blob/400cfa72066a437e5a76ec8f8217fe7064faf449/main/www/app.js#L708),
[HTTP handler](https://github.com/0Bu/tesla-key-esp32/blob/400cfa72066a437e5a76ec8f8217fe7064faf449/main/http_config.cpp#L113),
[key replacement guard](https://github.com/0Bu/tesla-key-esp32/blob/400cfa72066a437e5a76ec8f8217fe7064faf449/main/vehicle_pairing.cpp#L336).

`genKey()` requests confirmation only if `state && state.key_present` is true.
Before the first successful status response, `state` is null, but the request
still always uses `/gen_keys?force=1`. This disables the server's existing-key
replacement protection. A click during that window can invalidate an existing
pairing.

**Evidence:** The actual `app.js` was executed with the existing offline VM
harness. With `state=null`, it showed zero confirmation dialogs and made one
POST to `/gen_keys?force=1`. With a known existing key and a declined dialog, it
made no request. No real key operation was performed.

**Recommendation:** Treat unknown status as unknown, require confirmation for
replacement, and send `force=1` only after explicit replacement confirmation.

**Confidence/status:** Confirmed locally. Addressed by subsequent UI changes;
hardware behavior was not retested here.

### F05 — P2 / bug: a long query string silently disables `redact=1`

**Sources:** [query helper](https://github.com/0Bu/tesla-key-esp32/blob/400cfa72066a437e5a76ec8f8217fe7064faf449/main/http_common.cpp#L83),
[status and diagnostic handlers](https://github.com/0Bu/tesla-key-esp32/blob/400cfa72066a437e5a76ec8f8217fe7064faf449/main/http_status.cpp#L245),
[URI limit](https://github.com/0Bu/tesla-key-esp32/blob/400cfa72066a437e5a76ec8f8217fe7064faf449/sdkconfig.defaults#L80).

`query_param_is()` uses a 96-byte buffer and returns false for every query-read
error. HTTP configuration allows longer URIs. An otherwise accepted URI whose
query string is at least 96 bytes can therefore be treated as a request without
redaction. `/status` and `/diag` can return HTTP 200 with unredacted data instead
of rejecting an unreadable redaction request. Putting `redact=1` first does not
help, because reading the whole query fails before parameter lookup.

**Recommendation:** Validate query length and parsing errors before action or
response generation. Use one explicit shared buffer contract and reject unreadable
queries with HTTP 400. Cover inputs below, at, and above the buffer boundary.

**Confidence/status:** Confirmed by the helper's error path and both consumers;
no live HTTP request was made. A shared buffer and early validation were added
in the comparison tree, addressing this finding.

### F06 — P2 / bug: enabled redaction still exposes vehicle identifiers

**Sources:** [VIN recovery log](https://github.com/0Bu/tesla-key-esp32/blob/400cfa72066a437e5a76ec8f8217fe7064faf449/main/main.cpp#L550),
[log redaction rules](https://github.com/0Bu/tesla-key-esp32/blob/400cfa72066a437e5a76ec8f8217fe7064faf449/main/logic/redact.hpp#L96),
[BLE scan status emitter](https://github.com/0Bu/tesla-key-esp32/blob/400cfa72066a437e5a76ec8f8217fe7064faf449/main/logic/status_model.hpp#L326).

Two independent sources are missing from the redaction contract. The interrupted
VIN-transition message contains the previous VIN but has no corresponding log
rule. The scan list emits `ble.devices[].name` unchanged while redacting its
adjacent MAC address. The Tesla advertisement name contains a stable identifier
derived from the VIN.

**Evidence:** The actual redaction function retained a synthetic VIN in the
recovery message. The actual status emitter, with `redact=true`, retained the
synthetic advertisement name `S0123456789abcdefC`. Only synthetic identities
were used.

**Recommendation:** Add the missing log rule and scan-name redaction. Update the
field inventory, golden tests, and API documentation together.

**Confidence/status:** Confirmed locally. Both data sources and the field
inventory were corrected in the comparison tree.

### F07 — P2 / bug: delayed telemetry can repopulate a cleared pairing cache

**Sources:** [telemetry processing](https://github.com/0Bu/tesla-key-esp32/blob/400cfa72066a437e5a76ec8f8217fe7064faf449/main/vehicle_telemetry.cpp#L402),
[pairing cache cleanup](https://github.com/0Bu/tesla-key-esp32/blob/400cfa72066a437e5a76ec8f8217fe7064faf449/main/vehicle_pairing.cpp#L569),
[charge snapshot reader](https://github.com/0Bu/tesla-key-esp32/blob/400cfa72066a437e5a76ec8f8217fe7064faf449/main/vehicle_telemetry.cpp#L985).

Processing removes telemetry from the mailbox and parses it outside the locks.
Concurrently, key/pairing cleanup can clear all public caches. The already-running
processing step can then publish its old snapshot. Cleanup neither clears pending
telemetry nor invalidates in-flight snapshots using an identity generation, so
old data can appear fresh again.

The charge writer also publishes its cache and timestamp under different locking
conditions, although the reader assumes they belong to one locked snapshot. This
can cause transient false HTTP 503 responses.

**Recommendation:** Capture an identity generation when accepting data, increment
it and discard pending data during cleanup, and recheck it before publication.
Publish the cache, timestamp, and generation together.

**Confidence/status:** Confirmed as a permitted source-level interleaving; no
hardware race was measured. Corresponding epoch and locking changes are present
in the comparison tree.

### F08 — P2 / bug: configuration restarts are not reserved against OTA or identity work

**Sources:** [configuration restart callers](https://github.com/0Bu/tesla-key-esp32/blob/400cfa72066a437e5a76ec8f8217fe7064faf449/main/http_config.cpp#L546),
also lines 586 and 664; [image confirmation](https://github.com/0Bu/tesla-key-esp32/blob/400cfa72066a437e5a76ec8f8217fe7064faf449/main/ota_update.cpp#L126).
The same restart pattern appears in `main/provisioning.cpp`.

After saving configuration, callers invoke `ota_confirm_pending_image()`, wait
800 ms, and restart unconditionally. Its local `HealthCommit` guard protects
only confirmation and is released when the function returns. If an OTA or identity
operation is already active, confirmation correctly refuses to proceed, but the
caller still restarts. The subsequent delay also has no restart reservation
against background work.

**Impact:** A parallel operation that normally has mutual exclusion can still
be interrupted by a configuration restart. The mark-valid guard does not protect
the complete shutdown window.

**Recommendation:** Reserve restart exclusively, check whether reservation
succeeds, and accurately report whether configuration was stored or applied.

**Confidence/status:** Confirmed by the callers and guard lifetime. `ConfigRestart`
and checked return values are present in the comparison tree.

### F09 — P2 / bug: safe mode still enters risky controller initialization

**Source:** [boot sequence](https://github.com/0Bu/tesla-key-esp32/blob/400cfa72066a437e5a76ec8f8217fe7064faf449/main/main.cpp#L375),
especially lines 495–517, 601–650, and 777.

Safe mode is detected early, but Tesla NVS initialization, `VehicleController::init()`,
crypto/key loading, and identity recovery still run before networking and HTTP.
Only NimBLE startup, vehicle tasks, and MQTT are skipped later. A failure while
constructing the controller or crypto state can therefore recur in safe mode
before browser recovery becomes available. `start_tasks=false` does not bypass
this initialization.

**Recommendation:** Use an inert safe-mode controller with the safe read access
needed for diagnosis, and skip the risky initialization path.

**Confidence/status:** Confirmed by boot ordering; no crash was induced. A separate
`init_safe_mode()` path is present in the comparison tree.

### F10 — P3 / bug: baseline-update mode cannot reach its update after budget growth

**Sources:** [size-check wrapper](https://github.com/0Bu/tesla-key-esp32/blob/400cfa72066a437e5a76ec8f8217fe7064faf449/scripts/check-firmware-size.sh#L93),
also line 124; [builder budget enforcement](https://github.com/0Bu/tesla-key-esp32/blob/400cfa72066a437e5a76ec8f8217fe7064faf449/scripts/ci-build-all.sh#L234).

Even `--update-baseline` first invokes the builder with the old baseline enforced
through `--enforce-budget`. When growth exceeds that baseline, `set -e` terminates
the outer script before its update block. The recommended repair command thus
fails in the main case it is meant to handle.

**Recommendation:** Separate the hard partition/signed-image limit and measurement
from an explicitly reviewed baseline update. Keep hard safety limits enforced.

**Confidence/status:** Confirmed by control flow; no baseline was changed.
The comparison tree passes `--no-enforce-budget` specifically for baseline updates.

### F11 — P3 / documentation drift: consumer contracts omit `usable_soc`

**Sources:** [HTTP and MQTT documentation](https://github.com/0Bu/tesla-key-esp32/blob/400cfa72066a437e5a76ec8f8217fe7064faf449/docs/README.md#L318),
also lines 328 and 477; `docs/ARCHITECTURE.md:590`; `docs/MCP.md:102`.
Implementations include `main/logic/status_model.hpp:352`,
`main/mqtt_payloads.hpp:102`, and `main/mcp_json_payloads.hpp:203`.

Status, MQTT, and MCP expose `usable_soc`, but their documented field lists were
not fully updated. MQTT also does not document why the field is in the payload
without receiving one of the 55 Home Assistant discovery entities.

**Recommendation:** Document the field in all three contracts and state whether
its MQTT exposure is intentionally payload-only or should have its own entity.

**Confidence/status:** Confirmed by field-list comparison. The comparison tree
adds the field descriptions and the payload-only decision.

### F12 — P3 / skill drift: `$ship` recommends a merge command its own gate rejects

**Sources:** [ship instructions](https://github.com/0Bu/tesla-key-esp32/blob/400cfa72066a437e5a76ec8f8217fe7064faf449/.agents/skills/ship/SKILL.md#L52),
`tools/agent-hooks/merge_payload.py:176` and `:195`.

The skill combines resolution and merge in a compound shell block using `"$PR"`
and `"$PR_HEAD"`. The gate requires the actual tool call to contain a literal
numeric PR and a static full SHA. The real parser rejected both the whole block
and the isolated variable-based merge line. A control using literal arguments
was accepted. No GitHub command was executed for this probe.

**Recommendation:** Document the read, one separate canonical merge call with
resolved literal values, and the result read as distinct invocations.

**Confidence/status:** Confirmed by the parser probe. The skill was rewritten
accordingly in the comparison tree.

### F13 — P3 / skill drift: `$add-logic-test` omits mandatory ownership registration

**Sources:** [logic-test skill](https://github.com/0Bu/tesla-key-esp32/blob/400cfa72066a437e5a76ec8f8217fe7064faf449/.agents/skills/add-logic-test/SKILL.md#L90),
`test/README.md:268`, and `scripts/check-logic-test-ownership.py:223`.

The recipe adds a header, test definition, and test call, but skips
`test/logic_test_ownership.json`. Following the recipe exactly therefore creates
a header that fails the host gate recommended by the same skill. An in-memory
inventory probe produced:

```text
pure-logic headers missing a test owner: review_probe.hpp
```

**Recommendation:** Require registration of the test file and a concrete evidence
token for every new pure-logic header.

**Confidence/status:** Confirmed by the validator probe. The comparison tree adds
the missing step 3b.

### F14 — P3 / skill drift: sibling checklists omit `$mock-test`

**Sources:** [project-review checklist](https://github.com/0Bu/tesla-key-esp32/blob/400cfa72066a437e5a76ec8f8217fe7064faf449/.agents/skills/project-review/SKILL.md#L466),
[skill-audit checklist](https://github.com/0Bu/tesla-key-esp32/blob/400cfa72066a437e5a76ec8f8217fe7064faf449/.agents/skills/skill-audit/SKILL.md#L70).

Both checklists require inclusion of every existing skill, but neither includes
the existing `mock-test` skill with its specific checks. The general discovery
instruction is still correct, so this is incomplete prescribed coverage rather
than total loss of inventory discovery.

**Recommendation:** Add matching checks for `run-fast-tests.sh`, flags, the full
host suite, and the Linux sanitizer boundary to both sibling lists.

**Confidence/status:** Confirmed against the 13 skill files. Still present in the
comparison and publication bases.

### F15 — P3 / documentation and skill drift: stale endpoints, owners, and symbols

The following named references contradict their implementation or current policy:

| Location at the inspected commit | Stale claim | Actual contract |
|---|---|---|
| `.agents/rules/ble-tesla-protocol.md:10` | `POST /wake` | `/api/1/vehicles/<VIN>/command/wake_up`; no `/wake` route is registered |
| `.agents/skills/device-diag/SKILL.md:229` | WiFi reconnect and gateway watchdog belong to `main.cpp` | Implementation belongs to `main/net.cpp` |
| `test/README.md:43` | Active “compatibility manifest/fingerprint” checks | The configuration checker rejects the retired manifest |
| `docs/ARCHITECTURE.md:517` and `.agents/skills/ota-release-verify/SKILL.md:112` | `ver_newer()`; the skill's downgrade line reference also points elsewhere | `tk::compare_ota_versions()` and the actual downgrade gate |
| `docs/ARCHITECTURE.md:1012` | `kWdFailToReassoc` | `tk::kWatchFailsToRecover` |
| `main/logic/redact.hpp:122`, `:124`, `:142`, `:329` | Log phrases attributed to their old files | Current owners are `vehicle_telemetry.cpp` or `net.cpp` |

**Recommendation:** Name the actual endpoints, owners, and symbols. Describe
retired metadata checks as absence checks. Prefer function names over fragile
line-number references where appropriate.

**Confidence/status:** Confirmed by symbol and call-site comparison. The first
three rows remain stale in the publication base. Several OTA, watchdog, and log
redaction references were corrected subsequently. These stale references alone
do not establish an additional runtime defect.

## Cross-project coherence coverage

| Contract | Result at the inspected commit |
|---|---|
| Four targets, ESP-IDF v5.5.5, tesla-ble v5.1.3, five ordered patches | Consistent; exercised through the pinned build |
| Partitions, NVS geometry, app and bootloader identity | All four binary artifacts checked; no layout drift found |
| Build → independent rebuild → protected publication | Workflow dependency chain is present; matrix splitting alone is not a defect |
| Release, Pages, signature, and provenance contracts | Existing contract tests passed; F02 remains a validator input gap |
| 19 NVS records and caller inventory | Gate passed; F01 is a separate transaction-ordering defect |
| 21 fixed and three vehicle routes, 15 REST commands, nine MCP tools | Inventories agree; F15 identifies the false wake-path reference |
| 55 MQTT discovery entities and presence rules | Production payload tests passed; F11 identifies missing documentation |
| HTTP/MCP body bounds and cJSON OOM/ownership | Host and sanitizer checks passed; F05/F06 remain separate query/redaction gaps |
| Pairing/cache transitions, restart admission, safe mode | Findings F07–F09 |
| Heap watchdog, active windows, link state | Constants and containment inspected; no additional supported finding reported |
| Web UI and display/BLE presenters | Existing checks passed; F04 was independently reproduced as an omitted UI boundary case |
| Runner and PR policy | Mechanical gates passed; the native push adapter differs in F03 |

All canonical skills were inspected individually against their local owners:

| Skill | Local contract review result |
|---|---|
| `add-logic-test` | F13 |
| `device-diag` | F15; other status/live-access boundaries consistent |
| `display-preview` | CLI, parity, and hardware boundaries consistent |
| `feature-docs` | Document ownership and conditional gate paths consistent |
| `flash-esp32` | Target, provenance, and NVS boundaries consistent; shared validator issue F02 applies |
| `mock-test` | Runners and flags consistent |
| `ota-release-verify` | Symbol/line drift in F15; local release/16-part contract otherwise consistent |
| `pr-hygiene` | Independent privacy/language axis and SHA-bound records consistent |
| `project-review` | F14; document contracts checked against this report's findings |
| `ship` | F12; other local artifact and observation contracts consistent |
| `skill-audit` | F14 |
| `usb-recovery` | Artifact, app, otadata, and NVS boundaries consistent; F02 applies |
| `vehicle-command-audit` | Local pin, roles, boundaries, and three-way evidence requirement consistent |

All four reviewer manifests (`agent_config_reviewer`, `doc_drift_checker`,
`heap_safety_reviewer`, `multi_target_build_reviewer`) have appropriate read-only
boundaries and local review scopes. Global NVS and evcc E2E skills were neither
copied into the repository nor executed. External upstream APIs were not re-audited
as a separate complete conformance exercise.

## Verification and its limits

The following evidence is from source commit
`400cfa72066a437e5a76ec8f8217fe7064faf449`:

| Check | Observed result |
|---|---|
| Host suite, compiler fallback | 4,707 logic, 961 NVS-adapter, and 637 runtime-boundary checks passed |
| Additional host gates | Display parity, 960 BLE-row vectors, 53 Node tests, 20,000 deterministic fuzz cases, and build/NVS/ownership/protocol contracts passed |
| Offline browser | Real DOM, console, keyboard, live-region, desktop, and mobile checks passed |
| `repo-lint.sh` | Passed, including 10 YAML files, six workflows, 30 documents/222 local link targets, and 58 logic headers |
| `test-pr-gates.sh` | 106 hook self-tests passed |
| `tools/agent-config/selftest.sh` | 52 mutation canaries detected |
| Linux sanitizers in pinned IDF container | ASan/UBSan/LSan tripwires, logic, NVS, runtime boundaries, and 20,000 fuzz cases passed |
| Production cJSON and MQTT test paths | 120,252 OOM/ownership and 13,122 MQTT-payload checks passed in sanitizer mode |
| Canonical four-target build | esp32, esp32s3, esp32c3, esp32c6: build, effective compilation, partition, image-size, and stack gates passed |
| Disposable-key signing and Pages contract | Four targets, real test RSA signatures, negative tampering checks, and manifest contract passed; no production key used |
| Local reproducibility | Unsigned app and ELF bytes matched a fresh rebuild for every target |
| Additional defect reproductions | F01, F02, F03, F04, F06 reproduced locally; F12/F13 checked with independent parser/validator probes |
| Diff and worktree checks | No source edits or whitespace errors in the reviewed checkout |

The first macOS `--require-all` invocation stopped before tests because host CMake
was unavailable. The supported compiler fallback reached the browser gate, which
initially failed inside the sandbox. That offline browser gate was then run
successfully outside the sandbox. There was no single successful macOS
`--require-all` invocation. CMake and sanitizer execution are separately supported
by the Linux-container run.

The canonical build command was:

```bash
scripts/idf-docker.sh ./scripts/ci-build-verify.sh 1.4.0 400cfa72066a437e5a76ec8f8217fe7064faf449
```

It used the pinned image
`espressif/idf:v5.5.5@sha256:a9231d0697ab8f7517cc072e93b7c83e04907bfbfba80b6440d7dbbf90665cf2`
and retained the wrapper's 1.5 CPU / 1800 MiB limits.

| Target | Unsigned app bytes | Projected signed bytes |
|---|---:|---:|
| esp32 | 1,638,400 | 1,642,496 |
| esp32s3 | 1,703,936 | 1,708,032 |
| esp32c3 | 1,769,472 | 1,773,568 |
| esp32c6 | 1,966,080 | 1,970,176 |

Coverage is one primary build plus one fresh reproducibility build per target at
the inspected commit. C6 is 28,672 bytes below the 1,998,848-byte policy limit
using projected signed size. This measures image size, not available runtime heap.

**Not run:** remote CI as part of the original review, production signing or
publication, hardware/USB/flash, OTA, live vehicle or evcc API, real pairing,
sleep/wake or hardware fault injection, and physical display inspection. Green
local checks are not evidence for those boundaries.

## Recommended follow-up

1. Fix F01–F03 first. Preserve the journal across every cleanup failure, align the
   RSA parser with strict verification, and bind push checks to the actual operation.
2. Choose the implementation base deliberately. F04–F13 already have corresponding
   changes in the later tree. Inspect and validate those changes rather than
   implementing duplicate fixes based on historical source links.
3. Correct F14 and the remaining F15 references together, keeping sibling skill
   inventories and named source owners consistent.
4. Add regression coverage at the integration boundaries that the existing green
   suite does not distinguish. Avoid tests that merely restate the implementation.

This document completes the review record. It does not grant technical acceptance
of the affected firmware paths while P2 findings remain open, and it does not
claim clean project-review or skill-audit gate readiness.

## Publication recheck — 2026-09-12

This independent follow-up inspected publication base
`ebf3cdae61d351164c200efd67b7b7a76107da09` and the new report. Its results are
separate from the historical firmware validation above. In particular, a passed
lint at `400cfa7` must not be presented as a passed lint at the newer base.

### A01 — P3 / skill regression: `$ship` no longer assigns `MERGE_SHA`

The original F12 merge-parser syntax problem was corrected, but the rewrite
removed the assignment used by the next step. The new step only prints
`.mergeCommit.oid`; the following block enables `set -u` and expands `MERGE_SHA`
without assigning it anywhere in the skill.

**Source:** [ship steps 1 and 2](https://github.com/0Bu/tesla-key-esp32/blob/ebf3cdae61d351164c200efd67b7b7a76107da09/.agents/skills/ship/SKILL.md#L67).

**Evidence:** The actual step-2 block was extracted and run with `MERGE_SHA`
absent and a stub `gh` function that would report any execution. Bash returned
one with `MERGE_SHA: unbound variable`; the stub was never invoked. No GitHub
operation occurred.

**Recommendation:** Explicitly capture and validate the confirmed merge SHA
before using it for run selection, while retaining the separate literal-bound
merge command. Correcting F12's parser syntax does not establish that the whole
ship workflow now executes successfully.

### A02 — P2 / policy drift: the Renovate gate exception is absent from canonical guidance

The implementation exempts changes limited to the two Renovate maintenance
paths from all `merge`/aggregate `check` records. Canonical guidance and
`$pr-hygiene` still describe unconditional checks at every merge.

**Sources:** [implemented exception](https://github.com/0Bu/tesla-key-esp32/blob/ebf3cdae61d351164c200efd67b7b7a76107da09/tools/agent-hooks/require-pr-gates.sh#L341),
[canonical merge policy](https://github.com/0Bu/tesla-key-esp32/blob/ebf3cdae61d351164c200efd67b7b7a76107da09/AGENTS.md#L182).

**Impact:** A reviewer following the declared policy and a runner evaluating the
implemented gate can reach different conclusions about required evidence.

**Recommendation:** Resolve the intended policy explicitly and align the owning
documentation, skills, and gate tests. Do not silently widen or remove the
exception as part of a report-only change.

This exception does not apply to report-only changes or to PR creation. A draft
PR still requires current clean skill-audit and PR-hygiene records under the
present creation gate.

### A03 — P2 / validation regression: the Renovate update breaks workflow self-tests

The Renovate workflow now pins action version v46.3.0 at a new full commit SHA.
The validator's production path deliberately accepts a correctly pinned action
from the expected owner, but its mutation fixtures still search for the previous
literal SHA. `replace_once()` fails before those negative canaries can execute.

**Sources:** [updated action](https://github.com/0Bu/tesla-key-esp32/blob/ebf3cdae61d351164c200efd67b7b7a76107da09/.github/workflows/renovate.yaml#L19),
[stale mutation anchors](https://github.com/0Bu/tesla-key-esp32/blob/ebf3cdae61d351164c200efd67b7b7a76107da09/scripts/check-workflow-policy.py#L1132),
with another anchor at line 1530.

**Evidence:** `scripts/repo-lint.sh` on the publication base stops at the mandatory
`check-workflow-policy.py --self-test` invocation with:

```text
workflow-policy: self-test fixture text is absent from renovate.yaml: renovatebot/github-action@37beffda261423addd537c33f2d126df7f6ffbab
```

Only the Renovate workflow changed between the comparison and publication bases;
the new report cannot affect this fixture lookup. This is a repository-wide
validation blocker, not evidence of an unsupported action pin or a firmware fault.

**Recommendation:** Update or derive the fixture anchors while preserving the
negative tests for an unpinned action and the wrong action owner. Rerun the
workflow self-tests and repository lint afterward.

### Documentation and publication readiness

The standalone Markdown checker passed with 33 documents and 231 local targets.
The report's historical source permalinks were resolved against the local Git
objects, and the basic whitespace/private-data scan passed. Independent content
review is kept distinct from a clean firmware or skill-audit certification.

The complete repository lint did **not** pass at the publication base because of
A03. Existing F14/F15 skill drift and the additional A01/A02 contradictions prevent
a clean skill-audit declaration. These blockers are reported honestly; this
document does not fabricate checked review records or authorize bypassing them.
