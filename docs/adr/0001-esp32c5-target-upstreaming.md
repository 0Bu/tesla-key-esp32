# ADR-0001: Upstream esp32c5 support

**Status:** superseded by [ADR-0004](0004-drop-esp32c5-target.md), 2026-08-01.
**Date:** 2026-07-08.

## Context

The pinned library declared only esp32/s3/c3/c6, so Component Manager refused C5. The historical
workaround cloned the pin into `third_party/tesla-ble`, added C5 to the manifest and selected that
local path only for C5. It compiled, but every upstream bump required patch verification.

## Decision

Request upstream manifest support, retain the tested workaround until a release included it, then
remove the local clone/script/routing and verify identical five-target resolution/builds.
No upstream request was merged before the later size constraint forced removal.

## Consequences

This plan is historical and grants no current C5 support. The local workaround no longer exists;
[ADR-0004](0004-drop-esp32c5-target.md) defines the current four-target contract.
