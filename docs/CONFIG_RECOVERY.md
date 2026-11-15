# Configuration-only USB recovery

The default-off maintenance role repairs one recognized corrupt `tesla_cfg/cfg` blob over the
physical USB console. It leaves `tesla_ble` and other logical NVS records intact and starts no
normal services. App-only firmware replacement alone cannot repair corrupt settings.

**Physical use requires a qualified target, exact signed artifact and explicit cfg-write scope.**
[Qualification](#qualification-and-physical-use-boundaries)

## Select the correct recovery class

| Failure | Key-preserving route |
|---|---|
| Pre-HTTP clock, driver, memory, network or startup failure | Retain serial evidence and diagnose the failing component. Use a compatible signed app-only install for a firmware defect. |
| Recognized `TKC1` v1 corrupt CRC/body or truncated blob with its five-byte header | Consider cfg-only repair if existing NVS/key and independent board/key/VIN evidence pass and both identity journals are absent. Foreign, headerless and over-bound data refuse. |
| Intact newer or unknown schema | Use a verified signed reader for that schema. An old reader cannot classify an unknown version as corruption. |
| NVS init/open/type/read failure or missing/unparseable private key | USB diagnosis. Damaged secret storage needs a separate backup/recovery decision. Preserving NVS cannot recreate lost bytes. |
| VIN or key journal, including malformed/empty entries, or ambiguous identity initialization | Preserve evidence and diagnose the specific transaction. Do not infer VIN from mirrors, damaged data or a short fingerprint. |
| Fatal task/service failure after HTTP startup | Keep available recovery dashboard/diagnostics. Use serial evidence if HTTP is unavailable. |

The [startup inventory](ARCHITECTURE.md#startup-failure-policy-normative) owns fatal categories.
Use the [app-only USB procedure](DEVELOPMENT.md#delivery) when firmware repair is appropriate.

## Narrow maintenance transaction

The operator must possess independently known same-device board identity, the complete P-256 public
point and the existing VIN. A freshly returned public point or four-byte fingerprint is not by
itself independent authority. The explicit known-VIN assertion is an owner statement, not something
recoverable from a corrupt blob; if it is unavailable, stop. All credentials and private request/
console evidence stay outside Git in owner-only files and are never pasted into a public issue.

The maintenance image must be explicitly selected, signed by the existing approved authority,
provenance-bound and qualified for the exact board/target. Its early branch runs before normal
configuration decode, BootGuard/reboot-reason consumption, Vehicle construction and any background
writer. Only raw physical USB console access is allowed. Pending/NEW/unknown running-image state,
occupied owner, NVS failure, missing/invalid key and any present/unreadable identity journal refuse.
The routine never marks an image valid and never changes otadata, bootloader or partitions.

A fixed INFO frame gives the physical console the board ID, full public point and one-boot nonce.
An offline encoder compares them to the independently supplied owner record; it does not turn
INFO into a new trust anchor. The sole APPLY frame supplies that identity, the challenge, explicit
same VIN and a complete canonical replacement blob. New WiFi credentials (or intentionally empty
WiFi), MQTT, Syslog and OTA channel are explicit. Trial backups/flags are cleared as a disclosed
configuration replacement; corrupted fields are not described as recovered.

The device rechecks identity/journals/raw image under its actual ConfigRestart owner, reads only a
recognized corrupt v1 blob, and fingerprints every other logical NVS entry internally, including
unknown namespaces/keys and stale legacy mirrors. Unsupported types, more than 96 retained entries,
values above 4096 bytes or any read/hash error refuse. No raw retained value or retention digest is
sent to the host. The owner is sealed before the one cfg set/commit attempt. A matching complete
readback and unchanged non-cfg logical fingerprint are required for Confirmed. Physical NVS pages
may move during garbage collection; raw partition byte equality is not the retention contract.

All results park, including Confirmed. False/throwing commit or failed/mismatching readback/retention
is Ambiguous, never success or a reason to resend. A lost terminal reply does not authorize replay.
No normal startup, auto-restart, identity generation or second APPLY is possible in that entry.
Only separately authorized, provenance-verified app-only restoration of the normal image ends
maintenance. An Ambiguous outcome requires a new diagnosis and explicit decision before any new
maintenance attempt; rebooting into the same utility is not an automatic retry policy.

## Wire and private offline preparation

INFO is exactly `TKCR1`, board bytes (6), full uncompressed P-256 public bytes (65), nonce (32).
The full point is public cryptographic material, but still a private device identifier for reporting.
APPLY is `TKAP1`, uint16 little-endian full length, owner-confirmation byte `1`, board (6), public
point (65), nonce (32), VIN bytes (17), uint16 blob length, canonical existing TKC1/v1 blob and CRC32
of the complete preceding frame. Maximum APPLY is 646 bytes. No trailing/duplicate/alternate frame
shape is accepted. CRC detects transfer corruption and is not authentication; physical access and
explicit independent owner authority are the boundary. Request input has one absolute 30-second
deadline; writes have a 5-second deadline, to be enforced by the actual console driver.

`maintenance/config-repair/encode_request.py` only prepares an owner-only request file offline.
It opens no serial port, performs no flash/NVS/OTA operation and does not send the frame. Its exact
owner JSON has `schema:1`, `ownerConfirmedKnownIdentity:true`, `expectedBoardMac`,
`knownPublicKeyHex` (130 hex characters), `knownVin`, and `config` with precisely `wifi_ssid`,
`wifi_pass`, `vin`, `mqtt_uri`, `syslog_uri`, `ota_channel` (0 release, 1 dev). Supply actual private
values only in protected files. The input must be an owned regular file with no group/world
permissions; output is created exclusively with mode 0600. Never pass credentials on the command
line. Firmware independently validates the complete replacement; host encoding is not authority.

The host attempts a bounded-schema `result.json` independently of serial close. `closeOutcome`
records only CLOSED/FAILED; no exception text, endpoint or identity is retained there. Partial,
throwing or missing-receipt APPLY remains uncertain, while an established terminal verdict is kept
if close later fails. Close, terminal persistence or diagnostic failure makes the host fail without
replay. A failed filesystem write cannot guarantee durable evidence; preserve all earlier files.
Serial close and filesystem synchronization still need an outer physical-operation deadline; this
source does not claim an aggregate OS-I/O cleanup bound.

## Default-off build and artifact boundary

`TESLA_CONFIG_REPAIR_USB` defaults off. The two maintenance translation units enter the actual
component only through one closed enabled-only `target_sources` block; they also retain their
preprocessor guard. The normal literal source graph, stack baseline, project and default SDK
profiles remain unchanged. No empty sidecar, optional owner, dummy frame or numeric cap is added.
The normal `app_main` branch retains its startup body. Source structure is not proof of identical
normal machine code, BSS or stack use; compare all four normal targets after integration.

The only enabled role is `TESLA_BUILD_ROLE=config-repair`, project `tesla-key-config-repair`, version
`cfgrepair-<16 source-fingerprint hex characters>`. Role, effective config and actual CMake project
must agree. The effective compiler, dependency, include, Ninja, link and ELF-to-app gates inspect
that real project name. They do not rename maintenance files to impersonate normal output. Pins,
five patches, partitions, compiler options, 1.5 CPU / 1800 MiB wrapper limits and resource caps are
unchanged.

After integration and independent source review, this is the unsigned qualification command:

```bash
scripts/idf-docker.sh python3 scripts/build-config-repair.py --source-sha '<full-current-source-head>'
```

The driver takes no single-target or skip-budget option. It creates fresh exact-source primary and
reproduction copies for ESP32, S3, C3 and C6, compares both unsigned app and ELF bytes, retains each
primary's config/map/size/stack data, and applies the unchanged ordinary size caps. Enabled stack
admission uses `check-config-repair-stack.py` and its separate role-tagged owner
`maintenance/config-repair/firmware-stack-baseline.json`. It requires the exact actual enabled TU
set, nonempty observed sidecars and the canonical frame threshold and absolute maximum.
The reviewed role baseline must match the current enabled sources and measured large-frame set.
Source or frame-set changes require new enabled observations and independent review; an older
reviewed baseline is not permission to accept a changed inventory. No maintenance frame may enter
the ordinary baseline. Synthetic test frames are not shipping maxima. Do not turn refusal into
acceptance, add an optional owner or waive normal caps.
The script has per-child timeouts; it does not itself claim bounded cleanup of a Docker daemon or
all compiler descendants. Execute it only under the separately reviewed owned-container parent.

All outputs are under the separate ignored `maintenance-dist/`, with exactly four app-only entries
in `config-repair-manifest.json`. Its kind, role, real project, non-SemVer version, complete source
fingerprint and exact app/diagnostic hashes differ from the ordinary distribution. It has no normal
16-part manifest, merged image, bootloader/partition write plan or automatic signing/publication path.
The existing ordinary image validator still requires project `tesla-key-esp32`; ordinary channel
version admission rejects `cfgrepair-*`. Neither boundary is weakened to accept this variant.
The maintenance validator separately checks real chip/project/version, image structure, projected
signed size and, when explicitly validating a signed app, the existing public RSA pin.

Protected maintenance signing needs a separately reviewed publication role and independent exact-source
rebuild, with the existing authority and key-removal controls. This unsigned builder cannot provide
that authority or produce a flashable result. Normal Pages, Dev/Release and preview pipelines remain
ordinary-application-only; never insert these files into their 16 parts or reuse their manifests.

## Raw physical console ownership

Actual retained four-target configurations use UART0 at 115200 for ESP32 and native USB-Serial-JTAG
for S3/C3/C6. The enabled overlay selects SDK console NONE and compile-time maximum log level 0,
then the early maintenance branch alone installs its raw driver. Native targets explicitly keep
USB-Serial-JTAG enabled. An already installed driver is refused rather than stolen from another
owner. No normal diag logger, BootGuard, clock bootstrap, network, Vehicle or application writer
starts before this branch, and the branch always parks.

For classic ESP32, preserve the admitted normal bootloader's actual UART0 GPIO route; no board pin
is guessed or rerouted. The private owner configures 115200 baud, 8N1, a 1024-byte RX ring and no
TX ring. It uses zero-tick RX reads and deadline-checked bounded FIFO TX/idle polling, avoiding `uart_tx_chars`
and ordinary UART write APIs: pinned IDF can wait indefinitely on their TX mutex. Native USB uses
1024-byte RX and 256-byte TX rings, zero-tick bounded reads/writes and TX-completion polling. All loops
check the same absolute deadline and yield one tick when they cannot progress. SDK calls, ISR/clock routing,
allocation/driver failure and real USB disconnect behavior still require target and board proof;
no host mock establishes those facts.

## Concrete physical procedure after qualification

1. Independently admit the exact maintenance signed app, source/build/reproduction and signature
   evidence, target, existing bootloader and immutable partition table. Bind its version/project,
   hash and separate role manifest to the identified physical board/port. Retain NVS evidence only
   under its separately authorized private backup plan; no dump is an input to this host exchange.
2. Obtain explicit scope for the exact app-only USB write, any needed otadata activation, one cfg
   replacement and subsequent normal-app restoration. A normal flash approval does not imply cfg
   mutation, erase or automatic retry. No command below performs flashing, reset or restoration.
3. Put the independently known full identity/VIN and complete replacement settings in the protected
   owner JSON described above. Do not infer them from the new INFO response. Prepare a new private
   evidence directory path and a one-boot physical start that makes INFO observable. Serial open/
   close can affect board reset lines even with requested DTR/RTS false; include that actual behavior
   in the reviewed plan instead of promising a reset-free open.
4. Run the reviewed single exchange only after those conditions are met:

   ```bash
   python3 maintenance/config-repair/exchange_once.py \
     --port '<exact-approved-local-serial-port>' --owner-file '<protected-owner-json>' \
     --evidence-directory '<new-private-directory>' --execute-once-explicit-config-write
   ```

   Without the final flag it does not open a port. It scans at most 64 KiB over 15 seconds for one complete
   INFO, validates the independent authority before writing, prepares the frame offline in the same
   still-open session, and invokes one APPLY write only. It retains private console/request/receipt
   files with mode 0600 and never prints identities, credentials or SDK exception text. Its receipt wait
   is bounded to 40 seconds. PySerial is a host transport prerequisite, not firmware or signing authority.
5. Treat partial/throwing write, missing/malformed receipt, any uncertain result or lost reply as
   Ambiguous. Never resend, reset to retry, erase cfg or fall back to a stale mirror. A complete
   Refused result authorizes no change of identity or scope. Confirmed requires all four device
   evidence flags; even then the device remains parked. Restore only the separately admitted normal
   signed app through the approved app-only procedure, preserving NVS. Recheck actual identity,
   settings/key/session retention and ordinary startup on that same board. These checks qualify the
   physical operation; host tests and compilation alone do not prove device retention or restoration.

## Qualification and physical-use boundaries

Host qualifies actual Store, run, transport and exchange with passive effects and synthetic
records. It covers identity, journals, one cfg write, uncertain commit, readback/retention,
partial I/O and timeout/ownership. See the [test guide](../test/README.md#configuration-maintenance-backend-and-transport).
Actual crypto, flash/power loss, ABI, task reclamation, USB timing and device retention need
separate evidence.

Keep normal all-target builds and host/protocol/sanitizer evidence separate from the enabled
role's primary/reproduction, size and stack checks. Independently qualify both signed artifacts,
board/target/port, storage backup, app-only write and restoration. Use synthetic records for
physical qualification before secret-bearing repair. Follow the [single exchange](#concrete-physical-procedure-after-qualification).
An ambiguous effect never authorizes retry.
