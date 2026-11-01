#!/usr/bin/env python3
"""Structural guard for log provenance at the firmware integration boundary."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]


def source(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


header = source("main/vehicle_ctrl.hpp")
outcome_logic = source("main/logic/connect_outcome.hpp")
pairing = source("main/vehicle_pairing.cpp")
telemetry = source("main/vehicle_telemetry.cpp")
supervisor = source("main/heap_supervisor.cpp")
commands = source("main/vehicle_commands.cpp")
ble_client = source("main/ble_client.cpp")
ble_header = source("main/ble_client.hpp")
http_api = source("main/http_api.cpp")
http_config = source("main/http_config.cpp")

# Public entry points deliberately require a provenance choice. A default would let a new
# unattended caller silently become an unthrottled foreground error source again.
assert "get_vehicle_status(VehicleStatusResult& out, tk::ConnectOrigin origin," in header
assert "bool pair(" not in header
assert "enroll_existing_key_(int timeout_ms = 6000)" in header
status_decl = re.search(r"bool get_vehicle_status\([^;]+;", header, re.DOTALL)
assert status_decl and "origin =" not in status_decl.group(0)
private_pair = header.index("enroll_existing_key_(int timeout_ms")
assert header.rfind("private:", 0, private_pair) > header.rfind("public:", 0, private_pair)

# Firmware owns enrollment. UI/HTTP callers cannot choose foreground provenance for it.
assert pairing.count("get_vehicle_status(status, tk::ConnectOrigin::Background") == 1
assert "self->enroll_existing_key_();" in pairing
assert "get_vehicle_status(vs, tk::ConnectOrigin::Foreground)" in http_api
assert "handle_send_key" not in http_config
assert "timeout_ms, tk::ConnectOrigin::Background," in pairing

# Per-probe summaries must not bypass the lower connect rate limiter.
assert 'ESP_LOGD(TAG, "auto-pair: car not reachable over BLE' in pairing
assert 'ESP_LOGW(TAG, "auto-pair: car not reachable over BLE' not in pairing
pair_body = pairing[pairing.index("bool VehicleController::enroll_existing_key_("):]
assert "deadline, tk::ConnectOrigin::Background" in pair_body
assert "tk::CompletionTimeoutPolicy::ExpectedSilent" in pair_body
assert "await_completion_(completion, generation, deadline, name.c_str(), timeout_policy)" in commands
assert 'ESP_LOGD(TAG, "background \'%s\' timed out' in commands
assert "tk::CompletionTimeoutPolicy::BackgroundHealth" in pairing
assert "kConnectFailRepeatMs" in outcome_logic
assert "kCompletionTimeoutRepeatMs" in outcome_logic
assert "kAutoPairNoticeRepeatMs" in outcome_logic
assert "uint64_t now_ms" in outcome_logic
assert "esp_timer_get_time()" in commands
assert "esp_timer_get_time()" in pairing
assert "const int64_t attempt_start_us = esp_timer_get_time();" in commands
assert "ble_->target_connectable_since(attempt_start_us)" in commands
assert "return target_connectable_since(esp_timer_get_time() - 90LL * 1000 * 1000);" in ble_client
assert "s.last_us, s.connectable_us, since_us" in ble_client
assert "connectable_verdict_in_attempt" in outcome_logic
assert "connectable_observations_" in ble_header
assert "observation.seen_us > e->connectable_us" in ble_client
assert "pending->seen_us = now_us;" in ble_client
assert "tk::periodic_log_due(" in pairing
assert "tk::periodic_log_reset(unpaired_notice)" in pairing
assert 'ESP_LOGI(TAG, "auto-pair: pairing automatically with the existing key;' in pairing
assert 'ESP_LOGD(TAG, "auto-pair: waiting for automatic pairing with the existing key' in pairing
assert re.search(r"await_completion_\(completion, generation, deadline, name\.c_str\(\),\s*"
                 r"tk::CompletionTimeoutPolicy::ForegroundWarn\)", commands)

# The direct status callback path obeys the same completion policy and closes a previous health
# timeout run on success. Its queue wait must also distinguish foreground from automatic probes.
status_start = telemetry.index("bool VehicleController::get_vehicle_status(")
status_path = telemetry[status_start:]
assert 'ESP_LOGW(TAG, "vehicle-status deadline exhausted waiting for another request")' in status_path
assert 'ESP_LOGD(TAG, "background vehicle-status deadline exhausted waiting for another request")' in status_path
assert 'note_completion_timeout_(' in status_path
assert 'tk::CompletionTimeoutPolicy::ForegroundWarn' in status_path
assert 'tk::CompletionTimeoutPolicy::ExpectedSilent' in status_path
assert 'tk::completion_ok_note(completion_timeout_);' in status_path

# Raw GAP/GATT attempt detail is diagnostic DEBUG only. The origin-aware command-layer summary is
# the sole production signal, otherwise these lower sinks would bypass first-plus-hourly volume.
for raw_message in (
    "discovery scan started for %d ms",
    "discovery scan window ended",
    "scan start failed: %d",
    "scan cancel failed: %d",
    "scanning for Tesla BLE...",
    "Tesla '%s' found: %s — connecting",
    "connect failed: %d",
    "connect error: %d",
    "connected, handle=%d",
    "disconnected, reason=%d",
    "BLE write chunk failed: %d",
    "late GAP connection after canceled intent — dropping handle=%d",
    "MTU negotiated: %d",
    "svc discovery failed: %d",
    "Tesla service not found",
    "characteristic discovery error: %d",
    "CCCD discovery error: %d",
    "CCCD subscription write failed: %d",
    "BLE link deferred after CCCD (handle %d, generation %lu)",
    "ATT MTU exchange not started (%d); keeping 20-byte writes",
    "ATT MTU exchange failed (%d); keeping 20-byte writes",
):
    assert f'ESP_LOGD(TAG, "{raw_message}' in ble_client
    assert not re.search(rf'ESP_LOG[IEW]\(TAG, "{re.escape(raw_message)}', ble_client)

# Fail closed for the whole BLE file: adding/promoting any production-level line requires an
# explicit review here. Repeated attempt detail must never escape by simply using a new message.
production_ble_logs = re.findall(r'ESP_LOG([IEW])\(TAG,\s*"([^"]+)"', ble_client)
expected_production_ble_logs = {
    # F137 callback faults use the bounded iteration-fault reporter, separate from attempts.
    ("W", "BLE %s callback failed (suppressed=%u)"),
    ("W", "NimBLE host reset, reason=%d"),
    ("E", "failed to create BLE scan timer"),
    ("E", "BLE resource allocation failed"),
    ("E", "nimble_port_init failed: %d"),
    ("E", "NimBLE start acknowledgement gate is not idle"),
    ("E", "NimBLE host did not acknowledge sync within 5000 ms"),
    ("W", "NimBLE synced after the boot acknowledgement timeout; runtime stays closed"),
    ("I", "NimBLE synced"),
    ("W", "could not obtain stable BLE handle while disconnecting"),
    ("W", "RX defer queue full — dropping link fail-closed"),
    ("W", "RX notify length %u exceeds fixed host slot — dropping link"),
    ("E", "CCCD subscribed but connected callback is unavailable"),
    ("I", "RX notify len=%u: %s"),
    ("E", "on_gap_event exception (dropping event type=%d): %s"),
    ("E", "on_gap_event unknown exception (dropping event type=%d)"),
    ("E", "on_svc_disc exception (dropping connection): %s"),
    ("E", "on_svc_disc unknown exception (dropping connection)"),
    ("E", "on_chr_disc exception (dropping connection): %s"),
    ("E", "on_chr_disc unknown exception (dropping connection)"),
    ("E", "on_dsc_disc exception (dropping connection): %s"),
    ("E", "on_dsc_disc unknown exception (dropping connection)"),
    ("E", "CCCD subscribed but connected callback is unavailable"),
    ("E", "subscribe completion exception (dropping connection): %s"),
    ("E", "subscribe completion exception (dropping connection)"),
    # Once per boot: a timeout disables further exchanges; the value logs only when it changes.
    ("W", "ATT MTU exchange timed out; not retried until reboot"),
    ("I", "ATT MTU %u negotiated"),
    ("E", "MTU exchange completion exception: %s"),
    ("E", "MTU exchange completion exception"),
}
assert set(production_ble_logs) == expected_production_ble_logs
assert len(production_ble_logs) == len(expected_production_ble_logs)
verbose_start = ble_client.index("if (diag_verbose())")
verbose_end = ble_client.index("if (on_rx_data_ &&", verbose_start)
assert 'ESP_LOGI(TAG, "RX notify len=%u: %s"' in ble_client[verbose_start:verbose_end]

# Healthy periodic heap samples are INFO; watchdog threshold/recovery messages remain separate.
heap_format = 'HEAP free=%u largest_block=%u min_free=%u internal_largest=%u supervisor_min_stack=%u'
assert f'ESP_LOGI(TAG, "{heap_format}"' in supervisor
assert f'ESP_LOGW(TAG, "{heap_format}"' not in supervisor
assert 'HEAP free=' not in telemetry  # one sampler, independent of Vehicle progress

# A link established by the telemetry loop must close the previous failure streak too.
already_connected = commands.index("if (ble_->is_connected()) {")
connect_attempt = commands.index('ble_->connect("")', already_connected)
assert "tk::connect_ok_note(connect_fail_);" in commands[already_connected:connect_attempt]


# F129/A15: compile the complete actual Main log branch, not a mirrored decision.
# The existing structural owner remains mandatory; the qualified owned-stage helper bounds
# compiler/runtime children. All inputs and outputs are public synthetic host fixtures.
def check_boot_reason_log_branch():
    import hashlib
    import json
    import os
    import shutil
    import tempfile
    import time
    from f137_process import run_owned

    main = source("main/main.cpp")
    anchor = "    // Announce the breadcrumb only NOW"
    end_anchor = "    // Resolve WiFi credentials"
    assert main.count(anchor) == main.count(end_anchor) == 1
    start = main.index("    if (", main.index(anchor))
    branch = main[start:main.index(end_anchor, start)]
    guard = "boot_reboot_record.state == tk::RebootReasonState::Present"
    assert branch.count(guard) == 1
    assert branch.count("ESP_LOGW(") == 2
    prefix = r"""#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace tk {
enum class RebootReasonState { Missing, Present, Error };
struct RebootReasonRecord { RebootReasonState state; char text[24]; };
}
struct Log { const char* format; const char* reason; };
static Log logs[4]{};
static unsigned log_count = 0, checks = 0;
static const char* TAG = "fixture";
static void log_warn(const char*, const char* format, const char* reason) {
    if (log_count >= 4) std::exit(2);
    logs[log_count++] = {format, reason};
}
#define ESP_LOGW(tag, ...) log_warn(tag, __VA_ARGS__)
#define CHECK(expr) do { ++checks; if (!(expr)) { \
    std::fprintf(stderr, "f129-main-log CHECK: %s\n", #expr); std::exit(1); } } while (false)
static void actual_main_log(const tk::RebootReasonRecord& boot_reboot_record) {
"""
    suffix = r"""
}
static void error_case(const char* text) {
    tk::RebootReasonRecord record{tk::RebootReasonState::Error, {}};
    std::strcpy(record.text, text); log_count = 0; actual_main_log(record);
    CHECK(log_count == 1);
    CHECK(std::strstr(logs[0].format, "reboot reason unavailable or invalid") != nullptr);
    CHECK(std::strstr(logs[0].format, "restarted deliberately") == nullptr);
    CHECK(std::strcmp(logs[0].reason, text) == 0);
}
int main() {
    error_case("heap:nvs-read-error");
    error_case("heap:nvs-clear-error");
    error_case("heap:nvs-invalid");
    tk::RebootReasonRecord present{tk::RebootReasonState::Present, "heap:2"};
    log_count = 0; actual_main_log(present);
    CHECK(log_count == 1);
    CHECK(std::strstr(logs[0].format, "restarted deliberately because its heap stayed unusable") != nullptr);
    CHECK(std::strstr(logs[0].format, "unavailable or invalid") == nullptr);
    CHECK(std::strcmp(logs[0].reason, "heap:2") == 0);
    tk::RebootReasonRecord missing{tk::RebootReasonState::Missing, "ignored"};
    log_count = 0; actual_main_log(missing);
    CHECK(log_count == 0);
    std::printf("f129 main boot-reason logging: %u checks OK\n", checks);
}
"""
    compiler = shutil.which("c++")
    assert compiler, "F129 actual Main log test requires the host C++ compiler"
    parent = ROOT / ".tmp"
    parent.mkdir(exist_ok=True)
    assert not parent.is_symlink()
    output = Path(tempfile.mkdtemp(prefix="f129-main-log-", dir=parent))
    deadline = time.monotonic() + 60
    env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "LANG": "C", "LC_ALL": "C",
           "PYTHONDONTWRITEBYTECODE": "1", "TMPDIR": str(output)}
    protected = {ROOT / p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest() for p in (
        "main/main.cpp", "main/reboot_reason.hpp", "test/test_log_origin_contract.py",
        "test/f137_process.py")}
    stages, quiet = [], True
    def unchanged():
        assert all(hashlib.sha256(p.read_bytes()).hexdigest() == h for p, h in protected.items())
    def command(label, argv, expected, out_bytes, err_bytes, seconds):
        nonlocal quiet
        unchanged()
        prior = quiet
        if not prior:
            raise RuntimeError("F129 prior child quiescence unconfirmed; retain fixture")
        quiet = False
        rec = run_owned(argv, cwd=ROOT, env=env, output=output, label=label,
                        deadline=deadline, stage_seconds=seconds)
        assert type(rec) is dict and rec.get("processesQuiescent") is True
        assert rec.get("processStarted") is True and rec.get("error") is None
        assert rec.get("timeout") is False and rec.get("exit") == expected
        assert rec.get("label") == label and rec.get("command") == argv
        cleanup = rec.get("cleanup")
        assert type(cleanup) is dict and cleanup.get("reaped") is True
        assert cleanup.get("groupAbsent") is True and cleanup.get("errors") == []
        for stream, expected_bytes in (("stdout", out_bytes), ("stderr", err_bytes)):
            path = output / (label + "." + stream)
            assert rec.get(stream) == str(path)
            data = path.read_bytes()
            assert rec[stream + "Sha256"] == hashlib.sha256(data).hexdigest()
            assert rec["retainedBytes"][stream] == len(data)
            assert data == expected_bytes
        assert json.loads((output / (label + ".stage.json")).read_bytes()) == rec
        unchanged()
        stages.append(rec)
        quiet = prior and True  # only after every retained receipt/input check and append
    expected_error = (b'f129-main-log CHECK: std::strstr(logs[0].format, '
                      b'"reboot reason unavailable or invalid") != nullptr\n')
    try:
        for label, body, code, stdout, stderr in (
            ("baseline", branch, 0, b"f129 main boot-reason logging: 17 checks OK\n", b""),
            ("omit-present-guard", branch.replace(guard, "true", 1), 1, b"", expected_error),
            ("restored", branch, 0, b"f129 main boot-reason logging: 17 checks OK\n", b""),
        ):
            assert quiet
            unchanged()
            cpp, binary = output / (label + ".cpp"), output / (label + ".bin")
            cpp.write_text(prefix + body + suffix, encoding="utf-8")
            command(label + "-compile", [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    str(cpp), "-o", str(binary)], 0, b"", b"", 20)
            command(label + "-run", [str(binary)], code, stdout, stderr, 5)
        assert (output / "baseline.cpp").read_bytes() == (output / "restored.cpp").read_bytes()
        unchanged()
        (output / "result.json").write_text(json.dumps({"scope": "actual Main logging with passive host record/logger only",
            "stages": stages, "sourceUnchanged": True, "processesQuiescent": quiet,
            "result": "PASS_HOST_LOG_BRANCH_ONLY"}, indent=2) + "\n")
        print("F129 actual Main logging: baseline/guard-omission/restored complete; retained " + str(output))
    except BaseException:
        # No canonical source was modified. Keep private fixtures/receipts even when cleanup or
        # receipt processing is unknown; failure never authorizes another child or restoration.
        print("F129 actual Main logging failed; retained " + str(output))
        raise


check_boot_reason_log_branch()

print("log-origin integration contract: PASS")
