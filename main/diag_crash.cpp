// One-shot crash/reset capture (see diag_crash.hpp). The pure decisions — fault classification,
// notability, orphan detection, rendering — live in logic/crashinfo.hpp and are host-tested; this
// file is the ESP-IDF glue and the cache.
#include "diag_crash.hpp"

#include <sdkconfig.h>

#include <esp_app_desc.h>
#include <esp_core_dump.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_system.h>

#include <cstdlib>
#include <cstring>

static const char* TAG = "diag_crash";

namespace tk {

// logic/reset_reason.hpp mirrors esp_reset_reason_t BY VALUE so the header stays IDF-free and the
// firmware can pass the raw int. These asserts are what makes that safe: if IDF ever renumbers the
// enum, the build fails here instead of the device silently mislabelling every crash it reports —
// which is the one failure this whole path cannot tolerate, since a wrong label is worse than no
// label (it sends the next investigation somewhere else). Spot-check the values the classifier
// actually branches on.
static_assert(static_cast<int>(ResetCode::PowerOn)  == ESP_RST_POWERON,  "reset enum drift");
static_assert(static_cast<int>(ResetCode::Sw)       == ESP_RST_SW,       "reset enum drift");
static_assert(static_cast<int>(ResetCode::Panic)    == ESP_RST_PANIC,    "reset enum drift");
static_assert(static_cast<int>(ResetCode::IntWdt)  == ESP_RST_INT_WDT,  "reset enum drift");
static_assert(static_cast<int>(ResetCode::TaskWdt) == ESP_RST_TASK_WDT, "reset enum drift");
static_assert(static_cast<int>(ResetCode::Brownout) == ESP_RST_BROWNOUT, "reset enum drift");

// Filled once by diag_crash_capture(); read-only afterwards EXCEPT the `dismissed` byte (see
// diag_crash_dismiss for why that needs no lock).
static CrashInfo s_ci;

// Every esp_core_dump_image_* symbol lives in IDF's core_dump_flash.c, which is compiled ONLY
// when CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH is set — so on a target that disables core dumps these
// are not merely no-ops, they do not LINK. esp32c5 is such a target (see sdkconfig.defaults.esp32c5:
// the display + PSRAM build has no room for the component), which is why the guard is on the calls
// and not only on the parsing.
bool diag_crash_coredump_present() {
#if defined(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH)
    // EXACTLY what handle_coredump uses to decide it has something to stream — any extra condition
    // here could make /status advertise a download the endpoint refuses, or hide one it would
    // serve, and that disagreement is precisely what this path exists to prevent. An ESP_OK return
    // already implies a sane size: esp_core_dump_partition_and_size_get rejects a blank partition
    // (the size word reading back 0xffffffff) and anything under 4 bytes. One 4-byte flash read.
    //
    // On a device flashed BEFORE the coredump partition existed this simply returns false forever:
    // the partition lookup fails, nothing is advertised, and the reset-reason half of the report
    // (which needs no partition) still works. That is the supported degraded state, not a bug.
    size_t addr = 0, size = 0;
    return esp_core_dump_image_get(&addr, &size) == ESP_OK;
#else
    return false;   // this build writes no dumps, so there is never one to offer
#endif
}

CrashInfo diag_crash_info_live() {
    CrashInfo c = s_ci;                            // boot-time reason + parsed summary
    c.coredump  = diag_crash_coredump_present();   // …but the image itself may be gone by now
    return c;
}

const CrashInfo& diag_crash_info() { return s_ci; }

void diag_crash_capture() {
    crash_set_reset(s_ci, static_cast<int>(esp_reset_reason()));
    s_ci.coredump = diag_crash_coredump_present();

    // The running build's identity, always — not only when a dump exists. It is what ties a log
    // stream (and a downloaded dump) to the .elf that can symbolise it, and CI archives one .elf
    // per build precisely so that match can be made later.
    char run_sha[65] = {0};
    esp_app_get_elf_sha256(run_sha, sizeof(run_sha));
    s_ci.elf_sha256 = run_sha;

#if defined(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH) && defined(CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF)
    // Parse the summary only from a VALID image (checksum ok). The struct is ~2 KB, so it goes on
    // the heap rather than this task's stack — at boot the heap is still whole (WiFi/NimBLE/MQTT
    // have not started), which is the other reason capture belongs here and not on a request path.
    if (s_ci.coredump && esp_core_dump_image_check() == ESP_OK) {
        auto* sum = static_cast<esp_core_dump_summary_t*>(calloc(1, sizeof(esp_core_dump_summary_t)));
        if (sum && esp_core_dump_get_summary(sum) == ESP_OK) {
            s_ci.task = sum->exc_task;
            s_ci.pc   = sum->exc_pc;

            // THE BACKTRACE IS XTENSA-ONLY, and that is an IDF fact rather than a choice of ours.
            // esp_core_dump_bt_info_t is declared per ARCHITECTURE: on Xtensa it carries an
            // unwound PC array (bt/depth/corrupted), on RISC-V it carries a raw STACK DUMP
            // instead, because RISC-V has no windowed registers and IDF cannot unwind on-device.
            // Four of this project's five targets are RISC-V (c3/c6/c5), so getting this wrong is
            // not an edge case — it is most of the fleet.
            //
            // On RISC-V the report is not empty, it is SHORTER: reason, task, PC and the app-ELF
            // hash are all still captured, and the full stack is still in the dump that GET
            // /coredump streams — where an offline decoder with the matching .elf can unwind it
            // properly anyway. Publishing IDF's raw stack words as if they were a backtrace would
            // be worse than publishing none: they are not return addresses, and a reader has no
            // way to tell from /status which of the two they are holding.
#if CONFIG_IDF_TARGET_ARCH_XTENSA
            uint32_t depth = sum->exc_bt_info.depth;
            if (depth > kCrashBacktraceMax) depth = kCrashBacktraceMax;
            s_ci.backtrace.clear();
            s_ci.backtrace.reserve(depth);
            for (uint32_t i = 0; i < depth; i++) s_ci.backtrace.push_back(sum->exc_bt_info.bt[i]);

            s_ci.corrupted = sum->exc_bt_info.corrupted;
#endif

            // app_elf_sha256 is a uint8_t[] holding a HEX STRING, not a C string and not raw
            // digest bytes — so it neither converts to std::string on its own nor should be
            // treated as binary. Copy through a bounded buffer and terminate it ourselves rather
            // than trusting the array to carry its own NUL: this runs while decoding a crash, and
            // reading past the end of a struct field is a poor way to report one.
            char dump_sha[sizeof(sum->app_elf_sha256) + 1] = {0};
            std::memcpy(dump_sha, sum->app_elf_sha256, sizeof(sum->app_elf_sha256));
            s_ci.dump_elf_sha256 = dump_sha;
        }
        free(sum);
    }

    // A dump can OUTLIVE the firmware that wrote it: the coredump partition survives an OTA, and a
    // panic that cannot write its own dump (a stack overflow can overrun the writer) leaves the
    // PREVIOUS build's image in place. Such an orphan still passes esp_core_dump_image_check() — it
    // is a valid image, just of another binary — so `coredump` reads true, /status offers a
    // download, and only the offline decoder rejects it three steps later on a SHA-256 mismatch.
    // Erasing it makes `coredump` mean "a dump for THIS firmware is downloadable" and frees the
    // slot for the next real panic. The summary fields go with it: they describe the foreign binary
    // and would symbolise to garbage against the running .elf.
    //
    // coredump_is_foreign() declares foreign only on PROOF (two present hashes, a meaningful
    // comparison length, an actual mismatch), because the erase is destructive and the artifact it
    // would destroy is the only thing a panic left behind.
    if (crash_has_summary(s_ci) && coredump_is_foreign(s_ci.elf_sha256, s_ci.dump_elf_sha256)) {
        ESP_LOGW(TAG, "stale core dump from build %s (running %s) — erasing",
                 s_ci.dump_elf_sha256.c_str(), s_ci.elf_sha256.c_str());
        esp_err_t err = esp_core_dump_image_erase();
        if (err != ESP_OK)
            ESP_LOGW(TAG, "stale core dump erase failed: %s", esp_err_to_name(err));
        // Cleared regardless of the erase result: reporting a dump we KNOW is foreign is worse than
        // reporting none.
        s_ci.coredump = false;
        s_ci.task.clear();
        s_ci.pc = 0;
        s_ci.backtrace.clear();
        s_ci.corrupted = false;
        s_ci.dump_elf_sha256.clear();
    }
#endif

    if (crash_is_notable(s_ci)) {
        // Into the esp_log stream, which diag_log.cpp captures for /diag AND forwards to syslog —
        // so the one record that explains an unattended reboot reaches a place that outlives the
        // next one. (The forwarder is not up yet at this point in boot; logic/bootlog.hpp's replay
        // is what gets these lines to the collector — see syslog.cpp.)
        ESP_LOGE(TAG, "%s", build_crash_text(s_ci).c_str());
    }
}

// Acknowledge + delete this boot's crash report. Erase FIRST, mark second: on a failed erase
// nothing is marked, so the banner comes back rather than the device claiming a crash is gone while
// its dump is still downloadable.
//
// The erase is unconditional rather than gated on diag_crash_coredump_present():
// esp_core_dump_image_erase() succeeds on an already-blank partition, and gating it on the presence
// check would leave behind exactly the images that check REJECTS — a truncated or checksum-broken
// dump, which is stale crash residue like any other.
//
// `dismissed` is written from the httpd task while the MQTT task may be reading s_ci. It is a
// single byte store that only ever goes false -> true, so a concurrent reader sees one state or the
// other and both are self-consistent renderings of the same CrashInfo. No lock — and none of the
// paths involved may take one anyway (an allocation under a mutex is the wedge rule in CLAUDE.md).
bool diag_crash_dismiss() {
#if defined(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH)
    esp_err_t err = esp_core_dump_image_erase();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "dismiss failed — coredump erase: %s", esp_err_to_name(err));
        return false;
    }
#endif
    // On a build with no dump there is nothing to erase, and dismissal still MEANS something: the
    // report it clears is the fault RESET, which is what /status.last_crash carries there. Failing
    // it would leave a crash banner that no action can dismiss.
    s_ci.dismissed = true;
    ESP_LOGI(TAG, "crash report dismissed (reset=%s, dump erased)", reset_reason_slug(s_ci.reset_code));
    return true;
}

}  // namespace tk
