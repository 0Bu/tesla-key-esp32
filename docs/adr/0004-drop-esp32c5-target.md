# ADR-0004: Drop esp32c5

**Status:** accepted. **Date:** 2026-08-01.
**Supersedes:** [ADR-0001](0001-esp32c5-target-upstreaming.md).

## Context

C5 lacked upstream target support and depended on a manifest workaround. Crash/recovery work pushed
its unsigned app to 1,998,178 B: 64 KiB alignment plus a 4 KiB signature produced `0x1f1000`,
4 KiB beyond the OTA slot. Removing coredump alone did not recover enough space.

Whole-build `-Os` had frozen under evcc/BLE load. Enlarging slots moved the installed OTA layout,
which ordinary OTA cannot migrate. A reduced C5 feature fork would violate the single-tree contract.

## Decision

Support exactly **esp32, esp32s3, esp32c3, esp32c6**, the library's declared targets. Remove C5
manifest routing/local clone, defaults, display branches, 5 GHz option, suffix/platform mapping
and build/installer consumers. Adding an omitted target requires upstream support first.

## Consequences

Existing C5 installations keep running but receive no new manifest/image; they are not modified.
T-Dongle-S3 display/LED support stays. Reintroduction requires solving size and upstream support,
not bypassing target enforcement. Decision-time size figures are historical; current headroom comes
from generated size artifacts.
