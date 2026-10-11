# ADR-0003: Reject replayed CarServer responses

**Status:** superseded by [ADR-0005](0005-tesla-ble-seam.md).

## Context

In tesla-ble v5.1.1, failed response-counter validation logged but still dispatched. Patch 0001
returned before callbacks/FIFO completion. Its original rationale overstated cross-command risk:
AEAD request-hash binding already rejects stale encrypted responses from another request. The patch
blocked duplicate delivery of the same encrypted response.

Plaintext CarServer replies use counter zero and bypassed that replay check; foreign attribution
therefore remained possible. The Tesla reference routes both forms by request UUID.

## Historical Decision (v5.1.1–v5.1.3)

Apply an early return for invalid non-zero counters; independently verify charging-current ACKs with
fresh readback. Upstream v5.1.4/v5.2.0 incorporated early return and UUID matching.

## Consequences

Patch 0001 is retired. Native dispatcher UUID/domain routing under ADR-0005 handles response
attribution, while authenticated response counters retain their replay window. Do not confuse duplicate
delivery, cross-request authentication and plaintext routing as one check.
