#pragma once

#include "logic/crashinfo.hpp"

// One-shot boot capture of WHY the previous run ended, and what it left behind.
//
// Before this existed, a panic on this device produced no artifact at all. The reset reason was
// read at boot and printed into one log line; everything else — which task died, at which PC, with
// what call chain — was gone the moment the CPU reset. `/status` could not report it, MQTT could
// not report it, and the only place it ever appeared was a serial console nobody was attached to.
// A device that reboots once a week is then indistinguishable from a device that reboots once a
// week for a REASON.
//
// What this adds is one capture, at boot, of the two facts that survive a reset: the reset reason
// (always available — no partition needed, so this half works on already-deployed devices too) and,
// where the `coredump` partition exists, the core-dump SUMMARY — crashed task, exception PC,
// backtrace and the app-ELF hash that says which build produced it. The raw image stays in flash
// for GET /coredump to stream out and symbolise offline.
//
// PARSED ONCE, NEVER ON A REQUEST PATH. esp_core_dump_get_summary() reads and parses flash into a
// ~2 KB struct; doing that per /status request would put an unbounded parse on the httpd task, on a
// device whose /status builder already sits inside a hand-counted stack budget. Capture runs in
// app_main where the heap is still whole and no request is in flight.
//
// The pure half — what counts as a fault, what counts as NOTABLE, whether a dump belongs to this
// build, and how the report is rendered — lives in logic/crashinfo.hpp and is host-tested. This
// file is the ESP-IDF glue and the cache around it.

namespace tk {

// Read the reset reason + (if present and valid) the core-dump summary into the process-wide cache.
// Call ONCE from app_main, early — before WiFi, MQTT and the component start()s allocate, and
// before anything else can reboot. Also erases an ORPHAN dump (one written by a different build);
// see the implementation for why that erase is worth doing and why it is conservative.
void diag_crash_capture();

// The cached capture. Cheap; safe from any task.
const CrashInfo& diag_crash_info();

// The cached capture with `coredump` RE-READ from flash. The cached flag can go stale within a
// session — GET /coredump?clear=1 erases the image — and a stale `true` strands the UI on a crash
// banner whose download 404s. The re-read is a 4-byte flash read of the image size word, not the
// summary parse, so it is safe on a request path.
CrashInfo diag_crash_info_live();

// Is a dump for THIS build downloadable right now? Defined as EXACTLY the condition GET /coredump
// uses to decide whether it has anything to stream, so /status can never advertise a download the
// endpoint refuses (or hide one it would serve).
bool diag_crash_coredump_present();

// Acknowledge and DELETE this boot's crash report: erase the dump, then mark the cache dismissed.
// Erase FIRST — a dismissal that outlived a failed erase would report "no crash" while the evidence
// was still in flash. Returns false (and marks nothing) if the erase failed.
bool diag_crash_dismiss();

}  // namespace tk
