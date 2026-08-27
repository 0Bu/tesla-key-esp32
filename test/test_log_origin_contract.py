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
commands = source("main/vehicle_commands.cpp")
ble_client = source("main/ble_client.cpp")
ble_header = source("main/ble_client.hpp")
http_api = source("main/http_api.cpp")
http_config = source("main/http_config.cpp")

# Public entry points deliberately require a provenance choice. A default would let a new
# unattended caller silently become an unthrottled foreground error source again.
assert "get_vehicle_status(VehicleStatusResult& out, tk::ConnectOrigin origin," in header
assert "pair(tk::ConnectOrigin origin, int timeout_ms = 30000)" in header
status_decl = re.search(r"bool get_vehicle_status\([^;]+;", header, re.DOTALL)
pair_decl = re.search(r"bool pair\([^;]+;", header, re.DOTALL)
assert status_decl and "origin =" not in status_decl.group(0)
assert pair_decl and "origin =" not in pair_decl.group(0)

# Every automatic enrolment/health call is background; every blocking HTTP call is foreground.
assert pairing.count("get_vehicle_status(st, tk::ConnectOrigin::Background") == 2
assert pairing.count("pair(tk::ConnectOrigin::Background") == 1
assert "get_vehicle_status(vs, tk::ConnectOrigin::Foreground)" in http_api
assert "pair(tk::ConnectOrigin::Foreground)" in http_config
assert "timeout_ms, tk::ConnectOrigin::Background," in pairing  # paired health probe

# Per-probe summaries must not bypass the lower connect rate limiter.
assert 'ESP_LOGD(TAG, "auto-pair: car not reachable over BLE' in pairing
assert 'ESP_LOGW(TAG, "auto-pair: car not reachable over BLE' not in pairing
assert 'if (origin == tk::ConnectOrigin::Foreground)' in pairing
assert 'ESP_LOGD(TAG, "background pair request not confirmed")' in pairing
assert "await_completion_(completion, generation, deadline, name.c_str(), timeout_policy)" in commands
assert 'ESP_LOGD(TAG, "background \'%s\' timed out' in commands
assert "tk::CompletionTimeoutPolicy::BackgroundHealth" in pairing
assert "tk::CompletionTimeoutPolicy::ExpectedSilent" in pairing
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
assert 'ESP_LOGD(TAG, "background pair deadline exhausted waiting for another request")' in pairing
pair_mutex = pairing[pairing.index("tk::SemGuard cmd_guard(command_mutex_"):
                     pairing.index("// Check only after taking command_mutex_", pairing.index("tk::SemGuard cmd_guard(command_mutex_"))]
assert "if (origin == tk::ConnectOrigin::Foreground)" in pair_mutex
for message in (
    "auto-pair: not paired — requesting key enrolment from the car…",
    "auto-pair: enrolment request attempted — place a Tesla NFC keycard",
    "auto-pair: not registered yet — place a Tesla NFC keycard",
):
    assert f'ESP_LOGI(TAG, "{message}' in pairing
    assert f'ESP_LOGD(TAG, "{message}' in pairing
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
):
    assert f'ESP_LOGD(TAG, "{raw_message}' in ble_client
    assert not re.search(rf'ESP_LOG[IEW]\(TAG, "{re.escape(raw_message)}', ble_client)

# Fail closed for the whole BLE file: adding/promoting any production-level line requires an
# explicit review here. Repeated attempt detail must never escape by simply using a new message.
production_ble_logs = re.findall(r'ESP_LOG([IEW])\(TAG,\s*"([^"]+)"', ble_client)
expected_production_ble_logs = {
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
}
assert set(production_ble_logs) == expected_production_ble_logs
assert len(production_ble_logs) == len(expected_production_ble_logs)
verbose_start = ble_client.index("if (diag_verbose())")
verbose_end = ble_client.index("if (on_rx_data_ &&", verbose_start)
assert 'ESP_LOGI(TAG, "RX notify len=%u: %s"' in ble_client[verbose_start:verbose_end]

# Healthy periodic heap samples are INFO; watchdog threshold/recovery messages remain separate.
heap_format = 'HEAP free=%u largest_block=%u min_free=%u internal_largest=%u'
assert f'ESP_LOGI(TAG, "{heap_format}"' in telemetry
assert f'ESP_LOGW(TAG, "{heap_format}"' not in telemetry

# A link established by the telemetry loop must close the previous failure streak too.
already_connected = commands.index("if (ble_->is_connected()) {")
connect_attempt = commands.index('ble_->connect("")', already_connected)
assert "tk::connect_ok_note(connect_fail_);" in commands[already_connected:connect_attempt]

print("log-origin integration contract: PASS")
