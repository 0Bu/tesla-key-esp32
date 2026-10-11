# Firmware size and stack budgets

Apply [AGENTS.md](../../AGENTS.md) and [build contracts](../../docs/DEVELOPMENT.md#build-and-evidence-contracts).

- Each OTA slot is `0x1f0000`; raw-app policy is `0x1e8000` (32 KiB margin). Signing aligns raw
  bytes up to 64 KiB and appends 4 KiB. On C6, exceeding `0x1e0000` raw bytes crosses that cliff
  into `0x1f1000`, outside the slot. Never bypass either raw policy or projected/actual signed gate.
- `scripts/firmware-size-baseline.json` records reviewed maxima for raw app, ELF, flash code/rodata,
  static RAM, BSS and IRAM on all four targets. memoryModel/staticCapacity/iramCapacity are exact
  identities, not maxima; changing either direction fails. `report-firmware-size.py --enforce-budget` fails
  closed. Stack frames have their separate reviewed baseline; neither substitutes for runtime margin.
- Before committing/pushing main/CMake/sdkconfig/partition changes, measure with the pinned Docker
  `scripts/check-firmware-size.sh`; all-target pre-push uses `scripts/precheck-firmware.sh`.
- Intentional growth needs measured safe headroom and a reviewed baseline update in the same change
  (`check-firmware-size.sh --update-baseline`, raises maxima only). Reviewed reductions use
  `--tighten-baseline`: the old baseline must pass, no metric may grow, then maxima become measured
  values. The directions are mutually exclusive. Neither can legitimize an overflow or capacity change.
