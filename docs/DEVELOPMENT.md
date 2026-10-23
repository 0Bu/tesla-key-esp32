# Development

Build against the checked-out source, not documentation from another revision. Before integrating,
inspect status, `origin/main`, the merge-base and intervening changes; preserve local work.
[Agent policy](../AGENTS.md) defines authorization and review requirements.

## Build

Use `scripts/idf-docker.sh` with the ESP-IDF tag and immutable digest in
[`esp-idf-toolchain.txt`](../esp-idf-toolchain.txt). The wrapper limits each container to 1.5 CPU
and 1800 MiB. A remote runner without Docker can run host checks but cannot establish IDF evidence.

```bash
scripts/idf-docker.sh idf.py -B build set-target esp32s3 build
scripts/idf-docker.sh ./scripts/ci-build-all.sh local
scripts/idf-docker.sh idf.py menuconfig
```

The targets are exactly esp32, esp32s3, esp32c3 and esp32c6. The component manifest and four target
lockfiles pin the graph. CMake applies the ordered hash-checked tesla-ble patch series; do not edit
managed checkouts or silently substitute a host IDF. Protocol rationale: [ADR-0005](adr/0005-tesla-ble-seam.md).

Host gates use `scripts/with-host-python.sh` to resolve Python ≥3.11 for tomllib. It re-execs once,
not a hidden source change. Native Codex adapters are checked/generated with
`node tools/agent-config/sync-codex.mjs --check` / `--write`; keep reviewed primary-checkout hook
registration/trust in sync in linked worktrees. Adapter parity does not prove runtime enforcement.

Optional build acceleration: `IDF_FAST_BUILD=1` uses a container-native volume;
`scripts/idf-docker.sh daemon start` keeps a warm container, and `daemon stop` releases it.
The same limits and toolchain apply. USB flashing runs on the host, not Docker Desktop.

## Validate a change

Start with checks relevant to the diff. Host suite scope and prerequisites are in the
[test guide](../test/README.md); no host result establishes hardware or vehicle behavior.

```bash
scripts/run-mock-tests.sh
scripts/repo-lint.sh
scripts/test-build-contracts.sh
scripts/test-pr-gates.sh
tools/agent-config/selftest.sh
```

Before firmware handoff, use the complete four-target contract:

```bash
scripts/idf-docker.sh ./scripts/ci-build-verify.sh "$(cat version.txt)" "$(git rev-parse HEAD)"
```

This adds disposable-key signing, manifest/Pages byte checks and reproducibility to compilation.
Never use the real offline OTA key in tests. Before a firmware push, `scripts/precheck-firmware.sh`
checks all four size and stack baselines incrementally; `--target` narrows investigation and
`--clean` removes its cache. Narrow checks do not replace the four-target handoff.

## Build and evidence contracts

| Contract | Authority |
|---|---|
| Toolchain, locked components and patches | `esp-idf-toolchain.txt`, `main/idf_component.yml`, `dependencies.lock.*`, `patches/tesla-ble/` |
| Exact four targets and unsigned outputs | `scripts/ci-build-all.sh`, `check-build-artifact-inventory.py` |
| Physical flash layout | `partitions.csv`, `check-partition-contract.py`, `check-otadata-contract.py` |
| Effective compiler/generated/link inputs | `check-build-semantics.py` |
| Per-target memory and frame budgets | `firmware-size-baseline.json`, `firmware-stack-baseline.json`, `check-firmware-size.sh`, `check-stack-usage.py` under `scripts/` |
| Source fingerprint and independent byte reproducibility | `check-reproducible-build.sh`, `check-reproducible-artifacts.py` |
| Protected signing and immutable publication | [Security pipeline](SECURITY.md#release-pipeline-and-trust-boundaries) |

The effective-build validator closes every C/C++ compilation unit to reviewed source roots and
pinned compilers/includes. It verifies generated producers, Ninja commands/dependencies, archive
objects, linker response files and the exact ELF-to-image chain. One literal main-component `SRCS`
registration is permitted; hidden attachment, escaped sources or forced includes fail.
The gate checks that every main-component unit compiles with `-Og` and emits `-fstack-usage`;
the other components are covered by the checked `CONFIG_COMPILER_OPTIMIZATION_DEBUG=y` Kconfig
value, Mbed TLS/TF-PSA-Crypto `-Os` is checked as the effective Kconfig value, and the bootloader,
which builds with size optimization, is compared only between independent builds. Main privately
enforces C++17 and return-value/format warnings.
Compiler/include/search and `CCACHE_*` overrides are rejected, and ccache is disabled.

The size policy limits the projected signed app to slot minus 32 KiB (`0x1e8000`): the app is
padded to 64 KiB and signing adds one 4 KiB sector, bounded by the `0x1f0000` slot. Because size
moves in 64 KiB steps, each size report also shows the content headroom to the next step. Size
reports are current evidence; historical prose values are not. `--update-baseline` raises maxima only; `--tighten-baseline`
requires the old baseline to pass, then lowers each checked target to measured values. Memory-model
and static/IRAM capacities are identity fields, immutable under either ratchet. The stack baseline
uses the same modes (`check-stack-usage.py --observed-dir dist --write-baseline <file> --mode
raise|tighten`) and lists every changed entry; a PR lists each raised entry with its cause.
Stack tighten forbids growth and newly reviewed frames or translation units; it permits removed
units and removed or below-threshold frames, then lowers the remaining maxima to measurements.
`check-firmware-size.sh` builds all four targets by default. The pre-push hook runs the
incremental `precheck-firmware.sh` on all four (`PRE_PUSH_TARGETS` narrows it deliberately); it
checks only the size and stack gates, not CI's dependency, partition, otadata, semantics,
artifact-identity, reproducibility or signing gates. Stack reports inventory frames, reject unbounded dynamic frames
and frames over 4096 B; they do not prove call depth or runtime stack margin.

## Delivery

Unsigned local images **cannot boot**. Use provenance-bound signed artifacts and the canonical
`$flash-esp32`, `$deploy-bench`, `$ship` or `$usb-recovery` procedure, as appropriate. Every physical write
requires explicit authorization for the board, target and scope. Verify every image's signature;
production and preview artifacts must match the production public-key pin. An authorized local
development image must match the supplied development key. Size/chip/version checks alone do not
authenticate an image.

App-only USB delivery writes the signed app at `0x20000`, then activates by erasing `otadata`
last. NVS stays intact. Merged-image writes and full erases destroy configuration and keys;
[retention and signing](SECURITY.md) explain recovery limits. Signing with a development key creates
a separate trust lineage and needs an explicit key and scope.

`$deploy` handles commit, push, PR creation, current-head reviews/CI and canonical squash merge,
then observes the main build. It does not contact devices.
[`$deploy-bench`](../.agents/skills/deploy-bench/SKILL.md) identifies the physical USB bench across
Tesla/Daikin firmware, flashes a signed PR preview or development-key image through `$flash-esp32`
or an already-published image through `$usb-recovery`'s provenance and app-only steps, and tests
the implementation on that same board. Device delivery after merge remains a separately requested
`$ship` operation.

## Running workflow shell blocks

The project hooks judge each shell tool call by its text before the shell runs it. Sent inline,
most multi-line blocks in `.agents/skills/` are rejected: the PR gate cannot rule out that a
command substitution or an arithmetic assignment such as `X=$((…))` yields a publish or merge
action, and both hooks treat a glob such as the `case` arm `*)` as unbounded. A single-line
command shown with `<placeholders>` runs inline once each holds its literal value. Run multi-line
blocks this way:

1. Write the blocks approved for this call, unchanged and in document order, to one script in the
   session's scratch directory with the file-writing tool, not a shell redirect.
2. Run it with one static command, `bash <script-path>`. Put inputs the blocks require (`PR`,
   `TARGET`, `PORT`, `RUNID`, `DEVICE_IP`, …) in front as literal `NAME=value` assignments
   so the hooks still see them.
3. A later call that needs an earlier step's variables repeats that step's read-only blocks. Pass a
   value that only a completed write, erase or other mutation set (for example `DELIVERY` in
   `$ship`) as a literal assignment; never repeat the mutation.

Never put `git push`, `gh pr create`, `gh pr merge`, or a key, NVS or credential path into such a
script: those stay standalone, statically bound commands that the hooks inspect, or the operator's
step. A script grants no authorization; every approval boundary of the workflow still applies.

## Reviews and documentation

Required independent checks and exact-head records are in [AGENTS.md](../AGENTS.md#pr-and-review).
Skills specify how to perform them; a green build does not substitute for review.
Update the owning document with each changed contract. Keep [FEATURES](FEATURES.md) as an index,
not a second narrative. `$project-review` always assesses documentation structure, clarity,
concision and completeness; unresolved material documentation findings prevent a clean result.

### Static-analysis decision

The recorded clang-tidy 18 survey found 3355 blanket findings but only three curated bug-finding
findings, all in test helpers; conversion warnings were bounded gradient conversions, and shadow/
sign-conversion checks found none. This is historical measurement, not a current clean claim.
There is no inert `.clang-tidy` configuration. Before introducing an analyzer, remeasure useful
yield on the actual revision; do not replace host/runtime/IDF evidence with that survey.
