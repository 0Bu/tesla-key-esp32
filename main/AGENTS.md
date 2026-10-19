# Firmware implementation (`main/`)

Apply [repository policy](../AGENTS.md) and the normative
[concurrency contract](../docs/ARCHITECTURE.md#concurrency-normative-contract).

- Budget INTERNAL largest contiguous block. Avoid repeated dynamic allocations in cyclic code;
  stream large responses and use bounded pools/static buffers rather than large stack locals.
- Check C allocation failures explicitly. Own every cJSON tree through its cleanup/RAII path;
  a failed response must not become a partial success.
- Keep per-function stack frames within the reviewed `scripts/firmware-stack-baseline.json`; size
  task stacks from measured runtime minima. Compiler frame reports and runtime stack minima provide
  different evidence.
- Leaf cache/result locks allow only small bounded struct/string snapshots with `tk::SemGuard`
  RAII against allocation failure. Release them before logging, network/flash I/O, unbounded
  allocation or calls out. Serialize library access through the documented Vehicle lock,
  with RAII unwind; never wait for command completion while holding it. Critical sections copy
  bounded POD only.
- Contain exceptions/OOM at HTTP/MCP, task and C callback boundaries, including catch-all exits.
  Rejected input cannot dispatch, persist or restart.
- Extract pure bounded decisions into `main/logic/`; IDF handles, tasks and mutable state belong
  in the integration shell.
