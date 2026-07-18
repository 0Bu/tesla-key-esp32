# ADR-0001: Upstream the esp32c5 target to yoziru/tesla-ble, then retire the local patch

**Status:** accepted — waiting on the upstream PR being filed/merged/released
**Date:** 2026-07-08
**Relates to:** architecture review 2026-07 (P4), [`docs/ARCHITECTURE.md`](../ARCHITECTURE.md) §"esp32c5 via a local build-time patch"

## Context

tesla-ble v5.1.1 declares `targets: [esp32, esp32s3, esp32c3, esp32c6]` in its
`idf_component.yml`, and the ESP-IDF Component Manager enforces that list at dependency
resolution — it refuses `esp32c5` before anything compiles. The library's code is
target-agnostic (it already builds for the RISC-V c3/c6; the c5 is RISC-V too); the one
manifest line is the only blocker.

This project ships the esp32c5 (LilyGO T-Dongle-C5) anyway, via a build-time workaround:
`scripts/prepare-tesla-ble-c5.sh` clones the pinned tag into `third_party/tesla-ble`
(gitignored) and appends `esp32c5` to its `targets:`; `main/idf_component.yml` routes ONLY
the c5 target through that local copy (`rules: if: target == esp32c5`), while the other four
targets resolve byte-identically from git.

The workaround is proven in CI, but it is standing debt: every upstream version bump must
re-verify the patch (the script anchors on the `esp32c6` manifest line), and the c5 image is
built from a *clone-plus-edit* rather than the Component Manager's registry path.

## Decision

1. **File a one-line PR upstream** against `yoziru/tesla-ble` adding `- esp32c5` to
   `idf_component.yml` `targets:` (directly after `- esp32c6`). Our five-target CI — where the
   c5 image builds and runs from a checkout whose ONLY delta is that line — is the evidence
   the PR cites. *(The upstream PR is filed from a maintainer's GitHub account; this repo's
   automation only prepares the change.)*
2. **Until an upstream release contains the fix, nothing changes here.** The local patch
   stays; each Renovate/tesla-ble bump keeps re-running `prepare-tesla-ble-c5.sh` as today.
3. **On the first upstream release whose manifest includes `esp32c5`,** retire the patch in
   one cleanup PR:
   - `main/idf_component.yml`: collapse the two `tesla-ble` entries into the single git
     dependency (drop both `rules:` blocks and the `path:` entry).
   - Delete `scripts/prepare-tesla-ble-c5.sh`; remove its invocation from
     `scripts/ci-build-all.sh` and the mention from `scripts/idf-docker.sh`/CI docs if present.
   - Drop the `third_party/` entry from `.gitignore` if nothing else uses it.
   - Update the narrative: `.claude/CLAUDE.md` (header + build steps) and
     `docs/ARCHITECTURE.md` §"esp32c5 via a local build-time patch" (reduce to a historical
     note pointing at this ADR).
   - Verify: `scripts/ci-build-all.sh` — all five targets green, c5 size gate unchanged; the
     c5 image is expected byte-comparable (same library revision, now via the manager).

## Consequences

- Five targets resolve identically from the Component Manager; upstream bumps stop carrying
  a re-verify-the-patch step.
- Until upstream releases, no risk is added: the local patch remains the tested path.
- If upstream declines the PR, this ADR flips to "keep the local patch indefinitely" — the
  workaround is self-contained and CI-verified, so that outcome is acceptable, just noisier.
