#!/usr/bin/env python3
"""Extract the shipped response adapters/handlers for the locked-cJSON host gate.

Only platform I/O and vehicle/storage calls are replaced by test doubles. No copied handler logic.
"""
import pathlib
import hashlib
import json
import re
import runpy
import sys

root = pathlib.Path(__file__).resolve().parents[1]
helpers = runpy.run_path(str(root / "test/test_runtime_boundary_contract.py"))
body_in = helpers["function_body_in"]
scrub = helpers["scrub_cpp_preserving_layout"]
balanced = helpers["balanced_end"]


def function(file, name):
    source = (root / "main" / file).read_text()
    body = body_in(re.sub(r"\[\[.*?\]\]", lambda match: " " * len(match.group()), source), name)
    pattern = (r"^(?:\[\[gnu::noinline\]\] )?(?:static )?"
               r"(?:esp_err_t|void|unsigned|bool|BrowserClockStatus|const char\*|cJSON\*) " + re.escape(name) + r"\(")
    found = list(re.finditer(pattern, source, re.M))
    if len(found) != 1:
        raise AssertionError(f"ambiguous extracted production function: {file}:{name}")
    start = found[0].start()
    opening = source.index("{", found[0].end())
    if not source[opening:].startswith(body):
        raise AssertionError(f"function extraction does not match parsed body: {name}")
    return source[start:opening + len(body)]


def structure(file, name):
    source = (root / "main" / file).read_text()
    match = re.search(r"^struct " + re.escape(name) + r"\s*\{", source, re.M)
    if not match:
        raise AssertionError(f"missing production adapter {name}")
    opening = source.index("{", match.start())
    closing = balanced(scrub(source), opening, "{", "}", name)
    if source[closing + 1] != ";":
        raise AssertionError(f"unexpected adapter terminator: {name}")
    return source[match.start():closing + 2]


fixture = root / "test/fixtures/idf_ota_state_query.c"
metadata = json.loads(fixture.with_suffix(".json").read_text())
if hashlib.sha256(fixture.read_bytes()).hexdigest() != metadata["fixtureSha256"]:
    raise AssertionError("pinned SDK OTA-query fixture changed; review and bind its provenance")
if hashlib.sha256((root / "esp-idf-toolchain.txt").read_bytes()).hexdigest() != metadata["toolchainFileSha256"]:
    raise AssertionError("pinned SDK changed; refresh the actual OTA-query fixture before acceptance")

output = pathlib.Path(sys.argv[1])
owner = (root / "main/ota_update.hpp").read_text()
owner = owner[owner.index("class OtaHealthCommitGuard {"):]
output.with_name("config_commit_owner.generated.hpp").write_text(owner)
runtime = (root / "main/ota_update.cpp").read_text()
runtime = runtime[runtime.index("OtaHealthCommitGuard::OtaHealthCommitGuard()"):
                  runtime.index("// Short per-target image suffix")]
reference_source = (root / "main/ota_update.cpp").read_text()
reference_name = "ordinary_recovery_reference_ready_owned"
reference_body = body_in(reference_source, reference_name)
reference_match = re.search(r"^static bool ordinary_recovery_reference_ready_owned\([^;{}]*\) noexcept \{",
                            reference_source, re.M)
if not reference_match:
    raise AssertionError("actual owned recovery-reference definition missing")
reference_open = reference_source.index("{", reference_match.start())
reference = reference_source[reference_match.start():reference_open + len(reference_body)]
runtime = function("ota_update.cpp", "finish_operation") + "\n" + reference + "\n" + runtime
output.with_name("ota_health_task.generated.hpp").write_text(function("main.cpp", "ota_health_gate_task") + "\n")
output.with_name("config_commit_runtime.generated.hpp").write_text(runtime)

portal = [structure("provisioning.cpp", "PortalSaveContext")]
for name in ("url_decode", "form_field", "portal_save_send", "portal_save_result",
             "portal_save_continue", "save_post_impl", "save_post"):
    portal.append(function("provisioning.cpp", name))
output.with_name("config_portal.generated.hpp").write_text("\n\n".join(portal) + "\n")

context_path = pathlib.Path(sys.argv[1]).with_name("effect_reply_context.generated.hpp")
context_path.write_text(structure("http_handlers.hpp", "HttpGuardContext") + "\n" +
                        structure("http_handlers.hpp", "GuardedReq") + "\n")
crash_owner = (root / "main/diag_crash.hpp").read_text()
output.with_name("crash_diag_owner.generated.hpp").write_text(crash_owner[crash_owner.index("namespace tk {"):])
parts = ["// Generated from shipped production source; do not edit.\n"]
for name in ("http_status_text_",):
    parts.append(function("http_common.cpp", name))
for name in ("RestJsonTransport", "EffectJsonTransport"):
    parts.append(structure("http_common.cpp", name))
for name in ("send_json", "send_effect_json", "send_effect_exception", "run_effect_continuation", "finish_config_save", "send_config_reply", "make_response"):
    parts.append(function("http_common.cpp", name))
for name in ("apply_mqtt_submission_", "apply_syslog_submission_", "save_wifi_submission_", "apply_wifi_submission_", "handle_gen_keys", "handle_set_time", "handle_set_vin", "handle_set_mqtt", "handle_set_syslog", "handle_set_wifi", "handle_set_ota"):
    parts.append(function("http_config.cpp", name))
for name in ("apply_browser_time_query_", "parse_pr_param_", "handle_ota_check", "handle_ota_update"):
    parts.append(function("http_ota.cpp", name))
for name in ("coredump_error", "coredump_erase_reply", "handle_coredump", "handle_crash_dismiss"):
    parts.append(function("http_status.cpp", name))
for name in ("request_vin_matches", "json_bool_arg", "send_command_result", "handle_command"):
    parts.append(function("http_api.cpp", name))
parts.append(function("mcp_server.cpp", "handle_tools_call_"))
parts.append(function("http_server.cpp", "handle_all"))
pathlib.Path(sys.argv[1]).write_text("\n\n".join(parts) + "\n")
