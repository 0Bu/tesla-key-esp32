# Firmware Size & Memory Budget Invariants

This scoped policy governs firmware binary footprints, partition constraints, and memory baselines.
It complements the canonical repository policy in [`../../AGENTS.md`](../../AGENTS.md).

## 1. Hard Partition & Secure Boot v2 Constraints

- **OTA Slot Capacity**: In [`partitions.csv`](../../partitions.csv), each OTA partition (`ota_0`, `ota_1`) is strictly `0x1f0000` (2,031,616 Bytes).
- **CI Policy Limit**: `APP_POLICY_LIMIT` is `0x1e8000` (1,998,848 Bytes), leaving a mandatory 32 KiB safety margin below the partition boundary.
- **64 KiB Secure Boot v2 Quantization Cliff**:
  Before signing, the unsigned binary is aligned up to the next 64 KiB boundary (`0x10000 = 65,536 Bytes`) plus a 4 KiB signature sector.
  On **ESP32-C6**, the unsigned binary MUST remain below `1,966,080 Bytes` (`0x1e0000`). Crossing even 1 byte above 1,966,080 Bytes jumps the projected signed image by 64 KiB to 2,035,712 Bytes, which overshoots both `APP_POLICY_LIMIT` and the physical flash partition.

## 2. Strict Reviewed Maxima Baselines

- [`scripts/firmware-size-baseline.json`](../../scripts/firmware-size-baseline.json) records byte-exact reviewed maxima (`maxElfTotal`, `maxFlashCodeAndRodata`, `maxStaticUsed`, `maxBss`, `maxIramUsed`) for all four targets (`esp32`, `esp32s3`, `esp32c3`, `esp32c6`).
- CI enforces this budget fail-closed via `scripts/report-firmware-size.py --enforce-budget`.
- Any PR adding firmware code must verify that the footprint stays within reviewed limits.

## 3. Pre-Commit & Push Verification

- Before committing or pushing changes touching `main/`, `CMakeLists.txt`, `sdkconfig*`, or `partitions.csv`:
  Run `./scripts/check-firmware-size.sh` to measure the footprint locally inside the pinned ESP-IDF Docker container.
- If intentional code growth exceeds the existing baseline while remaining safely within partition headroom:
  Run `./scripts/check-firmware-size.sh --update-baseline` to refresh `scripts/firmware-size-baseline.json` and include the updated baseline in the same commit.
- Never commit or push a change where `esp32c6` unsigned size exceeds 1,966,080 Bytes.
