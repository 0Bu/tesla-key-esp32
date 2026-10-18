# Firmware size and stack budgets

Apply [AGENTS.md](../../AGENTS.md) and [build contracts](../../docs/DEVELOPMENT.md#build-and-evidence-contracts).

- Each OTA slot is `0x1f0000`; the projected signed app must stay within `0x1e8000` (32 KiB margin).
  The app is padded to 64 KiB and signing appends 4 KiB, so size moves in 64 KiB steps: a raw app
  above `0x1e0000` projects to `0x1f1000`, outside the slot, on any target. The size report shows
  each target's content headroom to the next step. Never bypass the projected or actual signed gate.
- `scripts/firmware-size-baseline.json` records reviewed maxima for raw app, ELF, flash code/rodata,
  static RAM and BSS on all four targets, and IRAM on esp32 and esp32s3 (esp32c3/esp32c6 report no
  separate IRAM region). memoryModel/staticCapacity/iramCapacity are exact
  identities, not maxima; changing either direction fails. `report-firmware-size.py --enforce-budget` fails
  closed. Stack frames have their separate reviewed baseline; neither substitutes for runtime margin.
- Before committing/pushing main/CMake/sdkconfig/partition changes, measure with the pinned Docker
  `scripts/check-firmware-size.sh` (all four targets by default); pre-push runs the incremental
  `scripts/precheck-firmware.sh` on all four, narrowed only by an explicit `PRE_PUSH_TARGETS`.
- Intentional growth needs measured safe headroom and a reviewed baseline update in the same change
  (`check-firmware-size.sh --update-baseline`, raises maxima only). Reviewed reductions use
  `--tighten-baseline`: the old baseline must pass, no metric may grow, then maxima become measured
  values. The directions are mutually exclusive. Neither can legitimize an overflow or capacity change.
  The stack baseline follows the same two modes (`check-stack-usage.py --observed-dir dist
  --write-baseline <file> --mode raise|tighten`); it is never regenerated wholesale. Every raised size or stack entry is
  listed in the PR with its measured delta and cause.
