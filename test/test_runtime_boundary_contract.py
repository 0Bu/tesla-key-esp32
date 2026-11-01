#!/usr/bin/env python3
"""Fail closed when a new C/RTOS callback boundary appears without review."""

from __future__ import annotations

import hashlib
import runpy
import re
from collections import Counter
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "main"
EXPECTED_COMPONENT_COMPILE_OPTIONS = (
    "-std=gnu++17",
    "-Werror=format",
    "-Werror=return-type",
    "-Werror=unused-result",
)
REVIEWED_LOCAL_CODE_SUFFIXES = (
    ".def",
    ".h",
    ".hh",
    ".hpp",
    ".hxx",
    ".inc",
    ".inl",
    ".ipp",
    ".tpp",
)


def cmake_source_paths(main: Path) -> dict[str, Path]:
    """Resolve the literal C++ SRCS shipped by the main IDF component, fail closed otherwise."""
    cmake = (main / "CMakeLists.txt").read_text(encoding="utf-8")
    import importlib.util
    contract_path = main.parent / "scripts/config_repair_contract.py"
    if "TESLA_REPAIR" in cmake:
        spec = importlib.util.spec_from_file_location("runtime_config_repair_contract", contract_path)
        if spec is None or spec.loader is None:
            raise AssertionError("missing maintenance source contract")
        maintenance = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(maintenance)
        cmake = maintenance.normal_component_text(main.parent, cmake)
    # The boundary inventory is only authoritative while this file has one closed source surface.
    # CMake commands are case-insensitive; reject any second attachment/include/subdirectory seam
    # instead of trying to emulate CMake and accidentally inventory only the visible SRCS block.
    cmake_code = re.sub(r"#[^\n]*", "", cmake)
    allowed_commands = {
        "idf_component_register",
        "target_compile_options",
        "add_custom_command",
        "add_custom_target",
        "add_dependencies",
    }
    commands = {
        command.lower()
        for command in re.findall(r"(?im)^[ \t]*([A-Za-z_]\w*)\s*\(", cmake_code)
    }
    unsupported = sorted(commands - allowed_commands)
    if unsupported:
        raise AssertionError(
            f"main/CMakeLists.txt: unsupported source-surface CMake commands: {unsupported}"
        )
    if len(re.findall(r"\bidf_component_register\s*\(", cmake_code, re.IGNORECASE)) != 1:
        raise AssertionError("main/CMakeLists.txt: expected exactly one component registration")
    if re.search(
        r"\b(?:target_sources|add_subdirectory|include|aux_source_directory|"
        r"set_property|set_target_properties|cmake_language)\s*\(",
        cmake_code,
        re.IGNORECASE,
    ):
        raise AssertionError("main/CMakeLists.txt: secondary source-adding CMake seam is forbidden")
    option_blocks = re.findall(
        r"\btarget_compile_options\s*\((.*?)\)", cmake_code, re.IGNORECASE | re.DOTALL
    )
    if len(option_blocks) != 1:
        raise AssertionError("main/CMakeLists.txt: expected exactly one compile-options block")
    option_tokens = option_blocks[0].split()
    expected_option_tokens = [
        "${COMPONENT_LIB}",
        "PRIVATE",
        *EXPECTED_COMPONENT_COMPILE_OPTIONS,
    ]
    if option_tokens != expected_option_tokens:
        raise AssertionError(
            "main/CMakeLists.txt: component compile options must be exactly "
            f"{expected_option_tokens!r}, got {option_tokens!r}"
        )
    blocks = re.findall(
        r"idf_component_register\s*\(\s*SRCS\s*(.*?)\n\s*"
        r"(?:INCLUDE_DIRS|PRIV_INCLUDE_DIRS|REQUIRES|PRIV_REQUIRES|EMBED_FILES)\b",
        cmake,
        re.DOTALL,
    )
    if len(blocks) != 1:
        raise AssertionError("main/CMakeLists.txt: expected one literal idf_component_register SRCS block")
    result: dict[str, Path] = {}
    for raw_line in blocks[0].splitlines():
        line = raw_line.split("#", 1)[0].strip()
        if not line:
            continue
        match = re.fullmatch(r'"([^"\n]+\.(?:cpp|cc|cxx))"', line)
        if not match:
            raise AssertionError(f"main/CMakeLists.txt: non-literal/unsupported C++ SRCS entry: {line!r}")
        relative = Path(match.group(1))
        if relative.is_absolute() or ".." in relative.parts:
            raise AssertionError(f"main/CMakeLists.txt: source escapes main/: {relative}")
        path = main / relative
        key = relative.as_posix()
        if key in result:
            raise AssertionError(f"main/CMakeLists.txt: duplicate source: {key}")
        if not path.is_file() or path.is_symlink():
            raise AssertionError(f"main/CMakeLists.txt: source is missing or a symlink: {key}")
        result[key] = path
    if not result:
        raise AssertionError("main/CMakeLists.txt: empty C++ source inventory")
    return result


SOURCE_PATHS = cmake_source_paths(MAIN)
SOURCES = {name: path.read_text(encoding="utf-8") for name, path in SOURCE_PATHS.items()}


def normalize_preprocessor_text(text: str) -> str:
    """Apply the source transformations that can change directive token adjacency.

    C/C++ removes escaped newlines before replacing comments with whitespace.  The inventory
    scanner must do the same before looking for ``# include``; otherwise a comment or line splice
    can hide a local source fragment from the runtime/cJSON boundary review.
    """
    text = re.sub(r"\\\r?\n", "", text)
    output: list[str] = []
    position = 0
    state = "code"
    quote = ""
    while position < len(text):
        char = text[position]
        following = text[position + 1] if position + 1 < len(text) else ""
        if state == "line-comment":
            if char == "\n":
                output.append(char)
                state = "code"
            else:
                output.append(" ")
        elif state == "block-comment":
            if char == "*" and following == "/":
                output.extend((" ", " "))
                position += 1
                state = "code"
            else:
                output.append("\n" if char == "\n" else " ")
        elif state == "quoted":
            output.append(char)
            if char == "\\" and following:
                output.append(following)
                position += 1
            elif char == quote:
                state = "code"
        elif char == "/" and following == "/":
            output.extend((" ", " "))
            position += 1
            state = "line-comment"
        elif char == "/" and following == "*":
            output.extend((" ", " "))
            position += 1
            state = "block-comment"
        else:
            output.append(char)
            if char in {'"', "'"}:
                quote = char
                state = "quoted"
        position += 1
    return "".join(output)


def local_code_paths(main: Path, source_paths: dict[str, Path]) -> list[Path]:
    """Inventory every reviewed local header/template fragment, including alternate include roots."""
    paths = {
        path for path in main.rglob("*")
        if path.is_file() and not path.is_symlink()
        and path.suffix.lower() in REVIEWED_LOCAL_CODE_SUFFIXES
    }
    queue = list(source_paths.values()) + list(paths)
    seen = set(queue)
    include = re.compile(r'^\s*#\s*include\s*["<]([^">\n]+)[">]', re.MULTILINE)
    while queue:
        owner = queue.pop()
        raw_text = owner.read_text(encoding="utf-8")
        if re.search(r"\?\?[=/'()!<>-]", raw_text):
            raise AssertionError(f"unsupported trigraph sequence in local source: {owner}")
        text = normalize_preprocessor_text(raw_text)
        if re.search(r"^\s*(?:%:|\?\?=)", text, re.MULTILINE):
            raise AssertionError(f"unsupported preprocessor directive spelling: {owner}")
        if re.search(r"^\s*#\s*(?:include_next|import)\b", text, re.MULTILINE):
            raise AssertionError(f"unsupported local include directive: {owner}")
        if re.search(r'^\s*#\s*include\s+(?!["<])\S', text, re.MULTILINE):
            raise AssertionError(f"non-literal local include directive: {owner}")
        for name in include.findall(text):
            relative = Path(name)
            candidates = (owner.parent / relative, main / relative)
            target = next((candidate for candidate in candidates if candidate.is_file()), None)
            if target is None:
                continue  # generated/SDK/component include outside main/
            if target.is_symlink():
                raise AssertionError(f"local code include is a symlink: {target}")
            target = target.resolve()
            try:
                target.relative_to(main.resolve())
            except ValueError as exc:
                raise AssertionError(f"local include escapes main/: {owner}: {name}") from exc
            if target not in seen:
                seen.add(target)
                paths.add(target)
                queue.append(target)
    return sorted(paths)


HEADER_PATHS = local_code_paths(MAIN, SOURCE_PATHS)
MAIN_CODE = {
    **SOURCES,
    **{path.relative_to(MAIN).as_posix(): path.read_text(encoding="utf-8") for path in HEADER_PATHS},
}
ALL_CODE = "\n".join(MAIN_CODE.values())


EXPECTED_TASKS = {
    "publisher_task",
    "ota_health_gate_task",
    "heap_supervisor_task",
    "ota_check_task",
    "ota_task",
    "ota_network_task",
    "syslog_task",
    "dns_task",
    "loop_task_fn_",
    "auto_pair_task_fn_",
    "display_task",
    "net_watchdog_task",
    "led_task",
    "nimble_host_task",
}

EXPECTED_CALLBACKS = {
    "ota_check_after_http_work",
    'parser_begin',
    'parser_field',
    'parser_value',
    'parser_headers',
    'parser_body',
    'parser_complete',
    'parser_chunk',
    'parser_chunk_done',
    "on_sync_cb",
    "on_reset_cb",
    "gap_event_cb",
    "svc_disc_cb",
    "chr_disc_cb",
    "dsc_disc_cb",
    "subscribe_write_cb",
    "mtu_exchange_cb",
    "scan_timeout_cb",
    "form_get",
    "save_post",
    "wifi_event_handler",
    "eth_event_handler",
    "mqtt_event_handler",
    "on_mqtt_probe",
    "ping_net_on_end",
    "ping_syslog_on_end",
    "sntp_sync_time",
    "handle_all",
    "diag_vprintf_",
    "app_main",
    "ble_link_event_cb_",
    "ble_rx_event_cb_",
}

# These boundaries contain their own catch-all in the registered function/task body. The gate
# inspects the body rather than trusting this list alone, so deleting the catch makes it red.
CONTAINED = {
    "ota_check_after_http_work",
    "publisher_task",
    "heap_supervisor_task",
    "ota_check_task",
    "ota_task",
    "ota_network_task",
    "syslog_task",
    "loop_task_fn_",
    "auto_pair_task_fn_",
    "display_task",
    "led_task",
    "ota_health_gate_task",
    "net_watchdog_task",
    "on_sync_cb",
    "on_reset_cb",
    "scan_timeout_cb",
    "form_get",
    "wifi_event_handler",
    "eth_event_handler",
    "save_post",
    "mqtt_event_handler",
    "sntp_sync_time",
    "handle_all",
    "app_main",
}

# Thin C adapters whose immediately delegated C++ method owns the catch-all. Their exact target is
# recorded so a future adapter cannot be waved through merely because its name ends in `_cb`.
DELEGATES_TO_CONTAINED = {
    "gap_event_cb": "on_gap_event",
    "svc_disc_cb": "on_svc_disc",
    "chr_disc_cb": "on_chr_disc",
    "dsc_disc_cb": "on_dsc_disc",
    "subscribe_write_cb": "on_subscribe_write",
    "mtu_exchange_cb": "on_mtu_exchange",
}

DELEGATE_ALLOWED_CALLS = {
    "gap_event_cb": {"on_gap_event"},
    "svc_disc_cb": {"ble_client_instance", "on_svc_disc"},
    "chr_disc_cb": {"ble_client_instance", "on_chr_disc"},
    "dsc_disc_cb": {"ble_client_instance", "on_dsc_disc"},
    "subscribe_write_cb": {"ble_client_instance", "on_subscribe_write"},
    "mtu_exchange_cb": {"ble_client_instance", "on_mtu_exchange"},
}

# Reviewed exceptions are deliberately restricted to fixed-buffer/C/atomic paths. Adding another
# exception is a code-review event because it weakens the default rule that C frames need a catch.
REVIEWED_NON_THROWING = {
    'parser_begin': "fixed bounded public HTTP parser adapter; complete transitive source closed separately",
    'parser_field': "fixed bounded public HTTP parser adapter; complete transitive source closed separately",
    'parser_value': "fixed bounded public HTTP parser adapter; complete transitive source closed separately",
    'parser_headers': "fixed bounded public HTTP parser adapter; complete transitive source closed separately",
    'parser_body': "fixed bounded public HTTP parser adapter; complete transitive source closed separately",
    'parser_complete': "fixed bounded public HTTP parser adapter; complete transitive source closed separately",
    'parser_chunk': "fixed bounded public HTTP parser adapter; complete transitive source closed separately",
    'parser_chunk_done': "fixed bounded public HTTP parser adapter; complete transitive source closed separately",
    "dns_task": "fixed stack buffers, atomic stop flag, socket C calls and the noexcept "
                "pure DNS reply builder only",
    "nimble_host_task": "NimBLE run/deinit trampoline only",
    "on_mqtt_probe": "plain-data probe verdict plus semaphore give only",
    "ping_net_on_end": "mapped noexcept own-task sample plus exact persistent ping completion delegate",
    "ping_syslog_on_end": "mapped noexcept own-task sample plus exact persistent ping completion delegate",
    "diag_vprintf_": "fixed-buffer/C/queue-only",
    "ble_link_event_cb_": "POD cast plus fixed queue delegate only",
    "ble_rx_event_cb_": "POD cast plus fixed queue delegate only",
}

REVIEWED_ALLOWED_CALLS = {
    'parser_begin': {'self', 'reset_fields'},
    'parser_field': {'finish_field', 'min', 'size', 'fill', 'self', 'reject'},
    'parser_value': {'equal_ci', 'min', 'size', 'memcpy', 'self', 'data', 'reject'},
    'parser_headers': {'http_parser_pause', 'self', 'finish_field', 'reject'},
    'parser_body': {'min', 'sink_', 'self', 'reject'},
    'parser_complete': {'http_parser_pause', 'self', 'finish_field', 'reject'},
    'parser_chunk': {'self', 'reject'},
    'parser_chunk_done': {'self'},
    "dns_task": {
        "socket", "ESP_LOGW", "vTaskDelete", "htonl", "htons", "bind", "close",
        "recvfrom", "memcpy", "sendto", "setsockopt", "load", "store", "build_captive_dns_reply",
        "vTaskDelay", "pdMS_TO_TICKS",
    },
    "nimble_host_task": {"nimble_port_run", "nimble_port_freertos_deinit"},
    "on_mqtt_probe": {"xSemaphoreGive"},
    "ping_net_on_end": {"ping_probe_on_end"},
    "ping_syslog_on_end": {"ping_probe_on_end"},
    "diag_vprintf_": {
        "va_copy", "vsnprintf", "va_end", "diag_append_", "syslog_send", "vprintf",
        "diag_mqtt_uri_error_", "diag_forward_fixed_", "load", "previous",
    },
    "ble_link_event_cb_": {"static_cast", "enqueue_ble_link_event_"},
    "ble_rx_event_cb_": {"static_cast", "enqueue_ble_rx_event_"},
}

REVIEWED_DELEGATED_HELPERS = {
    "ping_net_on_end": {
        "ping_probe_on_end": {"esp_ping_get_profile", "complete", "xSemaphoreGive"},
    },
    "ping_syslog_on_end": {
        "ping_probe_on_end": {"esp_ping_get_profile", "complete", "xSemaphoreGive"},
    },
    "diag_vprintf_": {
        "diag_mqtt_uri_error_": {"diag_mqtt_uri_error"},
        "diag_forward_fixed_": {"va_start", "previous", "load", "vprintf", "va_end"},
        "diag_append_": {"g", "diag_library_capture_content_end", "diag_capture_byte", "load", "fetch_add"},
        "syslog_send": {"sv", "find", "memcpy", "load", "xQueueSend", "fetch_add"},
    },
    "ble_link_event_cb_": {
        "enqueue_ble_link_event_": {"store", "portENTER_CRITICAL", "push_link", "portEXIT_CRITICAL"},
    },
    "ble_rx_event_cb_": {
        "enqueue_ble_rx_event_": {"store", "portENTER_CRITICAL", "esp_timer_get_time", "push_rx", "portEXIT_CRITICAL"},
    },
}


def call_arguments(text: str, function: str) -> list[list[str]]:
    """Return top-level arguments for every call to `function` (comments are harmless here)."""
    calls: list[list[str]] = []
    for match in re.finditer(rf"\b{re.escape(function)}\s*\(", text):
        start = text.find("(", match.start())
        depth = 1
        quote = ""
        escaped = False
        arg_start = start + 1
        args: list[str] = []
        pos = start + 1
        while pos < len(text) and depth:
            char = text[pos]
            if quote:
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == quote:
                    quote = ""
            elif char in ('"', "'"):
                quote = char
            elif char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
                if depth == 0:
                    args.append(text[arg_start:pos].strip())
                    break
            elif char == "," and depth == 1:
                args.append(text[arg_start:pos].strip())
                arg_start = pos + 1
            pos += 1
        if depth == 0:
            calls.append(args)
    return calls


STATIC_CALLBACK = re.compile(r"(?:[A-Za-z_]\w*::)*[A-Za-z_]\w*")


def normalize_registered_callback(expression: str, site: str) -> str:
    """Return a statically named callback, accepting C's optional address-of spelling only."""
    value = expression.strip()
    if value.startswith("&"):
        value = value[1:].strip()
    if not STATIC_CALLBACK.fullmatch(value):
        raise AssertionError(
            f"{site}: callback must be a statically named function, got {expression!r}"
        )
    return value.rsplit("::", 1)[-1]


def task_inventory(text: str) -> set[str]:
    code = scrub_cpp(text)
    task_arg = {
        "xTaskCreate": 0,
        "xTaskCreatePinnedToCore": 0,
        "xTaskCreateStatic": 0,
        "xTaskCreateStaticPinnedToCore": 0,
        "nimble_port_freertos_init": 0,
    }
    task_like = {
        name for name in called_functions(code)
        if name.startswith("xTaskCreate") or name == "nimble_port_freertos_init"
    }
    unsupported = sorted(task_like - set(task_arg))
    if unsupported:
        raise AssertionError(f"unreviewed task-registration APIs: {unsupported}")
    found: set[str] = set()
    for function, index in task_arg.items():
        for args in call_arguments(code, function):
            if index >= len(args) or not args[index]:
                continue  # prose such as "xTaskCreate()" is not a registration
            found.add(normalize_registered_callback(args[index], function))
    return found


def assignment_callbacks(text: str, pattern: str, site: str) -> set[str]:
    found: set[str] = set()
    for match in re.finditer(pattern, text):
        found.add(normalize_registered_callback(match.group(1), site))
    return found


def callback_inventory(text: str) -> set[str]:
    code = scrub_cpp(text)
    found: set[str] = set()

    # Discover from registration calls, not function-name suffixes. A newly named callback at any
    # of these C APIs therefore appears as unreviewed instead of evading the inventory regex.
    callback_arg = {
        "esp_mqtt_client_register_event": 2,
        "esp_event_handler_instance_register": 2,
        "esp_event_handler_register": 2,
        "sntp_set_time_sync_notification_cb": 0,
        "ble_gap_disc": 3,
        "ble_gap_connect": 4,
        "ble_gattc_disc_svc_by_uuid": 2,
        "ble_gattc_disc_all_chrs": 3,
        "ble_gattc_disc_all_dscs": 3,
        "ble_gattc_write_flat": 4,
        "ble_gattc_exchange_mtu": 1,
        "esp_log_set_vprintf": 0,
        "esp_ipc_call": 1,
        "esp_ipc_call_blocking": 1,
        "esp_register_shutdown_handler": 0,
        "httpd_queue_work": 1,
        "esp_eth_update_input_path": 1,
        "gpio_isr_handler_add": 1,
        "uart_isr_register": 1,
        "esp_intr_alloc": 2,
        "esp_intr_alloc_intrstatus": 4,
        "timer_isr_callback_add": 2,
        "timer_isr_register": 2,
        "xTimerCreate": 4,
        "xTimerCreateStatic": 4,
        "xTimerPendFunctionCall": 0,
        "xTimerPendFunctionCallFromISR": 0,
        "tcpip_callback": 0,
        "tcpip_try_callback": 0,
        "tcpip_callback_with_block": 0,
    }
    # These are synchronous tesla-ble std::function setters/helpers, not C ABI callback
    # boundaries. Their inline adapters are separately pinned to named helpers below.
    reviewed_cpp_callback_calls = {
        "set_charge_state_callback",
        "set_climate_state_callback",
        "set_closures_state_callback",
        "set_drive_state_callback",
        "set_message_callback",
        "set_tire_pressure_state_callback",
        "set_vehicle_status_callback",
    }
    reviewed_struct_callback_calls = {
        # The callback itself lives in a typed arguments/callback struct and is inventoried by
        # the exact field assignments below.
        "esp_timer_create",
        "gptimer_register_event_callbacks",
    }
    callback_like = {
        name for name in called_functions(code)
        if (
            re.search(r"(?:^|_)(?:set|add)_.*(?:callback|_cb)$", name)
            or name.startswith("tcpip_") and "callback" in name
            or name.startswith("esp_ipc_call")
            or (
                "register" in name
                and (
                    name.startswith(("esp_", "httpd_", "ble_", "nimble_"))
                    or "callback" in name
                    or "handler" in name
                )
            )
            or name.startswith("xTimerCreate")
            or name.startswith("xTimerPendFunctionCall")
            or re.search(r"(?:^|_)(?:isr|intr).*(?:add|alloc|register)$", name)
        )
    }
    known_callback_calls = set(callback_arg) | reviewed_cpp_callback_calls | reviewed_struct_callback_calls | {
        "set_connected_cb",
        "set_rx_data_cb",
        # The actual function is carried by httpd_uri_t.handler and inventoried below.
        "httpd_register_uri_handler",
        # Cleanup names the already-inventoried handler but does not register a new callback.
        "esp_event_handler_unregister",
    }
    unsupported = sorted(callback_like - known_callback_calls)
    if unsupported:
        raise AssertionError(f"unreviewed callback-registration APIs: {unsupported}")
    for function, index in callback_arg.items():
        for args in call_arguments(code, function):
            if index < len(args) and args[index]:
                found.add(normalize_registered_callback(args[index], function))

    # BleClient owns the actual NimBLE-host boundary but accepts two project callbacks through a
    # typed C-style setter. Require an exact statically named function: lambdas/std::function or a
    # runtime-selected callback are rejected before the semantic fixed-call audit below.
    for function, declaration_arg in {
        "set_connected_cb": "ConnectedCb cb",
        "set_rx_data_cb": "RxDataCb cb",
    }.items():
        registrations = 0
        for args in call_arguments(code, function):
            if not args or not args[0]:
                continue
            if args[0].strip() == declaration_arg:
                continue
            found.add(normalize_registered_callback(args[0], function))
            registrations += 1
        if registrations > 1:
            raise AssertionError(
                f"{function}: expected at most one statically named registration, got {registrations}"
            )

    # Registration through callback-bearing structs/designated initializers.
    found.update(
        assignment_callbacks(
            code,
            r"\.(?:[A-Za-z_]\w*(?:handler|callback|_cb)|handler|callback)\s*=\s*([^,;}\n]+)",
            "callback-bearing struct field",
        )
    )
    found.update(
        assignment_callbacks(
            code,
            r"\.(?:on_alarm)\s*=\s*([^,;}\n]+)",
            "gptimer callback field",
        )
    )
    found.update(
        assignment_callbacks(code, r"\b\w+\.callback\s*=\s*([^,;}\n]+)", ".callback")
    )
    found.update(
        assignment_callbacks(
            code,
            r"\bble_hs_cfg\.(?:sync_cb|reset_cb)\s*=\s*([^,;}\n]+)",
            "ble_hs_cfg callback",
        )
    )
    ping_callbacks = assignment_callbacks(
        code,
        r"\b\w+\.(?:on_ping_success|on_ping_timeout|on_ping_end)\s*=\s*([^,;}\n]+)",
        "esp_ping callback",
    )
    if "on_end" in ping_callbacks:
        # A named parameter is not an inventory exemption. Resolve it only through the exact
        # typed/default/null-guard/two-owner/delegate program; unknown aliases still fail closed.
        ping_callbacks.remove("on_end")
        ping_callbacks.update(f135_fixed_ping_targets(text))
    found.update(ping_callbacks)

    for table in re.findall(r"static const http_parser_settings settings = \{(.*?)\};", code, re.S):
        entries=[item.strip() for item in table.split(",")]
        if len(entries)!=10:raise AssertionError("public HTTP parser callback table shape changed")
        for callback in entries:
            if callback!="nullptr":found.add(normalize_registered_callback(callback,"http_parser settings"))

    # ESP-IDF invokes app_main through a C startup frame even though it is not registered by a
    # source-level function call.
    if re.search(r'extern\s+"C"\s+void\s+app_main\s*\(', text):
        found.add("app_main")
    # The documented IDF weak extension is a C callback boundary without registration.
    if re.search(r'extern\s+"C"\s+void\s+sntp_sync_time\s*\(', text):
        found.add("sntp_sync_time")
    return found


def function_body_in(text: str, name: str) -> str:
    definition = re.compile(
        rf"^[ \t]*(?:\[\[gnu::noinline\]\][ \t]+)?(?:extern\s+\"C\"\s+)?(?:static\s+)?"
        rf"(?:[A-Za-z_]\w*(?:::\w+)*(?:[<>,*&]+)?[ \t]+)+"
        rf"(?:[A-Za-z_]\w*::)*{re.escape(name)}\s*\([^;{{}}]*\)\s*"
        rf"(?:const\s*)?(?:noexcept\s*)?\{{",
        re.MULTILINE,
    )
    match = definition.search(text)
    if match:
        start = text.find("{", match.start())
        depth = 0
        for pos in range(start, len(text)):
            if text[pos] == "{":
                depth += 1
            elif text[pos] == "}":
                depth -= 1
                if depth == 0:
                    return text[start : pos + 1]
    raise AssertionError(f"cannot locate boundary definition: {name}")


def function_definition_names(text: str) -> set[str]:
    """Discover ordinary free/static and qualified definitions in one reviewed source file."""
    definition = re.compile(
        r"^[ \t]*(?:\[\[gnu::noinline\]\][ \t]+)?(?:extern\s+\"C\"\s+)?(?:static\s+)?"
        r"(?:[A-Za-z_]\w*(?:::\w+)*(?:[<>,*&]+)?[ \t]+)+"
        r"(?:[A-Za-z_]\w*::)*([A-Za-z_]\w*)\s*\([^;{}]*\)\s*"
        r"(?:const\s*)?(?:noexcept\s*)?\{",
        re.MULTILINE,
    )
    names = [match.group(1) for match in definition.finditer(scrub_cpp_preserving_layout(text))]
    duplicates = sorted(name for name, count in Counter(names).items() if count > 1)
    if duplicates:
        raise AssertionError(f"ambiguous duplicate local function definitions: {duplicates}")
    return set(names)


def function_body(name: str) -> str:
    for text in MAIN_CODE.values():
        try:
            return function_body_in(text, name)
        except AssertionError:
            pass
    raise AssertionError(f"cannot locate boundary definition: {name}")


def require_exact(label: str, actual: set[str], expected: set[str]) -> None:
    missing = sorted(expected - actual)
    unreviewed = sorted(actual - expected)
    if missing or unreviewed:
        raise AssertionError(f"{label} inventory drift: missing={missing}, unreviewed={unreviewed}")


def require_mutation_rejected(label: str, check) -> None:
    try:
        check()
    except AssertionError:
        return
    raise AssertionError(f"{label} mutation passed unexpectedly")


def scrub_cpp_preserving_layout(text: str) -> str:
    """Remove comments/literals without changing offsets or hiding newlines/braces."""
    return CPP_NOISE.sub(
        lambda match: "".join("\n" if char == "\n" else " " for char in match.group(0)),
        text,
    )


def skip_space(text: str, position: int, end: int) -> int:
    while position < end and text[position].isspace():
        position += 1
    return position


def balanced_end(text: str, opening: int, left: str, right: str, label: str) -> int:
    if opening >= len(text) or text[opening] != left:
        raise AssertionError(f"{label}: expected {left!r}")
    depth = 0
    for position in range(opening, len(text)):
        if text[position] == left:
            depth += 1
        elif text[position] == right:
            depth -= 1
            if depth == 0:
                return position
    raise AssertionError(f"{label}: unterminated {left}{right} group")


# F135 has a closed source program, not a general permission to put work before a C boundary.
F135_TASK_CONTEXTS = {
    "publisher_task": "Mqtt", "ota_health_gate_task": "OtaGate",
    "heap_supervisor_task": "HeapSupervisor", "ota_check_task": "OtaCheck",
    "ota_task": "OtaWriter", "syslog_task": "Syslog", "dns_task": "CaptiveDns",
    "loop_task_fn_": "Vehicle", "auto_pair_task_fn_": "AutoPair",
    "display_task": "Display", "net_watchdog_task": "NetWatchdog",
    "led_task": "Led", "nimble_host_task": "NimbleHost",
    "ota_network_task": "OtaNetwork",
}
F135_CALLBACK_CONTEXTS = {
    "ota_check_after_http_work": "Httpd",
    "on_sync_cb": "NimbleHost", "on_reset_cb": "NimbleHost",
    "gap_event_cb": "NimbleHost", "svc_disc_cb": "NimbleHost",
    "chr_disc_cb": "NimbleHost", "dsc_disc_cb": "NimbleHost",
    "subscribe_write_cb": "NimbleHost", "mtu_exchange_cb": "NimbleHost",
    "scan_timeout_cb": "BleScanTimer",
    "form_get": "CaptiveHttp", "save_post": "CaptiveHttp",
    "wifi_event_handler": "WifiEvent", "eth_event_handler": "EthEvent",
    "mqtt_event_handler": "MqttEvent", "on_mqtt_probe": "MqttProbeEvent",
    "sntp_sync_time": "SntpSync", "handle_all": "Httpd", "app_main": "Main",
    "ping_net_on_end": "PingNet", "ping_syslog_on_end": "PingSyslog",
    "parser_begin": "OtaParser",
    "parser_field": "OtaParser",
    "parser_value": "OtaParser",
    "parser_headers": "OtaParser",
    "parser_body": "OtaParser",
    "parser_complete": "OtaParser",
    "parser_chunk": "OtaParser",
    "parser_chunk_done": "OtaParser",

}


def f135_statement_pattern(statement: str) -> str:
    # Keep C++ token separation, permit formatting only. No wildcard executable prefix.
    return r"\s+".join(re.escape(word) for word in statement.split())


def f135_unwrap_sampling_prefix(name: str, body: str) -> str:
    """Remove only one exact audited noexcept prefix, leaving all old guard code visible."""
    code = scrub_cpp_preserving_layout(body)
    if name in F135_TASK_CONTEXTS:
        context = F135_TASK_CONTEXTS[name]
        statements = [f"tk::stack_watch_sample(tk::StackWatch::{context});",
                      f"tk::StackWatchScope f135_task_exit(tk::StackWatch::{context});"]
    elif name in F135_CALLBACK_CONTEXTS:
        context = F135_CALLBACK_CONTEXTS[name]
        statements = [f"tk::StackWatchScope f135_callback_exit(tk::StackWatch::{context});"]
        if name == "app_main":
            statements.append("tk::stack_watch_sample(tk::StackWatch::Main);")
    elif name == "diag_vprintf_":
        statements = ["struct F135SafeTaskSampleOnExit { ~F135SafeTaskSampleOnExit() noexcept { diag_stack_sample_safe_(); } } f135_callback_exit;"]
    else:
        return body
    pattern = r"\{\s*" + r"\s*".join(f135_statement_pattern(s) for s in statements)
    match = re.match(pattern, code)
    if match is None:
        raise AssertionError(f"{name}: exact mapped F135 noexcept sampling prefix missing/changed")
    # Only braces/declarations/calls above are removed; a throwing helper before/after them remains.
    return "{" + body[match.end():]


def f135_reviewed_body(name: str, body: str) -> str:
    normalized = f135_unwrap_sampling_prefix(name, body)
    # These are the only fixed nonthrowing task bodies with further instrumentation. Preserve
    # exact counts so omissions, aliasing, wrong lanes or new unreviewed sampling do not disappear.
    if name in {"dns_task", "nimble_host_task"}:
        context, expected = {"dns_task": ("CaptiveDns", 4),
                             "nimble_host_task": ("NimbleHost", 1)}[name]
        code = scrub_cpp_preserving_layout(normalized)
        token = f"tk::stack_watch_sample(tk::StackWatch::{context});"
        matches = list(re.finditer(f135_statement_pattern(token), code))
        if len(matches) != expected:
            raise AssertionError(f"{name}: exact F135 terminal sample count drifted")
        for match in reversed(matches):
            normalized = normalized[:match.start()] + " " * (match.end() - match.start()) + normalized[match.end():]
        if name == "dns_task":
            token = "tk::StackWatchScope f135_iteration_exit(tk::StackWatch::CaptiveDns);"
            code = scrub_cpp_preserving_layout(normalized)
            matches = list(re.finditer(f135_statement_pattern(token), code))
            if len(matches) != 1:
                raise AssertionError("dns_task: exact F135 iteration scope count drifted")
            match = matches[0]
            normalized = normalized[:match.start()] + " " * (match.end() - match.start()) + normalized[match.end():]
    return normalized


def f135_fixed_ping_targets(text: str) -> set[str]:
    """Resolve only the reviewed typed parameter, its two actual fixed consumers and wrappers."""
    code = scrub_cpp_preserving_layout(text)
    declaration = ("void (*on_end)(esp_ping_handle_t, void*) = ping_probe_on_end")
    if len(re.findall(f135_statement_pattern(declaration), code)) != 1:
        raise AssertionError("F135 ping: exact typed/default callback parameter missing/changed")
    bindings = re.findall(
        r"\b(\w+)\.(on_ping_success|on_ping_timeout|on_ping_end)\s*=\s*on_end\s*;", code)
    if bindings != [("callbacks", "on_ping_end")]:
        raise AssertionError("F135 ping: typed parameter may bind only the one reviewed end field")
    run = scrub_cpp_preserving_layout(function_body_in(text, "ping_probe_run"))
    null_guard = "if (!control.done || !on_end) return PingProbeResult::Unavailable;"
    if not re.match(r"\{\s*" + f135_statement_pattern(null_guard), run):
        raise AssertionError("F135 ping: actual null refusal must be the first executable statement")
    binding_program = ("esp_ping_callbacks_t callbacks{}; "
                       "callbacks.cb_args = &control.callback_args; "
                       "callbacks.on_ping_end = on_end;")
    if not re.search(f135_statement_pattern(binding_program), run):
        raise AssertionError("F135 ping: callback argument/end binding must be unconditional and adjacent")
    require_before("F135 ping null refusal before cleanup/effect", run,
                   "if (!control.done || !on_end)", "ping_probe_cleanup_completed(control)")
    calls = call_arguments(scrub_cpp(text), "ping_probe_run")
    declarations = [a for a in calls if a and a[0].strip() == "PingProbeControl& control"]
    effects = [a for a in calls if a not in declarations]
    if len(declarations) != 1 or len(effects) != 2:
        raise AssertionError("F135 ping: exactly two actual fixed consumers required")
    found = set()
    for owner, role, control in (("gateway_reachable", "ping_net_on_end", "s_wd"),
                                 ("syslog_ping_host", "ping_syslog_on_end", "s_ping")):
        body = function_body_in(text, owner)
        actual = call_arguments(scrub_cpp(body), "ping_probe_run")
        if len(actual) != 1 or len(actual[0]) != 5 or actual[0][0].strip() != control or actual[0][-1].strip() != role:
            raise AssertionError("F135 ping: fixed owner/role registration drifted: " + owner)
        if actual[0] not in effects:
            raise AssertionError("F135 ping: owner call differs from actual callback closure")
        if not re.search(r"void\s+" + re.escape(role) +
                         r"\s*\(esp_ping_handle_t handle,\s*void\* args\)\s*noexcept\s*\{", code):
            raise AssertionError("F135 ping: actual fixed wrapper must be noexcept: " + role)
        wrapper = f135_unwrap_sampling_prefix(role, function_body_in(text, role))
        if re.sub(r"\s+", "", scrub_cpp(wrapper)) != "{tk::ping_probe_on_end(handle,args);}":
            raise AssertionError("F135 ping: wrapper is not exactly the contained completion delegate")
        found.add(role)
    return found


def require_f135_sampling_programs(parts: dict[str, str]) -> None:
    """Pin the specific transitive noexcept prefix chain; no broad callback/hash exemption."""
    def same(actual: str, expected: str, label: str) -> None:
        if re.sub(r"\s+", "", scrub_cpp(actual)) != re.sub(r"\s+", "", scrub_cpp(expected)):
            raise AssertionError("F135 sampling source program drifted: " + label)
    header = parts["main/stack_watch.hpp"]
    marker = "class StackWatchScope {"
    if header.count(marker) != 1:
        raise AssertionError("F135 scope declaration missing/ambiguous")
    begin = header.index("{", header.index(marker));end = balanced_end(scrub_cpp_preserving_layout(header), begin, "{", "}", "F135 scope")
    same(header[begin:end+1], '''{
public:
    explicit StackWatchScope(StackWatch which) noexcept : which_(which) {}
    ~StackWatchScope() noexcept { stack_watch_sample(which_); }
    StackWatchScope(const StackWatchScope&) = delete;
    StackWatchScope& operator=(const StackWatchScope&) = delete;
private:
    StackWatch which_;
}''', "scope constructor/destructor/one-field lifetime")
    if not re.search(r"void\s+stack_watch_sample\s*\(StackWatch which\)\s*noexcept\s*;", header):
        raise AssertionError("F135 sampler public declaration must be noexcept")
    shell = parts["main/stack_watch.cpp"]
    for token in ("std::atomic<uint32_t> s_samples[stack_sample::kWordCount]{};",
                  "static_assert(sizeof(s_samples) == 24);",
                  "static_assert(std::atomic<uint32_t>::is_always_lock_free);"):
        if token not in shell:
            raise AssertionError("F135 atomic storage contract missing: " + token)
    if not re.search(r"void\s+stack_watch_sample\s*\(StackWatch which\)\s*noexcept", shell):
        raise AssertionError("F135 actual sampler must be noexcept")
    same(function_body_in(shell, "stack_watch_sample"), '''{
    const auto slot = static_cast<unsigned>(which);
    if (slot >= kStackContextCount) return;
    const uint32_t free_bytes = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
    auto& word = s_samples[slot / stack_sample::kLanesPerWord];
    stack_sample::publish_minimum(word, slot % stack_sample::kLanesPerWord, free_bytes);
}''', "own-task HWM and single atomic publication")
    codec = parts["main/logic/stack_sample_codec.hpp"]
    same(function_body_in(codec, "publish_minimum"), '''{
    uint32_t previous = word.load(std::memory_order_relaxed);
    for (;;) {
        const uint32_t next = merge(previous, lane, free_bytes);
        if (next == previous || word.compare_exchange_strong(previous, next,
                std::memory_order_relaxed, std::memory_order_relaxed)) return;
    }
}''', "strong CAS/recompute path")
    require_reviewed_nonthrowing("merge", function_body_in(codec, "merge"), {"encode"}, delegated=True)
    require_reviewed_nonthrowing("encode", function_body_in(codec, "encode"), {"static_cast"}, delegated=True)
    log = parts["main/diag_log.cpp"]
    if not re.search(r"void\s+diag_stack_sample_safe_\s*\(\)\s*noexcept", log):
        raise AssertionError("F135 logger sampler must be noexcept")
    same(function_body_in(log, "diag_stack_sample_safe_"), '''{
    if (xPortInIsrContext() || !xPortCanYield()) return;
    if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING ||
        xTaskGetCurrentTaskHandle() == nullptr) return;
    tk::stack_watch_sample(tk::StackWatch::DiagLogCallback);
}''', "logger ISR/critical/scheduler/current-task refusal")
    f135_unwrap_sampling_prefix("diag_vprintf_", function_body_in(log, "diag_vprintf_"))
    for path,text in parts.items():
        if re.search(r"^\s*#\s*define\s+(?:StackWatchScope|stack_watch_sample|uxTaskGetStackHighWaterMark|publish_minimum|merge|encode)\b",
                     normalize_preprocessor_text(text), re.MULTILINE):
            raise AssertionError("F135 sampler macro substitution refused: " + path)


def f135_program_parts() -> dict[str, str]:
    # Include every closed local-code input so a transitive header cannot substitute the
    # reviewed sampling chain through a macro outside its immediate owner files.
    return {"main/" + path: text for path, text in MAIN_CODE.items()}


def self_test_f135_programs(parts: dict[str, str]) -> None:
    require_f135_sampling_programs(parts)
    for path,old,new in (
        ("main/stack_watch.hpp", "noexcept : which_(which) {}", "noexcept : which_(which) { throwing_helper(); }"),
        ("main/stack_watch.hpp", "stack_watch_sample(which_);", "throwing_helper();"),
        ("main/stack_watch.cpp", "uxTaskGetStackHighWaterMark(nullptr)", "uxTaskGetStackHighWaterMark(foreign_task)"),
        ("main/stack_watch.cpp", "if (slot >= kStackContextCount) return;", ""),
        ("main/logic/stack_sample_codec.hpp", "compare_exchange_strong", "compare_exchange_weak"),
        ("main/logic/stack_sample_codec.hpp", "const uint32_t next = merge(previous, lane, free_bytes);", "const uint32_t next = 0;"),
        ("main/diag_log.cpp", "xPortInIsrContext() || !xPortCanYield()", "false"),
        ("main/diag_log.cpp", "xTaskGetCurrentTaskHandle() == nullptr", "false"),
    ):
        if parts[path].count(old) != 1:
            raise AssertionError("F135 program canary source missing/ambiguous: " + old)
        changed = {**parts,path:parts[path].replace(old,new,1)}
        require_mutation_rejected("F135 transitive program " + old,
                                  lambda changed=changed:require_f135_sampling_programs(changed))
    for name in sorted(set(F135_TASK_CONTEXTS) | set(F135_CALLBACK_CONTEXTS)):
        body = function_body(name)
        normalized = f135_unwrap_sampling_prefix(name,body)
        # Restoring a deleted/commented/false prefix must be required, and arbitrary code on
        # either side of the exact prefix remains visible to the original boundary validator.
        require_mutation_rejected("F135 missing mapped prefix " + name,
                                  lambda name=name,normalized=normalized:f135_unwrap_sampling_prefix(name,normalized))
        require_mutation_rejected("F135 executable before prefix " + name,
                                  lambda name=name,body=body:f135_unwrap_sampling_prefix(name,"{ throwing_helper();"+body[1:]))


def self_test_f135_ping_targets(header: str, net: str, syslog: str) -> None:
    def checked(h: str, n: str, s: str) -> None:
        f135_fixed_ping_targets("\n".join((h, n, s)))
    checked(header, net, syslog)
    fixtures = [
        ("typed-null-refusal-omitted", header.replace(" || !on_end", "", 1), net, syslog),
        ("typed-null-refusal-commented", header.replace("if (!control.done || !on_end) return PingProbeResult::Unavailable;", "/* if (!control.done || !on_end) return PingProbeResult::Unavailable; */", 1), net, syslog),
        ("typed-null-refusal-disabled", header.replace("if (!control.done || !on_end)", "if (false) if (!control.done || !on_end)", 1), net, syslog),
        ("typed-end-binding-disabled", header.replace("callbacks.on_ping_end = on_end;", "if (false) { callbacks.on_ping_end = on_end; }", 1), net, syslog),
        ("typed-common-bypass", header.replace("callbacks.on_ping_end = on_end;", "callbacks.on_ping_end = ping_probe_on_end;", 1), net, syslog),
        ("typed-extra-binding", header.replace("callbacks.on_ping_end = on_end;", "callbacks.on_ping_end = on_end; callbacks.on_ping_success = on_end;", 1), net, syslog),
        ("net-role-substitution", header, net.replace("pdMS_TO_TICKS(2000), ping_net_on_end", "pdMS_TO_TICKS(2000), ping_syslog_on_end", 1), syslog),
        ("net-completion-omitted", header, net.replace("tk::ping_probe_on_end(handle, args);", "", 1), syslog),
        ("net-wrapper-can-throw", header, net.replace("ping_net_on_end(esp_ping_handle_t handle, void* args) noexcept", "ping_net_on_end(esp_ping_handle_t handle, void* args)", 1), syslog),
        ("extra-default-owner", header, net + "\nvoid f135_extra(PingProbeControl& c, const esp_ping_config_t& cfg) { ping_probe_run(c, cfg, 1, 1); }\n", syslog),
    ]
    for label, h, n, s in fixtures:
        if (h, n, s) == (header, net, syslog):
            raise AssertionError("F135 ping canary did not alter its actual program: " + label)
        require_mutation_rejected("F135 fixed ping " + label,
                                  lambda h=h,n=n,s=s:checked(h,n,s))

# F137: exact reviewed programs, not general task/callback exemptions.
F137_MQTT_CLEANUP_PROGRAM = '  bool reported = false;\n  while (!watchdog.release()) {\n      if (!reported) {\n          reported = true;\n          try { ESP_LOGE(TAG, "mqtt publisher parked: watchdog removal unconfirmed"); }\n          catch (...) {}\n      }\n      watchdog.feed(true);\n      tk::stack_watch_sample(tk::StackWatch::Mqtt); // owned park after diagnostics/feed\n      vTaskDelay(pdMS_TO_TICKS(1000));\n  }\n  try { ESP_LOGE(TAG, "mqtt publisher stopped after boundary failure"); } catch (...) {}\n  tk::stack_watch_sample(tk::StackWatch::Mqtt); // deepest terminal path before self-delete\n  vTaskDelete(nullptr);\n}'
F137_RESET_PROGRAM = '{\n    // Required host-down/sticky reset publication precedes fallible diagnostics.\n    try {\n        if (g_instance) g_instance->on_reset();\n    } catch (...) {\n        if (g_instance) g_instance->report_callback_fault(false, "reset");\n    }\n    try {\n        ESP_LOGW(TAG, "NimBLE host reset, reason=%d", reason);\n    } catch (...) {\n        if (g_instance) g_instance->report_callback_fault(false, "reset diagnostic");\n    }\n}'
F137_REPORTER_PROGRAM = '{\n    try {\n        auto& faults = timer_context ? timer_callback_faults_ : host_callback_faults_;\n        tk::report_iteration_fault(faults,\n            static_cast<std::uint64_t>(esp_timer_get_time()) / 1000u,\n            [site](std::uint32_t suppressed) {\n                ESP_LOGW(TAG, "BLE %s callback failed (suppressed=%u)", site,\n                         static_cast<unsigned>(suppressed));\n            });\n    } catch (...) {}\n}'
F137_NET_TASK_PROGRAM = '{\n    try {\n      tk::LinkWatch watch{};\n      tk::GatewayIdentity watched_identity{};\n      tk::GatewayBaseline baselines[2]{};\n      tk::IterationFaultLog faults{};\n      for (;;) {\n        tk::StackWatchScope f135_iteration_exit(tk::StackWatch::NetWatchdog);\n        vTaskDelay(pdMS_TO_TICKS(kWdPeriodS * 1000));\n        try {\n        // When the link already knows it is down, the transport\'s own reconnect path owns\n        // recovery — there is nothing to detect (the ghost case is link=up by definition) and\n        // logging every period would fill the 16 KB /diag ring across a long router outage.\n        const bool up = net_is_up();\n        GatewaySample sample;\n        if (up) sample = gateway_reachable();\n        if (!up || !sample.usable) {\n            // Not measurable, or the transport changed during the probe: neither a reply nor a\n            // failure. Drop the failure streak — it described a transport that may be gone.\n            watch = tk::LinkWatch{};\n            watched_identity = {};\n            continue;\n        }\n        // A failure streak belongs to one transport generation; a switch starts a new streak.\n        if (sample.identity != watched_identity) {\n            watch = tk::LinkWatch{};\n            watched_identity = sample.identity;\n        }\n        const bool gw = up && sample.reachable;\n\n        // Keep proof for each leased transport across route failover; a new lease or gateway\n        // starts cold when that transport becomes active again. That includes the lease this\n        // watchdog ends itself: net_recover() re-associates, so the next lease must answer ICMP\n        // once before another recovery is allowed (docs/ARCHITECTURE.md, "State is per\n        // transport and lease").\n        const auto baseline_index = sample.identity.transport == NetLink::Eth ? 1 : 0;\n        const bool gw_ever = baselines[baseline_index].observe(sample.identity, sample.replied);\n\n        switch (tk::watch_step(watch, up, gw, gw_ever)) {\n            case tk::WatchAction::Idle:\n                break;\n            case tk::WatchAction::Wait:\n                ESP_LOGW(TAG, "watchdog: no LAN connectivity (%d/%d, link=up)",\n                         watch.fails, tk::kWatchFailsToRecover);\n                break;\n            case tk::WatchAction::NoBaseline:\n                ESP_LOGW(TAG, "watchdog: gateway has never answered ICMP — not forcing a "\n                              "re-establish");\n                break;\n            case tk::WatchAction::Recover:\n                net_recover(sample.identity);\n                break;\n        }\n        } catch (...) {\n            // An observation or a partially attempted recovery is uncertain.\n            // Require a new matching positive gateway reply before another reset.\n            watch = tk::LinkWatch{};\n            watched_identity = {};\n            baselines[0] = {};\n            baselines[1] = {};\n            try {\n                tk::report_iteration_fault(faults,\n                    static_cast<std::uint64_t>(esp_timer_get_time()) / 1000u,\n                    [](std::uint32_t suppressed) {\n                        ESP_LOGW(TAG, "network watchdog iteration failed; proof cleared (suppressed=%u)",\n                                 static_cast<unsigned>(suppressed));\n                    });\n            } catch (...) {}\n        }\n      }\n    } catch (...) {\n        try { ESP_LOGE(TAG, "network watchdog boundary failed; stopping task"); } catch (...) {}\n        tk::stack_watch_sample(tk::StackWatch::NetWatchdog);\n        vTaskDelete(nullptr);\n    }\n}'
F137_WDT_HEADER_PROGRAM = '#pragma once\n\n#include <esp_task_wdt.h>\n\nnamespace tk {\n\n// Current-task ownership only. Explicit release must precede FreeRTOS self-deletion;\n// vTaskDelete(nullptr) does not unwind a live C++ scope.\nclass TaskWatchdogSubscription {\npublic:\n    TaskWatchdogSubscription() = default;\n    TaskWatchdogSubscription(const TaskWatchdogSubscription&) = delete;\n    TaskWatchdogSubscription& operator=(const TaskWatchdogSubscription&) = delete;\n    TaskWatchdogSubscription(TaskWatchdogSubscription&&) = delete;\n    TaskWatchdogSubscription& operator=(TaskWatchdogSubscription&&) = delete;\n\n    bool subscribe() noexcept {\n        // This one consumer may subscribe once; never silently rearm a retired owner.\n        if (started_ || released_) return false;\n        started_ = true;\n        try {\n            active_ = esp_task_wdt_add(nullptr) == ESP_OK ||\n                      esp_task_wdt_status(nullptr) == ESP_OK;\n        } catch (...) {\n            active_ = false;\n        }\n        return active_;\n    }\n\n    void feed(bool uncertain_subscription = false) noexcept {\n        if (released_ || (!active_ && !uncertain_subscription)) return;\n        try { (void)esp_task_wdt_reset(); } catch (...) {}\n    }\n\n    bool release() noexcept {\n        if (released_) return true;\n        try {\n            const esp_err_t result = esp_task_wdt_delete(nullptr);\n            // In the pinned SDK these mean confirmed removal, no current-task\n            // entry, or an uninitialized TWDT with no entries, respectively.\n            if (result != ESP_OK && result != ESP_ERR_NOT_FOUND &&\n                result != ESP_ERR_INVALID_STATE) return false;\n            active_ = false;\n            released_ = true;\n            return true;\n        } catch (...) {\n            return false;\n        }\n    }\n\n    ~TaskWatchdogSubscription() { (void)release(); }\n\nprivate:\n    bool started_{false};\n    bool active_{false};\n    bool released_{false};\n};\n\n}  // namespace tk\n'


def f137_program(text: str) -> str:
    """Whitespace/comments/literal contents are irrelevant; executable token order is exact."""
    return re.sub(r"\s+", "", scrub_cpp_preserving_layout(text))


def f137_containment_code(name: str, code: str) -> str:
    if name == "publisher_task":
        prefix = re.match(r"\{\s*tk::TaskWatchdogSubscription\s+watchdog\s*;", code)
        marker = "bool reported = false;"
        if not prefix or code.count(marker) != 1:
            raise AssertionError("F137 MQTT: only the reviewed trivial guard precedes outer try")
        at = code.index(marker)
        if f137_program(code[at:]) != f137_program(F137_MQTT_CLEANUP_PROGRAM):
            raise AssertionError("F137 MQTT: explicit release/park/feed/self-delete program drift")
        # The existing generic checker must still verify the entire publisher catch chain.
        return "{" + code[prefix.end():at] + "}"
    if name == "on_reset_cb":
        if f137_program(code) != f137_program(F137_RESET_PROGRAM):
            raise AssertionError("F137 reset: essential publication then contained visible diagnostic required")
        opening = code.index("{", code.index("try"))
        end_try = balanced_end(code, opening, "{", "}", "F137 reset first try")
        catch_open = code.index("{", code.index("catch", end_try + 1))
        end_catch = balanced_end(code, catch_open, "{", "}", "F137 reset first catch")
        # Exact full-program equality above covered the second contained diagnostic; no arbitrary
        # prefix/suffix gets admitted. The original checker still validates the essential chain.
        return code[:end_catch + 1] + "}"
    return code


def require_f137_source_contract(parts: dict[str, str]) -> None:
    mqtt, net, ble, header, klass = (parts[k] for k in ("mqtt", "net", "ble", "wdt", "class"))
    require_catch_all("publisher_task", function_body_in(mqtt, "publisher_task"))
    require_catch_all("on_reset_cb", function_body_in(ble, "on_reset_cb"))
    if mqtt.count('#include "task_watchdog_subscription.hpp"') != 1:
        raise AssertionError("F137 MQTT: actual checked current-task helper include required")
    # This small one-consumer header has no unreviewed extra calls, members, entry points or
    # error-code authority. Its exact executable form is independently exercised by host tests.
    if f137_program(header) != f137_program(F137_WDT_HEADER_PROGRAM):
        raise AssertionError("F137 WDT: one-shot current-task removal/absence authority drift")
    if f137_program(f135_unwrap_sampling_prefix("net_watchdog_task", function_body_in(net, "net_watchdog_task"))) != f137_program(F137_NET_TASK_PROGRAM):
        raise AssertionError("F137 net: complete iteration/proof/diagnostic/persistent-owner program drift")
    if f137_program(function_body_in(ble, "BleClient::report_callback_fault")) != f137_program(F137_REPORTER_PROGRAM):
        raise AssertionError("F137 BLE: independent visible bounded host/timer reporter drift")
    class_code = f137_program(klass)
    for member in ("tk::IterationFaultLog host_callback_faults_{};",
                   "tk::IterationFaultLog timer_callback_faults_{};"):
        if class_code.count(f137_program(member)) != 1:
            raise AssertionError("F137 BLE: one process-lifetime state per real callback context required")


def self_test_f137_source_contract(parts: dict[str, str]) -> None:
    require_f137_source_contract(parts)
    cases = (
        ("MQTT explicit release omitted", "mqtt", "while (!watchdog.release()) {", "while (false && !watchdog.release()) {"),
        ("MQTT diagnostic hidden", "mqtt", 'ESP_LOGE(TAG, "mqtt publisher parked:', 'ESP_LOGD(TAG, "mqtt publisher parked:'),
        ("MQTT publish replay in cleanup", "mqtt", "watchdog.feed(true);", "(void)publish_discovery(); watchdog.feed(true);"),
        ("MQTT unprotected prefix", "mqtt", "  tk::TaskWatchdogSubscription watchdog;", "  std::string unprotected; tk::TaskWatchdogSubscription watchdog;"),
        ("MQTT unprotected suffix", "mqtt", "  vTaskDelete(nullptr);", "  vTaskDelete(nullptr); throw 1;"),
        ("WDT unknown cleanup accepted", "wdt", "result != ESP_ERR_INVALID_STATE) return false;", "result != ESP_ERR_INVALID_STATE) return true;"),
        ("WDT foreign task cleanup", "wdt", "esp_task_wdt_delete(nullptr)", "esp_task_wdt_delete(reinterpret_cast<TaskHandle_t>(1))"),
        ("WDT copy authority", "wdt", "TaskWatchdogSubscription(const TaskWatchdogSubscription&) = delete;", "TaskWatchdogSubscription(const TaskWatchdogSubscription&) = default;"),
        ("reset essential delegation omitted", "ble", "if (g_instance) g_instance->on_reset();", "(void)g_instance;"),
        ("reset normal diagnostic before publication", "ble", "if (g_instance) g_instance->on_reset();", 'ESP_LOGW(TAG, "before essential"); if (g_instance) g_instance->on_reset();'),
        ("net stale proof retained", "net", "            baselines[0] = {};\n            baselines[1] = {};", "            (void)baselines;"),
        ("net pending owner discarded", "net", "            watch = tk::LinkWatch{};\n            watched_identity = {};\n            baselines[0] = {};", "            s_wd.session = nullptr; watch = tk::LinkWatch{};\n            watched_identity = {};\n            baselines[0] = {};"),
        ("net quota refilled each iteration", "net", "        vTaskDelay(pdMS_TO_TICKS(kWdPeriodS * 1000));", "        faults = {}; vTaskDelay(pdMS_TO_TICKS(kWdPeriodS * 1000));"),
        ("BLE timer shares host quota", "ble", "timer_context ? timer_callback_faults_ : host_callback_faults_", "timer_context ? host_callback_faults_ : host_callback_faults_"),
        ("BLE missing timer state", "class", "    tk::IterationFaultLog timer_callback_faults_{};", ""),
    )
    for label, owner, old, new in cases:
        if parts[owner].count(old) != 1:
            raise AssertionError("F137 valid-source canary is not unique: " + label)
        changed = {**parts, owner: parts[owner].replace(old, new, 1)}
        require_mutation_rejected(label, lambda value=changed: require_f137_source_contract(value))
    require_f137_source_contract(parts)


def require_catch_all(name: str, body: str) -> None:
    """Require one structural outer try/catch chain to cover the whole boundary body.

    Auditing only known throwing syntax is not fail closed: an ordinary-looking helper call can
    throw today or become throwing tomorrow. Requiring `try` as the first body token and the final
    catch body as the last token makes every executable statement part of the protected region or
    its exception handler, independent of the statement spelling.
    """
    body = f135_unwrap_sampling_prefix(name, body)
    code = f137_containment_code(name, scrub_cpp_preserving_layout(body))
    if name == "handle_all":
        # Only these fixed POD/scalar declarations and the noexcept continuation are outside
        # the effect-aware catch chain. Arbitrary prefix/suffix work still fails closed.
        prefix = r"\{\s*HttpGuardContext context\{req, \{\}\};\s*const GuardedReq rq\{&context\};\s*esp_err_t result = ESP_FAIL;"
        suffix = r"run_effect_continuation\(rq\);\s*return result;\s*\}$"
        if not re.match(prefix, code) or not re.search(suffix, code):
            raise AssertionError("handle_all: only bounded noexcept outcome declarations/continuation may surround catch")
        code = re.sub(prefix, "{", code, count=1)
        code = re.sub(suffix, "}", code, count=1)
    if name == "save_post":
        prefix = r"\{\s*PortalSaveContext context\{\};\s*esp_err_t result = ESP_FAIL;"
        suffix = r"portal_save_continue\(context\);\s*return result;\s*\}$"
        if not re.match(prefix, code) or not re.search(suffix, code):
            raise AssertionError("save_post: only noexcept context/continuation may surround catch")
        code = re.sub(prefix, "{", code, count=1)
        code = re.sub(suffix, "}", code, count=1)
    if len(code) < 2 or code[0] != "{" or code[-1] != "}":
        raise AssertionError(f"{name}: malformed registered C boundary body")
    end = len(code) - 1
    position = skip_space(code, 1, end)
    try_match = re.match(r"try\b", code[position:end])
    if not try_match:
        raise AssertionError(
            f"{name}: outer try must be the first token in the registered C boundary"
        )
    position = skip_space(code, position + try_match.end(), end)
    try_close = balanced_end(code, position, "{", "}", f"{name}: outer try")
    position = skip_space(code, try_close + 1, end)

    catch_count = 0
    catch_all = False
    while position < end:
        catch_match = re.match(r"catch\b", code[position:end])
        if not catch_match:
            raise AssertionError(
                f"{name}: executable/declaration tokens appear after the outer catch chain"
            )
        catch_count += 1
        position = skip_space(code, position + catch_match.end(), end)
        parameters_end = balanced_end(code, position, "(", ")", f"{name}: catch")
        parameters = re.sub(r"\s+", "", code[position + 1 : parameters_end])
        if parameters == "...":
            catch_all = True
        elif catch_all:
            raise AssertionError(f"{name}: catch-all must terminate the outer catch chain")
        position = skip_space(code, parameters_end + 1, end)
        catch_close = balanced_end(code, position, "{", "}", f"{name}: catch body")
        position = skip_space(code, catch_close + 1, end)

    if catch_count == 0 or not catch_all:
        raise AssertionError(f"{name}: outer try has no terminal catch-all")


def require_thin_delegate(name: str, target: str, body: str) -> None:
    """A C adapter may only normalize POD arguments and invoke its contained target once."""
    body = f135_unwrap_sampling_prefix(name, body)
    code = scrub_cpp(body)
    for label, pattern in {
        "try/catch": r"\b(?:try|catch)\b",
        "throw": r"\bthrow\b",
        "new": r"\bnew\b",
        "std::string": r"\bstd::string\b",
        "std::vector": r"\bstd::vector\b",
        "std::function": r"\bstd::function\b",
        "cJSON": r"\bcJSON_[A-Za-z0-9_]+\b",
    }.items():
        if re.search(pattern, code):
            raise AssertionError(f"{name}: delegated C adapter contains forbidden {label}")
    calls = called_functions(body)
    expected = DELEGATE_ALLOWED_CALLS.get(name)
    if expected is None or calls != expected:
        raise AssertionError(
            f"{name}: delegated C adapter call inventory drift: expected={expected}, actual={calls}"
        )
    if len(re.findall(rf"\b{re.escape(target)}\s*\(", code)) != 1:
        raise AssertionError(f"{name}: contained delegate {target} must be invoked exactly once")


CPP_NOISE = re.compile(
    r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'",
    re.DOTALL,
)
CONTROL_CALLS = {"if", "for", "while", "switch", "sizeof", "catch", "return"}


def scrub_cpp(text: str) -> str:
    return CPP_NOISE.sub(" ", text)


def called_functions(body: str) -> set[str]:
    code = scrub_cpp(body)
    calls = {
        match.group(1).rsplit("::", 1)[-1]
        for match in re.finditer(r"(?<![A-Za-z0-9_:])([A-Za-z_]\w*(?:::\w+)*)\s*\(", code)
    }
    return calls - CONTROL_CALLS


def require_reviewed_nonthrowing(
    name: str, body: str, allowed_calls: set[str], *, delegated: bool = False
) -> None:
    body = f135_reviewed_body(name, body)
    code = scrub_cpp(body)
    forbidden = {
        "new": r"\bnew\b",
        "throw": r"\bthrow\b",
        "std::string": r"\bstd::string\b",
        "std::vector": r"\bstd::vector\b",
        "std::function": r"\bstd::function\b",
        "cJSON": r"\bcJSON_\w+",
    }
    for label, pattern in forbidden.items():
        if re.search(pattern, code):
            scope = "delegated helper" if delegated else "reviewed boundary"
            raise AssertionError(f"{name}: {scope} contains forbidden {label}")
    unknown = sorted(called_functions(body) - allowed_calls)
    if unknown:
        scope = "delegated helper" if delegated else "reviewed boundary"
        raise AssertionError(f"{name}: {scope} has unreviewed calls {unknown}")


def require_before(label: str, body: str, first: str, second: str) -> None:
    first_pos = body.find(first)
    second_pos = body.find(second)
    if first_pos < 0 or second_pos < 0 or first_pos >= second_pos:
        raise AssertionError(f"{label}: expected {first!r} before {second!r}")


def require_shared_json_reply(label: str, body: str) -> None:
    if "tk::json_http_reply" not in body:
        raise AssertionError(f"{label}: production path bypasses the shared print/OOM seam")


def require_json_materialize_policy(
    label: str, body: str, consequences: dict[str, str]
) -> None:
    """Pin each public parser to the pre-cJSON classifier and its route-specific mapping."""
    if "tk::json_materialize<cJSON>" not in body:
        raise AssertionError(f"{label}: production path bypasses bounded pre-cJSON classification")
    for status, consequence in consequences.items():
        token = f"JsonMaterializeStatus::{status}"
        position = body.find(token)
        if position < 0:
            raise AssertionError(f"{label}: missing {status} mapping")
        # Each branch is deliberately tiny. Keeping the expected consequence close to the
        # classifier prevents a token elsewhere in the handler from satisfying this gate.
        if consequence not in body[position : position + 360]:
            raise AssertionError(
                f"{label}: {status} no longer maps to {consequence!r}"
            )


def require_mcp_call_releases(body: str) -> None:
    error_returns = body.count("return send_rpc_error_")
    releases = body.count("request.reset();")
    if releases != error_returns + 1:
        raise AssertionError(
            "MCP tools/call: every error response plus the success/blocking path needs "
            "its own pre-response request release"
        )


def require_mcp_payload_seams(source: str, payloads: str) -> None:
    required_source = {
        "tk::json_top_level_numeric_id": "raw numeric-id lexeme validation",
        "tk::mcp_json::inspect_request_envelope": "common JSON-RPC envelope validation",
        "envelope.status != tk::mcp_json::RpcRequestStatus::Valid":
            "pre-dispatch envelope verdict",
        "tk::mcp_json::build_tools_list_result": "real tools/list producer",
        "tk::mcp_json::build_vehicle_state_result": "vehicle-state double-print producer",
        "JsonMaterializeStatus::UnsupportedNul": "pre-cJSON embedded-NUL rejection",
    }
    for token, label in required_source.items():
        if token not in source:
            raise AssertionError(f"MCP production bypasses {label}")
    if "cJSON_PrintUnformatted" in source:
        raise AssertionError("MCP production has an unshared inner print path")
    for token in (
        "kMaxStringIdBytes = 64",
        "static_assert(sizeof(RpcId) <= 80",
        "if (!value) return RpcIdStatus::Missing",
        "if (cJSON_IsNull(value)) return RpcIdStatus::Invalid",
        "raw_number.status != JsonRawNumberStatus::ValidInteger",
        "materialized != raw_number.value",
        "has_duplicate_json_keys(object)",
        'std::strcmp(version->valuestring, "2.0") != 0',
        "build_tools_list_result()",
        "build_vehicle_state_result(",
    ):
        if token not in payloads:
            raise AssertionError(f"MCP shared payload contract missing {token!r}")


CJSON_SYMBOL = re.compile(r"\b(cJSON_[A-Za-z0-9_]+)\b")
CJSON_CALL_COUNTS = {
    "http_api.cpp": {
        "cJSON_GetObjectItemCaseSensitive": 2, "cJSON_IsBool": 2,
        "cJSON_IsObject": 1, "cJSON_IsTrue": 2, "cJSON_Parse": 1,
    },
    "http_config.cpp": {
        "cJSON_GetObjectItemCaseSensitive": 2, "cJSON_IsNumber": 1,
        "cJSON_IsObject": 1, "cJSON_IsString": 1, "cJSON_Parse": 1,
    },
    "json_builder.hpp": {
        "cJSON_AddItemToArray": 1, "cJSON_AddItemToObject": 1, "cJSON_CreateArray": 3,
        "cJSON_CreateBool": 2, "cJSON_CreateNull": 2, "cJSON_CreateNumber": 2,
        "cJSON_CreateRaw": 1, "cJSON_CreateObject": 3, "cJSON_CreateString": 2,
        "cJSON_CreateStringReference": 2, "cJSON_Delete": 1, "cJSON_IsArray": 2,
    },
    "http_effect_reply.hpp": {"cJSON_PrintUnformatted": 1},
    "json_http_reply.hpp": {"cJSON_PrintUnformatted": 1, "cJSON_free": 1},
    "json_input_validation.hpp": {
        "cJSON_IsArray": 1, "cJSON_IsNumber": 1, "cJSON_IsObject": 2, "cJSON_IsString": 1,
    },
    "mcp_json_payloads.hpp": {
        "cJSON_GetObjectItemCaseSensitive": 2, "cJSON_IsNull": 1,
        "cJSON_IsNumber": 1, "cJSON_IsObject": 2, "cJSON_IsString": 2,
        "cJSON_PrintUnformatted": 1,
    },
    "mcp_server.cpp": {
        "cJSON_GetObjectItemCaseSensitive": 7, "cJSON_IsArray": 1, "cJSON_IsBool": 1,
        "cJSON_IsNumber": 2, "cJSON_IsObject": 1, "cJSON_IsString": 4,
        "cJSON_IsTrue": 1, "cJSON_Parse": 1,
    },
    "mqtt_json_publish.hpp": {"cJSON_PrintUnformatted": 1, "cJSON_free": 1},
    "ota_update.cpp": {
        "cJSON_Delete": 2, "cJSON_ParseWithLengthOpts": 1,
    },
    "ota_manifest.hpp": {"cJSON_IsObject": 1, "cJSON_IsString": 1},
    "status_json_emitter.hpp": {"cJSON_IsArray": 6},
}


def require_cjson_api_allowlist(files: dict[str, str]) -> None:
    """Fail closed on every new, duplicated, removed or relocated cJSON callsite."""
    actual_by_file = {
        relative: dict(sorted(Counter(CJSON_SYMBOL.findall(scrub_cpp(source))).items()))
        for relative, source in files.items()
        if CJSON_SYMBOL.search(scrub_cpp(source))
    }
    expected_by_file = CJSON_CALL_COUNTS
    if actual_by_file != expected_by_file:
        raise AssertionError(
            "production cJSON API callsite-count inventory drift: "
            f"expected={expected_by_file}, actual={actual_by_file}"
        )


def require_mqtt_production_seams(source: str) -> None:
    code = scrub_cpp(source)
    required = (
        "tk::mqtt_publish_json",
        "tk::mqtt_run_discovery_round",
        "tk::mqtt_run_state_round",
        "tk::mqtt::build_discovery_payload",
        "tk::mqtt::build_charge_payload",
        "tk::mqtt::build_climate_payload",
        "tk::mqtt::build_drive_payload",
        "tk::mqtt::build_tires_payload",
        "tk::mqtt::build_closures_payload",
        "tk::mqtt::build_vehicle_payload",
        "tk::mqtt::build_empty_payload",
        "tk::mqtt::build_device_payload",
    )
    for token in required:
        count = len(re.findall(rf"\b{re.escape(token)}\s*\(", code))
        if count != 1:
            raise AssertionError(
                f"MQTT production must call tested seam {token!r} exactly once, found {count}"
            )


MQTT_PAYLOAD_FACTORY = r"build_[A-Za-z0-9_]+_payload"


def require_mqtt_factory_inventory(payloads: str, production: str, tests: str) -> None:
    """Bind every production payload factory definition to one call and one OOM/success case."""
    payload_code = scrub_cpp(payloads)
    production_code = scrub_cpp(production)
    test_execution_code = scrub_cpp(tests)
    definitions = re.findall(
        rf"\b(?:inline\s+|static\s+|constexpr\s+)*(?:tk::)?JsonOwner\s+"
        rf"({MQTT_PAYLOAD_FACTORY})\s*\([^;{{}}]*\)\s*(?:noexcept\s*)?\{{",
        payload_code,
    )
    calls = re.findall(rf"\btk::mqtt::({MQTT_PAYLOAD_FACTORY})\s*\(", production_code)
    inventory_blocks = re.findall(
        r"\bkProductionPayloadFactoryCases\s*\{\{(.*?)\}\};", tests, re.DOTALL
    )
    if len(inventory_blocks) != 1:
        raise AssertionError("expected one explicit MQTT production payload test inventory")
    cases = re.findall(
        rf'\{{\s*"({MQTT_PAYLOAD_FACTORY})"\s*,\s*([A-Za-z_]\w*)\s*\}}',
        inventory_blocks[0],
    )
    definition_set = set(definitions)
    call_set = set(calls)
    case_names = [name for name, _ in cases]
    case_set = set(case_names)
    if not definition_set:
        raise AssertionError("MQTT payload factory inventory is empty")
    if len(definitions) != len(definition_set):
        raise AssertionError(f"duplicate MQTT payload factory definition: {definitions}")
    if len(calls) != len(call_set):
        raise AssertionError(f"MQTT payload factory must be called exactly once: {calls}")
    if len(case_names) != len(case_set):
        raise AssertionError(f"duplicate MQTT payload factory test case: {case_names}")
    if definition_set != call_set or definition_set != case_set:
        raise AssertionError(
            "MQTT payload factory inventory drift: "
            f"definitions={sorted(definition_set)}, calls={sorted(call_set)}, "
            f"oom_success_cases={sorted(case_set)}"
        )

    # Inventory the live publish callsites as tightly as the factories. This rejects a ninth
    # JsonBuilder/JsonOwner path even when it bypasses the named build_*_payload convention.
    compact_production = re.sub(r"\s+", "", production_code)
    compact_raw_production = re.sub(r"\s+", "", production)
    if re.search(r"\b(?:tk::)?JsonBuilder\s+[A-Za-z_]", production_code):
        raise AssertionError("mqtt_ha.cpp may not construct JsonBuilder outside tested factories")
    owner_declarations = re.findall(r"\b(?:tk::)?JsonOwner\s+[A-Za-z_]\w*", production_code)
    if owner_declarations != ["tk::JsonOwner root", "tk::JsonOwner document"]:
        raise AssertionError(
            f"mqtt_ha.cpp JsonOwner construction/parameter inventory drift: {owner_declarations}"
        )

    expected_pub_json = (
        "pub_json(config_topic,std::move(document))",
        "pub_json(state_topic(tk::mqtt::StateDomain::Charge),tk::mqtt::build_charge_payload(payload))",
        "pub_json(state_topic(tk::mqtt::StateDomain::Climate),tk::mqtt::build_climate_payload(payload))",
        "pub_json(state_topic(tk::mqtt::StateDomain::Drive),tk::mqtt::build_drive_payload(payload))",
        "pub_json(state_topic(tk::mqtt::StateDomain::Tires),tk::mqtt::build_tires_payload(payload))",
        "pub_json(state_topic(tk::mqtt::StateDomain::Closures),tk::mqtt::build_closures_payload(payload))",
        "pub_json(state_topic(tk::mqtt::StateDomain::Vehicle),tk::mqtt::build_vehicle_payload(ss))",
        "pub_json(state_topic(domain),tk::mqtt::build_empty_payload())",
        "pub_json(state_topic(tk::mqtt::StateDomain::Device),tk::mqtt::build_device_payload(payload))",
    )
    for call in expected_pub_json:
        if compact_production.count(call) != 1:
            raise AssertionError(f"MQTT pub_json callsite/argument inventory drift: {call}")
    if len(re.findall(r"\bpub_json\s*\(", production_code)) != len(expected_pub_json) + 1:
        raise AssertionError("MQTT pub_json callsite count drift")

    # The sole added owner is bound to the actual discovery loop. Its tree owns all copies
    # before the input strings/payload leave scope and configuration-topic construction begins.
    # This guards the lifetime reduction; numerical stack acceptance still needs target evidence.
    discovery = re.sub(r"\s+", "", scrub_cpp(function_body_in(production, "publish_discovery_rows")))
    if not discovery.startswith(
        "{usingField=tk::mqtt::DiscoveryTextField;"
        "for(constauto&entry:tk::mqtt::kDiscoveryEntries.packed_rows()){tk::JsonOwnerdocument;{"
        "conststd::stringuid=tk::mqtt::discovery_unique_id(s_node,entry);"
        "conststd::stringvalue_template=tk::mqtt::discovery_value_template(entry);"
    ) or (
        "document=tk::mqtt::build_discovery_payload(payload);}"
        "if(!document)returnfalse;conststd::stringconfig_topic="
        "tk::mqtt::discovery_config_topic(s_prefix,s_node,entry);"
        "if(!pub_json(config_topic,std::move(document)))returnfalse;"
    ) not in discovery:
        raise AssertionError("MQTT discovery owning-document lifetime/handoff drift")

    # Raw text publication is limited to the JSON delegate and retained availability heartbeat.
    # All state/discovery documents must stay behind pub_json and its OOM/retry owner.
    if len(re.findall(r"\bpub\s*\(", production_code)) != 3:  # definition + two reviewed calls
        raise AssertionError("MQTT raw pub() callsite count drift")
    if compact_production.count("pub(publish_topic,payload,retain)") != 1:
        raise AssertionError("MQTT raw pub() delegate argument inventory drift")
    if compact_raw_production.count('pub(s_avail.c_str(),"online")') != 1:
        raise AssertionError("MQTT availability pub() argument inventory drift")

    def body(name: str) -> str:
        match = re.search(
            rf"\b(?:tk::)?JsonOwner\s+{re.escape(name)}\s*\([^;{{}}]*\)\s*\{{",
            tests,
        )
        if not match:
            raise AssertionError(f"MQTT payload fixture definition missing: {name}")
        start = tests.find("{", match.start())
        depth = 0
        quote = ""
        escaped = False
        for pos in range(start, len(tests)):
            char = tests[pos]
            if quote:
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == quote:
                    quote = ""
                continue
            if char in ('"', "'"):
                quote = char
            elif char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    return tests[start : pos + 1]
        raise AssertionError(f"unterminated MQTT payload fixture: {name}")

    for factory, fixture in cases:
        fixture_calls = re.findall(
            rf"\btk::mqtt::({MQTT_PAYLOAD_FACTORY})\s*\(", scrub_cpp(body(fixture))
        )
        if fixture_calls != [factory]:
            raise AssertionError(
                f"MQTT payload case {factory!r} miswired to {fixture!r}: {fixture_calls}"
            )

    for token in (
        "for (const PayloadFactoryCase& factory : kProductionPayloadFactoryCases)",
        "exhaust_build_and_print_allocations(factory.production_name, factory.build);",
        "for (size_t nth = 1; nth <= all_allocations; ++nth)",
        "CHECK(build_and_publish(build, success))",
    ):
        if token not in test_execution_code:
            raise AssertionError(f"MQTT payload OOM/success execution seam missing: {token!r}")


def require_wifi_rollback_status_cache(main_source: str, producer: str) -> None:
    boot = function_body_in(main_source, "app_main")
    seed = "tk::cfg_init_wifi_rollback_status(cfg_blob.wifi_rolled_back);"
    require_before("WiFi rollback cache after verified boot load", boot,
                   "load_config_blob_verified(config_store, cfg_blob);", seed)
    require_before("WiFi rollback cache before HTTP startup", boot,
                   seed, "http_server_start(vehicle, config_store)")
    if "in.wifi_rolled_back = tk::cfg_wifi_rolled_back();" not in producer:
        raise AssertionError("/status must consume the current committed WiFi rollback cache")
    for forbidden in ("tk::cfg_load(", "tk::cfg_load_state(", "tk::ConfigBlob "):
        if forbidden in producer:
            raise AssertionError("/status must not reload allocating configuration snapshots")


def require_status_production_seams(producer: str, handler: str, emitter_header: str) -> None:
    # Review the actual two-step production path, not stale tokens in the gather frame.
    producer_code = re.sub(r"\s+", "", scrub_cpp(producer))
    delegate = "returntk::emit_status_object(in);"
    if producer_code.count(delegate) != 1 or not producer_code.endswith(delegate + "}"):
        raise AssertionError("/status gather must return the tested emitter helper exactly once")
    if "stack_watch_snapshot(" in producer_code:
        raise AssertionError("/status gather must not duplicate the cached snapshot")
    header_code = scrub_cpp_preserving_layout(emitter_header)
    signature = (r"\[\[gnu::noinline\]\]\s+inline\s+cJSON\s*\*\s+emit_status_object"
                 r"\s*\(\s*status::Inputs\s*&\s*in\s*\)\s*\{")
    if len(re.findall(signature, header_code)) != 1:
        raise AssertionError("/status actual emitter helper must retain its noinline Inputs reference seam")
    helper = function_body_in(emitter_header, "emit_status_object")
    expected = """{
        in.stack_context_samples = stack_watch_snapshot();
        JsonBuilder json;
        StatusJsonEmitter emitter(json);
        status::emit_status(in, emitter);
        return emitter.release();
    }"""
    # Complete executable body fixes one snapshot before allocation, sticky builder ownership,
    # the actual model/key loop and the sole release. Extra/reordered effects fail closed.
    if re.sub(r"\s+", "", scrub_cpp(helper)) != re.sub(r"\s+", "", scrub_cpp(expected)):
        raise AssertionError("/status actual snapshot/emitter helper program differs")
    for token in ("build_status_object", "send_json"):
        if token not in handler:
            raise AssertionError(f"/status handler bypasses tested production seam {token!r}")


VEHICLE_DATA_SEAMS = (
    "tk::StatusJsonEmitter", "tk::emit_vehicle_charge_state", "e.release()",
    "strchr(req->uri, '?')", 'tk::parse_vehicle_data_endpoints(query ? query + 1 : "")',
    "endpoints.empty() || endpoints.has_unsupported", "static_assert(kQueryBufBytes == 128",
    "endpoints.charge_state && g_vehicle->get_charge_state(cs)",
    "if (endpoints.charge_state)", "if (endpoints.climate_state)", "if (endpoints.drive_state)",
    "bool dr_ok = false;",
    "cl_ok = emit_cached_vehicle_climate(e)", "dr_ok = emit_cached_vehicle_drive(e)",
    "const bool ok = tk::vehicle_data_available(endpoints, cs_ok, cl_ok, dr_ok)",
    'e.boolean("result", ok)', "return send_json(req, ok ? 200 : 503, e.release())",
)
CLIMATE_CACHE_SEAMS = (
    "g_vehicle->get_vehicle_climate(climate)", "tk::emit_vehicle_climate_state(climate, e)",
    "return ok;",
)
DRIVE_CACHE_SEAMS = (
    "g_vehicle->get_cached_vehicle_drive()", "tk::emit_vehicle_drive_state(drive, e)",
    "return tk::vehicle_drive_available(drive)",
)


def require_vehicle_data_production_seams(handler: str) -> None:
    # Real cache selection, availability and status must delegate to the host-tested seams.
    code = scrub_cpp_preserving_layout(handler)
    for token in VEHICLE_DATA_SEAMS:
        if scrub_cpp_preserving_layout(token) not in code:
            raise AssertionError(f"/vehicle_data handler bypasses tested production seam {token!r}")


def require_vehicle_climate_cache_seam(helper: str) -> None:
    code = scrub_cpp_preserving_layout(helper)
    for token in CLIMATE_CACHE_SEAMS:
        if token not in code:
            raise AssertionError(f"climate cache helper bypasses tested production seam {token!r}")
    if "get_charge_state" in code or "charge_state_poll" in code or "WakePolicy" in code:
        raise AssertionError("climate cache helper added a vehicle-active read")


def require_vehicle_drive_cache_seam(helper: str) -> None:
    code = scrub_cpp_preserving_layout(helper)
    for token in DRIVE_CACHE_SEAMS:
        if token not in code:
            raise AssertionError(f"drive cache helper bypasses tested production seam {token!r}")
    if "get_charge_state" in code or "charge_state_poll" in code or "WakePolicy" in code:
        raise AssertionError("drive cache helper added a vehicle-active read")


DRIVE_GETTER_SEAMS = (
    "tk::SemGuard g(cache_mutex_);", "if (!g) return {};",
    "return tk::vehicle_drive_data(last_known_drive_);",
)


def require_vehicle_drive_getter_seam(source: str) -> None:
    code = scrub_cpp(function_body_in(source, "get_cached_vehicle_drive"))
    for token in DRIVE_GETTER_SEAMS:
        if token not in code:
            raise AssertionError(f"fixed drive getter lost ownership/projection: {token}")
    for token in ("ESP_LOG", "charge_state_poll", "vehicle_mutex_", "std::string", "ble_", "client_"):
        if token in code:
            raise AssertionError(f"fixed drive getter added a non-cache operation: {token}")


# get_vehicle_climate() is the cache-only freshness-bounded read for evcc's climater. Snapshots are
# taken under the leaf cache_mutex_; the window/age decision runs after the unlock.
CLIMATE_GETTER_SEAMS = (
    "tk::SemGuard g(cache_mutex_);", "if (!g)",
    "tk::vehicle_climate_data(last_known_climate_)",
    "last_climate_ticks_.load()", "climate_state_generation_.load()",
    "last_known_charge_.valid", "active_window_now_(", "tk::climate_cache_usable(",
    # Exact statements: the verdict must stay the shared window/age decision, the window must see
    # the charging arm, "have a sample" must come from the generation bit and the age must be
    # derived from the stamped tick (a forced-closed window, have_age = true, a dropped charging
    # arm or age_s = 0 would each let a dead feedback path pose as fresh data).
    "return tk::climate_cache_usable(snapshot.valid, active_window, have_age, age_s);",
    "active_window_now_(now, charging)",
    "have_age = generation != 0;",
    "(now - sample) / configTICK_RATE_HZ",
)
# A failed cache lock must clear the output and report failure, never serve a stale snapshot.
CLIMATE_GETTER_LOCK_FAILURE = re.compile(r"if\s*\(\s*!g\s*\)\s*\{\s*out\s*=\s*\{\};\s*return\s+false;\s*\}")
# (injected spelling, forbidden-pattern) pairs: a cache-only read must not poll, connect or copy.
CLIMATE_GETTER_FORBIDDEN = (
    ('ESP_LOGW(TAG, "x");', r"ESP_LOG"),
    ("charge_state_poll();", r"charge_state_poll"),
    ("tk::SemGuard v(vehicle_mutex_);", r"vehicle_mutex_"),
    ("enqueue_background_poll_();", r"enqueue_background_poll_"),
    ("std::string s;", r"std::string"),
    ("ble_->connect(\"\");", r"\bble_"),
    ("client_->x();", r"\bclient_"),
    ("ChargeStateResult c = last_known_charge_;", r"\bChargeStateResult\b"),
    ("auto c = copy_locked_(last_known_charge_);", r"copy_locked_"),
)


def if_statement_at(code: str, at: int, label: str) -> tuple[str, int, int]:
    """Return (condition, body_open, body_close) of the braced `if (...) { ... }` starting at `at`.

    `code` must be scrubbed with layout preserved, so braces in comments or literals do not count.
    """
    if not re.match(r"if\s*\(", code[at:]):
        raise AssertionError(f"{label}: expected an if statement")
    paren = skip_space(code, at + 2, len(code))
    condition_end = balanced_end(code, paren, "(", ")", label)
    body_open = skip_space(code, condition_end + 1, len(code))
    body_close = balanced_end(code, body_open, "{", "}", label)
    return " ".join(code[paren + 1:condition_end].split()), body_open, body_close


def enclosing_scope(code: str, at: int, label: str) -> tuple[int, int]:
    """Return (open, close) of the innermost brace scope that contains offset `at`."""
    opens: list[int] = []
    for pos, char in enumerate(code[:at]):
        if char == "{":
            opens.append(pos)
        elif char == "}":
            if not opens:
                raise AssertionError(f"{label}: unbalanced braces before the statement")
            opens.pop()
    if not opens:
        raise AssertionError(f"{label}: statement is not inside a brace scope")
    return opens[-1], balanced_end(code, opens[-1], "{", "}", label)


def scope_around(code: str, marker: str) -> tuple[int, int]:
    """Return (start, end) of the lexical brace scope that owns the first `marker` in `code`."""
    at = code.index(marker)
    scopes: list[int] = []
    for pos, char in enumerate(code[:at]):
        if char == "{": scopes.append(pos)
        elif char == "}": scopes.pop()
    start = scopes[-1]
    return start, balanced_end(code, start, "{", "}", "scope")


def require_vehicle_climate_getter_seam(source: str) -> None:
    raw = function_body_in(source, "get_vehicle_climate")
    code = scrub_cpp(raw)
    for token in CLIMATE_GETTER_SEAMS:
        if token not in code:
            raise AssertionError(f"climate getter lost ownership/projection/freshness: {token}")
    if not re.search(r"last_known_charge_\.charging_state\s*==", code):
        raise AssertionError("climate getter lost the in-place charging_state comparison")
    if not CLIMATE_GETTER_LOCK_FAILURE.search(code):
        raise AssertionError("climate getter lock failure no longer clears out and returns false")
    for _, pattern in CLIMATE_GETTER_FORBIDDEN:
        if re.search(pattern, code):
            raise AssertionError(f"climate getter added a non-cache operation: {pattern}")
    # The freshness decision must follow the unlock, never run under cache_mutex_.
    layout = scrub_cpp_preserving_layout(raw)
    _, locked_end = scope_around(layout, "tk::SemGuard g(cache_mutex_);")
    for token in ("active_window_now_(", "tk::climate_cache_usable("):
        if layout.find(token) < locked_end:
            raise AssertionError(f"climate getter evaluates {token} under cache_mutex_")


# get_charge_state() is the cache-only, freshness-bounded read behind evcc's charge_state poll. The
# snapshot is taken under the leaf cache_mutex_; the window/age decision runs after the unlock.
# Tokens are matched on whitespace-normalized code, so a statement may wrap across lines (the lock
# statement itself is located on the raw layout and must stay on one line).
CHARGE_GETTER_SNAPSHOT = (
    "cached = last_known_charge_;",
    "sample = last_charge_ticks_.load();", "generation = charge_state_generation_.load();",
)
CHARGE_GETTER_SEAMS = (
    "tk::MutexGuard g(cache_mutex_);", *CHARGE_GETTER_SNAPSHOT,
    # Exact statements: the window must see the cached charging arm, "have a sample" must come from
    # the generation bit, the age must be derived from the stamped tick and the shared verdict must
    # gate the response (a forced-closed window, have_sample_age = true, a dropped charging arm or an
    # age of 0 would each let a dead feedback path pose as fresh data inside the active window).
    "uint32_t now = xTaskGetTickCount();",
    "const bool active_window = active_window_now_(now, charging);",
    "bool have_sample_age = generation != 0;",
    "uint32_t sample_age_s = have_sample_age ? (now - sample) / configTICK_RATE_HZ : 0;",
    "if (!tk::charge_cache_usable(cached.valid, active_window, have_sample_age, sample_age_s)) {",
)
# The charging arm compares string literals, which scrub_cpp blanks, so it is matched on code with
# only the comments removed.
CHARGE_GETTER_CHARGING_ARM = (
    'const bool charging = cached.charging_state == "Charging" || '
    'cached.charging_state == "Starting";'
)
# Every freshness input is written exactly this often (the pinned statements plus the zero
# initializers of sample/generation), so a later reassignment cannot bypass the pinned values.
CHARGE_GETTER_WRITES = {
    "cached": 1, "sample": 2, "generation": 2, "now": 1, "charging": 1, "active_window": 1,
    "have_sample_age": 1, "sample_age_s": 1,
}
# (injected spelling, forbidden-pattern) pairs: the read serves the cache and never polls, connects,
# wakes or takes the vehicle lock. Logging stays allowed: the stale branch reports once.
CHARGE_GETTER_FORBIDDEN = (
    ("charge_state_poll();", r"charge_state_poll"),
    ("tk::SemGuard v(vehicle_mutex_);", r"vehicle_mutex_"),
    ("enqueue_background_poll_();", r"enqueue_background_poll_"),
    ("(void)ensure_connected_until_(0, tk::ConnectOrigin::Foreground);", r"ensure_connected_until_"),
    ("wake_up();", r"\bwake_up\b"),
    ("ble_->connect(\"\");", r"\bble_"),
    ("client_->x();", r"\bclient_"),
)
# active_window_now_() is the window both cache getters and the background poll share; dropping
# its command or charging arm would silently disable in-window freshness everywhere.
ACTIVE_WINDOW_SEAMS = (
    "const uint32_t lc = last_cmd_ticks_.load();",
    "const bool recent_cmd = (lc != 0) && ((now_ticks - lc) < pdMS_TO_TICKS(kActiveWindowMs));",
    "const bool have_contact = seconds_since_contact(contact_age);",
    "return tk::active_window_open({recent_cmd, charging_state, have_contact, contact_age});",
)


def cpp_writes(code: str, name: str) -> int:
    """Count assignments, compound assignments and increments of the plain identifier `name`."""
    ident = rf"(?<![\w.>:]){re.escape(name)}\b"
    return len(re.findall(
        rf"{ident}\s*(?:<<|>>|[-+*/%&|^])?=(?!=)|{ident}\s*(?:\+\+|--)|(?:\+\+|--)\s*{ident}", code))


def require_active_window_seam(source: str) -> None:
    code = " ".join(scrub_cpp(function_body_in(source, "active_window_now_")).split())
    for token in ACTIVE_WINDOW_SEAMS:
        if token not in code:
            raise AssertionError(f"active window lost its command/charging/contact input: {token}")
    if len(re.findall(r"\breturn\b", code)) != 1:
        raise AssertionError("active window must have exactly one return (the shared decision)")


def strip_cpp_comments(text: str) -> str:
    """Remove comments but keep literals, so a pin can name the strings a decision compares."""
    return CPP_NOISE.sub(lambda match: match.group(0) if match.group(0)[0] in "\"'" else " ", text)


def require_charge_state_getter_seam(source: str) -> None:
    raw = function_body_in(source, "get_charge_state")
    code = " ".join(scrub_cpp(raw).split())
    for token in CHARGE_GETTER_SEAMS:
        if token not in code:
            raise AssertionError(f"charge getter lost ownership/freshness: {token}")
    if CHARGE_GETTER_CHARGING_ARM not in " ".join(strip_cpp_comments(raw).split()):
        raise AssertionError("charge getter window lost the cached Charging/Starting arm")
    for name, expected in CHARGE_GETTER_WRITES.items():
        if cpp_writes(code, name) != expected:
            raise AssertionError(f"charge getter writes {name} {cpp_writes(code, name)}x, expected {expected}")
    for _, pattern in CHARGE_GETTER_FORBIDDEN:
        if re.search(pattern, code):
            raise AssertionError(f"charge getter added a non-cache operation: {pattern}")
    # The snapshot is copied inside the locked scope; the freshness decision follows the unlock.
    layout = scrub_cpp_preserving_layout(raw)
    locked_start, locked_end = scope_around(layout, "tk::MutexGuard g(cache_mutex_);")
    locked = " ".join(layout[locked_start:locked_end].split())
    for token in CHARGE_GETTER_SNAPSHOT:
        if token not in locked:
            raise AssertionError(f"charge getter copies {token} outside cache_mutex_")
    for token in ("active_window_now_(", "tk::charge_cache_usable("):
        if layout.find(token) < locked_end:
            raise AssertionError(f"charge getter evaluates {token} under cache_mutex_")
    # A stale verdict must refuse the response instead of falling through to serve the cache, and
    # the cache may only be served after that verdict: one success return, one output write.
    opening = layout.find("{", layout.find("tk::charge_cache_usable("))
    closing = balanced_end(layout, opening, "{", "}", "charge getter stale branch")
    if not re.search(r"return\s+false;\s*$", layout[opening + 1:closing]):
        raise AssertionError("charge getter stale verdict no longer returns false")
    for token, label in ((r"\breturn\s+true\s*;", "success return"), (r"\bout\s*=(?!=)", "output write")):
        hits = [match.start() for match in re.finditer(token, layout)]
        if len(hits) != 1 or hits[0] < closing:
            raise AssertionError(f"charge getter {label} must occur once, after the freshness verdict")


def require_climate_publication_stamp(telemetry_source: str) -> None:
    """The climate cache publication stamps its age and generation inside the guarded epoch branch."""
    body = scrub_cpp_preserving_layout(function_body_in(telemetry_source, "process_pending_telemetry_"))
    processing = body.find("portEXIT_CRITICAL(&telemetry_pending_mux_);")
    start = body.find("pending & PendingClimate", max(processing, 0))
    end = body.find("pending & PendingDrive", start)
    if processing < 0 or start < 0 or end < 0:
        raise AssertionError("process_pending_telemetry_ climate publication block missing")
    block = body[start:end]
    guard = re.search(r"tk::MutexGuard\s+\w+\(cache_mutex_\);", block)
    if not guard:
        raise AssertionError("climate publication lost its cache_mutex_ guard")
    _, guard_end = scope_around(block, guard.group(0))
    epoch = block.find("tk::telemetry_epoch_matches(climate_epoch", guard.end())
    for token in ("last_climate_ticks_.store(xTaskGetTickCount());",
                  "climate_state_generation_.fetch_add(1);"):
        at = block.find(token)
        if at < 0:
            raise AssertionError(f"climate publication does not stamp freshness: {token}")
        if not guard.end() < epoch < at < guard_end:
            raise AssertionError(f"climate freshness stamp {token} outside the guarded epoch branch")


def require_wake_companion_drive_poll(loop_source: str, rotation_source: str) -> None:
    """Wake/bootstrap one-shot refreshes drive only under the seam; rising edge restarts rotation."""
    layout = scrub_cpp_preserving_layout(loop_source)   # offsets match loop_source
    fire = layout.find("tk::charge_poll_should_fire(")
    if fire < 0:
        raise AssertionError("charge_poll_should_fire block missing from the telemetry loop")
    open_brace = layout.find("{", layout.index("wake_poll)", fire))
    block_end = balanced_end(layout, open_brace, "{", "}", "charge poll block")
    block = loop_source[open_brace:block_end + 1]
    code = layout[open_brace:block_end + 1]
    guard = "if (tk::wake_poll_refreshes_drive(poll_cadence)) {"
    if guard not in code:
        raise AssertionError("drive companion poll is not gated by tk::wake_poll_refreshes_drive")
    inner_open = code.index("{", code.index(guard))
    inner_end = balanced_end(code, inner_open, "{", "}", "drive companion block")
    if block.count('"Drive State Poll"') != 1 or not (
        inner_open < block.index('"Drive State Poll"') < inner_end
    ):
        raise AssertionError("Drive State Poll must be enqueued only inside the wake_poll_refreshes_drive gate")
    if "enqueue_background_poll_" not in code[inner_open:inner_end]:
        raise AssertionError("drive companion gate does not enqueue a background poll")
    require_before("charge poll before its drive companion", block,
                   '"Charge State Poll"', '"Drive State Poll"')
    shared_pattern = r'"Drive State Poll",\s*tk::BleDomain::Infotainment,\s*build_drive_state_poll\)'
    shared_loop = re.findall(shared_pattern, strip_cpp_comments(loop_source))
    shared_rotation = re.findall(shared_pattern, strip_cpp_comments(rotation_source))
    if len(shared_loop) != 1 or len(shared_rotation) != 1:
        raise AssertionError("rotation and wake one-shot must share build_drive_state_poll")

    edge = layout.find("if (paired && !prev_window && window) {")
    if edge < 0:
        raise AssertionError("window rising-edge block missing")
    edge_open = layout.index("{", edge)
    edge_body = layout[edge_open:balanced_end(layout, edge_open, "{", "}", "rising edge") + 1]
    if "telemetry_poll.reset();" not in edge_body:
        raise AssertionError("window rising edge must restart the telemetry rotation at climate")


def require_telemetry_poll_schedule(telemetry_source: str) -> None:
    """The firmware uses the tested cadence/priority and accounts for actual queue admission."""
    loop = function_body_in(telemetry_source, "loop_task_fn_")
    layout = scrub_cpp_preserving_layout(loop)
    gate = re.search(
        r"if\s*\(telemetry_poll\.due\(\{paired,\s*window,\s*self->ble_connected\(\),\s*"
        r"self->cmd_in_flight_\.load\(\),\s*now_ticks,\s*pdMS_TO_TICKS\(30000\)\}\)\)\s*\{",
        layout,
    )
    if not gate:
        raise AssertionError("telemetry must delegate all active-window/cadence gates to the tested scheduler")
    start = gate.end() - 1
    end = balanced_end(layout, start, "{", "}", "telemetry schedule")
    schedule = layout[start:end + 1]
    expected = "{telemetry_poll.begin_attempt(now_ticks);self->enqueue_telemetry_round_(telemetry_poll);}"
    if "".join(schedule.split()) != expected or loop.count("self->enqueue_telemetry_round_(") != 1:
        raise AssertionError("telemetry round must run once under its unchanged scheduler gate, after the attempt stamp")
    if "[[gnu::noinline]] void VehicleController::enqueue_telemetry_round_(" not in telemetry_source:
        raise AssertionError("optional telemetry round must retain its separate frame")
    round_body = function_body_in(telemetry_source, "enqueue_telemetry_round_")
    if "auto* self = this;" not in strip_cpp_comments(round_body):
        raise AssertionError("telemetry round must use its owning controller")
    block = "{telemetry_poll.begin_attempt(now_ticks);" + round_body + "}"
    code = scrub_cpp_preserving_layout(block)
    require_before("telemetry attempt throttled before fallible work", code,
                   "telemetry_poll.begin_attempt(now_ticks);", "self->get_vehicle_climate(climate)")
    require_before("climate snapshot before vehicle lock", code,
                   "const bool climate_usable = self->get_vehicle_climate(climate);",
                   "tk::SemGuard g(self->vehicle_mutex_);")
    require_before("priority selection before dispatch", code,
                   "const auto domain = telemetry_poll.next(climate_usable);", "switch (domain)")
    lock = code.index("tk::SemGuard g(self->vehicle_mutex_);")
    lock_start = code.rfind("{", 0, lock)
    lock_end = balanced_end(code, lock_start, "{", "}", "telemetry vehicle lock")
    account = code.find("telemetry_poll.note_admission(climate_usable, enqueued);")
    if account <= lock_end:
        raise AssertionError("scheduler must account for actual admission after releasing the vehicle lock")
    expected = {"Climate": ("Climate State Poll", "getClimateState"),
                "Drive": ("Drive State Poll", None),
                "Tires": ("Tire Pressure Poll", "getTirePressureState"),
                "Closures": ("Closures State Poll", "getClosuresState")}
    for domain, (name, selector) in expected.items():
        case = re.search(rf"case tk::TelemetryPollDomain::{domain}:\s*"
                         r"enqueued = self->enqueue_background_poll_\(", code)
        if not case or not lock < case.start() < lock_end:
            raise AssertionError(f"{domain} must dispatch under vehicle lock and retain queue admission")
        tail = block[case.end():block.index("break;", case.end())]
        if f'"{name}", tk::BleDomain::Infotainment,' not in tail:
            raise AssertionError(f"{domain} dispatches the wrong telemetry poll")
        if selector and f"CarServer_GetVehicleData_{selector}_tag" not in tail:
            raise AssertionError(f"{domain} requests the wrong protobuf selector")
    enqueue = scrub_cpp(function_body_in(telemetry_source, "enqueue_background_poll_"))
    for rejection in ("if (cmd_in_flight_.load()) return false;", "if (!command_identity_ready_()) return false;",
                      "if (command_runner_.is_queue_full()) return false;"):
        if rejection not in enqueue:
            raise AssertionError("background admission must report identity/queue refusal")
    if "tk::WakePolicy::NoWakeSkip, 10000" not in enqueue:
        raise AssertionError("background telemetry must retain NO_WAKE_SKIP and its bounded expiry")
    admitted = re.search(r"if \(cmd_id > 0\)\s*\{(.*?)\}", enqueue, re.DOTALL)
    if not admitted or "return true;" not in admitted.group(1) or not re.search(r"return false;\s*\}$", enqueue):
        raise AssertionError("background admission must report success only for a queued command")


def require_drive_poll_builder(telemetry_source: str) -> None:
    builder = scrub_cpp(function_body_in(telemetry_source, "build_drive_state_poll"))
    if "CarServer_GetVehicleData_getDriveState_tag" not in builder:
        raise AssertionError("build_drive_state_poll no longer requests DriveState")


def require_unlocked_diagnostic_logging(heap: str, telemetry: str) -> None:
    adoption = scrub_cpp(function_body_in(heap, "adopt_or_reset"))
    if "ESP_LOG" in adoption:
        raise AssertionError("heap adoption logs while its caller holds the trend mutex")
    # Locate the lexical scope owning each guard, including nested try/catch blocks.
    for source, name, marker in ((heap, "heap_trend_record", "SemGuard g(m, 0);"),
                                 (heap, "heap_trend_snapshot", "SemGuard g(m);"),
                                 (telemetry, "enqueue_telemetry_round_", "tk::SemGuard g(self->vehicle_mutex_);"),
                                 (telemetry, "loop_task_fn_", "tk::SemGuard g(self->vehicle_mutex_);")):
        body = scrub_cpp_preserving_layout(function_body_in(source, name))
        at = body.index(marker)
        scopes = []
        for pos, char in enumerate(body[:at]):
            if char == "{": scopes.append(pos)
            elif char == "}": scopes.pop()
        start = scopes[-1]
        depth = 0
        for pos in range(start, len(body)):
            if body[pos] == "{": depth += 1
            elif body[pos] == "}": depth -= 1
            if depth == 0:
                locked = body[start:pos + 1]
                if "ESP_LOG" in locked:
                    raise AssertionError(f"{name}: diagnostic logging under shared mutex")
                if name.startswith("heap_trend") and "log_adoption(adopted);" not in body[pos + 1:]:
                    raise AssertionError(f"{name}: adoption log must follow unlock")
                break


def require_diag_dump_completion_contract(handler: str, diag_header: str,
                                          diag_source: str,
                                          runtime_tests: str) -> None:
    for token in (
        "enum class DiagDumpResult",
        "Complete",
        "SinkFailed",
        "SnapshotInvalidated",
        "DiagDumpResult diag_log_dump_chunks(",
    ):
        if token not in diag_header:
            raise AssertionError(f"typed /diag dump result contract missing {token!r}")

    dump = function_body_in(diag_source, "diag_log_dump_chunks")
    for token in (
        "if (!mtx) return DiagDumpResult::SnapshotInvalidated;",
        "if (!g) return DiagDumpResult::SnapshotInvalidated;",
        "s_epoch != snapshot_epoch",
        "return DiagDumpResult::SnapshotInvalidated;",
        "return DiagDumpResult::SinkFailed;",
        "return DiagDumpResult::Complete;",
        "if (first != DiagDumpResult::Complete) return first;",
    ):
        if token not in dump:
            raise AssertionError(f"typed /diag producer outcome missing {token!r}")
    require_before("/diag sink failure before complete", dump,
                   "return DiagDumpResult::SinkFailed;",
                   "return DiagDumpResult::Complete;")

    redacted_marker = handler.find("// ?redact=1")
    if redacted_marker < 0:
        raise AssertionError("/diag redacted branch marker is missing")
    plain = handler[:redacted_marker]
    redacted = handler[redacted_marker:]
    for token in (
        "const DiagDumpResult dump = diag_log_dump_chunks",
        "if (dump != DiagDumpResult::Complete) return ESP_FAIL;",
        "return httpd_resp_send_chunk(req, nullptr, 0);",
    ):
        if token not in plain:
            raise AssertionError(f"plain /diag success-only completion missing {token!r}")
    require_before("plain /diag typed result before failure", plain,
                   "const DiagDumpResult dump = diag_log_dump_chunks",
                   "if (dump != DiagDumpResult::Complete) return ESP_FAIL;")
    require_before("plain /diag failure before terminator", plain,
                   "if (dump != DiagDumpResult::Complete) return ESP_FAIL;",
                   "return httpd_resp_send_chunk(req, nullptr, 0);")

    for token in (
        "const DiagDumpResult dump = diag_log_dump_chunks",
        "if (dump != DiagDumpResult::Complete || !ok) return ESP_FAIL;",
        "if (frame.overlong) flush_overlong(false);",
        "else                flush_line(frame.size, false);",
        "if (!ok) return ESP_FAIL;",
        "return httpd_resp_send_chunk(req, nullptr, 0);",
    ):
        if token not in redacted:
            raise AssertionError(f"redacted /diag success-only completion missing {token!r}")
    require_before("redacted /diag dump outcome before final-line flush", redacted,
                   "if (dump != DiagDumpResult::Complete || !ok) return ESP_FAIL;",
                   "if (frame.overlong) flush_overlong(false);")
    require_before("redacted /diag final flush result before terminator", redacted,
                   "if (!ok) return ESP_FAIL;",
                   "return httpd_resp_send_chunk(req, nullptr, 0);")
    if handler.count("httpd_resp_send_chunk(req, nullptr, 0)") != 2:
        raise AssertionError("/diag must have exactly two branch-local success terminators")

    for token in (
        "test_diag_http_completion_matrix",
        "DiagDumpResult::SinkFailed, true, terminating_chunks",
        "DiagDumpResult::SnapshotInvalidated, true, terminating_chunks",
        "DiagDumpResult::Complete, false, terminating_chunks",
        "terminating_chunks == 0",
        "terminating_chunks == 1",
        "first_sink_failure == DiagDumpResult::SinkFailed",
        "nth_sink_failure == DiagDumpResult::SinkFailed",
        "nth_failure_chunks == 3",
        "overwrite_result == DiagDumpResult::SnapshotInvalidated",
        "clear_result == DiagDumpResult::SnapshotInvalidated",
    ):
        if token not in runtime_tests:
            raise AssertionError(f"executable /diag failure completion matrix missing {token!r}")


def require_diag_redaction_stream_contract(
    handler: str, redactor_header: str, diag_source: str, runtime_tests: str
) -> None:
    """Keep the redacted /diag tail fixed-buffer and non-throwing once chunks can leave."""
    for token in (
        'static_assert(CONFIG_LOG_VERSION == 1,',
        'static_assert(CONFIG_LOG_TIMESTAMP_SOURCE_RTOS,',
        '#if defined(CONFIG_LOG_COLORS) && CONFIG_LOG_COLORS',
        'static std::atomic<SemaphoreHandle_t> s_mtx{nullptr};',
        'static std::atomic<vprintf_like_t> s_prev{nullptr};',
        's_prev.store(previous, std::memory_order_release);',
        's_mtx.store(mtx, std::memory_order_release);',
        'tk::diag_library_capture_content_end({data, len})',
        'tk::diag_capture_byte(data[i], i, library_end)',
    ):
        if token not in diag_source:
            raise AssertionError(f"qualified V1 diagnostic capture lost {token!r}")
    for name, handle in (
        ("diag_append_", "s_mtx"), ("diag_log_dump_chunks", "s_mtx"),
        ("diag_log_clear", "s_mtx"), ("diag_vprintf_", "s_prev"),
        ("diag_forward_fixed_", "s_prev"),
    ):
        body = scrub_cpp(function_body_in(diag_source, name))
        if body.count(f"{handle}.load(std::memory_order_acquire)") != 1 or len(re.findall(rf"\b{handle}\b", body)) != 1:
            raise AssertionError(f"{name}: load published handle once into a local snapshot")
    append = function_body_in(diag_source, "diag_append_")
    append_code = scrub_cpp(append)
    if "tk::SemGuard g(mtx, 0);" not in append_code:
        raise AssertionError("diagnostic producer capture must not wait for the ring mutex")
    if append_code.count("s_dropped_lines.fetch_add(1, std::memory_order_relaxed)") != 2:
        raise AssertionError("diagnostic producer must count unavailable and busy capture losses")

    require_before("library record classification before ring lock", append,
                   "tk::diag_library_capture_content_end", "tk::SemGuard")
    if "if (diag_library_identifier_line(line)) return collapse();" not in redactor_header:
        raise AssertionError("known library identifier export must collapse the complete line")
    capture = function_body_in(diag_source, "diag_vprintf_")
    for token in (
        "if (diag_mqtt_uri_error_(line, len))",
        "diag_append_(tk::kDiagMqttUriErrorLine, sizeof(tk::kDiagMqttUriErrorLine) - 1)",
        "syslog_send(tk::kDiagMqttUriErrorLine, sizeof(tk::kDiagMqttUriErrorLine) - 1)",
        'return diag_forward_fixed_("%s", tk::kDiagMqttUriErrorLine);',
    ):
        if token not in capture:
            raise AssertionError(f"MQTT credential capture guard missing {token!r}")
    require_before("MQTT protection before ordinary raw capture", capture,
                   "diag_mqtt_uri_error_", "diag_append_(line, len)")
    init = function_body_in(diag_source, "diag_log_init")
    require_before("protect logs even when optional ring mutex fails", init,
                   "esp_log_set_vprintf(diag_vprintf_)", "xSemaphoreCreateMutex()")
    if "if (!s_hook_installed)" not in init or "s_hook_installed = true;" not in init:
        raise AssertionError("diagnostic initialization can chain its hook to itself")
    marker = "// ?redact=1"
    start = handler.find(marker)
    if start < 0:
        raise AssertionError("/diag redacted streaming branch is missing")
    redacted = handler[start:]
    for token in (
        "char   line[kLineMax];",
        "char   redacted[kRedactedLineMax];",
        "tk::redact_diag_line_fixed",
        "tk::DiagLineFrame frame;",
        "tk::diag_line_step(frame, p[i], kLineMax)",
        "tk::DiagLineAction::EmitOverlong",
        "if (frame.overlong) flush_overlong(false);",
        "auto flush_line = [&](size_t len, bool had_newline) noexcept",
        "diag_log_dump_chunks([&](const char* p, size_t n) noexcept",
        "DiagDumpStart::AfterWrappedLineBoundary",
    ):
        if token not in redacted:
            raise AssertionError(f"/diag fixed-buffer stream contract missing {token!r}")

    # string_view is a non-owning fixed pair; owning strings, heap primitives and explicit throws
    # in this branch would again make a later chunk capable of rebooting an exhausted device.
    code = scrub_cpp(redacted)
    if re.search(r"\bstd::string\b|\b(?:new|delete|malloc|calloc|realloc|free|throw)\b", code):
        raise AssertionError("/diag redacted stream regained allocating/throwing work")
    require_before(
        "/diag redact before send",
        redacted,
        "tk::redact_diag_line_fixed",
        "httpd_resp_send_chunk(req, out, out_len)",
    )
    overlong_sink = re.search(
        r"auto\s+flush_overlong\s*=\s*\[&\]\s*\([^)]*\)\s*noexcept\s*"
        r"\{(?P<body>.*?)\n\s*\};",
        redacted,
        re.DOTALL,
    )
    if not overlong_sink:
        raise AssertionError("/diag overlong fail-closed sink is missing")
    overlong_body = scrub_cpp(overlong_sink.group("body"))
    if (
        "httpd_resp_send_chunk(req, tk::kRedacted, tk::kRedactedLength)"
        not in overlong_body
        or re.search(r"\bline\b", overlong_body)
    ):
        raise AssertionError("/diag overlong line can emit raw buffered bytes")

    signature = re.search(
        r"inline\s+FixedDiagRedaction\s+redact_diag_line_fixed\s*\([^)]*\)\s*noexcept",
        redactor_header,
        re.DOTALL,
    )
    if not signature:
        raise AssertionError("fixed /diag redactor lost its noexcept boundary")
    fixed_start = redactor_header.find("inline FixedDiagRedaction redact_diag_line_fixed")
    wrapper_start = redactor_header.find("inline std::string redact_diag_line", fixed_start)
    fixed_code = scrub_cpp(redactor_header[fixed_start:wrapper_start])
    if re.search(r"\bstd::string\b|\b(?:new|delete|malloc|calloc|realloc|free|throw)\b", fixed_code):
        raise AssertionError("fixed /diag redactor contains allocating/throwing work")

    for token in (
        "wrapped && start_mode == DiagDumpStart::AfterWrappedLineBoundary",
        "std::memchr(chunk, '\\n', n)",
        "delivered += skipped;",
        "delivered_size -= skipped;",
    ):
        if token not in diag_source:
            raise AssertionError(f"wrapped /diag boundary suppression missing {token!r}")
    for token in (
        "markerless_secret_prefix",
        "DiagDumpStart::AfterWrappedLineBoundary",
        "tk::redact_diag_line_fixed",
        'redacted_report.find("5YJ3E1EA7KF000316") == std::string::npos',
    ):
        if token not in runtime_tests:
            raise AssertionError(f"wrapped /diag executed fixture missing {token!r}")
    logic_tests = (ROOT / "test/test_logic.cpp").read_text(encoding="utf-8")
    for token in (
        "tk::diag_line_step",
        "LA::EmitOverlong",
        "unterminated.overlong",
        "repeated_marker",
        "repeated_text.find(\"5YJ3E1EA7KF000316\") == std::string::npos",
        "repeated_text.find(\"7SAYGDEE9PF000111\") == std::string::npos",
        "different_markers",
        "different_text.find(\"aa:bb:cc:dd:ee:ff\") == std::string::npos",
    ):
        if token not in logic_tests:
            raise AssertionError(f"overlong /diag framing fixture missing {token!r}")


def require_operation_wrapper_contract(ota_source: str) -> None:
    fault_begin = re.search(
        r"bool\s+ota_fault_restart_begin\s*\(\s*\)\s*\{([^}]*)\}",
        ota_source,
        re.DOTALL,
    )
    fault_cancel = re.search(
        r"void\s+ota_fault_restart_cancel\s*\(\s*\)\s*\{([^}]*)\}",
        ota_source,
        re.DOTALL,
    )
    if not fault_begin or (
        "s_operation_gate.try_begin(tk::OtaIdentityGateState::FaultRestart)"
        not in fault_begin.group(1)
    ):
        raise AssertionError("FaultRestart begin wrapper bypasses the shared CAS gate")
    if not fault_cancel or (
        "finish_operation(tk::OtaIdentityGateState::FaultRestart)"
        not in fault_cancel.group(1)
    ):
        raise AssertionError("FaultRestart cancel wrapper does not owner-release the shared gate")
    constructor = re.search(
        r"OtaHealthCommitGuard::OtaHealthCommitGuard\s*\(\s*\)\s*\{\s*"
        r"const auto acquired = s_operation_gate\.try_begin_observed\(tk::OtaIdentityGateState::HealthCommit\);\s*"
        r"held_ = acquired\.acquired;\s*observed_ = acquired\.observed;\s*\}",
        ota_source, re.DOTALL)
    destructor = re.search(
        r"OtaHealthCommitGuard::~OtaHealthCommitGuard\s*\(\s*\)\s*\{\s*"
        r"if\s*\(\s*held_\s*\)\s*finish_operation\s*\(\s*"
        r"tk::OtaIdentityGateState::HealthCommit\s*\)\s*;\s*\}",
        ota_source,
        re.DOTALL,
    )
    if not constructor:
        raise AssertionError("HealthCommit guard does not acquire the shared owner into held_")
    if not destructor:
        raise AssertionError("HealthCommit guard does not owner-release exactly when held")
    code = scrub_cpp_preserving_layout(ota_source)
    acquire = function_body_in(code, "try_begin")
    if "held_ = s_operation_gate.try_begin(tk::OtaIdentityGateState::ConfigRestart);" not in acquire:
        raise AssertionError("ConfigRestart guard bypasses shared CAS admission")
    owns = function_body_in(code, "owns_gate")
    if "return held_ && s_operation_gate.state() == tk::OtaIdentityGateState::ConfigRestart;" not in owns:
        raise AssertionError("ConfigRestart ownership requires this guard and current gate")
    cleanup = code[code.index("OtaConfigRestartGuard::~OtaConfigRestartGuard()"):
                   code.index("bool OtaConfigRestartGuard::try_begin()")]
    for token in ("noexcept", "if (held_ && !sealed_)", "held_ = false;",
                  "s_operation_gate.finish(tk::OtaIdentityGateState::ConfigRestart)"):
        if token not in cleanup: raise AssertionError("ConfigRestart cleanup missing " + token)
    if "ESP_LOG" in cleanup or "finish_operation" in cleanup:
        raise AssertionError("noexcept ConfigRestart cleanup cannot perform diagnostics")
    query = function_body_in(code, "ota_config_image_state")
    for token in ("if (!owner.owns_gate()) return Result::WrongOwner;",
                  "esp_ota_get_state_partition(running, &state) != ESP_OK",
                  "case ESP_OTA_IMG_VALID: return Result::Valid;",
                  "case ESP_OTA_IMG_UNDEFINED: return Result::Undefined;",
                  "case ESP_OTA_IMG_NEW: return Result::New;",
                  "case ESP_OTA_IMG_PENDING_VERIFY: return Result::Pending;",
                  "default: return Result::Unsupported;", "catch (...) { return Result::Unknown; }"):
        if token not in query: raise AssertionError("raw config image query missing " + token)
    require_before("config query owner before SDK", query, "owner.owns_gate()", "esp_ota_get_running_partition()")
    require_runtime_health_sampler_contract(code)
    require_recovery_reference_contract(code)
    confirm = function_body_in(code, "ota_confirm_pending_image")
    ordered = (
        "if (!owner.owns_gate()) return Result::WrongOwner;",
        "if (!tk::ota_reboot_confirms_pending_image(reboot_class)) return Result::WrongClass;",
        "Result result = ota_config_image_state(owner);",
        "if (result != Result::Pending) return result;",
        "ordinary_recovery_reference_ready_owned(tk::OtaIdentityGateState::ConfigRestart)",
        "return Result::Pending;",
        "const auto health = ota_runtime_health();",
        "result = tk::config_health_failure(health.failure);",
        "if (result != Result::None) return result;",
        "result = Result::MarkUnknown;",
        "esp_ota_mark_app_valid_cancel_rollback()",
        "result = Result::Confirmed;",
        "ESP_LOGI",
    )
    for token in ordered:
        if token not in confirm: raise AssertionError("borrowed config confirmation missing " + token)
    for first, second in zip(ordered, ordered[1:]):
        require_before("borrowed config confirmation ordering", confirm, first, second)
    if "OtaHealthCommitGuard" in confirm or "catch (...)" not in confirm or "return result;" not in confirm:
        raise AssertionError("borrowed confirmation must contain diagnostic throws without nested ownership")

    logic_tests = (ROOT / "test/test_logic.cpp").read_text(encoding="utf-8")
    runtime_tests = (ROOT / "test/test_runtime_boundaries.cpp").read_text(encoding="utf-8")
    for token in (
        "test_ota_confirm_runtime_admission_matrix",
        "may_mark_valid_after_owner(booting, true)",
        "may_mark_valid_after_owner(ready, true)",
        "may_mark_valid_after_owner(safe_mode, true)",
        "may_mark_valid_after_owner(fatal, true)",
    ):
        if token not in logic_tests:
            raise AssertionError(f"OTA Ready-only pure matrix missing {token!r}")
    for token in (
        "test_ota_confirm_ready_only_after_health_owner",
        "attempt_irreversible_mark(booting, true, mark_calls)",
        "attempt_irreversible_mark(ready, true, mark_calls)",
        "attempt_irreversible_mark(safe_mode, true, mark_calls)",
        "attempt_irreversible_mark(fatal, true, mark_calls)",
        "mark_calls == 1",
    ):
        if token not in runtime_tests:
            raise AssertionError(f"OTA Ready-only executable boundary matrix missing {token!r}")


def require_recovery_reference_contract(ota_source: str) -> None:
    query = function_body_in(ota_source, "ordinary_recovery_reference_ready_owned")
    tokens = ("owner != tk::OtaIdentityGateState::Ota", "owner != tk::OtaIdentityGateState::HealthCommit",
              "owner != tk::OtaIdentityGateState::ConfigRestart", "s_operation_gate.state() != owner",
              "auto* old = s_operation.load(std::memory_order_acquire);", "if (!old) return true;",
              "if (!old->coordinator_done.load(std::memory_order_acquire)) return false;",
              "return old->coordinator.can_reclaim() && s_network_slot.owns(old->job);")
    for token in tokens:
        if token not in query: raise AssertionError("owned recovery reference missing " + token)
    require_before("reference owner before pointer", query, "s_operation_gate.state() != owner", "s_operation.load")
    require_before("reference done before non-atomic state", query, "coordinator_done.load", "coordinator.can_reclaim()")
    for forbidden in (".store(", ".exchange(", "compare_exchange", "release_after_ack", "delete ", "new ",
                      "reclaim_finished_operation(", "memory_checkpoint(", "ESP_LOG", "vTaskDelay("):
        if forbidden in query: raise AssertionError("read-only recovery reference gained effect: " + forbidden)
    typed = function_body_in(ota_source, "ota_recovery_reference_ready")
    for token in ("!owner || s_operation_gate.state() != tk::OtaIdentityGateState::HealthCommit",
                  "return ordinary_recovery_reference_ready_owned(tk::OtaIdentityGateState::HealthCommit);"):
        if token not in typed: raise AssertionError("typed HealthCommit reference missing " + token)
    if "s_operation.load" in typed: raise AssertionError("typed wrapper bypasses private reference query")
    getter = function_body_in(ota_source, "ota_recovery_required_bytes")
    if "return tk::ota_recovery_required_bytes_for(use);" not in getter:
        raise AssertionError("actual recovery policy must delegate to the real pure selector")
    reservation = function_body_in(ota_source, "reserve_operation")
    ordered = ("try_begin_ota_operation()", "reclaim_finished_operation()",
               "s_target_pr.store(pr, std::memory_order_release);",
               "s_heap_abort_requested.store(false, std::memory_order_release);")
    for token in ordered:
        if token not in reservation: raise AssertionError("ordinary-entry reservation missing " + token)
    for first, second in zip(ordered, ordered[1:]):
        require_before("ordinary-entry reservation order", reservation, first, second)
    wrapper = function_body_in(ota_source, "prepare_operation")
    require_before("immediate entry reserves before allocation", wrapper,
                   "reserve_operation(pr, image)", "prepare_owned_operation(origin, pr, ota_get_channel(), image, operation)")
    entry = function_body_in(ota_source, "prepare_owned_operation")
    ordered = ("const auto entry_largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);",
               "tk::ota_recovery_required_bytes(tk::OtaRecoveryUse::OperationEntry)",
               "publish_operation_start(image)",
               "operation = new (std::nothrow) OtaOperation")
    for token in ordered:
        if token not in entry: raise AssertionError("ordinary-entry recovery missing " + token)
    for first, second in zip(ordered, ordered[1:]):
        require_before("ordinary-entry owner/reference/allocation order", entry, first, second)
    start = entry.index("if (!tk::ota_recovery_memory_affordable(entry_largest,")
    stop = entry.index("publish_operation_start(image)")
    refused = entry[start:stop]
    for token in ("set_check_error(kOtaNoMemory);", "release_unstarted_operation();", "return Result::NoMemory;"):
        if token not in refused: raise AssertionError("typed entry-memory refusal missing " + token)
    require_before("memory publication before owner release", refused, "set_check_error", "release_unstarted_operation")
    require_before("memory release before typed result", refused, "release_unstarted_operation", "return Result::NoMemory")


def require_runtime_health_sampler_contract(ota_source: str) -> None:
    sampler = function_body_in(ota_source, "ota_runtime_health")
    if any(token in sampler for token in ("s_operation", "ordinary_recovery_reference", "ota_recovery_reference_ready")):
        raise AssertionError("unowned health snapshot must not borrow an old Operation")
    ordered = ("return tk::sample_runtime_health(",
               "[] { return tk::runtime_admission_vehicle_ready(); }",
               "[] { return ble_host_synced(); }",
               "[] { return ble_host_reset_count(); }",
               "[] { return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL); }",
               "tk::ota_recovery_required_bytes(tk::OtaRecoveryUse::ImageConfirmation)")
    for token in ordered:
        if token not in sampler: raise AssertionError("shared OTA runtime sampler missing " + token)
    for first, second in zip(ordered, ordered[1:]):
        require_before("shared OTA runtime reader order", sampler, first, second)


def require_health_commit_contract(health_task: str) -> None:
    ordered = ("switch (tk::runtime_admission_action())",
               "const bool runtime_healthy = ota_runtime_health().healthy();",
               "tk::health_gate_decide", "if (v == tk::HealthVerdict::Commit)",
               "OtaHealthCommitGuard commit_guard;", "if (!commit_guard) continue;",
               "if (!ota_recovery_reference_ready(commit_guard)) continue;",
               "const auto commit_health = ota_runtime_health();",
               "if (!commit_health.healthy()) continue;",
               "const size_t commit_largest = commit_health.largest;",
               "const esp_err_t mark_result = esp_ota_mark_app_valid_cancel_rollback();",
               "ota_refresh_verification_cache();", "if (mark_result == ESP_OK)",
               "esp_err_to_name(mark_result)")
    for token in ordered:
        if token not in health_task: raise AssertionError("OTA health commit contract missing " + token)
    for first, second in zip(ordered, ordered[1:]):
        require_before("OTA fresh health owner/mark/cache/diagnostic ordering", health_task, first, second)
    for token in ("case tk::RuntimeAdmissionAction::Wait:", "case tk::RuntimeAdmissionAction::Stop:",
                  "case tk::RuntimeAdmissionAction::Run:", "verification is uncertain", "without retry"):
        if token not in health_task: raise AssertionError("OTA health admission/error diagnostic missing " + token)
    if health_task.count("ota_runtime_health()") != 2 or health_task.count("esp_ota_mark_app_valid_cancel_rollback()") != 1:
        raise AssertionError("OTA health requires distinct pre/post-owner samples and one mark call site")


def require_vehicle_task_start_contract(vehicle_source: str, telemetry_source: str,
                                        pairing_source: str) -> None:
    start = function_body_in(vehicle_source, "start_tasks")
    await_start = function_body_in(vehicle_source, "await_task_start_")
    loop_entry = function_body_in(telemetry_source, "loop_task_fn_")
    pair_entry = function_body_in(pairing_source, "auto_pair_task_fn_")

    if start.count("xTaskCreate(") != 2:
        raise AssertionError("vehicle start must create exactly the loop and auto-pair tasks")
    require_before("vehicle start barrier before first create", start,
                   "task_start_gate_.begin()", "xTaskCreate(")
    first_create = start.find("xTaskCreate(")
    second_create = start.find("xTaskCreate(", first_create + 1)
    second_failure_end = start.find("auto_pair_task_ = auto_pair_task", second_create)
    if second_create < 0 or second_failure_end < 0:
        raise AssertionError("vehicle second-create failure region is missing")
    second_failure = start[second_create:second_failure_end]
    for token in (
        "task_start_gate_.cancel();",
        "task_start_gate_.cancelled_tasks_acknowledged(1)",
        "loop_task_ = nullptr;",
        "task_start_gate_.reset_cancelled(1)",
    ):
        if token not in second_failure:
            raise AssertionError(f"vehicle cooperative second-create cleanup missing {token!r}")
    require_before("vehicle cancel before acknowledgement", second_failure,
                   "task_start_gate_.cancel();",
                   "task_start_gate_.cancelled_tasks_acknowledged(1)")
    require_before("vehicle acknowledgement before handle clear", second_failure,
                   "task_start_gate_.cancelled_tasks_acknowledged(1)",
                   "loop_task_ = nullptr;")
    if "vTaskDelete(" in start or "vTaskDelete(loop_task_)" in vehicle_source:
        raise AssertionError("vehicle task startup performs unsafe external task deletion")
    require_before("vehicle second allocation before release", start,
                   'xTaskCreate(auto_pair_task_fn_, "auto_pair"',
                   "task_start_gate_.release()")

    for token in (
        "task_start_gate_.entry_action()",
        "case tk::TaskEntryAction::Run:",
        "switch (tk::runtime_admission_action())",
        "case tk::RuntimeAdmissionAction::Run:  return true;",
        "case tk::RuntimeAdmissionAction::Stop: return false;",
        "case tk::RuntimeAdmissionAction::Wait:",
        "task_start_gate_.acknowledge_cancel();",
        "vTaskDelay(1);",
    ):
        if token not in await_start:
            raise AssertionError(f"vehicle entry admission loop missing {token!r}")
    require_before("dual-task gate before global Ready gate", await_start,
                   "task_start_gate_.entry_action()",
                   "tk::runtime_admission_action()")

    for label, entry, sensitive_patterns in (
        (
            "vehicle loop",
            loop_entry,
            (r"\besp_task_wdt_", r"\bTaskWatchdogSubscription\b",
             r"\b(?:SemGuard|MutexGuard)\b", r"\bself->ble_\b"),
        ),
        (
            "auto-pair",
            pair_entry,
            (r"\bvTaskDelay\s*\(", r"\bstack_watch_sample\s*\(",
             r"\b(?:SemGuard|MutexGuard)\b", r"\bself->ble_\b",
             r"\bself->health_probe_\s*\("),
        ),
    ):
        # Only the exact own-task diagnostic prefix may run before admission. It cannot log,
        # allocate, inspect another task, subscribe TWDT, wait, lock or touch BLE/health state.
        # All pre-existing sensitive-resource and cancellation checks remain below.
        entry_name = "loop_task_fn_" if label == "vehicle loop" else "auto_pair_task_fn_"
        code = scrub_cpp(f135_unwrap_sampling_prefix(entry_name, entry))
        await_at = code.find("self->await_task_start_()")
        if await_at < 0:
            raise AssertionError(f"{label} entry bypasses the task/global start barrier")
        sensitive = [
            match.start()
            for pattern in sensitive_patterns
            for match in re.finditer(pattern, code)
        ]
        if sensitive and await_at >= min(sensitive):
            raise AssertionError(f"{label} touches runtime resources before start admission")
        cancel = re.search(
            r"if\s*\(\s*!self\s*\|\|\s*!self->await_task_start_\(\)\s*\)\s*"
            r"\{(?P<body>.*?)\}", entry, re.DOTALL,
        )
        if not cancel or "vTaskDelete(nullptr);" not in cancel.group("body") or \
                "return;" not in cancel.group("body"):
            raise AssertionError(f"{label} cancellation is not ack/self-delete/return")

    watchdog_class = telemetry_source[
        telemetry_source.find("class TaskWatchdogSubscription"):
        telemetry_source.find("}  // namespace", telemetry_source.find(
            "class TaskWatchdogSubscription"))
    ]
    for token in ("esp_task_wdt_add(nullptr)", "~TaskWatchdogSubscription()",
                  "esp_task_wdt_delete(nullptr)"):
        if token not in watchdog_class:
            raise AssertionError(f"vehicle TWDT RAII contract missing {token!r}")
    require_before("vehicle TWDT subscription after entry admission", loop_entry,
                   "self->await_task_start_()", "TaskWatchdogSubscription watchdog;")


def require_vin_transition_cleanup_contract(main_source: str) -> None:
    marker = "recovery == tk::VinTransitionRecovery::CompleteNewIdentity"
    start = main_source.find(marker)
    if start < 0:
        raise AssertionError("main: CompleteNewIdentity VIN transition branch missing")
    end = main_source.find("esp_restart();", start)
    if end < 0:
        raise AssertionError("main: CompleteNewIdentity branch missing esp_restart()")
    branch = main_source[start : end + len("esp_restart();")]
    for storage_key in (
        "tk::nvs_contract::kSessionVcsec",
        "tk::nvs_contract::kSessionInfotainment",
        "tk::nvs_contract::kPairedAt",
        "tk::nvs_contract::kBleMac",
    ):
        if storage_key not in branch:
            raise AssertionError(f"main: CompleteNewIdentity missing removal of {storage_key}")
    require_before(
        "VIN transition cleanup check before marker removal",
        branch,
        "tk::vin_transition_cleanup_ready_for_marker_removal(",
        "config_store.remove(tk::nvs_contract::kVinTransition)",
    )
    require_before(
        "VIN transition completion gate before marker removal",
        branch,
        'return "VIN transition completion"',
        "config_store.remove(tk::nvs_contract::kVinTransition)",
    )
    require_before(
        "VIN transition marker removal before restart",
        branch,
        "config_store.remove(tk::nvs_contract::kVinTransition)",
        "esp_restart();",
    )


def cache_writes(code: str, name: str) -> list[int]:
    """Offsets of every write to `name`: assignment, member assignment or swap."""
    field = re.escape(name)
    pattern = (rf"\b{field}\s*(?:(?:\.|->)\s*\w+\s*)?=(?!=)"
               rf"|\bswap\s*\([^;]*\b{field}\b"
               rf"|\b{field}\s*\.\s*(?:store|fetch_add|fetch_sub|exchange)\s*\(")
    return [match.start() for match in re.finditer(pattern, code)]


def require_telemetry_epoch_cache_contract(telemetry_source: str) -> None:
    # Scrub once, before any search: a flag name or brace inside a comment or log literal must not
    # move a block boundary.
    body = scrub_cpp_preserving_layout(function_body_in(telemetry_source, "process_pending_telemetry_"))
    critical_end = body.find("portEXIT_CRITICAL(&telemetry_pending_mux_);")
    if critical_end < 0:
        raise AssertionError("process_pending_telemetry_ missing portEXIT_CRITICAL")
    processing_body = body[critical_end:]
    epoch_expr = "identity_epoch_.load(std::memory_order_acquire)"
    kinds = (
        ("charge", "PendingCharge", "charge_epoch", "last_known_charge_"),
        ("climate", "PendingClimate", "climate_epoch", "last_known_climate_"),
        ("drive", "PendingDrive", "drive_epoch", "last_known_drive_"),
        ("tires", "PendingTires", "tires_epoch", "last_known_tires_"),
        ("closures", "PendingClosures", "closures_epoch", "last_known_closures_"),
    )
    # The charge cache carries its freshness stamps; they must move with the sample they date.
    companions = {"charge": ("last_charge_ticks_", "charge_state_generation_", "charge_cache_stale_reported_")}
    branches: dict[str, tuple[int, int]] = {}
    for name, flag, epoch_var, target in kinds:
        flag_pos = processing_body.find(f"pending & {flag}")
        if flag_pos < 0:
            raise AssertionError(f"process_pending_telemetry_ missing handler for {flag}")
        next_pos = len(processing_body)
        for _, other_flag, _, _ in kinds:
            if other_flag == flag:
                continue
            pos = processing_body.find(f"pending & {other_flag}", flag_pos + len(flag))
            if 0 <= pos < next_pos:
                next_pos = pos
        block = processing_body[flag_pos:next_pos]
        label = f"process_pending_telemetry_ {name}"

        mutex_match = re.search(r"tk::MutexGuard\s+\w+\(cache_mutex_\);", block)
        if not mutex_match:
            raise AssertionError(f"{label} missing cache_mutex_ guard")
        # The guard's lexical scope is the critical section; a statement after its closing brace
        # runs unlocked even though it still follows the guard in token order.
        _, scope_close = enclosing_scope(block, mutex_match.start(), label)

        epoch_check = f"tk::telemetry_epoch_matches({epoch_var}"
        epoch_if = re.search(r"\bif\s*\(\s*" + re.escape(epoch_check), block[mutex_match.end():scope_close])
        if not epoch_if:
            raise AssertionError(f"{label} missing epoch check {epoch_check!r} under cache_mutex_")
        condition, body_open, body_close = if_statement_at(
            block, mutex_match.end() + epoch_if.start(), f"{label} epoch check")
        expected = f"tk::telemetry_epoch_matches({epoch_var}, {epoch_expr})"
        if condition != expected:
            raise AssertionError(f"{label} epoch check must be exactly {expected!r}, found {condition!r}")
        # An unlocked pre-check (charge) may skip a stale sample early, but must not be weakened.
        for early in re.finditer(r"\bif\s*\(", block[:mutex_match.start()]):
            early_condition, _, _ = if_statement_at(block, early.start(), f"{label} early epoch check")
            if epoch_check in early_condition and early_condition != f"!{expected}":
                raise AssertionError(f"{label} early epoch check must be exactly '!{expected}'")
        branches[name] = (flag_pos + body_open, flag_pos + body_close)
        if not re.search(r"\bnote_contact_\s*\(\s*\)\s*;", block[body_open:body_close]):
            raise AssertionError(f"{label} must note contact inside its epoch-checked cache_mutex_ branch")

    # Publication must happen only in the epoch-checked branch, inside the locked scope. Writes are
    # searched in the whole processing body, so a write placed in another kind's block is caught.
    def inside(pos: int, branch: tuple[int, int]) -> bool:
        return branch[0] < pos < branch[1]

    for name, _, _, target in kinds:
        label = f"process_pending_telemetry_ {name}"
        writes = cache_writes(processing_body, target)
        if not writes:
            raise AssertionError(f"{label} missing assignment to {target}")
        if any(not inside(pos, branches[name]) for pos in writes):
            raise AssertionError(f"{label} assigns {target} outside its epoch-checked cache_mutex_ branch")
        for companion in companions.get(name, ()):
            stamps = cache_writes(processing_body, companion)
            if not stamps or any(not inside(pos, branches[name]) for pos in stamps):
                raise AssertionError(f"{label} must update {companion} only inside its epoch-checked branch")
    for match in re.finditer(r"\bnote_contact_\s*\(\s*\)\s*;", processing_body):
        if not any(inside(match.start(), branch) for branch in branches.values()):
            raise AssertionError("process_pending_telemetry_ notes contact outside every epoch-checked branch")


def require_pairing_cleanup_epoch_contract(pairing_source: str) -> None:
    body = function_body_in(pairing_source, "clear_session_and_cache_")
    require_before(
        "pairing cleanup identity epoch before pending telemetry reset",
        body,
        "identity_epoch_.fetch_add",
        "telemetry_pending_mask_ = 0;",
    )
    require_before(
        "pairing cleanup pending telemetry reset before cache mutex",
        body,
        "telemetry_pending_mask_ = 0;",
        "cache_mutex_",
    )
    cache_block_start = body.find("cache_mutex_")
    if cache_block_start < 0:
        raise AssertionError("clear_session_and_cache_ missing cache_mutex_ guard")
    cache_block = body[cache_block_start:]
    for field in (
        "last_known_charge_",
        "last_known_status_",
        "last_known_climate_",
        "last_known_drive_",
        "last_known_tires_",
        "last_known_closures_",
    ):
        if not re.search(r"\b" + re.escape(field) + r"\s*=\s*\{\};", cache_block):
            raise AssertionError(f"clear_session_and_cache_ missing reset of {field}")
        require_before(
            f"pairing cleanup cache mutex before {field} reset",
            body,
            "cache_mutex_",
            field,
        )
    # The freshness stamps live beside the caches they date: a pairing reset that kept them would
    # let the next pairing's first read see the old sample as fresh (charge and climate alike).
    for statement in (
        "last_charge_ticks_.store(0);",
        "charge_state_generation_.store(0);",
        "last_climate_ticks_.store(0);",
        "climate_state_generation_.store(0);",
    ):
        if statement not in scrub_cpp(cache_block):
            raise AssertionError(f"clear_session_and_cache_ missing freshness reset {statement}")


def require_runtime_admission_contract(logic_header: str, facade_header: str,
                                       facade_source: str, main_source: str,
                                       http_route_header: str, dispatch: str) -> None:
    for token in (
        "std::atomic<std::uint8_t> state_{runtime_admission_pack(RuntimeAdmissionState::Booting)};",
        "std::uint8_t expected = runtime_admission_pack(RuntimeAdmissionState::Booting);",
        "RuntimeAdmissionState::Ready",
        "RuntimeAdmissionState::SafeMode",
        "if (runtime_admission_unpack(current).state == RuntimeAdmissionState::Fatal) return;",
        "state_.compare_exchange_weak(current,",
        "runtime_admission_pack(RuntimeAdmissionState::Fatal, component)",
        "(current & kRuntimeIdentityReloadClaim)",
        "kRuntimeIdentityReloadClaim = 0x40;",
        "(value & ~kRuntimeIdentityReloadClaim) >> 2",
        "runtime_admission_unpack(state_.load(std::memory_order_acquire))",
        "sizeof(RuntimeAdmissionGate) == sizeof(std::atomic<std::uint8_t>)",
        "== RuntimeAdmissionState::Ready",
        "case RuntimeAdmissionState::Booting:  return RuntimeAdmissionAction::Wait;",
        "case RuntimeAdmissionState::Ready:    return RuntimeAdmissionAction::Run;",
        "case RuntimeAdmissionState::SafeMode: return RuntimeAdmissionAction::Stop;",
        "case RuntimeAdmissionState::Fatal:    return RuntimeAdmissionAction::Stop;",
    ):
        if token not in logic_header:
            raise AssertionError(f"global runtime admission is not default-false: missing {token!r}")
    if logic_header.count("compare_exchange_strong") != 3:
        raise AssertionError("Ready/SafeMode and exact Ready-only reload claim must be the only strong CAS transitions")
    for method in ("mark_ready", "mark_safe_mode"):
        transition = function_body_in(logic_header, method)
        if "expected = runtime_admission_pack(RuntimeAdmissionState::Booting)" not in transition:
            raise AssertionError("Ready/SafeMode must remain Booting-only CAS transitions")
    claim = function_body_in(logic_header, "try_claim_identity_reload")
    for token in ("expected = runtime_admission_pack(RuntimeAdmissionState::Ready)",
                  "state_.compare_exchange_strong(expected,", "kRuntimeIdentityReloadClaim",
                  "std::memory_order_acq_rel, std::memory_order_acquire"):
        if token not in claim:
            raise AssertionError("runtime reload claim must use exact Ready-only atomic linearization")

    if facade_header.count("runtime_admission_vehicle_ready() noexcept;") != 1:
        raise AssertionError("runtime admission facade declaration drifted")
    if facade_source.count("RuntimeAdmissionGate s_runtime_admission;") != 1:
        raise AssertionError("runtime admission must have exactly one process-global gate")
    for token in (
        "s_runtime_admission.mark_ready()",
        "s_runtime_admission.mark_safe_mode()",
        "s_runtime_admission.mark_fatal(component)",
        "s_runtime_admission.snapshot()",
        "s_runtime_admission.try_claim_identity_reload()",
        "s_runtime_admission.vehicle_ready()",
        "s_runtime_admission.action()",
    ):
        if token not in facade_source:
            raise AssertionError(f"runtime admission facade bypasses the global gate: {token!r}")

    if main_source.count("tk::runtime_admission_mark_ready()") != 1:
        raise AssertionError("app_main must publish Ready exactly once")
    fatal = function_body_in(main_source.replace("[[noreturn]]", ""), "boot_fatal")
    hook = "tk::runtime_admission_mark_fatal(tk::runtime_fatal_component(component));"
    if not re.match(r"\{\s*" + re.escape(hook), scrub_cpp(fatal)):
        raise AssertionError("boot fatal component must be the first executable statement")
    require_before("late essential failure closes admission", fatal,
                   "tk::runtime_admission_mark_fatal(tk::runtime_fatal_component(component));",
                   "fatal_startup_rollback();")
    rollback = function_body_in(main_source, "fatal_startup_rollback")
    require_before("fatal rollback owns before metadata query", rollback,
                   "OtaFaultRestartGuard owner;", "esp_ota_get_running_partition()")
    require_before("vehicle tasks allocated before Ready", main_source,
                   "vehicle.start_tasks()", "tk::runtime_admission_mark_ready()")
    require_before("safe-mode latch before Ready branch", main_source,
                   "tk::runtime_admission_mark_safe_mode()",
                   "tk::runtime_admission_mark_ready()")

    route_body = function_body_in(http_route_header,
                                  "http_route_requires_vehicle_runtime")
    groups = re.findall(
        r"((?:case\s+HttpRoute::[A-Za-z0-9_]+\s*:\s*)+)"
        r"return\s+(true|false)\s*;", scrub_cpp(route_body), re.DOTALL,
    )
    classified: dict[bool, set[str]] = {True: set(), False: set()}
    for case_group, verdict in groups:
        classified[verdict == "true"].update(
            re.findall(r"case\s+HttpRoute::([A-Za-z0-9_]+)\s*:", case_group)
        )
    expected_active = {
        "Command", "VehicleData", "BodyController", "GenKeys", "SetVin",
        "Scan", "McpPost",
    }
    expected_all = set(HTTP_ROUTE_DISPATCH) | {"NotFound"}
    if classified[True] != expected_active or classified[True] | classified[False] != expected_all:
        raise AssertionError(
            "HTTP vehicle-runtime classification drift: "
            f"active={sorted(classified[True])}, "
            f"missing={sorted(expected_all - classified[True] - classified[False])}"
        )
    for token in (
        "tk::http_route_requires_vehicle_runtime(route)",
        "!tk::runtime_admission_vehicle_ready()",
        'httpd_resp_set_status(req, "503 Service Unavailable")',
        '"{\\"error\\":\\"vehicle runtime unavailable\\",\\"retryable\\":false}"',
    ):
        if token not in dispatch:
            raise AssertionError(f"HTTP runtime-admission response missing {token!r}")
    require_before("HTTP admission before every route handler", dispatch,
                   "tk::http_route_requires_vehicle_runtime(route)", "switch (route)")


def require_nimble_safe_mode_start_contract(main_source: str) -> None:
    """Pin the actual start call to the branch controlled by the durable boot verdict."""
    whole = scrub_cpp_preserving_layout(main_source)
    if len(re.findall(r"\bble_client\s*\.\s*start\s*\(", whole)) != 1:
        raise AssertionError("NimBLE must have exactly one production start call")
    code = scrub_cpp_preserving_layout(function_body_in(main_source, "app_main"))
    declarations = list(re.finditer(
        r"\bconst\s+bool\s+safe_mode\s*=\s*tk::safe_mode_begin\s*\(\s*"
        r"config_store\s*,\s*tk::boot_counter_cause\s*\(\s*tk::diag_crash_reset_info"
        r"\s*\(\s*\)\s*\.\s*code\s*,\s*boot_reboot_record\.state\s*\)\s*,\s*boot_reboot_record\.state\s*,\s*boot_reboot_record\.heap_restarts\s*\)\s*;", code))
    if len(declarations) != 1 or len(re.findall(r"\bbool\s+safe_mode\b", code)) != 1:
        raise AssertionError("NimBLE SafeMode input is not the single durable boot verdict")
    call = re.search(r"\bble_client\s*\.\s*start\s*\(", code)
    if call is None:
        raise AssertionError("NimBLE start is outside app_main")
    owner = enclosing_scope(code, call.start(), "NimBLE start scope")
    guards = []
    for match in re.finditer(r"\bif\s*\(\s*!safe_mode\s*\)\s*\{", code):
        _, opening, closing = if_statement_at(code, match.start(), "NimBLE SafeMode branch")
        if (opening, closing) == owner:
            guards.append((match.start(), opening, closing))
    if len(guards) != 1:
        raise AssertionError("NimBLE start must be inside the true !safe_mode body")
    guard, opening, closing = guards[0]
    if enclosing_scope(code, guard, "NimBLE guard scope") != enclosing_scope(
            code, declarations[0].start(), "SafeMode verdict scope"):
        raise AssertionError("NimBLE SafeMode guard is hidden in another control scope")
    if re.search(r"(?m)^\s*#", code[opening:closing]):
        raise AssertionError("NimBLE SafeMode branch has unsupported conditional preprocessing")


def require_nimble_start_ack_contract(gate_header: str, client_header: str,
                                      client_source: str, main_source: str,
                                      logic_tests: str, runtime_tests: str) -> None:
    for token in (
        "std::atomic<NimbleStartState> state_{NimbleStartState::Idle};",
        "NimbleStartState expected = NimbleStartState::Idle;",
        "NimbleStartState::AwaitingSync",
        "NimbleStartState expected = NimbleStartState::AwaitingSync;",
        "NimbleStartState::Synced",
        "NimbleStartState::TimedOut",
        "case NimbleStartState::Synced:",
        "return NimbleStartAction::Ready;",
        "case NimbleStartState::TimedOut:",
        "return NimbleStartAction::Fail;",
    ):
        if token not in gate_header:
            raise AssertionError(f"NimBLE host-start acknowledgement gate missing {token!r}")
    if gate_header.count("compare_exchange_strong") != 3:
        raise AssertionError("NimBLE start transitions must be exactly begin/sync/timeout CAS")
    for method, expected, target in (
        ("begin", "NimbleStartState::Idle", "NimbleStartState::AwaitingSync"),
        ("acknowledge_sync", "NimbleStartState::AwaitingSync", "NimbleStartState::Synced"),
        ("mark_timed_out", "NimbleStartState::AwaitingSync", "NimbleStartState::TimedOut"),
    ):
        body = function_body_in(gate_header, method)
        for token in (
            f"NimbleStartState expected = {expected};",
            f"compare_exchange_strong(expected, {target}",
        ):
            if token not in body:
                raise AssertionError(f"NimBLE {method} transition drift: missing {token!r}")
    if "tk::NimbleStartGate    start_gate_;" not in client_header:
        raise AssertionError("BleClient does not persist the host-start acknowledgement gate")

    start = function_body_in(client_source, "start")
    on_sync = function_body_in(client_source, "on_sync")
    on_sync_callback = function_body_in(client_source, "on_sync_cb")
    for token in (
        "ble_hs_cfg.sync_cb  = on_sync_cb;",
        "start_gate_.begin()",
        "nimble_port_freertos_init(nimble_host_task)",
        "constexpr TickType_t kSyncTimeout = pdMS_TO_TICKS(5000);",
        "switch (start_gate_.action())",
        "case tk::NimbleStartAction::Ready:",
        "case tk::NimbleStartAction::Fail:",
        "case tk::NimbleStartAction::Wait:",
        "xTaskGetTickCount() - started >= kSyncTimeout",
        "start_gate_.mark_timed_out()",
        "vTaskDelay(kPoll)",
    ):
        if token not in start:
            raise AssertionError(f"BleClient::start sync acknowledgement missing {token!r}")
    require_before("NimBLE gate begins before hidden task create", start,
                   "start_gate_.begin()", "nimble_port_freertos_init(nimble_host_task)")
    require_before("NimBLE callback registered before gate begin", start,
                   "ble_hs_cfg.sync_cb  = on_sync_cb;", "start_gate_.begin()")
    require_before("NimBLE hidden task create before acknowledgement wait", start,
                   "nimble_port_freertos_init(nimble_host_task)",
                   "switch (start_gate_.action())")
    require_before("NimBLE state consumed before timeout", start,
                   "switch (start_gate_.action())",
                   "xTaskGetTickCount() - started >= kSyncTimeout")
    start_code = scrub_cpp(start)
    if start_code.count("return true;") != 1:
        raise AssertionError("BleClient::start may return true only for one acknowledged Ready path")
    ready_branch = re.search(
        r"case\s+tk::NimbleStartAction::Ready\s*:\s*return\s+true\s*;",
        start_code,
    )
    fail_branch = re.search(
        r"case\s+tk::NimbleStartAction::Fail\s*:\s*return\s+false\s*;",
        start_code,
    )
    timeout_branch = re.search(
        r"if\s*\(\s*start_gate_\.mark_timed_out\(\)\s*\)\s*"
        r"\{(?P<body>.*?)\}", start, re.DOTALL,
    )
    if not ready_branch or not fail_branch:
        raise AssertionError("BleClient::start does not map Ready/Fail to true/false exactly")
    begin_denied = re.search(
        r"if\s*\(\s*!start_gate_\.begin\(\)\s*\)\s*"
        r"\{(?P<body>.*?)\}", start, re.DOTALL,
    )
    if not begin_denied or "return false;" not in scrub_cpp(begin_denied.group("body")):
        raise AssertionError("NimBLE acknowledgement gate begin failure is not fail-closed")
    if not timeout_branch or "return false;" not in scrub_cpp(timeout_branch.group("body")):
        raise AssertionError("NimBLE missing-sync timeout does not fail startup")

    for token in (
        "start_gate_.acknowledge_sync()",
        "start_gate_.state() == tk::NimbleStartState::TimedOut",
    ):
        if token not in on_sync:
            raise AssertionError(f"NimBLE on_sync terminal acknowledgement missing {token!r}")
    require_before("NimBLE callback acknowledgement before scanning", on_sync,
                   "start_gate_.acknowledge_sync()", "ensure_scanning_(0)")
    if "g_instance->on_sync()" not in on_sync_callback:
        raise AssertionError("registered NimBLE on_sync callback bypasses BleClient acknowledgement")

    require_nimble_safe_mode_start_contract(main_source)
    start_failure = re.search(
        r"if\s*\(\s*!ble_client\.start\(\)\s*\)\s*"
        r"boot_fatal\s*\(\s*\"NimBLE\"\s*\)\s*;",
        main_source,
    )
    if not start_failure:
        raise AssertionError("app_main does not make missing NimBLE sync boot-fatal")
    require_before("NimBLE sync acknowledged before RuntimeAdmission Ready", main_source,
                   "ble_client.start()", "tk::runtime_admission_mark_ready()")

    for token in (
        "test_nimble_start_gate",
        "hidden_create_failure",
        "hidden_create_failure.mark_timed_out()",
        "!hidden_create_failure.acknowledge_sync()",
        "timeout_wins",
        "!success.mark_timed_out()",
    ):
        if token not in logic_tests:
            raise AssertionError(f"NimBLE pure start matrix missing {token!r}")
    for token in (
        "test_nimble_start_ack_runtime_matrix",
        "create_failure",
        "create_failure.mark_timed_out()",
        "!create_failure.acknowledge_sync()",
        "ready_publications == 0",
        "publish_runtime_ready(acknowledged, ready_publications)",
    ):
        if token not in runtime_tests:
            raise AssertionError(f"NimBLE executable admission matrix missing {token!r}")


def require_dynamic_ble_host_health_contract(client_header: str, client_source: str,
                                             main_source: str, ota_source: str,
                                             logic_tests: str, runtime_tests: str) -> None:
    if (
        "bool host_synced() const noexcept { return host_synced_.load(std::memory_order_acquire); }"
        not in client_header
    ):
        raise AssertionError("BleClient current host-health accessor is not atomic/acquire")
    if "bool ble_host_synced() noexcept;" not in client_header:
        raise AssertionError("current NimBLE health facade is missing")
    if (
        "return host_reset_count_.load(std::memory_order_acquire);" not in client_header or
        "std::uint32_t ble_host_reset_count() noexcept;" not in client_header
    ):
        raise AssertionError("sticky NimBLE reset evidence has no atomic/acquire facade")

    facade = function_body_in(client_source, "ble_host_synced")
    for token in ("BleClient* instance = g_instance;",
                  "return instance && instance->host_synced();"):
        if token not in facade:
            raise AssertionError(f"current NimBLE health facade missing {token!r}")
    reset_facade = function_body_in(client_source, "ble_host_reset_count")
    for token in ("BleClient* instance = g_instance;",
                  "return instance ? instance->host_reset_count() : 0;"):
        if token not in reset_facade:
            raise AssertionError(f"sticky NimBLE reset facade missing {token!r}")
    on_sync = function_body_in(client_source, "on_sync")
    on_reset = function_body_in(client_source, "on_reset")
    if "host_synced_ = true;" not in on_sync:
        raise AssertionError("NimBLE on_sync does not publish current host health")
    if "host_synced_ = false;" not in on_reset:
        raise AssertionError("NimBLE on_reset does not revoke current host health")
    for token in (
        "resets != UINT32_MAX",
        "host_reset_count_.compare_exchange_weak(",
        "resets, resets + 1, std::memory_order_acq_rel, std::memory_order_acquire",
    ):
        if token not in on_reset:
            raise AssertionError(f"NimBLE sticky/saturating reset evidence missing {token!r}")
    require_before("NimBLE reset evidence before sync revocation", on_reset,
                   "host_reset_count_.compare_exchange_weak(", "host_synced_ = false;")

    health_task = function_body_in(main_source, "ota_health_gate_task")
    require_health_commit_contract(health_task)
    require_operation_wrapper_contract(ota_source)

    for token in (
        "test_ble_host_health_ota_matrix",
        "host_synced = false",
        "host_reset_count = 1",
        "host_synced = true;  // later on_sync after reset",
        "!explicit_commit_allowed(true, host_synced, host_reset_count, true, true)",
        "host_reset_count = 0;  // fresh stable boot",
        "UINT32_MAX",
        "tk::HealthVerdict::Wait",
        "tk::HealthVerdict::Commit",
    ):
        if token not in logic_tests:
            raise AssertionError(f"dynamic BLE OTA pure matrix missing {token!r}")
    for token in (
        "test_dynamic_ble_host_health_blocks_ota_commit",
        "host_synced = false;  // on_reset, no later on_sync",
        "host_reset_count = 1",
        "timed_mark_calls == 1",
        "explicit_mark_calls == 1",
        "host_synced = true;  // quick resync cannot erase sticky reset evidence",
        "host_reset_count = 0;  // only a fresh stable boot is eligible again",
        "timed_mark_calls == 2",
        "explicit_mark_calls == 2",
    ):
        if token not in runtime_tests:
            raise AssertionError(f"dynamic BLE OTA executable matrix missing {token!r}")


def require_ping_probe_contract(header: str, generation_header: str,
                                net_source: str, syslog_source: str,
                                runtime_tests: str) -> None:
    f135_fixed_ping_targets("\n".join((header, net_source, syslog_source)))
    for label, source in (("network watchdog", net_source), ("syslog", syslog_source)):
        if len(re.findall(r"\bping_probe_run\s*\(", scrub_cpp(source))) != 1:
            raise AssertionError(f"{label} must delegate to ping_probe_run exactly once")
        for token in ("PingProbeControl", "xSemaphoreCreateBinary()"):
            if token not in source:
                raise AssertionError(f"{label} does not own persistent ping state: {token!r}")

    control_start = header.find("struct PingProbeControl {")
    control_end = header.find("enum class PingProbeResult", control_start)
    if control_start < 0 or control_end < 0:
        raise AssertionError("persistent PingProbeControl is missing")
    control = header[control_start:control_end]
    for token in ("SemaphoreHandle_t done", "PingProbeGeneration generation",
                  "PingProbeCallbackArgs callback_args", "esp_ping_handle_t session",
                  "std::uint32_t session_generation", "bool session_started"):
        if token not in control:
            raise AssertionError(f"persistent ping owner missing {token!r}")

    callback = function_body_in(header, "ping_probe_on_end")
    for token in (
        "const bool measurement_valid =",
        "esp_ping_get_profile(handle, ESP_PING_PROF_REPLY",
        "control.generation.complete(args->generation, replies, measurement_valid)",
    ):
        if token not in callback:
            raise AssertionError(f"ping callback measurement-validity contract missing {token!r}")
    require_before("ping callback exact generation before wake", callback,
                   "control.generation.complete(args->generation, replies, measurement_valid)",
                   "xSemaphoreGive(control.done)")
    cleanup = function_body_in(header, "ping_probe_cleanup_completed")
    for token in (
        "if (!control.session_started)",
        "control.generation.abandon_unstarted(control.session_generation)",
        "control.generation.ended(control.session_generation)",
        "esp_ping_delete_session(control.session)",
        "control.generation.retire(control.session_generation)",
        "control.session = nullptr",
        "control.callback_args = {}",
    ):
        if token not in cleanup:
            raise AssertionError(f"ping exact-end cleanup missing {token!r}")
    unstarted_at = cleanup.find("if (!control.session_started)")
    started_at = cleanup.find(
        "if (!control.generation.ended(control.session_generation))", unstarted_at
    )
    if unstarted_at < 0 or started_at < 0:
        raise AssertionError("ping started/unstarted cleanup ownership branches are missing")
    unstarted_cleanup = cleanup[unstarted_at:started_at]
    started_cleanup = cleanup[started_at:]
    for token in (
        "if (esp_ping_delete_session(control.session) != ESP_OK) return false;",
        "control.generation.abandon_unstarted(control.session_generation)",
        "control.session = nullptr",
        "control.callback_args = {}",
    ):
        if token not in unstarted_cleanup:
            raise AssertionError(f"ping failed-start delete retry missing {token!r}")
    require_before("ping unstarted delete before generation release", unstarted_cleanup,
                   "esp_ping_delete_session(control.session)",
                   "control.generation.abandon_unstarted(control.session_generation)")
    require_before("ping unstarted release before callback storage reuse", unstarted_cleanup,
                   "control.generation.abandon_unstarted(control.session_generation)",
                   "control.callback_args = {}")
    require_before("ping exact end before started delete", started_cleanup,
                   "control.generation.ended(control.session_generation)",
                   "esp_ping_delete_session(control.session)")
    require_before("ping started delete before generation retire", started_cleanup,
                   "esp_ping_delete_session(control.session)",
                   "control.generation.retire(control.session_generation)")
    require_before("ping retire before callback storage reuse", started_cleanup,
                   "control.generation.retire(control.session_generation)",
                   "control.callback_args = {}")

    run = function_body_in(header, "ping_probe_run")
    for token in (
        "if (!ping_probe_cleanup_completed(control)) return PingProbeResult::PendingEnd;",
        "control.generation.begin()",
        "callbacks.cb_args = &control.callback_args;",
        "callbacks.on_ping_end = on_end;",
        "control.session = session;",
        "control.session_generation = generation;",
        "control.session_started = false;",
        "if (esp_ping_delete_session(session) == ESP_OK)",
        "control.session_started = true;",
        "control.generation.abandon_unstarted(generation)",
        "esp_ping_stop(session);",
        "return PingProbeResult::PendingEnd;",
        "control.generation.result(generation, replies, measurement_valid)",
        "if (!measurement_valid) return PingProbeResult::Unavailable;",
    ):
        if token not in run:
            raise AssertionError(f"ping lifecycle contract missing {token!r}")
    require_before("ping quarantine cleanup before generation begin", run,
                   "ping_probe_cleanup_completed(control)", "control.generation.begin()")
    require_before("ping callback storage before session creation", run,
                   "control.callback_args = {&control, generation};",
                   "esp_ping_new_session")
    require_before("ping retain handle before start", run,
                   "control.session = session;", "esp_ping_start(session)")
    require_before("ping classify unstarted before start", run,
                   "control.session_started = false;", "esp_ping_start(session)")
    require_before("ping publish started only after successful start", run,
                   "esp_ping_start(session)", "control.session_started = true;")
    start_failure_at = run.find("if (esp_ping_start(session) != ESP_OK)")
    started_publish_at = run.find("control.session_started = true;", start_failure_at)
    start_failure = run[start_failure_at:started_publish_at]
    if not re.search(
        r"if\s*\(\s*esp_ping_delete_session\s*\(\s*session\s*\)\s*==\s*ESP_OK\s*\)\s*"
        r"\{(?P<body>.*?)\}\s*return\s+PingProbeResult::Unavailable\s*;",
        start_failure, re.DOTALL,
    ):
        raise AssertionError(
            "ping start failure does not retain ownership unless exact delete succeeds"
        )
    first_wait = run.find("ping_probe_wait_for_end(control, generation, completion_timeout)")
    stop = run.find("esp_ping_stop(session)", first_wait)
    second_wait = run.find("ping_probe_wait_for_end(control, generation, stop_ack_timeout)", stop)
    pending = run.find("return PingProbeResult::PendingEnd;", second_wait)
    result = run.find("control.generation.result(generation, replies, measurement_valid)", pending)
    if min(first_wait, stop, second_wait, pending, result) < 0 or not (
            first_wait < stop < second_wait < pending < result):
        raise AssertionError("ping timeout must stop, await exact end, quarantine, then read result")
    timeout_region = run[stop:result]
    if "esp_ping_delete_session" in timeout_region or "abandon_unstarted" in timeout_region:
        raise AssertionError("timed-out ping is deleted/reused before exact on_ping_end")

    for token in (
        "active_.load(std::memory_order_acquire) != generation",
        "ended_.load(std::memory_order_acquire) == generation",
        "active_.compare_exchange_strong(expected, 0",
        "measurement_valid_.store(false, std::memory_order_relaxed)",
        "measurement_valid_.store(measurement_valid, std::memory_order_relaxed)",
        "measurement_valid = measurement_valid_.load(std::memory_order_acquire)",
    ):
        if token not in generation_header:
            raise AssertionError(f"ping generation exactness missing {token!r}")

    # The static gate is backed by the real inline production implementation running against a
    # deterministic esp_ping/FreeRTOS stub, including the late and stale callback paths.
    for token in (
        "test_ping_probe_lifecycle",
        "PingScenario::SetupFail",
        "PingScenario::StartFail",
        "PingScenario::Reply",
        "PingScenario::NoReply",
        "PingScenario::ProfileFail",
        "PingScenario::TimeoutLateEnd",
        "stale_generation",
        "pending_session->callbacks.on_ping_end",
        "ping_new_calls == 1",
        "ping_delete_calls == 0",
        "ping_delete_calls == 2",
        "gateway_failure(tk::PingProbeResult::Unavailable)",
        "gateway_failure(tk::PingProbeResult::PendingEnd)",
        "PingScenario::StartDeleteFail",
        "ping_delete_failures_remaining = 2",
        "control.session == retained_session",
        "control.generation.active() == retained_generation",
        "ping_new_calls == 1",
        "ping_delete_calls == 4",
    ):
        if token not in runtime_tests:
            raise AssertionError(f"executable ping lifecycle matrix missing {token!r}")

    gateway = function_body_in(net_source, "gateway_reachable")
    if net_source.count("advance_lease_generation(kind);") != 2 or \
       "s_lease_generations.compare_exchange_weak(" not in net_source:
        raise AssertionError("every link-up and link-down must advance the lease generation")
    for token in (
        "out.identity.address = ip.gw.addr;",
        "out.identity.lease_generation = lease_generation(kind);",
        "out.reachable = result != PingProbeResult::NoReply;",
        "out.replied = result == PingProbeResult::Reply;",
        # Transport-attribution seams (F12): the probe is bound to the sampled netif, and a verdict
        # is discarded when the default route moved while the blocking echo was in flight.
        "cfg.interface   = static_cast<uint32_t>(esp_netif_get_netif_impl_index(netif));",
        "if (kind == NetLink::None || s_kind.load() != kind || net_active_netif() != netif) return out;",
        "lease_generation(kind) != out.identity.lease_generation) return out;",
        "current_ip.gw.addr != out.identity.address) return out;",
    ):
        if token not in gateway:
            raise AssertionError(f"gateway unknown-vs-failure policy missing {token!r}")
    require_before("gateway route validation before reply attribution", gateway,
                   "current_ip.gw.addr != out.identity.address) return out;",
                   "out.replied = result == PingProbeResult::Reply;")
    require_before("gateway transport-change discard before reset verdict", gateway,
                   "if (s_kind.load() != kind || net_active_netif() != netif ||",
                   "out.reachable = result != PingProbeResult::NoReply;")
    watchdog = function_body_in(net_source, "net_watchdog_task")
    for token in ("sample.identity != watched_identity", "tk::GatewayBaseline baselines[2]{};",
                  "baselines[baseline_index].observe(sample.identity, sample.replied)",
                  "net_recover(sample.identity)"):
        if token not in watchdog:
            raise AssertionError(f"gateway identity baseline missing {token!r}")
    recover = function_body_in(net_source, "net_recover")
    if "current_ip.gw.addr != sampled.address" not in recover or \
       "lease_generation(sampled.transport) != sampled.lease_generation" not in recover or \
       "switch (sampled.transport)" not in recover:
        raise AssertionError("net_recover must act only on the gateway that was probed")


def require_ota_fetch_contract(ota_source: str, ota_logic: str,
                               manifest_header: str) -> None:
    fetch = function_body_in(ota_source, "http_get_to_buffer")
    check = function_body_in(ota_source, "perform_check")
    for token in ("out.reserve(tk::kOtaManifestMaxBytes)",
                  "operation.coordinator.start(url, limits, false, MetadataSink::append, &sink, optional)",
                  "operation.job.phase() == tk::ota::Phase::Closed", "poll_operation(operation)"):
        if token not in fetch:raise AssertionError("bounded actual OTA fetch missing "+token)
    require_before("body allocation before worker request",fetch,"out.reserve(","operation.coordinator.start(")

    required_check_tokens = (
        "http_get_to_buffer(operation, manifest_url, body)",
        "tk::json_materialize<cJSON>",
        "cJSON_ParseWithLengthOpts(",
        "tk::inspect_ota_manifest(j.get())",
        "parse_end != body.c_str() + body.size()",
        "std::string{}.swap(body);",
        "const std::string_view available(manifest.value);",
        "tk::canonical_ota_version(available)",
        "tk::canonical_ota_version(res.current)",
        "res.available.assign(available.data(), available.size());",
        "tk::is_ota_update_available(res.available, res.current, pr_number, channel)",
    )
    for token in required_check_tokens:
        if token not in check:
            raise AssertionError(f"OTA manifest integration contract missing {token!r}")
    ordered = (
        "http_get_to_buffer(operation, manifest_url, body)",
        "tk::json_materialize<cJSON>",
        "tk::inspect_ota_manifest(j.get())",
        "parse_end != body.c_str() + body.size()",
        "std::string{}.swap(body);",
        "tk::canonical_ota_version(available)",
        "res.available.assign(available.data(), available.size());",
        "tk::is_ota_update_available(res.available, res.current, pr_number, channel)",
    )
    for before, after in zip(ordered, ordered[1:]):
        require_before("OTA validate/release/copy ordering", check, before, after)
    if re.search(r"\bsscanf\s*\(|%d\s*\.\s*%d\s*\.\s*%d", ota_source):
        raise AssertionError("OTA version comparison regressed to fixed-width integer parsing")

    for token in (
        "inline constexpr std::size_t kOtaVersionMaxBytes = 31;",
        "inline constexpr std::size_t kOtaManifestMaxBytes = 8192;",
        "input.size() > kOtaVersionMaxBytes",
        "class BoundedHttpBodyGate",
        "remaining_with_probe = (limit_ - received_) + 1",
    ):
        if token not in ota_logic:
            raise AssertionError(f"OTA bounded pure contract missing {token!r}")
    for token in (
        "for (const cJSON* item = root->child; item; item = item->next)",
        "for (const cJSON* later = item->next; later; later = later->next)",
        "std::strcmp(item->string, later->string) == 0",
        "OtaManifestInspectStatus::DuplicateKey",
    ):
        if token not in manifest_header:
            raise AssertionError(f"OTA duplicate-root-key gate missing {token!r}")


def require_coredump_stream_contract(status_source: str, runtime_tests: str) -> None:
    handler = function_body_in(status_source, "handle_coredump")
    loop_start = handler.find("for (uint32_t offset = 0; offset < source.bytes;)")
    terminal = handler.find("httpd_resp_send_chunk(req, nullptr, 0)", loop_start)
    if loop_start < 0 or terminal < 0:
        raise AssertionError("coredump fixed-chunk stream/terminator is missing")
    loop = handler[loop_start:terminal]
    read_at = loop.find("esp_partition_read(source.partition, offset, buffer.bytes, bytes)")
    send_at = loop.find("httpd_resp_send_chunk(req, buffer.bytes, bytes)", read_at)
    if read_at < 0 or send_at < 0 or read_at >= send_at:
        raise AssertionError("coredump flash read is not checked before sending its chunk")
    read_failure_region = scrub_cpp(loop[read_at:send_at])
    if "return ESP_FAIL;" not in read_failure_region:
        raise AssertionError("coredump flash-read failure does not abort the HTTP stream")
    if re.search(r"\bbreak\s*;", read_failure_region):
        raise AssertionError("coredump flash-read failure can fall through to a clean terminator")
    # Token order alone accepts a guard that can never fire (`if (false && read_err != ESP_OK)`):
    # require the one guard between read and send to test the read result itself and to own the
    # failure return.
    clean_loop = scrub_cpp_preserving_layout(loop)
    if not re.search(r"\bconst\s+esp_err_t\s+read_err\s*=\s*esp_partition_read\(source\.partition,\s*offset,\s*buffer\.bytes,\s*bytes\)\s*;",
                     clean_loop[:send_at]):
        raise AssertionError("coredump flash read result is not kept in read_err")
    guard = re.search(r"\bif\s*\(", clean_loop[read_at:send_at])
    if not guard:
        raise AssertionError("coredump flash read has no failure guard before its chunk is sent")
    condition, body_open, body_close = if_statement_at(clean_loop, read_at + guard.start(), "coredump read guard")
    if condition != "read_err != ESP_OK":
        raise AssertionError(f"coredump read guard must test exactly read_err != ESP_OK, found {condition!r}")
    if body_close >= send_at:
        raise AssertionError("coredump read guard does not close before the chunk is sent")
    # The failure return must be an unconditional statement of the guard body: at its top brace
    # level and directly after the previous statement, not behind a nested or unbraced condition.
    guard_body = clean_loop[body_open + 1:body_close]
    owned = False
    for match in re.finditer(r"\breturn\s+ESP_FAIL\s*;", guard_body):
        depth = guard_body[:match.start()].count("{") - guard_body[:match.start()].count("}")
        previous = guard_body[:match.start()].rstrip()
        if depth == 0 and (not previous or previous[-1] in ";}"):
            owned = True
    if not owned:
        raise AssertionError("coredump read guard does not own an unconditional failure return")
    if handler.count("httpd_resp_send_chunk(req, nullptr, 0)") != 1:
        raise AssertionError("coredump stream must have exactly one success-only terminating chunk")
    require_before("coredump final data chunk before terminator", handler,
                   "httpd_resp_send_chunk(req, buffer.bytes, bytes)",
                   "httpd_resp_send_chunk(req, nullptr, 0)")

    for token in (
        "test_coredump_read_failure_aborts_stream",
        "first_failure.fail_read_index = 0",
        "nth_failure.fail_read_index = 2",
        "first_failure.terminating_chunks == 0",
        "nth_failure.terminating_chunks == 0",
        "complete.terminating_chunks == 1",
    ):
        if token not in runtime_tests:
            raise AssertionError(f"coredump executable failure matrix missing {token!r}")


def require_heap_json_stream_contract(status_source: str, stream_header: str,
                                      logic_tests: str) -> None:
    handler = function_body_in(status_source, "handle_heap")
    clean_handler = scrub_cpp(handler)
    if re.search(r"\bcJSON\b|\bJsonBuilder\b|\bstd::(?:string|vector)\b|\bnew\b|\bmalloc\s*\(",
                 clean_handler):
        raise AssertionError("/heap regressed to a tree/dynamic whole-body response")
    for token in (
        "static constexpr size_t kMax = tk::kHeapHistorySamples;",
        "tk::HeapTrendSample free_s[kMax];",
        "tk::HeapTrendSample large_s[kMax];",
        "tk::heap_trend_snapshot(free_s, large_s, kMax, &bucket0, &boot_bucket)",
        "const tk::HeapJsonStreamView view",
        "tk::stream_heap_json(view, [req](const char* data, size_t size)",
        "return httpd_resp_send_chunk(req, data, size) == ESP_OK;",
        "if (!sent) return ESP_FAIL;",
        "return httpd_resp_send_chunk(req, nullptr, 0);",
    ):
        if token not in handler:
            raise AssertionError(f"/heap bounded production stream missing {token!r}")
    if handler.count("httpd_resp_send_chunk(req") != 2:
        raise AssertionError("/heap must have one data-send seam and one success terminator")
    require_before("/heap snapshot before streaming", handler,
                   "tk::heap_trend_snapshot", "tk::stream_heap_json")
    require_before("/heap stream result before failure branch", handler,
                   "const bool sent = tk::stream_heap_json", "if (!sent) return ESP_FAIL;")
    require_before("/heap failure branch before success terminator", handler,
                   "if (!sent) return ESP_FAIL;",
                   "return httpd_resp_send_chunk(req, nullptr, 0);")

    clean_stream = scrub_cpp(stream_header)
    if re.search(r"\bcJSON\b|\bJsonBuilder\b|\bstd::(?:string(?!_view)|vector)\b|"
                 r"\b(?:new|delete|malloc|calloc|realloc|free|throw)\b", clean_stream):
        raise AssertionError("heap stream seam contains allocating/throwing whole-body work")
    for token in (
        "char buffer_[192]{};",
        "if (view.count != 0 && (!view.free_samples || !view.largest_samples)) return false;",
        "if (used_ == sizeof(buffer_) && !flush()) return false;",
        "if (!send_(buffer_, used_)) return false;",
        "if (view.free_samples[i] == view.absent)",
        "if (view.largest_samples[i] == view.absent)",
        'writer.text("null")',
        "writer.signed_number(view.free_samples[i])",
        "writer.signed_number(view.largest_samples[i])",
        'return writer.text("]}") && writer.finish();',
    ):
        if token not in stream_header:
            raise AssertionError(f"heap fixed-buffer JSON seam missing {token!r}")
    if stream_header.count("for (std::size_t i = 0; i < view.count; ++i)") != 2:
        raise AssertionError("heap stream does not traverse both series at the exact count")

    for token in (
        "test_heap_json_stream",
        "constexpr std::size_t kFullCount = 288;",
        "free_samples.data(), largest_samples.data(), kFullCount",
        "free_edges[] = {kAbsent, -32767, 0, 32767}",
        "largest_edges[] = {32767, 0, -32767, kAbsent}",
        "missing_free.calls == 0",
        "missing_largest.calls == 0",
        "first_send_failure.fail_at = 0",
        "first_send_failure.calls == 1",
        "nth_send_failure.fail_at = 2",
        "nth_send_failure.calls == 3",
        "full.joined() == expected",
        "size <= 192",
    ):
        if token not in logic_tests:
            raise AssertionError(f"heap executable stream matrix missing {token!r}")


def require_syslog_start_lifetime_contract(syslog_source: str, gate_header: str,
                                           logic_tests: str, runtime_tests: str) -> None:
    for token in (
        "enum class SyslogStartState", "Idle", "Waiting", "Running", "Cancelled",
        "enum class SyslogStartAction", "class SyslogStartGate",
        "bool begin() noexcept", "bool commit() noexcept", "bool cancel() noexcept",
        "SyslogStartAction action() const noexcept",
    ):
        if token not in gate_header:
            raise AssertionError(f"Syslog start gate missing {token!r}")
    for operation, expected, target in (
        ("begin", "SyslogStartState::Idle", "SyslogStartState::Waiting"),
        ("commit", "SyslogStartState::Waiting", "SyslogStartState::Running"),
        ("cancel", "SyslogStartState::Waiting", "SyslogStartState::Cancelled"),
    ):
        body = function_body_in(gate_header, operation)
        if expected not in body or target not in body:
            raise AssertionError(
                f"Syslog {operation} transition is not exactly {expected} -> {target}"
            )
    action_start = gate_header.find("SyslogStartAction action() const noexcept")
    action_end = gate_header.find("SyslogStartState state() const noexcept", action_start)
    if action_start < 0 or action_end < 0:
        raise AssertionError("Syslog start action/state accessors are missing")
    action = gate_header[action_start:action_end]
    for state, result in (
        ("SyslogStartState::Waiting", "SyslogStartAction::Wait"),
        ("SyslogStartState::Running", "SyslogStartAction::Run"),
        ("SyslogStartState::Cancelled", "SyslogStartAction::Cancel"),
    ):
        if not re.search(rf"case\s+{re.escape(state)}:\s*return\s+{re.escape(result)};", action):
            raise AssertionError(f"Syslog start action mapping missing {state} -> {result}")

    if "static std::atomic<QueueHandle_t> s_queue{nullptr};" not in syslog_source:
        raise AssertionError("Syslog sender queue is not an atomic boot-lifetime publication")
    owner_start = syslog_source.find("struct SyslogStartResources")
    owner_end = syslog_source.find("static void set_status", owner_start)
    if owner_start < 0 or owner_end < 0:
        raise AssertionError("Syslog unpublished startup resource owner is missing")
    owner = syslog_source[owner_start:owner_end]
    for token in (
        "QueueHandle_t queue{nullptr};", "SemaphoreHandle_t ping_done{nullptr};",
        "SemaphoreHandle_t status_mtx{nullptr};", "if (queue) vQueueDelete(queue);",
        "void release() noexcept",
    ):
        if token not in owner:
            raise AssertionError(f"Syslog unpublished owner missing {token!r}")
    if syslog_source.count("vQueueDelete(") != 1:
        raise AssertionError("Syslog has queue deletion outside unpublished local ownership")

    start = function_body_in(syslog_source, "syslog_start_impl")
    for token in (
        "SyslogStartResources resources;", "resources.queue      = xQueueCreate",
        "s_task_start.queue = resources.queue;", "if (!s_task_start.gate.begin())",
        "xTaskCreate(syslog_task", "(void)s_task_start.gate.cancel();",
        "QueueHandle_t const queue = resources.queue;", "resources.release();",
        "s_queue.store(queue, std::memory_order_release);",
        "if (!s_task_start.gate.commit())",
        "s_queue.store(nullptr, std::memory_order_release);",
    ):
        if token not in start:
            raise AssertionError(f"Syslog startup lifetime missing {token!r}")
    require_before("Syslog task gate before task creation", start,
                   "s_task_start.gate.begin()", "xTaskCreate(syslog_task")
    require_before("Syslog failed create cancels before local unwind", start,
                   "xTaskCreate(syslog_task", "s_task_start.gate.cancel()")
    require_before("Syslog local delete authority released before sender publication", start,
                   "resources.release();", "s_queue.store(queue, std::memory_order_release);")
    require_before("Syslog queue publication before consumer Run", start,
                   "s_queue.store(queue, std::memory_order_release);",
                   "s_task_start.gate.commit()")

    task = function_body_in(syslog_source, "syslog_task")
    for token in (
        "start->gate.action()", "tk::SyslogStartAction::Run",
        "tk::SyslogStartAction::Cancel", "QueueHandle_t const queue = start->queue;",
        "xQueueReceive(queue",
    ):
        if token not in task:
            raise AssertionError(f"Syslog consumer start/lifetime missing {token!r}")
    if "xQueueReceive(s_queue" in task:
        raise AssertionError("Syslog consumer re-reads sender publication")

    send = function_body_in(syslog_source, "syslog_send")
    if "if (xQueueSend(queue, &m, 0) != pdPASS)" not in scrub_cpp(send) or \
       "s_dropped_lines.fetch_add(1, std::memory_order_relaxed)" not in scrub_cpp(send):
        raise AssertionError("Syslog producer must count queue-full loss without waiting or logging")

    for token in (
        "s_queue.load(std::memory_order_acquire)", "if (!queue) return;",
        "xQueueSend(queue, &m, 0)",
    ):
        if token not in send:
            raise AssertionError(f"Syslog hook queue lifetime missing {token!r}")

    for token in (
        "test_syslog_start_gate", "task_create_failure", "Action::Cancel",
        "!task_create_failure.commit()",
    ):
        if token not in logic_tests:
            raise AssertionError(f"Syslog pure start matrix missing {token!r}")
    runtime_matrix = function_body_in(runtime_tests, "test_syslog_queue_publication_lifetime")
    for token in (
        "allocation_failure_deletes == 1",
        "task_failure_deletes == 1", "success_deletes == 0", "send_from_log_hook",
        "task_step() == tk::SyslogStartAction::Wait",
        "task_step() == tk::SyslogStartAction::Cancel",
        "task_step() == tk::SyslogStartAction::Run",
    ):
        if token not in runtime_matrix:
            raise AssertionError(f"Syslog executable publication matrix missing {token!r}")
    if "test_syslog_queue_publication_lifetime();" not in function_body_in(runtime_tests, "main"):
        raise AssertionError("Syslog executable publication matrix is not invoked")


def require_ethernet_start_cleanup_contract(net_source: str, runtime_tests: str) -> None:
    cleanup = function_body_in(net_source, "eth_cleanup_startup")
    for token in (
        "esp_eth_stop(r.handle)", "esp_event_handler_unregister(",
        "IP_EVENT, IP_EVENT_ETH_GOT_IP, eth_event_handler",
        "ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler", "if (r.published)",
        "esp_eth_del_netif_glue(r.glue)", "esp_eth_driver_uninstall(r.handle)",
        "r.mac->del(r.mac)", "r.phy->del(r.phy)", "esp_netif_destroy(r.netif)",
        "vEventGroupDelete(r.events)", "eth_release_spi_bus()",
    ):
        if token not in cleanup:
            raise AssertionError(f"Ethernet reverse cleanup missing {token!r}")
    ordered = (
        "esp_eth_stop(r.handle)", "IP_EVENT, IP_EVENT_ETH_GOT_IP, eth_event_handler",
        "ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler", "if (r.published)",
        "esp_eth_del_netif_glue(r.glue)", "esp_eth_driver_uninstall(r.handle)",
        "r.mac->del(r.mac)", "r.phy->del(r.phy)", "esp_netif_destroy(r.netif)",
        "vEventGroupDelete(r.events)", "eth_release_spi_bus()",
    )
    positions = [cleanup.find(token) for token in ordered]
    if min(positions) < 0 or positions != sorted(positions):
        raise AssertionError("Ethernet startup cleanup is not in reverse ownership order")
    retention = re.search(r"if \(r\.start_attempted\) \{(.*?)^    \}", cleanup, re.M | re.S)
    if retention is None or "return false;" not in retention.group(1):
        raise AssertionError("Ethernet activated graph must be retained before passive cleanup")
    if any(token in retention.group(1) for token in (
        "esp_event_handler_unregister", "esp_eth_del_netif_glue", "esp_netif_destroy",
        "esp_eth_driver_uninstall", "r.mac->del", "r.phy->del", "vEventGroupDelete",
        "eth_release_spi_bus", "= nullptr", "= false",
    )):
        raise AssertionError("Ethernet activated graph retention releases or retracts owned state")

    start = function_body_in(net_source, "net_start_eth")
    for token in (
        "EthStartupResources r{};", "return eth_startup_fallback(r);",
        'eth_startup_fatal(r, "Ethernet event handler registration")',
        'eth_startup_fatal(r, "Ethernet IP handler registration")',
        "r.published = true;", "r.start_attempted = true;",
        "err = esp_eth_start(r.handle);", 'eth_startup_fatal(r, "Ethernet driver start")',
        "Ownership is now process-lifetime state",
    ):
        if token not in start:
            raise AssertionError(f"Ethernet startup ownership missing {token!r}")
    require_before("Ethernet passive construction before global publication", start,
                   "r.ip_handler_registered = true;", "s_eth_events = r.events;")
    require_before("Ethernet coherent publication before activation", start,
                   "r.published = true;", "esp_eth_start(r.handle)")
    post_start = start[start.find("esp_eth_start(r.handle)"):]
    if "eth_startup_fallback(r)" in post_start:
        raise AssertionError("Ethernet no-link/DHCP fallback tears down the live driver")

    if "EthernetStartupFixture" in runtime_tests or "test_ethernet_partial_start_cleanup" in runtime_tests:
        raise AssertionError("Ethernet cleanup must execute actual bodies instead of the retired mirrored fixture")


def require_explicit_idf_boot_error_contract(main_source: str, net_source: str,
                                             provisioning_source: str,
                                             runtime_tests: str) -> None:
    for label, source in (
        ("app boot", main_source),
        ("network boot", net_source),
        ("setup AP boot", provisioning_source),
    ):
        if re.search(r"\bESP_ERROR_CHECK\s*\(", scrub_cpp(source)):
            raise AssertionError(f"{label} reintroduced abort-on-IDF-error instead of fail-closed routing")

    app_main = function_body_in(main_source, "app_main")
    for token in (
        "const esp_err_t flash_init_err = nvs_flash_init();",
        "if (flash_init_err != ESP_OK)",
        'boot_fatal("NVS flash initialization");',
    ):
        if token not in app_main:
            raise AssertionError(f"NVS boot failure routing missing {token!r}")
    if "nvs_flash_erase" in app_main:
        raise AssertionError("NVS initialization failure regained a destructive erase fallback")
    require_before("NVS failure check before fatal halt", app_main,
                   "if (flash_init_err != ESP_OK)", 'boot_fatal("NVS flash initialization");')

    net_require = function_body_in(net_source, "net_boot_require")
    for token in ("if (err == ESP_OK) return;", "boot_fatal(component);"):
        if token not in net_require:
            raise AssertionError(f"network critical-IDF helper missing {token!r}")
    net_substrate = function_body_in(net_source, "net_init_substrate")
    for call, component in (
        ("esp_netif_init()", '"network interface substrate"'),
        ("esp_event_loop_create_default()", '"network event loop"'),
    ):
        if call not in net_substrate or component not in net_substrate:
            raise AssertionError(f"network substrate error is not routed explicitly: {call}")
    if net_substrate.count("err != ESP_OK && err != ESP_ERR_INVALID_STATE") != 2:
        raise AssertionError("network substrate does not preserve exact idempotent INVALID_STATE policy")
    net_init = function_body_in(net_source, "net_init")
    if "net_boot_require(net_init_substrate(&failed_component), failed_component)" not in net_init:
        raise AssertionError("public network initialization bypasses fail-closed substrate routing")

    wifi = function_body_in(net_source, "net_start_wifi")
    for token in (
        'if (!s_wifi_events) boot_fatal("WiFi event group");',
        'if (!s_sta_netif) boot_fatal("WiFi station netif");',
        'net_boot_require(esp_wifi_init(&cfg), "WiFi driver initialization");',
        "net_boot_require(esp_event_handler_instance_register(",
        'net_boot_require(esp_wifi_set_mode(WIFI_MODE_STA), "WiFi station mode");',
        'net_boot_require(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), "WiFi station configuration");',
        'net_boot_require(esp_wifi_start(), "WiFi station start");',
        'net_boot_require(esp_wifi_set_ps(WIFI_PS_MIN_MODEM), "WiFi/BLE coexistence power mode");',
    ):
        if token not in wifi:
            raise AssertionError(f"WiFi critical-IDF error route missing {token!r}")

    provisioning_require = function_body_in(provisioning_source, "provisioning_boot_require")
    for token in ("if (err == ESP_OK) return;", "boot_fatal(component);"):
        if token not in provisioning_require:
            raise AssertionError(f"setup AP critical-IDF helper missing {token!r}")
    setup = function_body_in(provisioning_source, "portal_open")
    if setup.count("!= ESP_OK &&") < 2 or setup.count("!= ESP_ERR_INVALID_STATE") < 2:
        raise AssertionError("setup AP substrate does not handle only idempotent INVALID_STATE")
    setup_flat = " ".join(setup.split())
    for token in (
        'provisioning_boot_require(netif_err, "setup network interface substrate")',
        'provisioning_boot_require(loop_err, "setup network event loop")',
        'if (!ap_netif) boot_fatal("setup AP network interface");',
        'provisioning_boot_require(esp_wifi_init(&cfg), "setup WiFi driver initialization");',
        'provisioning_boot_require(esp_wifi_set_mode(keep_station ? WIFI_MODE_APSTA : WIFI_MODE_AP), "setup AP mode");',
        'provisioning_boot_require(esp_wifi_set_config(WIFI_IF_AP, &ap), "setup AP configuration");',
        'if (!keep_station) provisioning_boot_require(esp_wifi_start(), "setup AP start");',
        'if (httpd_start(&portal.server, &hcfg) != ESP_OK)',
        'boot_fatal("setup HTTP server");',
        "httpd_register_uri_handler(portal.server, &save) != ESP_OK",
        "httpd_register_uri_handler(portal.server, &form) != ESP_OK",
        "httpd_stop(portal.server);",
        'boot_fatal("setup HTTP handlers");',
    ):
        if token not in setup_flat:
            raise AssertionError(f"setup AP fail-closed route missing {token!r}")
    require_before("setup handler failure stops partial server", setup,
                   "httpd_stop(portal.server);", 'boot_fatal("setup HTTP handlers");')
    close = function_body_in(provisioning_source, "portal_close_ap")
    close_code = " ".join(scrub_cpp(close).split())
    if not re.search(r"if \(stop_err != ESP_OK\) \{[^{}]*return false; \}", close_code):
        raise AssertionError("setup HTTP stop failure must retain ownership and defer the close")
    require_before("setup HTTP stop checked before forgetting its handle", close,
                   "const esp_err_t stop_err = httpd_stop(portal.server);", "if (stop_err != ESP_OK)")
    require_before("setup HTTP stop checked before forgetting its handle", close,
                   "if (stop_err != ESP_OK)", "portal.server = nullptr;")
    require_before("setup server stopped before changing WiFi mode", close,
                   "portal.server = nullptr;", "esp_wifi_set_mode(WIFI_MODE_STA)")
    failed_stop = re.search(r"if \(stop_err != ESP_OK\) \{([^{}]*)\}", close_code)
    if failed_stop and "portal.server =" in failed_stop.group(1):
        raise AssertionError("setup HTTP stop failure must not forget the running server")
    # Portal scheduling only suppresses its own new calls. A station whose boot deadline expired
    # before MAX_RETRY still reconnects from its event handler, including while an AP client exists.
    event = " ".join(scrub_cpp(function_body_in(net_source, "wifi_event_handler")).split())
    if "!s_ever_up.load() && !s_rollback_active.load() && s_retry_num >= MAX_RETRY" not in event:
        raise AssertionError("review the shared station/portal retry ownership after an event-policy change")
    if not re.search(r"else \{ esp_wifi_connect\(\); s_retry_num\+\+;", event):
        raise AssertionError("remaining boot retries must be accounted for independently of portal scheduling")
    # First-time setup stays terminal; the stored-credential portal returns only through the
    # host-tested policy (logic/setup_portal.hpp) and never restarts the device itself.
    terminal = " ".join(function_body_in(provisioning_source, "provisioning_run").split())
    if "portal_open(config_store, /*keep_station=*/false);" not in terminal or \
            "while (true) { tk::StackWatchScope f135_portal_wait_exit(tk::StackWatch::Main); vTaskDelay(pdMS_TO_TICKS(1000)); }" not in terminal:
        raise AssertionError("first-time setup portal must stay terminal")
    retry = function_body_in(provisioning_source, "provisioning_run_with_retry")
    for token in ("if (!tk::net_wifi_station_started()) {", "provisioning_run(config_store);",
                  "portal_open(config_store, /*keep_station=*/true);", "tk::portal_step(state,",
                  "tk::net_wifi_retry_station();", "return;", "esp_timer_get_time()"):
        if token not in retry:
            raise AssertionError(f"retrying setup portal lost {token!r}")
    # A failed AP close keeps the AP marked open and is reported to the policy (backoff); Leave
    # continues the boot only once the AP is down.
    retry_flat = " ".join(scrub_cpp(retry).split())
    for token in ("if (portal_close_ap(portal)) ap_open = false; else tk::portal_close_failed(state, now_s);",
                  "if (ap_open && !portal_close_ap(portal)) { tk::portal_close_failed(state, now_s); break; }"):
        if token not in retry_flat:
            raise AssertionError(f"retrying setup portal lost {token!r}")
    # APSTA needs the running station: rejected credentials take the terminal portal first.
    require_before("retrying setup portal falls back before joining a missing station", retry,
                   "if (!tk::net_wifi_station_started()) {", "provisioning_run(config_store);")
    require_before("retrying setup portal falls back before joining a missing station", retry,
                   "provisioning_run(config_store);", "portal_open(config_store, /*keep_station=*/true);")
    # The terminal fallback must sit INSIDE the station check, or the retrying portal is dead code.
    retry_code = scrub_cpp(retry)
    guard = retry_code.find("if (!tk::net_wifi_station_started()) {")
    if guard < 0 or "provisioning_run(config_store);" not in retry_code[guard:retry_code.find("}", guard)]:
        raise AssertionError("the terminal setup portal must be reached only without a running station")
    if "[[noreturn]] void provisioning_run(NvsStorageAdapter& config_store);" not in \
            (ROOT / "main/provisioning.hpp").read_text(encoding="utf-8"):
        raise AssertionError("provisioning_run must be declared [[noreturn]]")
    if "xTaskGetTickCount" in scrub_cpp(retry):
        raise AssertionError("the retrying setup portal clock must not wrap with the 32-bit tick count")
    # The boot takes the retrying portal after a stored-credential WiFi failure; the terminal one
    # stays for missing credentials and for a rollback that could not be persisted.
    boot = " ".join(main_source.split())
    if boot.count("provisioning_run_with_retry(config_store);") != 1:
        raise AssertionError("the boot must call the retrying setup portal exactly once")
    require_before("stored-credential WiFi failure takes the retrying portal", boot,
                   "!tk::net_start_wifi(ssid.c_str(), password.c_str(), cfg_blob.wifi_rollback_active)) {",
                   "provisioning_run_with_retry(config_store);")
    unpersisted = boot.find("} else if (rollback_unpersisted) {")
    if unpersisted < 0 or "provisioning_run(config_store);" not in \
            boot[unpersisted:boot.find("provisioning_run_with_retry(config_store);")]:
        raise AssertionError("an unpersisted rollback must take the terminal setup portal")
    # The captive DNS task must be stoppable (receive timeout applied, or no task) and must not
    # spin on a persistent socket error.
    dns = " ".join(function_body_in(provisioning_source, "dns_task").split())
    for token in ("if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {",
                  "if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) { vTaskDelay(pdMS_TO_TICKS(100));"):
        if token not in dns:
            raise AssertionError(f"captive DNS task lost {token!r}")
    started = " ".join(function_body_in(net_source, "net_start_wifi").split())
    require_before("station flag only after the driver started", started,
                   'net_boot_require(esp_wifi_start(), "WiFi station start");', "s_sta_started.store(true);")
    if re.search(r"\besp_restart\s*\(", scrub_cpp(retry)):
        raise AssertionError("the retrying setup portal must never restart the device itself")

    for token in (
        "test_explicit_idf_boot_error_injection",
        "for (int fail_step = 0; fail_step < InjectedCriticalBoot::kSteps; ++fail_step)",
        "boot.calls == fail_step + 1",
        "boot.fatal_calls == 1",
        "boot.ready_publications == 0",
        "for (int idempotent_step : {1, 2})",
        "already_initialized.fatal_calls == 0",
        "successful.ready_publications == 1",
    ):
        if token not in runtime_tests:
            raise AssertionError(f"IDF boot error-injection matrix missing {token!r}")


def require_ota_status_lock_contract(ota_source: str, runtime_tests: str) -> None:
    ensure = function_body_in(ota_source, "ensure_lock")
    for token in (
        "s_lock.load(std::memory_order_acquire)",
        "SemaphoreHandle_t candidate = xSemaphoreCreateMutex();",
        "if (!candidate) return nullptr;",
        "s_lock.compare_exchange_strong(lock, candidate",
        "std::memory_order_release",
        "std::memory_order_acquire",
        "vSemaphoreDelete(candidate);",
        "return lock;",
    ):
        if token not in ensure:
            raise AssertionError(f"OTA lazy status-lock publication missing {token!r}")
    cas_loser = ensure[ensure.find("if (!s_lock.compare_exchange_strong") :]
    require_before("OTA losing lock candidate deleted before winner returned", cas_loser,
                   "vSemaphoreDelete(candidate);", "return lock;")

    unavailable = function_body_in(ota_source, "unavailable_status_snapshot")
    if "s_status" in scrub_cpp(unavailable):
        raise AssertionError("OTA unavailable snapshot reads shared status without a lock")
    if 'return {OtaState::Error, 0, "unavailable", "", false, "", tk::ota_channel_name(ota_get_channel())};' not in unavailable:
        raise AssertionError("OTA lock failure lost its independent unavailable snapshot")

    reader = function_body_in(ota_source, "ota_get_status")
    for token in (
        "SemaphoreHandle_t lock = ensure_lock();",
        "if (!lock) return unavailable_status_snapshot();",
        "OtaStatusPod snapshot{};",
        "tk::SemGuard g(lock);",
        "if (!g) return unavailable_status_snapshot();",
        "snapshot = s_status;",
        "snapshot.message.data()",
        "snapshot.available.data()",
        "snapshot.current.data()",
    ):
        if token not in reader:
            raise AssertionError(f"OTA status reader lock contract missing {token!r}")
    if reader.count("s_status") != 1:
        raise AssertionError("OTA status reader has an unlocked or duplicate shared-status access")
    require_before("OTA reader allocation failure before shared read", reader,
                   "if (!lock) return unavailable_status_snapshot();", "snapshot = s_status;")
    require_before("OTA reader guard failure before shared read", reader,
                   "if (!g) return unavailable_status_snapshot();", "snapshot = s_status;")
    require_before("OTA reader fixed snapshot before string materialization", reader,
                   "snapshot = s_status;", "snapshot.message.data()")
    reader_layout = scrub_cpp_preserving_layout(reader)
    snapshot_pos = reader_layout.find("OtaStatusPod snapshot{};")
    locked_open = reader_layout.find("{", snapshot_pos + len("OtaStatusPod snapshot{};"))
    if locked_open < 0:
        raise AssertionError("OTA reader fixed snapshot lock scope is missing")
    locked_close = balanced_end(
        reader_layout, locked_open, "{", "}", "OTA reader status-lock scope"
    )
    locked_scope = reader_layout[locked_open:locked_close + 1]
    if "snapshot.message.data()" in locked_scope or "return {" in locked_scope:
        raise AssertionError("OTA reader materializes allocating status while holding its lock")

    for token in (
        "struct OtaStatusPod",
        "std::array<char, kOtaStatusMessageCapacity>",
        "std::array<char, kOtaStatusVersionCapacity>",
        "static OtaStatusPod                   s_status",
    ):
        if token not in ota_source:
            raise AssertionError(f"OTA status fixed snapshot contract missing {token!r}")

    for writer_name in ("set_state", "publish_check_status"):
        writer = function_body_in(ota_source, writer_name)
        first_status = writer.find("s_status")
        if first_status < 0:
            raise AssertionError(f"OTA status writer {writer_name} no longer publishes status")
        for token in (
            "SemaphoreHandle_t lock = ensure_lock();",
            "if (!lock) return;",
            "tk::SemGuard g(lock)",
            "if (!g) return;",
        ):
            if token not in writer[:first_status]:
                raise AssertionError(
                    f"OTA status writer {writer_name} can access shared state without guard: {token!r}"
                )
        if (writer_name == "set_state" and
                "copy_status_text" not in writer[: writer.find("ensure_lock")]):
            raise AssertionError(
                f"OTA status writer {writer_name} must build fixed text before taking the lock"
            )
        locked_tail = scrub_cpp(writer[writer.find("tk::SemGuard g(lock)") :])
        if re.search(r"\b(?:std::string|assign|append|new|throw)\b", locked_tail):
            raise AssertionError(
                f"OTA status writer {writer_name} allocates/throws under the status lock"
            )

    # A completed manifest check has one publication point. ota_check() computes only its local
    # result; set_check_done() moves all public fields as one fixed snapshot. Re-introducing the
    # former set_available()+set_state() intermediates exposes a new version/message alongside
    # stale current/update_available values to a concurrent /ota/status reader.
    check = scrub_cpp(function_body_in(ota_source, "perform_check"))
    if re.search(r"\b(?:set_state|set_check_error|set_check_done)\s*\(", check):
        raise AssertionError("OTA manifest check publishes a partial shared-status generation")
    done = scrub_cpp(function_body_in(ota_source, "set_check_done"))
    for token in (
        "OtaStatusPod candidate{};",
        "candidate.state =",
        "candidate.progress = 0;",
        "candidate.update_available = r.update_available;",
        "copy_status_text(candidate.message, r.reason.c_str());",
        "copy_status_text(candidate.available, r.available.c_str());",
        "copy_status_text(candidate.current, r.current.c_str());",
        "publish_check_status(candidate);",
    ):
        if token not in done:
            raise AssertionError(f"OTA completed-check snapshot missing {token!r}")
    if done.count("publish_check_status(candidate);") != 1 or "s_status" in done:
        raise AssertionError("OTA completed check is not one whole-snapshot publication")
    publisher = scrub_cpp(function_body_in(ota_source, "publish_check_status"))
    if publisher.count("s_status = candidate;") != 1 or re.search(r"s_status\s*\.", publisher):
        raise AssertionError("OTA completed-check publisher is not one whole-snapshot assignment")

    # set_check_error() is the last-resort path used from exception/OOM catches and task-create
    # failure. It is noexcept by design: keep the entire path fixed-buffer-only and publish one
    # complete generation, otherwise a second allocation failure would terminate the task and a
    # partial set_state() fallback could expose mixed check fields to concurrent readers.
    if not re.search(
        r"static\s+void\s+set_check_error\s*\(\s*const\s+char\s*\*\s*message\s*\)\s*"
        r"noexcept\s*\{",
        ota_source,
    ):
        raise AssertionError("OTA check-error fallback must remain noexcept")
    error = scrub_cpp(function_body_in(ota_source, "set_check_error"))
    for token in (
        "OtaStatusPod candidate{};",
        "candidate.state = OtaState::Error;",
        "copy_status_text(candidate.message, message);",
        "const std::string_view current = running_version();",
        "copy_status_text(candidate.current, current.data());",
        "publish_check_status(candidate);",
    ):
        if token not in error:
            raise AssertionError(f"OTA check-error fixed snapshot missing {token!r}")
    expected_error = (
        "{ OtaStatusPod candidate{}; candidate.state = OtaState::Error; "
        "candidate.target_pr = s_target_pr.load(std::memory_order_relaxed); "
        "copy_status_text(candidate.message, message); "
        "const std::string_view current = running_version(); "
        "copy_status_text(candidate.current, current.data()); "
        "publish_check_status(candidate); }"
    )
    if " ".join(error.split()) != expected_error:
        raise AssertionError("OTA check-error fallback is not the exact fixed snapshot program")
    require_exact(
        "OTA check-error fallback call allowlist",
        called_functions(error),
        {"copy_status_text", "data", "publish_check_status", "running_version", "load"},
    )

    for token in (
        "test_ota_status_lock_failure_matrix",
        "read_alloc_failure.status_reads == 0",
        "read_take_failure.status_reads == 0",
        "write_alloc_failure.status_writes == 0",
        "write_take_failure.status_writes == 0",
        "cas_loser.publish_cas_winner = true",
        "cas_loser.deleted_candidates == 1",
        "cas_loser.status_reads == 1",
        "shared.status_writes == 1",
        "shared.lock_takes == shared.lock_gives",
    ):
        if token not in runtime_tests:
            raise AssertionError(f"OTA status lock fault matrix missing {token!r}")


def require_ble_callback_lock_contract(ble_source: str) -> None:
    """Keep NimBLE-host and esp_timer callbacks off unbounded lifecycle waits."""
    callback_entries = {
        "on_scan_timeout",
        "on_sync",
        "on_reset",
        "disconnect_from_callback_",
        "on_gap_event",
        "on_svc_disc",
        "on_chr_disc",
        "subscribe_notify_",
        "on_dsc_disc",
        "on_subscribe_write",
        "on_mtu_exchange",
    }
    # Include both BleClient members and translation-unit free/static helpers. A callback path
    # must not be able to hide an allocating helper merely by moving it outside the class.
    local_functions = function_definition_names(ble_source)
    callback_reachable = set(callback_entries)
    pending = list(callback_entries)
    while pending:
        name = pending.pop()
        body = function_body_in(ble_source, name)
        for helper in called_functions(body) & local_functions:
            if helper not in callback_reachable:
                callback_reachable.add(helper)
                pending.append(helper)

    expected_callback_reachable = {
        "cancel_scan_locked_",
        "connection_snapshot_matches_",
        "disconnect_from_callback_",
        "ensure_scanning_",
        "ensure_scanning_locked_",
        "has_gap_link_",
        "note_connectable_",
        "note_scan_",
        "on_chr_disc",
        "on_dsc_disc",
        "on_gap_event",
        "on_mtu_exchange",
        "on_reset",
        "on_scan_timeout",
        "on_subscribe_write",
        "on_svc_disc",
        "on_sync",
        "start_scan_locked_",
        "subscribe_notify_",
        "terminate_published_link_",
    }
    require_exact(
        "BLE callback-reachable local helper closure",
        callback_reachable,
        expected_callback_reachable,
    )

    allowed_calls = {
        "cancel_scan_locked_": {
            "ESP_LOGD", "ble_gap_disc_cancel", "load", "scan_running_after_cancel", "store",
        },
        "connection_snapshot_matches_": {"load"},
        "disconnect_from_callback_": {"store", "terminate_published_link_"},
        "ensure_scanning_": {"ensure_scanning_locked_", "intent"},
        "ensure_scanning_locked_": {
            "connect_scan_should_start", "has_gap_link_", "load", "start_scan_locked_",
        },
        "has_gap_link_": {"gap_link_available", "load"},
        "note_connectable_": {"esp_timer_get_time", "g", "memcmp", "memcpy"},
        "note_scan_": {"esp_timer_get_time", "g", "memcmp", "memcpy", "size"},
        "on_chr_disc": {
            "ESP_LOGD", "ESP_LOGE", "ble_uuid_cmp", "connection_snapshot_matches_",
            "disconnect_from_callback_", "load", "store", "subscribe_notify_", "what",
        },
        "on_dsc_disc": {
            "ESP_LOGD", "ESP_LOGE", "ble_gattc_write_flat", "ble_uuid_cmp",
            "connection_snapshot_matches_", "disconnect_from_callback_", "what",
        },
        "on_gap_event": {
            "ESP_LOGD", "ESP_LOGE", "ESP_LOGI", "ESP_LOGW", "OS_MBUF_PKTLEN",
            "ble_gap_connect", "ble_gap_terminate", "ble_gattc_disc_svc_by_uuid",
            "ble_hs_adv_parse_fields", "ble_write_payload_for_mtu", "cancel_scan_locked_",
            "connect_attempt_may_advance", "connect_lifecycle_after_gap_connected",
            "connect_lifecycle_after_start_failure", "connect_lifecycle_during_gap_start",
            "connection_snapshot_matches_", "data", "diag_verbose",
            "disconnect_from_callback_", "ensure_scanning_", "ensure_scanning_locked_",
            "esp_timer_get_time", "fetch_add", "fill", "g", "intent",
            "is_tesla_vehicle_name", "load", "memcpy", "note_connectable_", "note_scan_",
            "on_connected_", "on_rx_data_", "os_mbuf_copydata", "size", "snprintf", "store",
            "strcmp", "terminate_published_link_", "what",
        },
        "on_mtu_exchange": {
            "ESP_LOGD", "ESP_LOGE", "ESP_LOGI", "ESP_LOGW", "connection_snapshot_matches_",
            "exchange", "store", "what",
        },
        "on_reset": {
            "compare_exchange_weak", "connect_lifecycle_after_host_reset", "fetch_add", "intent",
            "load", "on_connected_", "store",
        },
        "on_scan_timeout": {
            "ESP_LOGD", "cancel_scan_locked_", "esp_timer_start_once", "has_gap_link_", "intent",
            "load", "manual_discovery_timeout_may_cancel",
        },
        "on_subscribe_write": {
            "ESP_LOGD", "ESP_LOGE", "ble_gattc_exchange_mtu", "connect_attempt_may_advance",
            "connection_snapshot_matches_", "disconnect_from_callback_", "intent", "load",
            "on_connected_", "store", "what",
        },
        "on_svc_disc": {
            "ESP_LOGD", "ESP_LOGE", "ble_gattc_disc_all_chrs", "ble_uuid_cmp",
            "connection_snapshot_matches_", "disconnect_from_callback_", "what",
        },
        "on_sync": {"ESP_LOGI", "ESP_LOGW", "acknowledge_sync", "ensure_scanning_", "load", "state"},
        "start_scan_locked_": {"ESP_LOGD", "ble_gap_disc", "load", "scan_running_after_start", "store"},
        "subscribe_notify_": {
            "ESP_LOGD", "ble_gattc_disc_all_dscs", "connection_snapshot_matches_",
            "disconnect_from_callback_",
        },
        "terminate_published_link_": {"ESP_LOGW", "ble_gap_terminate", "load", "store", "taskYIELD"},
    }
    require_exact("BLE callback call-allowlist owners", set(allowed_calls), callback_reachable)

    # Calls alone cannot see a constructor spelled through a type alias (or a blocking RAII
    # object). Pin the normalized executable program for every closure member as the final closed
    # world: comments, formatting and literal contents may move, but adding/removing a declaration
    # or statement is an explicit fingerprint review event. The semantic allowlists above/below
    # keep these hashes explainable instead of treating them as correctness evidence by themselves.
    program_fingerprints = {
        "cancel_scan_locked_": "ed79b53939e28078f96405b2be3b17f8f14830b9ca33bcd5b51db9b2a4cf4c40",
        "connection_snapshot_matches_": "e27c016ecb425d1c9715778c6671bbffe257308cf5583325aecd4e67ba66402e",
        "disconnect_from_callback_": "73bfc2472e7d1f93de72de34be27a8b10a756c14175cffee401cc2433d10438b",
        "ensure_scanning_": "b0df06d9554e0e89430a71423e5da1c124c5ae0a2dcff53999eb020ae12b858e",
        "ensure_scanning_locked_": "96dda6c77fa2762bce01e467af651b603ab8e63cf2856f6a7958d0a8e0002766",
        "has_gap_link_": "87be523038b9d7843373203c7d307a6946e2bad88340360a926cfaef0caf3508",
        "note_connectable_": "f22081647b1c6b17e884c21a5173fb45ba567dbefa0a3dbdc8cfdec519e6931a",
        "note_scan_": "0bc069b1372559facfd9e3e2adb29a307f1332d2de58d80c7f60042cf57e44b3",
        "on_chr_disc": "02ae0d160e0c2e143672a61f53d687abb98b8f9268aa0af3cd925ed0a5f162f3",
        "on_dsc_disc": "5cb6705aca81956c2e1d6a493d3915dd89ed893f305b5f73b7c133e329d7f618",
        "on_gap_event": "8119fa18dcc9a2d10783fbf88c814e15bba9ec0caf79529d5065cf64b0a054e0",
        "on_mtu_exchange": "28a7ad2c78f9efd5f6f9a53b31c53ec7a51c79b0c6e2431837c79f752c4dc520",
        "on_reset": "a6a11090a79118c9ef6526cb04fe27e35adb57b18f5801fa2479cb9a2110b990",
        "on_scan_timeout": "7611bc6220fd5cc504a1f6b3b0503645a304f5ee97d32c47b91b22486f1fce95",
        "on_subscribe_write": "a369a89bd9bbdb4530995e36d99a966033f6e83cb018d1db625f941df7aad4b1",
        "on_svc_disc": "77cd5b3c710eddaba67c822084dca5bee1a39de1028fd5d330b30823d0dae50f",
        "on_sync": "7c8b104bd654a1ac3a460c0764aa100d2ef356eead442851ae6729ad4b9266bc",
        "start_scan_locked_": "15d90a0dd3771816e41339b817f74ccf8c8068bc86d26b87f9077e173cc629ca",
        "subscribe_notify_": "00365f0a70b509e1ae52003e893292d393066d8f10bdce97b87c32a3e6712091",
        "terminate_published_link_": "278504e6e169798cd796886fc12cb9eebc75fab2d3c52b20beefef339501fb9a",
    }
    require_exact(
        "BLE callback program-fingerprint owners",
        set(program_fingerprints),
        callback_reachable,
    )

    allowed_std_tokens = {
        "on_chr_disc": {"std::exception"},
        "on_dsc_disc": {"std::exception"},
        "on_gap_event": {"std::array", "std::exception", "std::memcpy", "std::min", "std::strcmp"},
        "on_mtu_exchange": {"std::exception"},
        "on_reset": {"std::memory_order_acq_rel", "std::memory_order_acquire", "std::uint32_t"},
        "on_subscribe_write": {"std::exception"},
        "on_svc_disc": {"std::exception"},
    }

    allocation_or_throw = re.compile(
        r"\b(?:std::string|std::vector|std::function|new|throw|malloc|calloc|realloc|"
        r"strdup|asprintf|cJSON_\w+)\b"
    )
    blocking_wait = re.compile(
        r"\b(?:xSemaphoreTake|xSemaphoreTakeRecursive|vTaskDelay|vTaskDelayUntil|"
        r"ulTaskNotifyTake|xTaskNotifyWait|xQueueReceive|xQueuePeek)\s*\("
    )
    for name in sorted(callback_reachable):
        body = scrub_cpp(function_body_in(ble_source, name))
        normalized_program = " ".join(body.split()).encode("utf-8")
        if hashlib.sha256(normalized_program).hexdigest() != program_fingerprints[name]:
            raise AssertionError(f"BLE callback path {name} executable program changed")
        require_exact(
            f"BLE callback path {name} call allowlist",
            called_functions(body),
            allowed_calls[name],
        )
        require_exact(
            f"BLE callback path {name} standard-library token allowlist",
            set(re.findall(r"\bstd::[A-Za-z_]\w*", body)),
            allowed_std_tokens.get(name, set()),
        )
        if allocation_or_throw.search(body):
            raise AssertionError(
                f"BLE callback path {name} contains an allocation or explicit throw"
            )
        if blocking_wait.search(body):
            raise AssertionError(f"BLE callback path {name} contains a blocking RTOS wait")
        guards = re.findall(r"\btk::SemGuard\s+\w+\s*\(([^()]*)\)\s*;", body)
        if len(guards) != body.count("tk::SemGuard"):
            raise AssertionError(f"BLE callback path {name} has an unparsed semaphore guard")
        for raw_arguments in guards:
            arguments = [argument.strip() for argument in raw_arguments.split(",")]
            if len(arguments) != 2:
                raise AssertionError(
                    f"BLE callback path {name} uses an unbounded semaphore guard"
                )
            deadline = arguments[1].strip()
            if deadline != "0" and not (name == "ensure_scanning_" and deadline == "timeout"):
                raise AssertionError(
                    f"BLE callback path {name} uses non-zero semaphore deadline {deadline!r}"
                )
        if re.search(r"tk::SemGuard\s+\w+\s*\(\s*intent_mutex_\s*\)\s*;", body):
            raise AssertionError(
                f"BLE callback path {name} uses the blocking intent-mutex guard"
            )
        if name in callback_entries - {"disconnect_from_callback_"}:
            if re.search(r"\bdisconnect\s*\(", body):
                raise AssertionError(
                    f"BLE callback path {name} delegates to blocking disconnect()"
                )
            for deadline in re.findall(r"\bensure_scanning_\s*\(([^)]*)\)", body):
                if deadline.strip() != "0":
                    raise AssertionError(
                        f"BLE callback path {name} delegates to a non-zero scan-lock deadline"
                    )

    # The only callback-reachable parameterized wait adapter is accepted because every incoming
    # edge in this reviewed closure passes zero. A task-side default call may still block without
    # weakening the NimBLE-host boundary.
    for caller in sorted(callback_reachable - {"ensure_scanning_"}):
        body = function_body_in(ble_source, caller)
        for deadline in re.findall(r"\bensure_scanning_\s*\(([^)]*)\)", scrub_cpp(body)):
            if deadline.strip() != "0":
                raise AssertionError(
                    f"BLE callback helper {caller} reaches ensure_scanning_ with non-zero deadline"
                )

    timeout = function_body_in(ble_source, "on_scan_timeout")
    for token in (
        "tk::SemGuard intent(intent_mutex_, 0);",
        "if (!intent)",
        "esp_timer_start_once(scan_timer_, 10 * 1000)",
    ):
        if token not in timeout:
            raise AssertionError(f"BLE timer retry contract missing {token!r}")

    reset = function_body_in(ble_source, "on_reset")
    subscribe = function_body_in(ble_source, "on_subscribe_write")
    gap = function_body_in(ble_source, "on_gap_event")
    scan_adapter = function_body_in(ble_source, "ensure_scanning_")
    drop_adapter = function_body_in(ble_source, "disconnect_from_callback_")
    for label, body in (
        ("host reset", reset),
        ("GAP lifecycle", gap),
        ("CCCD completion", subscribe),
    ):
        if "tk::SemGuard intent(intent_mutex_, 0);" not in body:
            raise AssertionError(f"BLE {label} lost its zero-wait lifecycle lock")
    if "tk::SemGuard intent(intent_mutex_, timeout);" not in scan_adapter:
        raise AssertionError("BLE shared scan adapter lost its caller-selected lock deadline")
    for token in (
        "ready_generation_.store(tk::ble::kNoReadyGeneration);",
        "disconnecting_.store(true);",
        "want_connect_.store(false);",
        "connecting_.store(false);",
        "terminate_published_link_();",
    ):
        if token not in drop_adapter:
            raise AssertionError(
                f"BLE callback disconnect adapter lost fail-closed atomic publication {token!r}"
            )
    if "SemGuard" in drop_adapter:
        raise AssertionError("BLE callback disconnect adapter unnecessarily takes a lifecycle lock")
    for name in ("on_svc_disc", "on_chr_disc", "subscribe_notify_",
                 "on_dsc_disc", "on_subscribe_write"):
        if "disconnect_from_callback_();" not in function_body_in(ble_source, name):
            raise AssertionError(f"BLE callback {name} lost fail-closed nonblocking disconnect")
    if "ensure_scanning_(0);" not in function_body_in(ble_source, "on_sync"):
        raise AssertionError("NimBLE sync callback delegates to a blocking scan path")


def require_tesla_cpp_callback_contract(
    controller: str, telemetry: str, ble_source: str, commands: str
) -> None:
    """Keep every tesla-ble std::function callback on a reviewed, bounded seam."""
    # Native orchestration layer: process_rx_frame_ dispatches incoming CarServer and VCSEC
    # responses directly to the reviewed fixed-mailbox helpers.
    for helper in (
        "on_charge_state_",
        "on_climate_state_",
        "on_drive_state_",
        "on_tire_pressure_state_",
        "on_closures_state_",
        "on_vehicle_message_",
    ):
        if helper + "(" not in telemetry:
            raise AssertionError(f"process_rx_frame_ must dispatch to {helper}")

    # Persistent state callbacks run synchronously inside Vehicle::loop. They may only publish a
    # trivially-copyable latest value under the bounded portMUX; parsing and cache publication are
    # deferred until vehicle_mutex_ is no longer held.
    for helper, pending in {
        "on_charge_state_": "telemetry_pending_charge_",
        "on_climate_state_": "telemetry_pending_climate_",
        "on_drive_state_": "telemetry_pending_drive_",
        "on_tire_pressure_state_": "telemetry_pending_tires_",
        "on_closures_state_": "telemetry_pending_closures_",
    }.items():
        body = function_body_in(telemetry, helper)
        for token in (
            "portENTER_CRITICAL(&telemetry_pending_mux_);",
            f"{pending} = state;",
            "telemetry_pending_mask_ |= Pending",
            "portEXIT_CRITICAL(&telemetry_pending_mux_);",
        ):
            if token not in body:
                raise AssertionError(f"{helper}: fixed mailbox callback missing {token!r}")
        for forbidden in (
            "std::string", "std::vector", "tk::SemGuard", "tk::MutexGuard",
            "save_str", "ESP_LOG", "parse_", "throw", "new ",
        ):
            if forbidden in scrub_cpp(body):
                raise AssertionError(f"{helper}: callback contains forbidden {forbidden!r}")

    charge_callback = function_body_in(telemetry, "on_charge_state_")
    for token in (
        "charging_amps_feedback_.generation + 1",
        "charging_amps_feedback_.generation = generation ? generation : 1;",
        "charging_amps_feedback_.has_charging_amps = true;",
        "charging_amps_feedback_.has_current_request = true;",
        "charging_amps_feedback_.has_actual_current = true;",
    ):
        if token not in charge_callback:
            raise AssertionError(f"charge callback fixed readback seam missing {token!r}")
    feedback_snapshot = function_body_in(telemetry, "charging_amps_feedback_snapshot_")
    for token in (
        "portENTER_CRITICAL(&telemetry_pending_mux_);",
        "const ChargingAmpsFeedback snapshot = charging_amps_feedback_;",
        "portEXIT_CRITICAL(&telemetry_pending_mux_);",
        "return snapshot;",
    ):
        if token not in feedback_snapshot:
            raise AssertionError(f"charging-amps feedback snapshot missing {token!r}")

    message = function_body_in(controller, "on_vehicle_message_")
    for forbidden in (
        "std::string", "std::vector", "tk::SemGuard", "tk::MutexGuard",
        "save_str", "ESP_LOG", "throw", "new ",
    ):
        if forbidden in scrub_cpp(message):
            raise AssertionError(f"on_vehicle_message_: callback contains forbidden {forbidden!r}")
    if "pairing_recovery_hint_.store(true, std::memory_order_release);" not in message:
        raise AssertionError("on_vehicle_message_: recovery hint is not an atomic publication")

    deferred = function_body_in(telemetry, "process_pending_telemetry_")
    if "vehicle_mutex_" in scrub_cpp(deferred):
        raise AssertionError("deferred telemetry parser reacquires vehicle_mutex_")
    for parser in (
        "parse_charge_state", "parse_climate_state", "parse_drive_state",
        "parse_tire_pressure", "parse_closures_state",
    ):
        if deferred.count(parser + "(") != 1:
            raise AssertionError(f"deferred telemetry parser inventory drift: {parser}")

    cache_blocks = re.findall(
        r"tk::MutexGuard\s+\w+\s*\(\s*cache_mutex_\s*\)\s*;(?P<body>.*?)\}",
        telemetry,
        re.DOTALL,
    )
    if not cache_blocks:
        raise AssertionError("vehicle_telemetry.cpp cache_mutex_ critical section inventory missing")
    for body in cache_blocks:
        if "ESP_LOG" in body:
            raise AssertionError("cache_mutex_ critical section contains forbidden ESP_LOG logging")

    loop = scrub_cpp(function_body_in(telemetry, "loop_task_fn_"))
    require_before("BLE host drain before command runner pump", loop,
                   "process_ble_host_events_();", "drive_command_runner_();")
    require_before("telemetry parse after command runner pump", loop,
                   "drive_command_runner_();", "process_pending_telemetry_();")

    combined = controller + "\n" + telemetry
    if combined.count("process_rx_frame_(") == 0:
        raise AssertionError("process_rx_frame_ must have task-owned callsite")
    if "process_rx_frame_(" in scrub_cpp(ble_source):
        raise AssertionError("NimBLE host callback directly reentered process_rx_frame_")
    ble_events = function_body_in(telemetry, "process_ble_host_events_")
    if "process_rx_frame_(payload, len);" not in ble_events:
        raise AssertionError("process_rx_frame_ escaped the deferred event processor")
    if ble_source.count("BleClient::complete_ready(") != 1:
        raise AssertionError("BleClient ready publication definition inventory drift")
    subscribe = function_body_in(ble_source, "on_subscribe_write")
    if "complete_ready" in scrub_cpp(subscribe) or "ready_generation_.store(generation)" in scrub_cpp(subscribe):
        raise AssertionError("NimBLE subscribe callback publishes readiness before Vehicle ack")
    if "ble_->complete_ready(event.conn_handle, event.generation)" not in ble_events:
        raise AssertionError("deferred Vehicle LinkUp acknowledgement does not publish readiness")

    # The request-scoped VCSEC callback is dynamic, but still executes while vehicle_mutex_
    # is held. It may publish POD and signal a pre-created semaphore only; string shaping is
    # required after the callback has been cleared and the lock released.
    status = function_body_in(telemetry, "get_vehicle_status")
    callback_start = status.find("auto callback = [this, completion, generation]")
    callback_end = status.find(";\n\n    try", callback_start)
    if callback_start < 0 or callback_end < 0:
        raise AssertionError("vehicle-status callback capture/containment shape drift")
    callback = scrub_cpp(status[callback_start:callback_end])
    for forbidden in (
        "std::string", "std::vector", "tk::SemGuard", "tk::MutexGuard",
        "save_str", "ESP_LOG", "throw", "new ", ".status",
    ):
        if forbidden in callback:
            raise AssertionError(f"vehicle-status callback contains forbidden {forbidden!r}")
    for token in (
        "completion->lock_state = static_cast<int32_t>(vs.vehicleLockState);",
        "completion->sleep_status = static_cast<int32_t>(vs.vehicleSleepStatus);",
        "completion->user_presence = static_cast<int32_t>(vs.userPresence);",
        "xSemaphoreGive(completion->sem);",
    ):
        if token not in callback:
            raise AssertionError(f"vehicle-status callback POD seam missing {token!r}")
    require_before("vehicle-status callback shaped before Vehicle lock", status,
                   "auto callback =", "tk::SemGuard g(vehicle_mutex_);")
    require_before("vehicle-status callback cleared before string shaping", status,
                   "vehicle_status_callback_ = nullptr;", "out.valid = true;")

    result_factory = function_body_in(commands, "make_result_cb_")
    result_code = scrub_cpp(result_factory)
    for forbidden in (
        "std::string", "std::vector", "tk::SemGuard", "tk::MutexGuard",
        "save_str", "ESP_LOG", "throw", "new ", "catch (const",
    ):
        if forbidden in result_code:
            raise AssertionError(f"command result callback contains forbidden {forbidden!r}")
    for token in (
        "const auto& msg = result.error()->message();",
        "std::memcpy(completion->error.data(), visible, error_size);",
        "const char* visible = tk::execution_uncertain(completion->failure)",
        "result.error()->severity(), result.error()->outcome()",
        "completion->failure = tk::uncertain_command_failure();",
        "completion->callback_fault = true;",
        "xSemaphoreGive(completion->sem);",
    ):
        if token not in result_factory:
            raise AssertionError(f"command result fixed completion seam missing {token!r}")
    waiter = function_body_in(commands, "await_completion_")
    for token in (
        "out.error     = completion->error.data();",
        'ESP_LOGW(TAG, "command failed: %s", out.error.c_str());',
        'ESP_LOGE(TAG, "result callback failed — command result may be partial");',
    ):
        if token not in waiter:
            raise AssertionError(f"command result task-side consequence missing {token!r}")
    amps = function_body_in(commands, "set_charging_amps")
    for token in (
        "const ChargingAmpsFeedback feedback_before = charging_amps_feedback_snapshot_();",
        "const ChargingAmpsFeedback feedback_after = charging_amps_feedback_snapshot_();",
        "feedback_after.generation != feedback_before.generation",
        "feedback_after.has_charging_amps",
    ):
        if token not in amps:
            raise AssertionError(f"charging-amps fixed readback contract missing {token!r}")
    if "copy_locked_(last_known_charge_)" in scrub_cpp(amps):
        raise AssertionError("charging-amps verification races deferred public cache publication")


def require_crash_snapshot_contract(crash_source: str) -> None:
    body = scrub_cpp(function_body_in(crash_source, "diag_crash_info_live"))
    borrow = "CrashInfoView info = crash_borrow(s_boot);"
    final_load = "const uint32_t after = s_admission.load(std::memory_order_acquire);"
    finish = "crash_finish_snapshot(info, {before, after, same_header(query.header, s_header),"
    require_before("crash final generation after borrowed snapshot", body, borrow, final_load)
    require_before("crash final generation passed to pure guard", body, final_load, finish)
    if body.count(final_load) != 1 or body.count("crash_finish_snapshot(") != 1:
        raise AssertionError("crash snapshot must finalize once with the actual final generation")
    if "state(after) == Current, state(after) == Foreign," not in body or \
       "state(after) == NoArtifact});" not in body:
        raise AssertionError("crash snapshot final admission is not forwarded to the pure guard")


def require_crash_dismiss_atomic_contract(crash_source: str) -> None:
    require_crash_snapshot_contract(crash_source)
    for token in (
        "static std::atomic<bool> s_dismissed{false};",
        "s_dismissed.store(false, std::memory_order_release);",
        "info.dismissed = s_dismissed.load(std::memory_order_acquire);",
        "s_dismissed.store(true, std::memory_order_release);",
    ):
        if token not in crash_source:
            raise AssertionError(f"crash dismissal atomic contract missing {token!r}")
    code = scrub_cpp(crash_source)
    if "s_ci.dismissed =" in code or re.search(r"static\s+bool\s+s_dismissed", code):
        raise AssertionError("crash dismissal reintroduced a racy shared bool/string snapshot")
    erase = function_body_in(crash_source, "diag_crash_erase")
    require_before("crash erase ownership", erase,
                   "if (!lease.held()) return CrashEraseResult::Busy;", "locate_partition()")
    require_before("crash effect before SDK erase", erase,
                   "reply.attempting(RequestEffect::CoredumpErase)", "esp_core_dump_image_erase()")
    require_before("crash invalidation before SDK erase", erase,
                   "invalidate(Quarantined)", "esp_core_dump_image_erase()")
    guard = "if (esp_core_dump_image_erase() != ESP_OK)"
    start = erase.find(guard)
    if start < 0:
        raise AssertionError("crash erase result is not checked")
    condition, opening, closing = if_statement_at(scrub_cpp_preserving_layout(erase), start,
                                                "crash erase result")
    if "return CrashEraseResult::Unconfirmed;" not in erase[opening:closing]:
        raise AssertionError("failed erase may publish dismissal")
    if "s_dismissed.store(true" in erase[opening:closing]:
        raise AssertionError("failed erase publishes dismissal")
    require_before("crash erase success before atomic dismissal", erase[closing:],
                   "invalidate(NoArtifact)", "s_dismissed.store(true")
    if "esp_core_dump_image_erase" in function_body_in(crash_source, "capture_artifact"):
        raise AssertionError("boot capture may erase crash evidence")


def require_heap_supervisor_start_contract(main_source: str) -> None:
    """Bind the essential checked admission to the real unconditional early app_main scope."""
    raw = function_body_in(main_source, "app_main")
    code = scrub_cpp_preserving_layout(raw)
    record = re.search(
        r"\bconst\s+tk::RebootReasonRecord\s+boot_reboot_record\s*=\s*"
        r"VehicleController::take_reboot_reason\s*\(\s*config_store\s*\)\s*;", code)
    call = r"tk::heap_supervisor_start\s*\(\s*config_store\s*,\s*boot_reboot_record\s*\)"
    checked = list(re.finditer(r"\bif\s*\(\s*!\s*" + call + r"\s*\)\s*\{", code))
    if record is None or len(checked) != 1 or len(re.findall(r"\bheap_supervisor_start\s*\(", code)) != 1:
        raise AssertionError("essential Heap startup needs one typed reason and one checked real call")
    match = checked[0]
    condition, opening, closing = if_statement_at(code, match.start(), "essential Heap startup")
    if re.sub(r"\s+", "", condition) != "!tk::heap_supervisor_start(config_store,boot_reboot_record)":
        raise AssertionError("essential Heap startup condition is not the checked real call")
    if not re.fullmatch(r'\s*boot_fatal\s*\(\s*"heap supervisor"\s*\)\s*;\s*', raw[opening + 1:closing]):
        raise AssertionError("essential Heap startup failure must be boot-fatal")
    if (enclosing_scope(code, record.start(), "Heap reason") !=
            enclosing_scope(code, match.start(), "Heap admission")):
        raise AssertionError("essential Heap startup is hidden in another conditional scope")
    previous = code[:match.start()].rstrip()
    if not previous or previous[-1] not in ";}":
        raise AssertionError("essential Heap startup is controlled by an unbraced outer statement")
    verdict = code.find("const bool safe_mode")
    if not record.end() < verdict < match.start():
        raise AssertionError("essential Heap startup must follow typed reason and BootGuard verdict")
    for token in (r"\bsyslog_start\s*\(", r"\bnet_eth_probe\s*\(",
                  r"\bnet_start_eth\s*\(", r"\bnet_start_wifi\s*\(",
                  r"\bprovisioning_run(?:_with_retry)?\s*\(", r"\bhttp_server_start\s*\("):
        sites = list(re.finditer(token, code))
        if not sites or any(site.start() <= closing for site in sites):
            raise AssertionError("essential Heap startup must precede every optional/network/portal entry")


def require_heap_restart_gate_contract(supervisor: str) -> None:
    """Pin actual independent restart admission; native tests execute all effects/owners."""
    body = scrub_cpp_preserving_layout(function_body_in(supervisor, "attempt_heap_restart"))
    guards = ("!s.budget.may_restart()", "!s.restart_write.due(now_ms)", "!owner.held()",
              "largest>=kHeapCriticalBytes||ota_is_busy()",
              "!reservation",
              "!persist_reboot_reason(s.cfg,why.text)")
    positions = []
    for expected in guards:
        matches = []
        for found in re.finditer(r"\bif\s*\(", body):
            paren = body.index("(", found.start())
            condition_end = balanced_end(body, paren, "(", ")", expected)
            condition = "".join(body[paren + 1:condition_end].split())
            if condition != expected:
                continue
            opening = skip_space(body, condition_end + 1, len(body))
            closing = (balanced_end(body, opening, "{", "}", expected)
                       if body[opening] == "{" else body.index(";", opening))
            matches.append((found.start(), opening, closing))
        if len(matches) != 1:
            raise AssertionError("heap restart guard missing/duplicated: " + expected)
        at, opening, closing = matches[0]
        if enclosing_scope(body, at, expected) != (0, len(body) - 1):
            raise AssertionError("heap restart refusal hidden in another branch: " + expected)
        refusal = body[opening + 1:closing] if body[opening] == "{" else body[opening:closing + 1]
        if not refusal.strip().endswith("return;") or re.search(r"\b(?:if|for|while|goto)\b", refusal):
            raise AssertionError("heap restart guard does not return directly: " + expected)
        positions.append(at)
    if positions != sorted(positions):
        raise AssertionError("heap restart guard order drifted")
    for token in ("OtaFaultRestartGuard owner;", "s.restart_write.attempt(fresh_ms);",
                  "const auto reservation = safe_mode_prepare_heap_restart(*s.cfg, owner);",
                  "const uint8_t next = static_cast<uint8_t>(s.budget.effective + 1);",
                  "const HeapReason why = heap_reason_format(next);", "owner.seal();",
                  "s.shutdown_committed = true;", "esp_restart();", "park_recovery();"):
        if body.count(token) != 1:
            raise AssertionError("heap restart authority missing/duplicated: " + token)
    if re.search(r"s\.budget\.(?:effective|trusted)\s*(?:=(?!=)|[+*/%-]=|\+\+|--)", body):
        raise AssertionError("heap restart overwrites its verified effective budget")
    require_before("heap cap before lease", body, "!s.budget.may_restart()", "OtaFaultRestartGuard owner;")
    require_before("heap lease before post-CAS sample", body, "!owner.held()", "const size_t largest")
    require_before("write cadence before fallible latch", body, "s.restart_write.attempt(fresh_ms);",
                   "safe_mode_prepare_heap_restart")
    require_before("confirmed shared charge before reason", body, "if (!reservation)", "persist_reboot_reason")
    maintenance = scrub_cpp_preserving_layout(function_body_in(supervisor, "maintain_healthy_run"))
    require_before("durable shared zero before diagnostic refill", maintenance,
                   "safe_mode_clear_after_healthy_run", "s.budget.decay_after_healthy_run")
    require_before("confirmed reason before seal", body, "persist_reboot_reason", "owner.seal();")
    require_before("sealed owner before shutdown", body, "owner.seal();", "s.shutdown_committed = true;")
    require_before("shutdown before restart", body, "s.shutdown_committed = true;", "esp_restart();")
    require_before("returning restart parks", body, "esp_restart();", "park_recovery();")
    iteration = " ".join(scrub_cpp(function_body_in(supervisor, "supervisor_iteration")).split())
    raw_iteration = scrub_cpp_preserving_layout(function_body_in(supervisor, "supervisor_iteration"))
    begin = raw_iteration.find("const bool writer_busy = ota_is_busy();")
    end = raw_iteration.find("maintain_healthy_run(s, now_ms);", begin)
    if begin < 0 or end < 0 or "".join(raw_iteration[begin:end].split()) != 'constboolwriter_busy=ota_is_busy();constboolexcused=s.writer_grace.excused(now_ms,largest,writer_busy,ota::kCriticalWriterGraceMs);if(verdict.action==HeapAction::Restart){if(writer_busy){if(!excused)ota_request_heap_abort();report_degraded(s,now_ms,excused?:);}else{attempt_heap_restart(s,now_ms,verdict);}}':
        raise AssertionError("independent critical verdict must request writer abort or call admitted restart after quiescence")
    start = " ".join(scrub_cpp(function_body_in(supervisor, "heap_supervisor_start")).split())
    if "boot_reason.heap_restarts, boot_reason.state != RebootReasonState::Error" not in start:
        raise AssertionError("heap allowance must seed from complete immutable boot evidence")


HTTP_ROUTE_DISPATCH = {
    "Command": "handle_command",
    "VehicleData": "handle_vehicle_data",
    "BodyController": "handle_body_controller",
    "OtaCheck": "handle_ota_check",
    "OtaUpdate": "handle_ota_update",
    "OtaStatus": "handle_ota_status",
    "OtaChangelog": "handle_ota_changelog",
    "GenKeys": "handle_gen_keys",
    "SetTime": "handle_set_time",
    "SetVin": "handle_set_vin",
    "SetMqtt": "handle_set_mqtt",
    "SetSyslog": "handle_set_syslog",
    "SetWifi": "handle_set_wifi",
    "SetOta": "handle_set_ota",
    "Scan": "handle_scan",
    "Coredump": "handle_coredump",
    "CrashDismiss": "handle_crash_dismiss",
    "Heap": "handle_heap",
    "McpPost": "mcp_handle_post",
    "McpGet": "mcp_handle_get",
    "Version": "handle_version",
    "Status": "handle_status",
    "Diag": "handle_diag",
    "Index": "handle_index",
}


def require_http_route_dispatch(body: str) -> None:
    if body.count("tk::classify_http_route") != 1:
        raise AssertionError("HTTP dispatcher must use the tested route classifier exactly once")
    for bypass in ("strstr(path", "strcmp(path", "path_ends_with"):
        if bypass in body:
            raise AssertionError(f"HTTP dispatcher reintroduced an ad-hoc route match: {bypass}")
    cases = set(re.findall(r"case\s+tk::HttpRoute::([A-Za-z0-9_]+)\s*:", body))
    expected_cases = set(HTTP_ROUTE_DISPATCH) | {"NotFound"}
    if cases != expected_cases:
        raise AssertionError(
            f"HTTP route case inventory drift: missing={sorted(expected_cases - cases)}, "
            f"unreviewed={sorted(cases - expected_cases)}"
        )
    for route, handler in HTTP_ROUTE_DISPATCH.items():
        pattern = (
            rf"case\s+tk::HttpRoute::{re.escape(route)}\s*:\s*"
            rf"return\s+{re.escape(handler)}\s*\(\s*rq\s*\)\s*;"
        )
        if not re.search(pattern, body):
            raise AssertionError(f"HTTP route {route} no longer dispatches to {handler}")


def require_http_path_seam(path_helper: str, dispatch: str) -> None:
    if "tk::http_path_only" not in path_helper:
        raise AssertionError("HTTP path copy bypasses the tested query-strip seam")
    require_before(
        "HTTP query strip before route classification",
        dispatch,
        "uri_path(req, path, sizeof(path));",
        "tk::classify_http_route",
    )


def require_safe_mode_source_contract(source: str) -> None:
    if "boot_fail_parse(raw.c_str(), count)" not in source:
        raise AssertionError("safe-mode glue bypasses the fail-closed counter parser")
    if "Clears itself after a boot that stays up" in source:
        raise AssertionError("safe-mode operator text still promises an unarmed auto-clear timer")
    operator_text = "".join(re.findall(r'"([^"\\]*(?:\\.[^"\\]*)*)"', strip_cpp_comments(source)))
    for phrase in ("Exit needs a known clean boot and a confirmed counter reset.",
                   "a clean boot exits only if counter storage is healthy."):
        if phrase not in operator_text:
            raise AssertionError("safe-mode operator text lost the storage condition for leaving recovery")


ITERATION_FAULT_TASKS = (
    ("vehicle_telemetry.cpp", "loop_task_fn_", "while (true)", 50),
    ("display.cpp", "display_task", "for (;;)", 200),
    ("led_status.cpp", "led_task", "for (int frame = 0;; ++frame)", 200),
)


def require_iteration_fault_contract(source: str, task: str, loop: str, delay: int) -> None:
    body = strip_cpp_comments(function_body_in(source, task))
    declaration = "tk::IterationFaultLog fault_log{};"
    declarations = list(re.finditer(r"(?:^|[;{}])\s*(tk::IterationFaultLog\s+fault_log\s*\{\s*\}\s*;)", body))
    if len(declarations) != 1 or body.count(declaration) != 1 or declarations[0].start(1) > body.index(loop):
        raise AssertionError("iteration fault log state must be task-owned outside the loop")
    if body.count("tk::report_iteration_fault(") != 1:
        raise AssertionError("iteration catch must use the shared bounded sink helper once")
    code = " ".join(body.split())
    clock = "tk::report_iteration_fault(fault_log, static_cast<uint64_t>(esp_timer_get_time()) / 1000,"
    if clock not in code:
        raise AssertionError("iteration fault reporting lost its monotonic 64-bit clock")
    if f"vTaskDelay(pdMS_TO_TICKS({delay}));" not in body:
        raise AssertionError("iteration fault handling lost its existing task delay")


def require_provisioning_body_contract(
    save_body: str, decode_body: str, http_body_header: str
) -> None:
    """Bind the captive POST to 1024 wire bytes in fixed storage plus one NUL byte."""
    required_save_tokens = (
        "static constexpr size_t kSaveBodyMaxBytes = 1024;",
        "std::array<char, kSaveBodyMaxBytes + 1> body{};",
        "tk::http_body_fits_buffer(static_cast<size_t>(len), body.size())",
        "tk::http_body_read(body.data(), body.size(), len,",
        "got < 0 || got != len",
        "tk::http_body_has_embedded_nul(body.data(), static_cast<size_t>(got))",
        "const std::string_view body_view(body.data(), static_cast<size_t>(got));",
        'form_field(body_view, "ssid", ssid)',
        'form_field(body_view, "pass", pass)',
        'form_field(body_view, "vin", vin)',
    )
    for token in required_save_tokens:
        if token not in save_body:
            raise AssertionError(f"provisioning save body contract missing {token!r}")
    if re.search(r"\bstd::(?:string|vector)\s+body\b", scrub_cpp(save_body)):
        raise AssertionError("provisioning save body regressed to dynamic whole-body storage")
    if save_body.count("std::array<char, kSaveBodyMaxBytes + 1> body{};") != 1:
        raise AssertionError("provisioning save must own exactly one fixed request-body buffer")
    require_before(
        "provisioning length preflight",
        save_body,
        "tk::http_body_fits_buffer",
        "tk::http_body_read",
    )
    require_before(
        "provisioning complete read",
        save_body,
        "got < 0 || got != len",
        "const std::string_view body_view",
    )
    require_before(
        "provisioning embedded-NUL rejection",
        save_body,
        "tk::http_body_has_embedded_nul",
        "const std::string_view body_view",
    )

    for token in (
        "return total > 0 && total < capacity;",
        "if (!buf || !http_body_fits_buffer(total, cap)) return -1;",
        "if (data[i] == '\\0') return true;",
    ):
        if token not in http_body_header:
            raise AssertionError(f"shared fixed-body boundary missing {token!r}")
    for token in (
        "if (i + 2 >= in.size()) return false;",
        "if (high < 0 || low < 0) return false;",
        "if (decoded == '\\0') return false;",
    ):
        if token not in decode_body:
            raise AssertionError(f"provisioning URL decoder missing {token!r}")


# Review-bounded source checks complement the compiled native response adapters. The allowlist
# includes local diagnostics (paired_at may persist a guarded timestamp); it is a BLE-effect
# boundary for these named consumers, not a C++ dataflow or general no-wake proof.
CACHE_READERS = {
    'ble_connect_fail',
    'ble_connected',
    'ble_nearby',
    'ble_peer',
    'ble_phase',
    'ble_rssi',
    'ble_scanning',
    'ble_seen_rssi',
    'ble_target_connectable',
    'get_cached_charge',
    'get_cached_charge_with_age',
    'get_cached_climate',
    'get_cached_closures',
    'get_cached_drive',
    'get_cached_tires',
    'get_charge_state',
    'has_key',
    'has_session',
    'key_created_at',
    'key_fingerprint',
    'link_state',
    'paired_at',
    'pairing_block_reason',
    'reauth_required',
    'seconds_since_contact',
    'session_availability',
    'session_available',
    'ui_snapshot',
    'vcsec_sleep_raw',
    'vin',
}
VEHICLE_CALL = re.compile(r"\b(?:v|vehicle|s_vehicle|g_vehicle)\s*(?:\.|->)\s*(\w+)\s*\(")



def require_enrolment_role_pin(pairing: str, all_code: str) -> None:
    """F-017: the firmware enrols a Charging Manager key and never names another key role."""
    clean = scrub_cpp(pairing)
    if clean.count("const Keys_Role role = Keys_Role_ROLE_CHARGING_MANAGER;") != 1:
        raise AssertionError("enrolment must pin exactly one Charging Manager role")
    roles = set(re.findall(r"\bKeys_Role_ROLE_\w+", scrub_cpp(all_code)))
    if roles != {"Keys_Role_ROLE_CHARGING_MANAGER"}:
        raise AssertionError(f"firmware must name only the Charging Manager key role, found {sorted(roles)}")
    whitelist = call_arguments(clean, "c->build_white_list_message")
    if len(whitelist) != 1 or whitelist[0][0] != "role":
        raise AssertionError("whitelist enrolment must use the pinned Charging Manager role")
    if len(call_arguments(scrub_cpp(all_code), "build_white_list_message")) != 1:
        raise AssertionError("the firmware must build exactly one whitelist enrolment")


def require_background_wake_policies(telemetry: str, pairing: str, commands: str) -> None:
    """F-109: background telemetry and pairing traffic never asks for a wake."""
    for name, source, allowed in (
        ("vehicle_telemetry.cpp", telemetry, {"NoWakeSkip"}),
        ("vehicle_pairing.cpp", pairing, {"NoWakeFail"}),
    ):
        policies = set(re.findall(r"\bWakePolicy::(\w+)", scrub_cpp(source)))
        if not policies or not policies <= allowed:
            raise AssertionError(f"{name} background traffic may use only {sorted(allowed)}, found {sorted(policies)}")
        # send_infotainment_ defaults to WakeIfNeeded; a background caller must not rely on it.
        if call_arguments(scrub_cpp(source), "send_infotainment_"):
            raise AssertionError(f"{name} must not call the foreground send_infotainment_ helper")
        # Every background request names its no-wake policy literally; a variable could carry a wake.
        for helper in ("command_runner_.enqueue", "send_vcsec_", "send_vcsec_locked_"):
            for args in call_arguments(scrub_cpp(source), helper):
                if not any(re.fullmatch(r"(?:tk::)?WakePolicy::NoWake(?:Skip|Fail)", arg) for arg in args):
                    raise AssertionError(f"{name} {helper} call lacks a literal no-wake policy")
    # The command name is a string literal, so this one check reads the unscrubbed function.
    status = " ".join(function_body_in(telemetry, "get_vehicle_status").split())
    if '"VCSEC Status Poll", tk::BleDomain::VehicleSecurity, tk::WakePolicy::NoWakeSkip,' not in status:
        raise AssertionError("the VCSEC status poll must stay NO_WAKE_SKIP")
    foreground = set(re.findall(r"\bWakePolicy::(\w+)", scrub_cpp(commands)))
    if not foreground <= {"WakeIfNeeded", "NoWakeFail"}:
        raise AssertionError(f"foreground commands use an unreviewed wake policy: {sorted(foreground)}")


def require_cache_only_consumers(sources: dict[str, str]) -> None:
    """F-109: status, MQTT, display, LEDs and MCP get_vehicle_state read caches only."""
    scopes = (
        ("display.cpp", scrub_cpp(sources["display.cpp"])),
        ("led_status.cpp", scrub_cpp(sources["led_status.cpp"])),
        ("mqtt_ha.cpp", scrub_cpp(sources["mqtt_ha.cpp"])),
        ("/status", scrub_cpp(function_body_in(sources["http_status.cpp"], "build_status_object"))),
        ("MCP get_vehicle_state", scrub_cpp(function_body_in(sources["mcp_server.cpp"], "vehicle_state_result_"))),
    )
    for label, code in scopes:
        calls = set(VEHICLE_CALL.findall(code))
        if not calls:
            raise AssertionError(f"{label} no longer reads the vehicle caches (pin is stale)")
        unknown = calls - CACHE_READERS
        if unknown:
            raise AssertionError(f"{label} calls non-cache VehicleController methods: {sorted(unknown)}")
        if re.search(r"\bexecute_vehicle_command\s*\(", code):
            raise AssertionError(f"{label} reaches the command executor")
        receivers = {"v", "vehicle", "s_vehicle", "g_vehicle"}
        for alias, _ in re.findall(r"(\w+)\s*=\s*[&*]?\s*(v|vehicle|s_vehicle|g_vehicle)\s*;", code):
            if alias not in receivers:
                raise AssertionError(f"{label} aliases the vehicle controller as {alias!r}")


def require_command_path_seams(telemetry: str, commands: str, ctrl: str) -> None:
    """F-113: command-path decisions whose pure halves are host-tested but whose shells were not."""
    # A completion clears the builder slot and pops the queue only for its own command ID.
    slot = (r"if \(cmd && cmd->id == ({id})\) \{{\s*command_builders_\[\1 % tk::CommandRunner::kMaxQueueSize\] = nullptr;"
            r"\s*command_runner_\.pop_current\(\);\s*\}}")
    for label, source, function, ident in (
        ("send_vcsec_locked_", commands, "send_vcsec_locked_", r"cmd_id"),
        ("send_infotainment_locked_", commands, "send_infotainment_locked_", r"cmd_id"),
        ("get_vehicle_status", telemetry, "get_vehicle_status", r"completion->cmd_id"),
    ):
        body = scrub_cpp(function_body_in(source, function))
        if not re.search(r"tk::SemGuard g\(vehicle_mutex_\);[^{}]*?auto\* cmd = command_runner_\.current_command\(\);\s*"
                         + slot.format(id=ident), body):
            raise AssertionError(f"{label} must clear only its own command slot under vehicle_mutex_")
    # VCSEC sleep stays three-valued: UNKNOWN neither proves sleep nor confirms a wake.
    vcsec = scrub_cpp(function_body_in(telemetry, "handle_vcsec_frame_"))
    for token in (
        "tk::sleep_state_from_vcsec( status.vehicleSleepStatus == VCSEC_VehicleSleepStatus_E_VEHICLE_SLEEP_STATUS_AWAKE, "
        "status.vehicleSleepStatus == VCSEC_VehicleSleepStatus_E_VEHICLE_SLEEP_STATUS_ASLEEP);",
        "vcsec_sleep_state_.store(static_cast<int>(reported));",
        "note_vcsec_sleep_(reported);",
        "if (tk::vcsec_status_confirms_wake(reported, status.has_closureStatuses)) {",
    ):
        if token not in " ".join(vcsec.split()):
            raise AssertionError(f"VCSEC sleep handling lost {token!r}")
    runner = " ".join(scrub_cpp(function_body_in(telemetry, "drive_command_runner_")).split())
    for token in (
        "const bool awake = (sleep_state == tk::SleepState::Awake);",
        "const bool asleep = (sleep_state == tk::SleepState::Asleep);",
        "command_runner_.tick(now_ms, connected, awake, asleep);",
    ):
        if token not in runner:
            raise AssertionError(f"command runner input lost {token!r}")
    if len(call_arguments(runner, "command_runner_.tick")) != 1:
        raise AssertionError("the command runner must be ticked exactly once, with the pinned inputs")
    # The loop and the cached getters ask the same member whether the active window is open.
    loop = scrub_cpp(function_body_in(telemetry, "loop_task_fn_"))
    if not re.search(r"bool window = self->active_window_now_\(now_ticks, charging_state\);", loop):
        raise AssertionError("vehicle loop must take its active window from active_window_now_")
    if len(re.findall(r"\bwindow\s*=(?!=)", loop)) != 1:
        raise AssertionError("vehicle loop must not overwrite its active window after deciding it")
    # Boot seeds activity only for the reviewed reset/reboot-evidence decision.
    init = scrub_cpp(function_body_in(ctrl, "init"))
    seeds = [match.start() for match in re.finditer(
        r"\blast_cmd_ticks_\s*(?:\.\s*(?:store|exchange|fetch_add)\s*\(|=(?!=))", init)]
    gate = re.search(r"\bif\s*\(\s*tk::boot_activity_seed_allowed\(\s*tk::diag_crash_reset_info\(\).code,\s*boot_reason_slot\(\).state\)\s*\)\s*\{", init)
    if len(seeds) != 1 or not gate:
        raise AssertionError("boot must seed the active window exactly once, behind the boot-activity gate")
    gate_close = balanced_end(init, gate.end() - 1, "{", "}", "boot seed gate")
    if not gate.end() <= seeds[0] < gate_close:
        raise AssertionError("boot seeds the active window outside the boot-activity gate")


def self_test_ble_safety_contract() -> None:
    # PKG-18 (#391) canaries: each mutation breaks one vehicle-safety rule the pins must catch.
    pairing_src = SOURCES["vehicle_pairing.cpp"]
    telemetry_src = SOURCES["vehicle_telemetry.cpp"]
    commands_src = SOURCES["vehicle_commands.cpp"]
    ctrl_src = SOURCES["vehicle_ctrl.cpp"]

    def mutate(label: str, text: str, old: str, new: str) -> str:
        mutated = text.replace(old, new, 1)
        if mutated == text:
            raise AssertionError(f"PKG-18 canary did not apply: {label}")
        return mutated

    for label, old, new in (
        ("owner role enrolled (A04 C-G1)", "Keys_Role_ROLE_CHARGING_MANAGER;", "Keys_Role_ROLE_OWNER;"),
        ("whitelist built with a literal owner role", "build_white_list_message(\n                role,",
         "build_white_list_message(\n                Keys_Role_ROLE_OWNER,"),
        ("role constant made reassignable", "const Keys_Role role = Keys_Role_ROLE_CHARGING_MANAGER;",
         "Keys_Role role = Keys_Role_ROLE_CHARGING_MANAGER;"),
    ):
        mutated = mutate(label, pairing_src, old, new)
        require_mutation_rejected(
            f"PKG-18 {label}",
            lambda mutated=mutated: require_enrolment_role_pin(mutated, ALL_CODE.replace(pairing_src, mutated, 1)))
    for label, telemetry, pairing, commands in (
        ("VCSEC status poll may wake (A07-G2)",
         mutate("A07-G2", telemetry_src, '"VCSEC Status Poll", tk::BleDomain::VehicleSecurity, tk::WakePolicy::NoWakeSkip,',
                '"VCSEC Status Poll", tk::BleDomain::VehicleSecurity, tk::WakePolicy::WakeIfNeeded,'),
         pairing_src, commands_src),
        ("background poll may wake (A07-G1)",
         mutate("A07-G1", telemetry_src, "tk::WakePolicy::NoWakeSkip, 10000", "tk::WakePolicy::WakeIfNeeded, 10000"),
         pairing_src, commands_src),
        ("pairing traffic may wake",
         telemetry_src, mutate("pairing wake", pairing_src, "WakePolicy::NoWakeFail, deadline, tk::ConnectOrigin::Background,",
                               "WakePolicy::WakeIfNeeded, deadline, tk::ConnectOrigin::Background,"), commands_src),
        ("waking policy named outside a helper call",
         mutate("policy const", telemetry_src, "void VehicleController::drive_command_runner_() {",
                "static constexpr tk::WakePolicy kBackground = tk::WakePolicy::WakeIfNeeded;\n"
                "void VehicleController::drive_command_runner_() {"),
         pairing_src, commands_src),
        ("background poll with a non-literal policy",
         mutate("policy var", telemetry_src, "name, domain, tk::WakePolicy::NoWakeSkip, 10000,", "name, domain, policy_, 10000,"),
         pairing_src, commands_src),
    ):
        require_mutation_rejected(
            f"PKG-18 {label}",
            lambda t=telemetry, p=pairing, c=commands: require_background_wake_policies(t, p, c))
    for label, name, old, new in (
        ("MQTT wakes the car", "mqtt_ha.cpp", "ChargeStateResult cs = s_vehicle->get_cached_charge();",
         "s_vehicle->wake_up(); ChargeStateResult cs = s_vehicle->get_cached_charge();"),
        ("display wakes the car", "display.cpp", "tk::UiSnapshot s = safe_mode ? tk::UiSnapshot{} : v.ui_snapshot();",
         "v.wake_up(); tk::UiSnapshot s = safe_mode ? tk::UiSnapshot{} : v.ui_snapshot();"),
        ("LEDs wake the car", "led_status.cpp", "snap = v.ui_snapshot();", "v.wake_up(); snap = v.ui_snapshot();"),
        ("/status polls the car", "http_status.cpp", "in.vin             = g_vehicle->vin();",
         "g_vehicle->charge_state_poll();\n    in.vin             = g_vehicle->vin();"),
        ("MCP get_vehicle_state polls the car", "mcp_server.cpp",
         "const bool charge_available = g_vehicle->get_charge_state(cs);",
         "g_vehicle->charge_state_poll(); const bool charge_available = g_vehicle->get_charge_state(cs);"),
    ):
        consumers = dict(SOURCES)
        consumers[name] = mutate(label, SOURCES[name], old, new)
        require_mutation_rejected(f"PKG-18 {label}", lambda consumers=consumers: require_cache_only_consumers(consumers))
    for label, name, old, new in (
        ("MCP get_vehicle_state runs a command", "mcp_server.cpp",
         "const bool charge_available = g_vehicle->get_charge_state(cs);",
         "execute_vehicle_command(*g_vehicle, tk::CmdKind::WakeUp); const bool charge_available = g_vehicle->get_charge_state(cs);"),
        ("display aliases the controller", "display.cpp", "tk::UiSnapshot s = safe_mode ? tk::UiSnapshot{} : v.ui_snapshot();",
         "auto& car = v; car.wake_up(); tk::UiSnapshot s = safe_mode ? tk::UiSnapshot{} : v.ui_snapshot();"),
    ):
        consumers = dict(SOURCES)
        consumers[name] = mutate(label, SOURCES[name], old, new)
        require_mutation_rejected(f"PKG-18 {label}", lambda consumers=consumers: require_cache_only_consumers(consumers))
    for label, telemetry, commands, ctrl in (
        ("status poll clears any command slot",
         mutate("slot", telemetry_src, "if (cmd && cmd->id == completion->cmd_id) {", "if (cmd) {"),
         commands_src, ctrl_src),
        ("VCSEC runner clears any command slot",
         telemetry_src, mutate("vcsec slot", commands_src, "if (cmd && cmd->id == cmd_id) {", "if (cmd) {"), ctrl_src),
        ("UNKNOWN sleep treated as awake",
         mutate("awake", telemetry_src, "const bool awake = (sleep_state == tk::SleepState::Awake);",
                "const bool awake = (sleep_state != tk::SleepState::Asleep);"), commands_src, ctrl_src),
        ("any VCSEC status confirms a wake",
         mutate("confirm", telemetry_src, "if (tk::vcsec_status_confirms_wake(reported, status.has_closureStatuses)) {",
                "if (true) {"), commands_src, ctrl_src),
        ("vehicle loop window forced open",
         mutate("window", telemetry_src, "bool window = self->active_window_now_(now_ticks, charging_state);",
                "bool window = true;"), commands_src, ctrl_src),
        ("raw VCSEC sleep state not handed to the runner",
         mutate("raw store", telemetry_src, "vcsec_sleep_state_.store(static_cast<int>(reported));",
                "vcsec_sleep_state_.store(static_cast<int>(tk::SleepState::Awake));"), commands_src, ctrl_src),
        ("vehicle loop window overwritten after the decision",
         mutate("window overwrite", telemetry_src, "bool window = self->active_window_now_(now_ticks, charging_state);",
                "bool window = self->active_window_now_(now_ticks, charging_state);\n        window = true;"),
         commands_src, ctrl_src),
        ("boot seeds the window by plain assignment",
         telemetry_src, commands_src,
         mutate("seed assign", ctrl_src, "    if (tk::boot_activity_seed_allowed(tk::diag_crash_reset_info().code, boot_reason_slot().state)) {\n        last_cmd_ticks_.store(xTaskGetTickCount());",
                "    last_cmd_ticks_ = xTaskGetTickCount();\n    if (tk::boot_activity_seed_allowed(tk::diag_crash_reset_info().code, boot_reason_slot().state)) {\n        last_cmd_ticks_.store(xTaskGetTickCount());")),
        ("boot seeds the window outside the reset/evidence gate",
         telemetry_src, commands_src,
         mutate("seed", ctrl_src, "    if (tk::boot_activity_seed_allowed(tk::diag_crash_reset_info().code, boot_reason_slot().state)) {\n        last_cmd_ticks_.store(xTaskGetTickCount());",
                "    last_cmd_ticks_.store(xTaskGetTickCount());\n    if (tk::boot_activity_seed_allowed(tk::diag_crash_reset_info().code, boot_reason_slot().state)) {")),
    ):
        require_mutation_rejected(
            f"PKG-18 {label}", lambda t=telemetry, c=commands, x=ctrl: require_command_path_seams(t, c, x))

    infotainment = function_body_in(commands_src, "send_infotainment_locked_")
    wrong_owner = mutate("Infotainment slot owner", infotainment,
                         "if (cmd && cmd->id == cmd_id) {", "if (cmd) {")
    changed = commands_src.replace(infotainment, wrong_owner, 1)
    require_mutation_rejected("PKG-18 Infotainment runner clears any command slot",
        lambda: require_command_path_seams(telemetry_src, changed, ctrl_src))
    extra_tick = mutate("duplicate runner tick", telemetry_src,
                        "tk::TxAction action = command_runner_.tick(now_ms, connected, awake, asleep);",
                        "tk::TxAction action = command_runner_.tick(now_ms, connected, awake, asleep);\n"
                        "    (void)command_runner_.tick(now_ms, connected, awake, asleep);")
    require_mutation_rejected("PKG-18 command runner ticked twice",
        lambda: require_command_path_seams(extra_tick, commands_src, ctrl_src))



def require_runtime_source_contracts() -> None:
    require_enrolment_role_pin(SOURCES["vehicle_pairing.cpp"], ALL_CODE)
    require_background_wake_policies(SOURCES["vehicle_telemetry.cpp"], SOURCES["vehicle_pairing.cpp"],
                                     SOURCES["vehicle_commands.cpp"])
    require_cache_only_consumers(SOURCES)
    require_command_path_seams(SOURCES["vehicle_telemetry.cpp"], SOURCES["vehicle_commands.cpp"],
                               SOURCES["vehicle_ctrl.cpp"])
    # Response construction is centralized. Every cJSON operation is an exact, file-scoped
    # inventory item: a new Create/Add/mutator/print API or a relocated parser/read operation is a
    # review event rather than an untested way around sticky ownership and the shared reply seam.
    require_cjson_api_allowlist(MAIN_CODE)

    builder = (MAIN / "json_builder.hpp").read_text(encoding="utf-8")
    fail_body = re.search(r"void\s+fail_\s*\([^)]*\)\s*noexcept\s*\{([^}]*)\}", builder)
    if not fail_body or "failed_ = true" not in fail_body.group(1):
        raise AssertionError("JsonBuilder: fail_ must set the sticky failure state")
    if "root_.reset" in fail_body.group(1):
        raise AssertionError("JsonBuilder: fail_ invalidates StatusJsonEmitter parent pointers")
    if "if (failed_)" not in builder or "root_.reset();" not in builder:
        raise AssertionError("JsonBuilder: failed tree is not discarded at finish")

    send_json = function_body("send_json")
    send_rpc = function_body("send_rpc_")
    require_shared_json_reply("REST send_json", send_json)
    require_shared_json_reply("MCP send_rpc", send_rpc)

    reply = (MAIN / "json_http_reply.hpp").read_text(encoding="utf-8")
    null_branch = reply[reply.find("if (!root)") : reply.find("JsonPrintOwner body")]
    print_branch = reply[reply.find("if (!body)") :]
    require_before(
        "shared reply null-tree status ordering",
        null_branch,
        "transport.set_status(503);",
        "return transport.send(oom_body);",
    )
    require_before(
        "shared reply print-OOM status ordering",
        print_branch,
        "transport.set_status(503);",
        "return transport.send(oom_body);",
    )
    require_before(
        "shared reply complete-print ordering",
        reply,
        "JsonPrintOwner body(cJSON_PrintUnformatted(root.get()));",
        "if (success_status != 200)",
    )

    # Both receive-buffer and parse-tree owners must be gone before a command can block in BLE.
    command = function_body("handle_command")
    require_json_materialize_policy(
        "REST command JSON",
        command,
        {
            "Malformed": '"invalid JSON"',
            "TooDeep": '"JSON nesting too deep"',
            "UnsupportedNul": '"JSON NUL escape not supported"',
            "TooManyValues": '"too many JSON values"',
            "NoMemory": '"out of memory"',
        },
    )
    require_before("REST body lifetime", command, "body_owner.reset();", "execute_vehicle_command")
    require_before("REST parse lifetime", command, "json.reset();", "execute_vehicle_command")

    mcp_post = function_body("mcp_handle_post")
    require_json_materialize_policy(
        "MCP JSON",
        mcp_post,
        {
            "Malformed": "tk::kJsonRpcParseError",
            "TooDeep": "tk::kJsonRpcInvalidRequest",
            "UnsupportedNul": "tk::kJsonRpcInvalidRequest",
            "TooManyValues": "tk::kJsonRpcInvalidRequest",
            "NoMemory": "send_oom_503_",
        },
    )
    require_before("MCP body lifetime", mcp_post, "body_owner.reset();", "switch (m)")
    require_before(
        "MCP raw numeric id validation",
        mcp_post,
        "tk::json_top_level_numeric_id",
        "tk::json_materialize<cJSON>",
    )
    require_before(
        "MCP envelope before notification/dispatch",
        mcp_post,
        "tk::mcp_json::inspect_request_envelope",
        "if (id_status == tk::mcp_json::RpcIdStatus::Missing)",
    )
    route_tail = mcp_post[mcp_post.find("if (m == tk::McpMethod::ToolsCall)") :]
    require_before("MCP tools/list input lifetime", route_tail, "msg.reset();", "handle_tools_list_")
    require_before("MCP routed input lifetime", route_tail, "msg.reset();", "switch (m)")
    mcp_call = function_body("handle_tools_call_")
    require_mcp_call_releases(mcp_call)
    require_before("MCP parse lifetime/state", mcp_call, "request.reset();", "vehicle_state_result_")
    require_before("MCP parse lifetime/command", mcp_call, "request.reset();", "execute_vehicle_command")
    require_mcp_payload_seams(
        SOURCES["mcp_server.cpp"],
        (MAIN / "mcp_json_payloads.hpp").read_text(encoding="utf-8"),
    )
    require_status_production_seams(
        function_body("build_status_object"), function_body("handle_status"),
        MAIN_CODE["status_json_emitter.hpp"],
    )
    require_wifi_rollback_status_cache(SOURCES["main.cpp"], function_body("build_status_object"))
    require_vehicle_data_production_seams(function_body("handle_vehicle_data"))
    require_vehicle_climate_cache_seam(function_body("emit_cached_vehicle_climate"))
    require_vehicle_drive_cache_seam(function_body("emit_cached_vehicle_drive"))
    require_vehicle_drive_getter_seam((MAIN / "vehicle_ctrl.hpp").read_text())
    require_vehicle_climate_getter_seam(SOURCES["vehicle_telemetry.cpp"])
    require_charge_state_getter_seam(SOURCES["vehicle_telemetry.cpp"])
    require_active_window_seam(SOURCES["vehicle_telemetry.cpp"])
    require_climate_publication_stamp(SOURCES["vehicle_telemetry.cpp"])
    require_wake_companion_drive_poll(function_body_in(SOURCES["vehicle_telemetry.cpp"], "loop_task_fn_"),
                                      function_body_in(SOURCES["vehicle_telemetry.cpp"], "enqueue_telemetry_round_"))
    require_telemetry_poll_schedule(SOURCES["vehicle_telemetry.cpp"])
    require_drive_poll_builder(SOURCES["vehicle_telemetry.cpp"])
    require_unlocked_diagnostic_logging(SOURCES["heap_trend.cpp"], SOURCES["vehicle_telemetry.cpp"])
    require_long_uptime_clocks(SOURCES["vehicle_telemetry.cpp"], SOURCES["heap_trend.cpp"], SOURCES["heap_supervisor.cpp"])
    require_diag_dump_completion_contract(
        function_body("handle_diag"),
        (MAIN / "diag_log.hpp").read_text(encoding="utf-8"),
        SOURCES["diag_log.cpp"],
        (ROOT / "test/test_runtime_boundaries.cpp").read_text(encoding="utf-8"),
    )
    require_diag_redaction_stream_contract(
        function_body("handle_diag"),
        (MAIN / "logic/redact.hpp").read_text(encoding="utf-8"),
        SOURCES["diag_log.cpp"],
        (ROOT / "test/test_runtime_boundaries.cpp").read_text(encoding="utf-8"),
    )
    require_operation_wrapper_contract(SOURCES["ota_update.cpp"])
    require_health_commit_contract(function_body("ota_health_gate_task"))
    http_dispatch = function_body("handle_all_dispatch")
    require_http_route_dispatch(http_dispatch)
    require_http_path_seam(function_body("uri_path"), http_dispatch)
    require_vehicle_task_start_contract(
        SOURCES["vehicle_ctrl.cpp"],
        SOURCES["vehicle_telemetry.cpp"],
        SOURCES["vehicle_pairing.cpp"],
    )
    require_vin_transition_cleanup_contract(SOURCES["main.cpp"])
    require_telemetry_epoch_cache_contract(SOURCES["vehicle_telemetry.cpp"])
    require_pairing_cleanup_epoch_contract(SOURCES["vehicle_pairing.cpp"])
    require_runtime_admission_contract(
        (MAIN / "logic/runtime_admission.hpp").read_text(encoding="utf-8"),
        (MAIN / "runtime_admission.hpp").read_text(encoding="utf-8"),
        SOURCES["runtime_admission.cpp"],
        SOURCES["main.cpp"],
        (MAIN / "logic/http_route.hpp").read_text(encoding="utf-8"),
        http_dispatch,
    )
    require_nimble_start_ack_contract(
        (MAIN / "logic/nimble_start_gate.hpp").read_text(encoding="utf-8"),
        (MAIN / "ble_client.hpp").read_text(encoding="utf-8"),
        SOURCES["ble_client.cpp"],
        SOURCES["main.cpp"],
        (ROOT / "test/test_logic.cpp").read_text(encoding="utf-8"),
        (ROOT / "test/test_runtime_boundaries.cpp").read_text(encoding="utf-8"),
    )
    require_dynamic_ble_host_health_contract(
        (MAIN / "ble_client.hpp").read_text(encoding="utf-8"),
        SOURCES["ble_client.cpp"],
        SOURCES["main.cpp"],
        SOURCES["ota_update.cpp"],
        (ROOT / "test/test_logic.cpp").read_text(encoding="utf-8"),
        (ROOT / "test/test_runtime_boundaries.cpp").read_text(encoding="utf-8"),
    )
    require_ping_probe_contract(
        (MAIN / "ping_probe.hpp").read_text(encoding="utf-8"),
        (MAIN / "logic/ping_probe.hpp").read_text(encoding="utf-8"),
        SOURCES["net.cpp"],
        SOURCES["syslog.cpp"],
        (ROOT / "test/test_runtime_boundaries.cpp").read_text(encoding="utf-8"),
    )
    require_syslog_start_lifetime_contract(
        SOURCES["syslog.cpp"],
        (MAIN / "logic/syslog_start_gate.hpp").read_text(encoding="utf-8"),
        (ROOT / "test/test_logic.cpp").read_text(encoding="utf-8"),
        (ROOT / "test/test_runtime_boundaries.cpp").read_text(encoding="utf-8"),
    )
    require_ethernet_start_cleanup_contract(
        SOURCES["net.cpp"],
        (ROOT / "test/test_runtime_boundaries.cpp").read_text(encoding="utf-8"),
    )
    require_ota_fetch_contract(
        SOURCES["ota_update.cpp"],
        (MAIN / "logic/ota_contract.hpp").read_text(encoding="utf-8"),
        (MAIN / "ota_manifest.hpp").read_text(encoding="utf-8"),
    )
    require_ota_status_lock_contract(
        SOURCES["ota_update.cpp"],
        (ROOT / "test/test_runtime_boundaries.cpp").read_text(encoding="utf-8"),
    )
    require_ble_callback_lock_contract(SOURCES["ble_client.cpp"])
    require_tesla_cpp_callback_contract(
        SOURCES["vehicle_ctrl.cpp"],
        SOURCES["vehicle_telemetry.cpp"],
        SOURCES["ble_client.cpp"],
        SOURCES["vehicle_commands.cpp"],
    )
    require_crash_dismiss_atomic_contract(SOURCES["diag_crash.cpp"])
    require_coredump_stream_contract(
        SOURCES["http_status.cpp"],
        (ROOT / "test/test_runtime_boundaries.cpp").read_text(encoding="utf-8"),
    )
    require_heap_json_stream_contract(
        SOURCES["http_status.cpp"],
        (MAIN / "logic/heap_json_stream.hpp").read_text(encoding="utf-8"),
        (ROOT / "test/test_logic.cpp").read_text(encoding="utf-8"),
    )
    require_explicit_idf_boot_error_contract(
        SOURCES["main.cpp"],
        SOURCES["net.cpp"],
        SOURCES["provisioning.cpp"],
        (ROOT / "test/test_runtime_boundaries.cpp").read_text(encoding="utf-8"),
    )
    f137_parts = {"mqtt": SOURCES["mqtt_ha.cpp"], "net": SOURCES["net.cpp"],
                  "ble": SOURCES["ble_client.cpp"], "class": (MAIN / "ble_client.hpp").read_text(),
                  "wdt": (MAIN / "task_watchdog_subscription.hpp").read_text()}
    require_f137_source_contract(f137_parts)
    require_mqtt_production_seams(SOURCES["mqtt_ha.cpp"])
    require_mqtt_factory_inventory(
        (MAIN / "mqtt_payloads.hpp").read_text(encoding="utf-8"),
        SOURCES["mqtt_ha.cpp"],
        (ROOT / "test/test_mqtt_json_publish.cpp").read_text(encoding="utf-8"),
    )

    # Config handlers copy every input-dependent value first, then release the input tree before
    # NVS, key rotation, validation/response allocation, or reboot work.
    require_json_materialize_policy(
        "configuration JSON",
        function_body("parse_json_object_body_"),
        {
            "Malformed": "ConfigSubmissionStatus::MalformedJson",
            "TooDeep": "ConfigSubmissionStatus::JsonTooDeep",
            "UnsupportedNul": "ConfigSubmissionStatus::JsonUnsupportedNul",
            "TooManyValues": "ConfigSubmissionStatus::JsonTooManyValues",
            "NoMemory": "ConfigSubmissionStatus::JsonNoMemory",
        },
    )
    config_lifetimes = {
        "handle_set_time": ("const double epoch_ms", "parsed.json.reset();", "clock_synced_via_ntp"),
        "handle_set_vin": ("std::string vin", "parsed.json.reset();", "tk::cfg_load"),
        "handle_set_wifi": ("std::string pass", "parsed.json.reset();", "wifi_credentials_error"),
    }
    for name, (copied, released, long_work) in config_lifetimes.items():
        body = function_body(name)
        require_before(f"{name} input copy", body, copied, released)
        require_before(f"{name} parse lifetime", body, released, long_work)

    # No throwing allocation may be introduced after the first C handle is acquired. The owner is
    # separately executed at every partial-acquire stage in runtime_boundary_tests.
    mqtt_probe = function_body("mqtt_probe_broker")
    acquire = mqtt_probe.find("xSemaphoreCreateBinary")
    if acquire < 0:
        raise AssertionError("mqtt_probe_broker: semaphore acquisition seam missing")
    after_acquire = scrub_cpp(mqtt_probe[acquire:])
    if re.search(r"\b(?:std::string|new|throw)\b", after_acquire):
        raise AssertionError("mqtt_probe_broker: throwing construction after resource acquire")
    require_before("MQTT probe join", mqtt_probe, "esp_mqtt_client_stop", "owner.started = false")
    if "MqttProbeResourceOwner<EspMqttProbeOps> owner" not in SOURCES["http_config.cpp"]:
        raise AssertionError("MQTT probe: production path no longer uses the tested RAII owner")

    owner = (MAIN / "mqtt_probe_owner.hpp").read_text(encoding="utf-8")
    require_before("MQTT owner stop/destroy", owner, "Ops::stop", "Ops::destroy")
    require_before("MQTT owner destroy/semaphore", owner, "Ops::destroy", "Ops::delete_semaphore")

    # The heap watchdog must atomically exclude OTA/identity before reboot_why becomes durable.
    # A busy owner postpones the attempt; a failed persistence releases FaultRestart, while the
    # success path deliberately reaches esp_restart without releasing its owner.
    require_heap_restart_gate_contract(SOURCES["heap_supervisor.cpp"])
    require_heap_supervisor_start_contract(SOURCES["main.cpp"])

    require_safe_mode_source_contract(SOURCES["safe_mode.cpp"])
    for filename, task, loop, delay in ITERATION_FAULT_TASKS:
        require_iteration_fault_contract(SOURCES[filename], task, loop, delay)
    require_provisioning_body_contract(
        function_body("save_post_impl"),
        function_body("url_decode"),
        (MAIN / "logic/http_body.hpp").read_text(encoding="utf-8"),
    )


def check_contract(tasks: set[str], callbacks: set[str]) -> None:
    require_f135_sampling_programs(f135_program_parts())
    require_exact("task", tasks, EXPECTED_TASKS)
    require_exact("callback", callbacks, EXPECTED_CALLBACKS)

    all_boundaries = tasks | callbacks
    recorded = CONTAINED | set(DELEGATES_TO_CONTAINED) | set(REVIEWED_NON_THROWING)
    require_exact("boundary review", recorded, all_boundaries)

    for name in CONTAINED:
        require_catch_all(name, function_body(name))
    for adapter, target in DELEGATES_TO_CONTAINED.items():
        require_thin_delegate(adapter, target, function_body(adapter))
        require_catch_all(target, function_body(target))
    for name in REVIEWED_NON_THROWING:
        require_reviewed_nonthrowing(name, function_body(name), REVIEWED_ALLOWED_CALLS[name])
        for helper, calls in REVIEWED_DELEGATED_HELPERS.get(name, {}).items():
            require_reviewed_nonthrowing(helper, function_body(helper), calls, delegated=True)

    require_runtime_source_contracts()


def self_test_canaries(tasks: set[str], callbacks: set[str]) -> None:
    self_test_ble_safety_contract()
    attributed = "[[gnu::noinline]] void Fixture::round() { if (true) { return; } }\n"
    if function_body_in(attributed, "round") != "{ if (true) { return; } }":
        raise AssertionError("noinline function body discovery is ineffective")
    if function_definition_names(attributed) != {"round"}:
        raise AssertionError("noinline function inventory discovery is ineffective")
    # Source discovery follows the actual IDF SRCS list, including nested .cc/.cxx files. A
    # filename glob over main/*.cpp would miss this registered task entirely.
    with tempfile.TemporaryDirectory(prefix="runtime-cmake-inventory-") as directory:
        fixture_main = Path(directory) / "main"
        (fixture_main / "subdir").mkdir(parents=True)
        canonical_cmake = (
            'idf_component_register(\n    SRCS\n        "root.cpp"\n'
            '        "subdir/runtime_canary.cc"\n    INCLUDE_DIRS\n        "."\n)\n'
            'target_compile_options(${COMPONENT_LIB} PRIVATE\n'
            '    -std=gnu++17\n'
            '    -Werror=format\n'
            '    -Werror=return-type\n'
            '    -Werror=unused-result\n'
            ')\n'
        )
        (fixture_main / "CMakeLists.txt").write_text(canonical_cmake, encoding="utf-8")
        (fixture_main / "root.cpp").write_text("void root() {}\n", encoding="utf-8")
        (fixture_main / "subdir/runtime_canary.cc").write_text(
            "void fixture_task(void*) {}\n"
            "void start() { xTaskCreate(fixture_task, \"fixture\", 1, nullptr, 1, nullptr); }\n",
            encoding="utf-8",
        )
        fixture_sources = cmake_source_paths(fixture_main)
        if set(fixture_sources) != {"root.cpp", "subdir/runtime_canary.cc"}:
            raise AssertionError("CMake nested/.cc source discovery mutation canary is ineffective")
        fixture_text = "\n".join(
            path.read_text(encoding="utf-8") for path in fixture_sources.values()
        )
        if task_inventory(fixture_text) != {"fixture_task"}:
            raise AssertionError("registered nested-source task mutation canary is ineffective")

        compile_anchor = "    -Werror=unused-result\n)\n"
        for label, forced_include in (
            ("in-main forced include", "    -include include/evil.hpp\n"),
            ("outside-main forced include", "    -include ../outside.hpp\n"),
        ):
            mutated_cmake = canonical_cmake.replace(
                compile_anchor,
                "    -Werror=unused-result\n" + forced_include + ")\n",
            )
            if mutated_cmake == canonical_cmake:
                raise AssertionError(f"{label} mutation fixture did not change CMake")
            (fixture_main / "CMakeLists.txt").write_text(mutated_cmake, encoding="utf-8")
            try:
                cmake_source_paths(fixture_main)
            except AssertionError as exc:
                if "compile options" not in str(exc):
                    raise AssertionError(f"{label} failed for the wrong reason: {exc}") from exc
            else:
                raise AssertionError(f"{label} compile-option mutation passed unexpectedly")

        # Both an in-tree and an escaping target_sources attachment, plus a subdirectory source
        # surface, must fail before the literal inventory can be mistaken for the production set.
        for label, injection in (
            ("in-main target_sources", 'target_sources(${COMPONENT_LIB} PRIVATE "extra.cpp")\n'),
            ("outside-main target_sources", 'target_sources(${COMPONENT_LIB} PRIVATE "../outside.cpp")\n'),
            ("subdirectory", 'add_subdirectory("extra")\n'),
        ):
            (fixture_main / "CMakeLists.txt").write_text(
                canonical_cmake + injection, encoding="utf-8"
            )
            try:
                cmake_source_paths(fixture_main)
            except AssertionError as exc:
                if "source" not in str(exc) and "CMake commands" not in str(exc):
                    raise AssertionError(f"{label} failed for the wrong reason: {exc}") from exc
            else:
                raise AssertionError(f"{label} source-surface mutation passed unexpectedly")
        (fixture_main / "CMakeLists.txt").write_text(canonical_cmake, encoding="utf-8")

        (fixture_main / "include").mkdir()
        (fixture_main / "include" / "runtime_inline.hpp").write_text(
            "inline void header_task(void*) {}\n"
            "inline void header_start() { xTaskCreate(header_task, \"h\", 1, nullptr, 1, nullptr); }\n",
            encoding="utf-8",
        )
        (fixture_main / "subdir" / "cjson_gate.inc").write_text(
            "inline void cjson_bypass(cJSON* root) { cJSON_Print(root); }\n",
            encoding="utf-8",
        )
        (fixture_main / "root.cpp").write_text(
            '#include "include/runtime_inline.hpp"\n'
            '#include <cjson_gate.inc>\n'
            "void root() {}\n",
            encoding="utf-8",
        )
        fixture_code_paths = local_code_paths(fixture_main, fixture_sources)
        fixture_names = {path.as_posix() for path in fixture_code_paths}
        if not all(
            any(path.endswith(name) for path in fixture_names)
            for name in ("/include/runtime_inline.hpp", "/subdir/cjson_gate.inc")
        ):
            raise AssertionError(
                "local header/alternate-root include-fragment discovery mutation canary is ineffective"
            )
        combined_fixture = fixture_text + "\n" + "\n".join(
            path.read_text(encoding="utf-8") for path in fixture_code_paths
        )
        if task_inventory(combined_fixture) != {"fixture_task", "header_task"}:
            raise AssertionError("header-defined task registration escaped inventory")
        fragment_fixture = dict(MAIN_CODE)
        fragment_fixture["fixture/cjson_gate.inc"] = (
            fixture_main / "subdir" / "cjson_gate.inc"
        ).read_text(encoding="utf-8")
        try:
            require_cjson_api_allowlist(fragment_fixture)
        except AssertionError:
            pass
        else:
            raise AssertionError("included .inc cJSON bypass mutation passed unexpectedly")

        outside_header = fixture_main.parent / "outside.hpp"
        outside_header.write_text("inline void escaped_header() {}\n", encoding="utf-8")
        (fixture_main / "root.cpp").write_text(
            "#include <../outside.hpp>\nvoid root() {}\n", encoding="utf-8"
        )
        try:
            local_code_paths(fixture_main, fixture_sources)
        except AssertionError as exc:
            if "escapes main" not in str(exc):
                raise AssertionError(f"angled escaping include failed for the wrong reason: {exc}") from exc
        else:
            raise AssertionError("angled escaping local include mutation passed unexpectedly")

        for label, directive, expected in (
            ("macro include", '#define ESCAPED "../outside.hpp"\n#include ESCAPED\n',
             "non-literal local include"),
            ("include_next", "#include_next <outside.hpp>\n", "unsupported local include"),
            ("comment-split include", '#/**/include "../outside.hpp"\n',
             "escapes main"),
            ("line-spliced include", '#inc\\\nlude "../outside.hpp"\n',
             "escapes main"),
            ("digraph include", '%:include "../outside.hpp"\n',
             "unsupported preprocessor directive spelling"),
            ("trigraph include", '??=include "../outside.hpp"\n',
             "unsupported trigraph sequence"),
            ("trigraph-spliced include", '#inc??/\nlude "subdir/cjson_gate.inc"\n',
             "unsupported trigraph sequence"),
        ):
            (fixture_main / "root.cpp").write_text(directive + "void root() {}\n", encoding="utf-8")
            try:
                local_code_paths(fixture_main, fixture_sources)
            except AssertionError as exc:
                if expected not in str(exc):
                    raise AssertionError(f"{label} failed for the wrong reason: {exc}") from exc
            else:
                raise AssertionError(f"{label} mutation passed unexpectedly")

    static_task_fixture = (
        "void fixture_task(void*) {}\n"
        "void start() { xTaskCreateStatic(fixture_task, \"fixture\", 1, nullptr, nullptr, 1, "
        "nullptr, nullptr); }\n"
    )
    if task_inventory(static_task_fixture) != {"fixture_task"}:
        raise AssertionError("xTaskCreateStatic task discovery drifted")
    try:
        task_inventory("void start() { xTaskCreateCanary(fixture_task, nullptr); }")
    except AssertionError:
        pass
    else:
        raise AssertionError("unknown xTaskCreate* API mutation passed unexpectedly")

    shutdown_fixture = (
        "void fixture_callback() {}\n"
        "void start() { esp_register_shutdown_handler(fixture_callback); }\n"
    )
    if callback_inventory(shutdown_fixture) != {"fixture_callback"}:
        raise AssertionError("esp_register_shutdown_handler callback discovery drifted")
    ipc_fixture = (
        "void fixture_callback(void*) {}\n"
        "void start() { esp_ipc_call_blocking(0, fixture_callback, nullptr); }\n"
    )
    if callback_inventory(ipc_fixture) != {"fixture_callback"}:
        raise AssertionError("esp_ipc_call callback discovery drifted")
    for api, arguments in (
        ("esp_ipc_call", "0, fixture_callback, nullptr"),
        ("esp_ipc_call_blocking", "1, fixture_callback, nullptr"),
        ("httpd_queue_work", "server, fixture_callback, nullptr"),
        ("esp_eth_update_input_path", "eth_handle, fixture_callback, nullptr"),
        ("gpio_isr_handler_add", "0, fixture_callback, nullptr"),
        ("uart_isr_register", "0, fixture_callback, nullptr, 0, nullptr"),
        ("esp_intr_alloc", "0, 0, fixture_callback, nullptr, nullptr"),
        ("esp_intr_alloc_intrstatus",
         "0, 0, 0, 0, fixture_callback, nullptr, nullptr"),
        ("timer_isr_callback_add", "0, 0, fixture_callback, nullptr, 0"),
        ("timer_isr_register", "0, 0, fixture_callback, nullptr, 0, nullptr"),
        ("xTimerCreate", '"timer", 1, 0, nullptr, fixture_callback'),
        ("xTimerCreateStatic", '"timer", 1, 0, nullptr, fixture_callback, &storage'),
        ("xTimerPendFunctionCall", "fixture_callback, nullptr, 0, 0"),
    ):
        fixture = f"void start() {{ {api}({arguments}); }}"
        if callback_inventory(fixture) != {"fixture_callback"}:
            raise AssertionError(f"{api} callback discovery drifted")
    for fixture, label in (
        ("gptimer_event_callbacks_t c{}; c.on_alarm = fixture_callback;",
         "gptimer on_alarm"),
        ("esp_timer_create_args_t c{}; c.callback = fixture_callback;",
         "esp_timer callback"),
    ):
        if callback_inventory(fixture) != {"fixture_callback"}:
            raise AssertionError(f"{label} callback discovery drifted")
    try:
        callback_inventory(
            "void start() { vendor_register_callback(fixture_callback, nullptr); }"
        )
    except AssertionError:
        pass
    else:
        raise AssertionError("unknown callback-registration API mutation passed unexpectedly")

    # A newly registered task/callback must make the exact-inventory gate fail.
    for mutated_tasks, mutated_callbacks in (
        (tasks | {"unreviewed_task_fixture"}, callbacks),
        (tasks, callbacks | {"unreviewed_callback_fixture"}),
    ):
        try:
            check_contract(mutated_tasks, mutated_callbacks)
        except AssertionError:
            pass
        else:
            raise AssertionError("runtime boundary inventory mutation fixture passed unexpectedly")

    # Removing a catch from either a directly registered boundary or a delegated target must also
    # make the semantic half of the gate fail, independently of inventory spelling.
    for name in (next(iter(CONTAINED)), next(iter(DELEGATES_TO_CONTAINED.values()))):
        mutated = function_body(name).replace("catch (...)", "catch (int)")
        try:
            require_catch_all(name, mutated)
        except AssertionError:
            pass
        else:
            raise AssertionError(f"catch-removal mutation fixture passed unexpectedly: {name}")

    contained_name = next(iter(CONTAINED))
    contained_body = function_body(contained_name)
    for injected in ("std::string outside_try;", "throw 1;"):
        mutated = "{" + injected + contained_body[1:]
        try:
            require_catch_all(contained_name, mutated)
        except AssertionError:
            pass
        else:
            raise AssertionError(
                f"{contained_name}: throwing work outside try mutation passed: {injected}"
            )

    adapter, target = next(iter(DELEGATES_TO_CONTAINED.items()))
    adapter_body = function_body(adapter)
    for injected in ("std::string extra;", "throwing_helper();"):
        mutated = "{" + injected + adapter_body[1:]
        try:
            require_thin_delegate(adapter, target, mutated)
        except AssertionError:
            pass
        else:
            raise AssertionError(
                f"{adapter}: additional throwing adapter work mutation passed: {injected}"
            )

    # Registration spelling cannot evade inventory: address-of is normalized, while inline or
    # runtime-selected callbacks are rejected before the exact-count comparison even runs.
    if normalize_registered_callback("&fixture_callback", "fixture") != "fixture_callback":
        raise AssertionError("address-of callback normalization failed")
    ping_fixture = "esp_ping_callbacks_t c{}; c.on_ping_success = &fixture_callback;"
    if callback_inventory(ping_fixture) != {"fixture_callback"}:
        raise AssertionError("esp_ping success/timeout/end callback discovery drifted")
    for expression in ("[](void*) {}", "choose_callback()", "condition ? first : second"):
        try:
            normalize_registered_callback(expression, "mutation fixture")
        except AssertionError:
            pass
        else:
            raise AssertionError(f"dynamic callback mutation passed unexpectedly: {expression}")

    # REVIEWED_NON_THROWING is semantic, not a name allowlist. Each common way to introduce a
    # throwing path must make the mechanical body/call audit red.
    reviewed = "on_mqtt_probe"
    allowed = REVIEWED_ALLOWED_CALLS[reviewed]
    for injected in ("std::string s;", "auto* p = new int;", "throwing_helper();"):
        mutated = function_body(reviewed)[:-1] + injected + "}"
        try:
            require_reviewed_nonthrowing(reviewed, mutated, allowed)
        except AssertionError:
            pass
        else:
            raise AssertionError(f"reviewed-boundary mutation passed unexpectedly: {injected}")

    # The two project callbacks invoked directly by the NimBLE host are a stricter subset: their
    # named adapters and immediate helpers must remain POD/atomic/nonblocking-queue only. Exercise
    # the exact regressions this gate previously missed.
    for reviewed in ("ble_link_event_cb_", "ble_rx_event_cb_"):
        allowed = REVIEWED_ALLOWED_CALLS[reviewed]
        for injected in ("std::string s;", "tk::SemGuard g(m);", "save_str(key, value);"):
            mutated = function_body(reviewed)[:-1] + injected + "}"
            require_mutation_rejected(
                f"{reviewed} host-callback mutation {injected}",
                lambda mutated=mutated, reviewed=reviewed, allowed=allowed:
                    require_reviewed_nonthrowing(reviewed, mutated, allowed),
            )
        for helper, helper_allowed in REVIEWED_DELEGATED_HELPERS[reviewed].items():
            body = function_body(helper)
            for injected in ("std::string s;", "tk::SemGuard g(m);", "save_str(key, value);"):
                mutated = body[:-1] + injected + "}"
                require_mutation_rejected(
                    f"{helper} host-helper mutation {injected}",
                    lambda mutated=mutated, helper=helper, helper_allowed=helper_allowed:
                        require_reviewed_nonthrowing(
                            helper, mutated, helper_allowed, delegated=True
                        ),
                )

    # Catch-all presence is insufficient: a helper before a narrow try or cleanup after its catch
    # can throw through the same C/RTOS frame. Exercise every contained production boundary with an
    # arbitrary call on both sides; the structural outer-try rule must reject spelling-independent
    # pre/post escape paths.
    for name in sorted(CONTAINED | set(DELEGATES_TO_CONTAINED.values())):
        contained_body = function_body(name)
        boundary_mutations = {
            "before outer try": contained_body[:1] + " throwing_helper(); " + contained_body[1:],
            "after outer catch": contained_body[:-1] + " throwing_helper(); " + contained_body[-1:],
        }
        for label, mutated in boundary_mutations.items():
            try:
                require_catch_all(name, mutated)
            except AssertionError:
                pass
            else:
                raise AssertionError(
                    f"{name}: {label} throwing-helper mutation passed unexpectedly"
                )

    # Lifetime and sticky-builder contracts also carry deletion/bypass canaries, so the gate
    # proves it can see the exact regressions it is intended to prevent.
    command = function_body("handle_command")
    for removed in ("body_owner.reset();", "json.reset();"):
        mutated = command.replace(removed, "", 1)
        try:
            require_before("REST lifetime mutation", mutated, removed, "execute_vehicle_command")
        except AssertionError:
            pass
        else:
            raise AssertionError(f"REST lifetime deletion passed unexpectedly: {removed}")

    parser_contracts = {
        "REST command JSON": (
            function_body("handle_command"),
            {
                "Malformed": '"invalid JSON"',
                "TooDeep": '"JSON nesting too deep"',
                "UnsupportedNul": '"JSON NUL escape not supported"',
                "NoMemory": '"out of memory"',
            },
        ),
        "MCP JSON": (
            function_body("mcp_handle_post"),
            {
                "Malformed": "tk::kJsonRpcParseError",
                "TooDeep": "tk::kJsonRpcInvalidRequest",
                "UnsupportedNul": "tk::kJsonRpcInvalidRequest",
                "NoMemory": "send_oom_503_",
            },
        ),
        "configuration JSON": (
            function_body("parse_json_object_body_"),
            {
                "Malformed": "ConfigSubmissionStatus::MalformedJson",
                "TooDeep": "ConfigSubmissionStatus::JsonTooDeep",
                "UnsupportedNul": "ConfigSubmissionStatus::JsonUnsupportedNul",
                "NoMemory": "ConfigSubmissionStatus::JsonNoMemory",
            },
        ),
    }
    for label, (body, consequences) in parser_contracts.items():
        for removed in (
            "tk::json_materialize<cJSON>",
            "JsonMaterializeStatus::TooDeep",
            "JsonMaterializeStatus::UnsupportedNul",
        ):
            mutated = body.replace(removed, "fixture_parser_bypass", 1)
            try:
                require_json_materialize_policy(label, mutated, consequences)
            except AssertionError:
                pass
            else:
                raise AssertionError(
                    f"{label}: parser-policy mutation passed unexpectedly: {removed}"
                )

    for bypass in (
        "cJSON_CreateObject()",
        "cJSON_Print(root)",
        'cJSON_SetValuestring(root, "x")',
        'cJSON_ReplaceItemInObject(root, "k", child)',
        "cJSON_InsertItemInArray(root, 0, child)",
    ):
        raw_fixture = dict(MAIN_CODE)
        raw_fixture["helpers/response_canary.hpp"] = f"void bypass() {{ {bypass}; }}\n"
        try:
            require_cjson_api_allowlist(raw_fixture)
        except AssertionError:
            pass
        else:
            raise AssertionError(
                f"out-of-pattern cJSON API mutation passed unexpectedly: {bypass}"
            )
    # A symbol-set allowlist would miss these same-file duplicates. Exact counters make every
    # additional parser, printer, creator and ownership-transferring add call review-visible.
    for relative, duplicate, label in (
        ("http_api.cpp", "\nvoid duplicate_parse() { cJSON_Parse(nullptr); }\n", "Parse"),
        ("json_http_reply.hpp",
         "\ninline void duplicate_print(cJSON* p) { cJSON_PrintUnformatted(p); }\n", "Print"),
        ("json_builder.hpp",
         "\ninline void duplicate_create() { cJSON_CreateObject(); }\n", "Create"),
        ("json_builder.hpp",
         "\ninline void duplicate_add(cJSON* a, cJSON* b) { cJSON_AddItemToArray(a, b); }\n",
         "Add"),
    ):
        duplicate_fixture = dict(MAIN_CODE)
        duplicate_fixture[relative] += duplicate
        try:
            require_cjson_api_allowlist(duplicate_fixture)
        except AssertionError:
            pass
        else:
            raise AssertionError(f"same-file duplicate cJSON {label} mutation passed unexpectedly")
    removed_print = dict(MAIN_CODE)
    removed_print["json_http_reply.hpp"] = removed_print["json_http_reply.hpp"].replace(
        "cJSON_PrintUnformatted(root.get())", "fixture_print(root.get())", 1
    )
    try:
        require_cjson_api_allowlist(removed_print)
    except AssertionError:
        pass
    else:
        raise AssertionError("shared cJSON print removal mutation passed unexpectedly")

    for label, body in (
        ("REST reply mutation", function_body("send_json")),
        ("MCP reply mutation", function_body("send_rpc_")),
    ):
        mutated = body.replace("tk::json_http_reply", "bypassed_json_reply", 1)
        try:
            require_shared_json_reply(label, mutated)
        except AssertionError:
            pass
        else:
            raise AssertionError(f"{label}: shared-seam bypass mutation passed unexpectedly")

    mcp_post = function_body("mcp_handle_post")
    route_tail = mcp_post[mcp_post.find("if (m == tk::McpMethod::ToolsCall)") :]
    mutated_route = route_tail.replace("msg.reset();", "", 1)
    try:
        require_before(
            "MCP tools/list lifetime mutation",
            mutated_route,
            "msg.reset();",
            "handle_tools_list_",
        )
    except AssertionError:
        pass
    else:
        raise AssertionError("MCP tools/list lifetime deletion passed unexpectedly")

    mcp_call = function_body("handle_tools_call_")
    mutated_call = mcp_call.replace("request.reset();", "", 1)
    try:
        require_mcp_call_releases(mutated_call)
    except AssertionError:
        pass
    else:
        raise AssertionError("MCP tools/call error lifetime deletion passed unexpectedly")

    mcp_source = SOURCES["mcp_server.cpp"]
    payloads = (MAIN / "mcp_json_payloads.hpp").read_text(encoding="utf-8")
    mutated_mcp = mcp_source.replace(
        "tk::mcp_json::build_tools_list_result", "fixture_tools_list_result", 1
    )
    try:
        require_mcp_payload_seams(mutated_mcp, payloads)
    except AssertionError:
        pass
    else:
        raise AssertionError("MCP real tools/list bypass mutation passed unexpectedly")

    mutated_id = payloads.replace("kMaxStringIdBytes = 64", "kMaxStringIdBytes = 2048", 1)
    try:
        require_mcp_payload_seams(mcp_source, mutated_id)
    except AssertionError:
        pass
    else:
        raise AssertionError("MCP unbounded-id mutation passed unexpectedly")

    mutated_null_id = payloads.replace(
        "if (cJSON_IsNull(value)) return RpcIdStatus::Invalid",
        "if (cJSON_IsNull(value)) return RpcIdStatus::Missing",
        1,
    )
    try:
        require_mcp_payload_seams(mcp_source, mutated_null_id)
    except AssertionError:
        pass
    else:
        raise AssertionError("MCP null-id-as-notification mutation passed unexpectedly")

    for removed in (
        "tk::json_top_level_numeric_id",
        "tk::mcp_json::inspect_request_envelope",
        "envelope.status != tk::mcp_json::RpcRequestStatus::Valid",
    ):
        mutated_envelope = mcp_source.replace(removed, "fixture_envelope_bypass", 1)
        try:
            require_mcp_payload_seams(mutated_envelope, payloads)
        except AssertionError:
            pass
        else:
            raise AssertionError(f"MCP envelope-seam mutation passed unexpectedly: {removed}")
    for removed in (
        "raw_number.status != JsonRawNumberStatus::ValidInteger",
        "materialized != raw_number.value",
        "has_duplicate_json_keys(object)",
        'std::strcmp(version->valuestring, "2.0") != 0',
    ):
        mutated_payload = payloads.replace(removed, "fixture_envelope_bypass", 1)
        try:
            require_mcp_payload_seams(mcp_source, mutated_payload)
        except AssertionError:
            pass
        else:
            raise AssertionError(f"MCP envelope-policy mutation passed unexpectedly: {removed}")

    mutated_nul_gate = mcp_source.replace(
        "JsonMaterializeStatus::UnsupportedNul", "JsonMaterializeStatus::Malformed", 1
    )
    try:
        require_mcp_payload_seams(mutated_nul_gate, payloads)
    except AssertionError:
        pass
    else:
        raise AssertionError("MCP embedded-NUL rejection mutation passed unexpectedly")

    status_producer = function_body("build_status_object")
    status_handler = function_body("handle_status")
    for boot, producer in (
        (SOURCES["main.cpp"].replace("tk::cfg_init_wifi_rollback_status", "fixture_missing_init"),
         status_producer),
        (SOURCES["main.cpp"], status_producer.replace("tk::cfg_wifi_rolled_back()", "true")),
        (SOURCES["main.cpp"], status_producer.replace("{", "{ tk::ConfigBlob cb;", 1)),
    ):
        require_mutation_rejected(
            "WiFi rollback status cache wiring",
            lambda boot=boot, producer=producer: require_wifi_rollback_status_cache(boot, producer),
        )
    status_emitter = MAIN_CODE["status_json_emitter.hpp"]
    require_status_production_seams(status_producer, status_handler, status_emitter)
    # Migrate the three old effective emitter/model/release canaries to their actual owner.
    for token in ("StatusJsonEmitter emitter(json);", "status::emit_status(in, emitter);",
                  "return emitter.release();"):
        if status_emitter.count(token) != 1:
            raise AssertionError(f"/status actual helper canary target missing/ambiguous: {token}")
        mutated_emitter = status_emitter.replace(token, "fixture_status_bypass", 1)
        require_mutation_rejected(
            f"/status helper seam removal {token}",
            lambda changed=mutated_emitter: require_status_production_seams(
                status_producer, status_handler, changed),
        )
    snapshot = "in.stack_context_samples = stack_watch_snapshot();"
    builder = "JsonBuilder json;"
    delegate = "return tk::emit_status_object(in);"
    for source, token in ((status_producer, delegate), (status_emitter, snapshot),
                          (status_emitter, "[[gnu::noinline]]")):
        if source.count(token) != 1:
            raise AssertionError(f"/status source canary target missing/ambiguous: {token}")
    status_cases = (
        ("actual helper call", status_producer.replace(delegate, "return nullptr;", 1), status_emitter),
        ("one cached snapshot", status_producer, status_emitter.replace(snapshot, "(void)in;", 1)),
        ("no duplicate snapshot", status_producer, status_emitter.replace(snapshot, snapshot + snapshot, 1)),
        ("snapshot before allocation", status_producer,
         status_emitter.replace(snapshot, "", 1).replace(builder, builder + snapshot, 1)),
        ("separate emitter frame", status_producer, status_emitter.replace("[[gnu::noinline]]", "", 1)),
    )
    for label, producer, emitter in status_cases:
        require_mutation_rejected(
            "/status " + label,
            lambda producer=producer, emitter=emitter: require_status_production_seams(
                producer, status_handler, emitter),
        )
    require_status_production_seams(status_producer, status_handler, status_emitter)

    vehicle_data_handler = function_body("handle_vehicle_data")
    for token in VEHICLE_DATA_SEAMS:
        mutated_vd = vehicle_data_handler.replace(token, "fixture_vehicle_data_bypass")
        try:
            require_vehicle_data_production_seams(mutated_vd)
        except AssertionError:
            pass
        else:
            raise AssertionError(f"/vehicle_data seam-removal mutation passed unexpectedly: {token}")
    climate_helper = function_body("emit_cached_vehicle_climate")
    for token in CLIMATE_CACHE_SEAMS:
        try:
            require_vehicle_climate_cache_seam(climate_helper.replace(token, "fixture_climate_bypass"))
        except AssertionError:
            pass
        else:
            raise AssertionError(f"climate cache seam-removal mutation passed unexpectedly: {token}")
    drive_helper = function_body("emit_cached_vehicle_drive")
    for token in DRIVE_CACHE_SEAMS:
        try:
            require_vehicle_drive_cache_seam(drive_helper.replace(token, "fixture_drive_bypass"))
        except AssertionError:
            pass
        else:
            raise AssertionError(f"drive cache seam-removal mutation passed unexpectedly: {token}")
    for injected in ("get_charge_state(cs);", "charge_state_poll();", "WakePolicy p;"):
        require_mutation_rejected(
            f"drive cache helper vehicle-active read {injected!r}",
            lambda injected=injected: require_vehicle_drive_cache_seam(
                drive_helper.replace("{", "{ " + injected, 1)),
        )
    getter_source = (MAIN / "vehicle_ctrl.hpp").read_text()
    drive_getter = function_body_in(getter_source, "get_cached_vehicle_drive")
    for token in DRIVE_GETTER_SEAMS:
        try:
            require_vehicle_drive_getter_seam(
                getter_source.replace(drive_getter, drive_getter.replace(token, "fixture_getter_bypass", 1), 1))
        except AssertionError:
            pass
        else:
            raise AssertionError(f"fixed drive getter mutation passed unexpectedly: {token}")
    for injected in ('ESP_LOGW(TAG, "x");', "tk::SemGuard v(vehicle_mutex_);", "std::string s;",
                     'ble_->connect("");', "client_->x();"):
        require_mutation_rejected(
            f"fixed drive getter non-cache operation {injected!r}",
            lambda injected=injected: require_vehicle_drive_getter_seam(
                getter_source.replace(drive_getter, drive_getter.replace("{", "{ " + injected, 1), 1)),
        )

    climate_telemetry = SOURCES["vehicle_telemetry.cpp"]
    climate_getter = function_body_in(climate_telemetry, "get_vehicle_climate")
    for token in CLIMATE_GETTER_SEAMS:
        mutated_getter = climate_getter.replace(token, "fixture_getter_bypass")
        if mutated_getter == climate_getter:
            raise AssertionError(f"climate getter mutation did not apply: {token}")
        try:
            require_vehicle_climate_getter_seam(climate_telemetry.replace(climate_getter, mutated_getter, 1))
        except AssertionError:
            pass
        else:
            raise AssertionError(f"climate getter seam-removal mutation passed unexpectedly: {token}")
    for injected, _ in CLIMATE_GETTER_FORBIDDEN:
        require_mutation_rejected(
            f"climate getter non-cache operation {injected!r}",
            lambda injected=injected: require_vehicle_climate_getter_seam(
                climate_telemetry.replace(climate_getter, climate_getter.replace("{", "{ " + injected, 1), 1)),
        )
    no_in_place_charging = climate_getter.replace(
        "last_known_charge_.charging_state", "charging_state_copy")
    require_mutation_rejected(
        "climate getter charging_state comparison removed",
        lambda: require_vehicle_climate_getter_seam(
            climate_telemetry.replace(climate_getter, no_in_place_charging, 1)),
    )
    unlock_marker = "tk::SemGuard g(cache_mutex_);"
    early_decision = climate_getter.replace(
        unlock_marker, unlock_marker + " const bool early = tk::climate_cache_usable(true, true, true, 0);", 1)
    require_mutation_rejected(
        "climate getter freshness decision under cache_mutex_",
        lambda: require_vehicle_climate_getter_seam(climate_telemetry.replace(climate_getter, early_decision, 1)),
    )
    # Semantic mutations of the freshness verdict: each keeps the getter compiling and cache-only
    # but would let a dead feedback path pose as live climate data.
    def climate_getter_mutated(old: str, new: str) -> str:
        if old not in climate_getter:
            raise AssertionError(f"climate getter semantic mutation did not apply: {old}")
        return climate_telemetry.replace(climate_getter, climate_getter.replace(old, new, 1), 1)

    for label, old, new in (
        ("window forced closed",
         "active_window_now_(now, charging)", "false"),
        ("charging arm dropped from the window",
         "active_window_now_(now, charging)", "active_window_now_(now, false)"),
        ("have_age forced true",
         "have_age = generation != 0;", "have_age = true;"),
        ("age forced to zero",
         "(now - sample) / configTICK_RATE_HZ", "0"),
        ("verdict bypassed",
         "return tk::climate_cache_usable(snapshot.valid, active_window, have_age, age_s);",
         "return snapshot.valid;"),
        ("lock failure returns true",
         "out = {};\n            return false;", "out = {};\n            return true;"),
        ("lock failure keeps the stale output",
         "out = {};\n            return false;", "return false;"),
    ):
        require_mutation_rejected(
            f"climate getter {label}",
            lambda old=old, new=new: require_vehicle_climate_getter_seam(
                climate_getter_mutated(old, new)),
        )

    charge_getter = function_body_in(climate_telemetry, "get_charge_state")
    for token in CHARGE_GETTER_SEAMS + (CHARGE_GETTER_CHARGING_ARM,):
        # Pins are whitespace-normalized; remove the first occurrence however the source wraps it.
        pattern = r"\s+".join(re.escape(part) for part in token.split(" "))
        mutated_getter, applied = re.subn(pattern, "fixture_getter_bypass", charge_getter, count=1)
        if applied != 1:
            raise AssertionError(f"charge getter mutation did not apply: {token}")
        require_mutation_rejected(
            f"charge getter seam removed {token!r}",
            lambda mutated_getter=mutated_getter: require_charge_state_getter_seam(
                climate_telemetry.replace(charge_getter, mutated_getter, 1)),
        )
    charge_lock = "tk::MutexGuard g(cache_mutex_);"
    require_mutation_rejected(
        "charge getter freshness decision under cache_mutex_",
        lambda: require_charge_state_getter_seam(climate_telemetry.replace(charge_getter, charge_getter.replace(
            charge_lock, charge_lock + " const bool early = tk::charge_cache_usable(true, true, true, 0);", 1), 1)),
    )

    # Semantic mutations of the freshness verdict: each keeps the getter compiling and cache-only
    # but would let a dead feedback path pose as a live ChargeState inside the active window.
    def charge_getter_mutated(old: str, new: str) -> str:
        if old not in charge_getter:
            raise AssertionError(f"charge getter semantic mutation did not apply: {old}")
        return climate_telemetry.replace(charge_getter, charge_getter.replace(old, new, 1), 1)

    for label, old, new in (
        ("charging arm dropped from the window",
         "active_window_now_(now, charging)", "active_window_now_(now, false)"),
        ("window forced closed",
         "active_window_now_(now, charging)", "false"),
        ("Starting dropped from the charging arm",
         ' ||\n                          cached.charging_state == "Starting"', ""),
        ("have_sample_age forced true",
         "have_sample_age = generation != 0;", "have_sample_age = true;"),
        ("age forced to zero",
         "? (now - sample) / configTICK_RATE_HZ", "? 0"),
        ("verdict bypassed",
         "if (!tk::charge_cache_usable(", "if (false && !tk::charge_cache_usable("),
        ("stale verdict still serves the cache",
         "        return false;\n    }\n\n    out = std::move(cached);",
         "        return true;\n    }\n\n    out = std::move(cached);"),
        ("stale branch falls through to serve the cache",
         "        return false;\n    }\n\n    out = std::move(cached);",
         "    }\n\n    out = std::move(cached);"),
        ("tick read from the sample stamp",
         "uint32_t now = xTaskGetTickCount();", "uint32_t now = sample;"),
        ("age reset before the verdict",
         "    if (!tk::charge_cache_usable(", "    sample_age_s = 0;\n    if (!tk::charge_cache_usable("),
        ("have-sample bit forced after it was derived",
         "    if (!tk::charge_cache_usable(", "    have_sample_age = true;\n    if (!tk::charge_cache_usable("),
        ("charging window bypasses the verdict",
         "    if (!tk::charge_cache_usable(",
         "    if (charging) { out = std::move(cached); return true; }\n    if (!tk::charge_cache_usable("),
        ("invalid cache reported as success",
         "if (!cached.valid) return false;", "if (!cached.valid) return true;"),
    ):
        require_mutation_rejected(
            f"charge getter {label}",
            lambda old=old, new=new: require_charge_state_getter_seam(
                charge_getter_mutated(old, new)),
        )
    # Move (not delete) the snapshot copy behind the lock scope, so every pinned token survives.
    snapshot_line = "        cached = last_known_charge_;\n"
    validity_line = "    if (!cached.valid) return false;"
    if snapshot_line not in charge_getter or validity_line not in charge_getter:
        raise AssertionError("charge getter snapshot-move mutation did not apply")
    moved_snapshot = charge_getter.replace(snapshot_line, "", 1).replace(
        validity_line, "    cached = last_known_charge_;\n" + validity_line, 1)
    require_mutation_rejected(
        "charge getter snapshot copied after the unlock",
        lambda: require_charge_state_getter_seam(climate_telemetry.replace(charge_getter, moved_snapshot, 1)),
    )
    for injected, _ in CHARGE_GETTER_FORBIDDEN:
        require_mutation_rejected(
            f"charge getter non-cache operation {injected!r}",
            lambda injected=injected: require_charge_state_getter_seam(
                climate_telemetry.replace(charge_getter, charge_getter.replace("{", "{ " + injected, 1), 1)),
        )

    window_body = function_body_in(climate_telemetry, "active_window_now_")
    for token in ACTIVE_WINDOW_SEAMS:
        pattern = r"\s+".join(re.escape(part) for part in token.split(" "))
        mutated_window, applied = re.subn(pattern, "fixture_window_bypass", window_body, count=1)
        if applied != 1:
            raise AssertionError(f"active window mutation did not apply: {token}")
        require_mutation_rejected(
            f"active window seam removed {token!r}",
            lambda mutated_window=mutated_window: require_active_window_seam(
                climate_telemetry.replace(window_body, mutated_window, 1)),
        )
    for label, old, new in (
        ("forced closed", "    const uint32_t lc", "    return false;\n    const uint32_t lc"),
        ("charging arm dropped", "{recent_cmd, charging_state,", "{recent_cmd, false,"),
        ("command arm dropped", "{recent_cmd, charging_state,", "{false, charging_state,"),
    ):
        if old not in window_body:
            raise AssertionError(f"active window semantic mutation did not apply: {old}")
        require_mutation_rejected(
            f"active window {label}",
            lambda old=old, new=new: require_active_window_seam(
                climate_telemetry.replace(window_body, window_body.replace(old, new, 1), 1)),
        )

    stamp_lines = ("                last_climate_ticks_.store(xTaskGetTickCount());\n"
                   "                climate_state_generation_.fetch_add(1);\n")
    if stamp_lines not in climate_telemetry:
        raise AssertionError("climate publication stamp lines not found for mutation")
    for label, mutated in (
        ("tick stamp removed", climate_telemetry.replace(
            "                last_climate_ticks_.store(xTaskGetTickCount());\n", "", 1)),
        ("generation bump removed", climate_telemetry.replace(
            "                climate_state_generation_.fetch_add(1);\n", "", 1)),
        ("stamps moved outside the guarded epoch branch", climate_telemetry.replace(stamp_lines, "", 1).replace(
            "    if (pending & PendingDrive) {", stamp_lines + "    if (pending & PendingDrive) {", 1)),
    ):
        if mutated == climate_telemetry:
            raise AssertionError(f"climate publication mutation did not apply: {label}")
        require_mutation_rejected(
            f"climate publication {label}",
            lambda mutated=mutated: require_climate_publication_stamp(mutated),
        )

    wake_loop = function_body_in(climate_telemetry, "loop_task_fn_")
    wake_guard = "if (tk::wake_poll_refreshes_drive(poll_cadence)) {"
    ungated_drive = wake_loop.replace(wake_guard, "if (true) {", 1)
    no_drive_enqueue = wake_loop.replace(
        '"Drive State Poll", tk::BleDomain::Infotainment, build_drive_state_poll);\n            }\n        }\n\n        // Background telemetry',
        '"Charge State Poll", tk::BleDomain::Infotainment, build_drive_state_poll);\n            }\n        }\n\n        // Background telemetry', 1)
    drive_outside_gate = wake_loop.replace(
        "            if (poll_cadence) last_poll_ticks = now_ticks;\n",
        "            if (poll_cadence) last_poll_ticks = now_ticks;\n"
        '            self->enqueue_background_poll_("Drive State Poll", tk::BleDomain::Infotainment, build_drive_state_poll);\n', 1)
    no_edge_reset = wake_loop.replace("            telemetry_poll.reset();\n", "", 1)
    rotation = function_body_in(climate_telemetry, "enqueue_telemetry_round_")
    lambda_rotation = rotation.replace(
        '"Drive State Poll", tk::BleDomain::Infotainment, build_drive_state_poll)',
        '"Drive State Poll", tk::BleDomain::Infotainment, [](TeslaBLE::Client*, uint8_t*, size_t*) { return 0; })', 1)
    for label, mutated, rotated in (
        ("wake gate removed", ungated_drive, rotation),
        ("drive enqueue replaced", no_drive_enqueue, rotation),
        ("drive enqueue outside the gate", drive_outside_gate, rotation),
        ("rising-edge telemetry reset removed", no_edge_reset, rotation),
        ("rotation stops sharing the builder", wake_loop, lambda_rotation),
    ):
        if mutated == wake_loop and rotated == rotation:
            raise AssertionError(f"wake companion mutation did not apply: {label}")
        require_mutation_rejected(
            f"wake companion {label}",
            lambda mutated=mutated, rotated=rotated: require_wake_companion_drive_poll(mutated, rotated),
        )
    require_mutation_rejected(
        "drive poll builder no longer requests DriveState",
        lambda: require_drive_poll_builder(climate_telemetry.replace(
            "CarServer_GetVehicleData_getDriveState_tag);\n}", "CarServer_GetVehicleData_getChargeState_tag);\n}", 1)),
    )

    for label, old, new in (
        ("window gate removed", "telemetry_poll.due({paired, window,", "telemetry_poll.due({paired, true,"),
        ("priority ignored", "telemetry_poll.next(climate_usable)", "telemetry_poll.next(true)"),
        ("attempt throttle removed", "telemetry_poll.begin_attempt(now_ticks);", ""),
        ("round call removed", "self->enqueue_telemetry_round_(telemetry_poll);", ""),
        ("round may inline", "[[gnu::noinline]] void VehicleController::enqueue_telemetry_round_", "void VehicleController::enqueue_telemetry_round_"),
        ("queue refusal ignored", "note_admission(climate_usable, enqueued)",
         "note_admission(climate_usable, true)"),
        ("climate admission lost", 'enqueued = self->enqueue_background_poll_(',
         'self->enqueue_background_poll_('),
        ("background wake enabled", "tk::WakePolicy::NoWakeSkip, 10000", "tk::WakePolicy::AutoWake, 10000"),
        ("wrong climate selector", "CarServer_GetVehicleData_getClimateState_tag",
         "CarServer_GetVehicleData_getChargeState_tag"),
        ("wrong tires selector", "CarServer_GetVehicleData_getTirePressureState_tag",
         "CarServer_GetVehicleData_getChargeState_tag"),
        ("wrong closures selector", "CarServer_GetVehicleData_getClosuresState_tag",
         "CarServer_GetVehicleData_getChargeState_tag"),
    ):
        mutated = climate_telemetry.replace(old, new, 1)
        if mutated == climate_telemetry:
            raise AssertionError(f"telemetry schedule mutation did not apply: {label}")
        require_mutation_rejected(
            f"telemetry schedule {label}",
            lambda mutated=mutated: require_telemetry_poll_schedule(mutated),
        )

    heap = SOURCES["heap_trend.cpp"]
    telemetry = SOURCES["vehicle_telemetry.cpp"]
    loop = function_body_in(telemetry, "loop_task_fn_")
    locked_loop_log = loop.replace("tk::SemGuard g(self->vehicle_mutex_);",
                                  'tk::SemGuard g(self->vehicle_mutex_); ESP_LOGW(TAG, "canary");', 1)
    assert locked_loop_log != loop
    for source, replacement, is_heap in (
        (heap, heap.replace("SemGuard g(m);", 'SemGuard g(m); ESP_LOGW(TAG, "canary");', 1), True),
        (heap, heap.replace("if (s_ready) return Adoption::None;",
           'ESP_LOGW(TAG, "canary"); if (s_ready) return Adoption::None;', 1), True),
        (telemetry, telemetry.replace("tk::SemGuard g(self->vehicle_mutex_);",
           'tk::SemGuard g(self->vehicle_mutex_); ESP_LOGW(TAG, "canary");', 1), False),
        (telemetry, telemetry.replace(loop, locked_loop_log, 1), False),
    ):
        assert source != replacement
        try:
            require_unlocked_diagnostic_logging(replacement if is_heap else heap,
                                                 telemetry if is_heap else replacement)
        except AssertionError:
            pass
        else:
            raise AssertionError("logging-under-mutex mutation passed unexpectedly")

    route_dispatch = function_body("handle_all_dispatch")
    route_mutations = (
        route_dispatch.replace("tk::classify_http_route", "fixture_route_classifier", 1),
        route_dispatch.replace("return handle_ota_update(rq);", "return handle_status(rq);", 1),
        route_dispatch.replace("case tk::HttpRoute::SetWifi:", "case tk::HttpRoute::Fixture:", 1),
        route_dispatch.replace(
            "case tk::HttpRoute::NotFound:       break;",
            "case tk::HttpRoute::Fixture:        return handle_status(rq);\n"
            "        case tk::HttpRoute::NotFound:       break;",
            1,
        ),
    )
    for mutated_route in route_mutations:
        try:
            require_http_route_dispatch(mutated_route)
        except AssertionError:
            pass
        else:
            raise AssertionError("HTTP route mutation passed unexpectedly")
    mutated_path_helper = function_body("uri_path").replace(
        "tk::http_path_only", "fixture_raw_uri", 1
    )
    try:
        require_http_path_seam(mutated_path_helper, route_dispatch)
    except AssertionError:
        pass
    else:
        raise AssertionError("HTTP query-strip seam mutation passed unexpectedly")

    mqtt_source = SOURCES["mqtt_ha.cpp"]
    for token in (
        "tk::mqtt_publish_json",
        "tk::mqtt_run_discovery_round",
        "tk::mqtt_run_state_round",
        "tk::mqtt::build_discovery_payload",
        "tk::mqtt::build_charge_payload",
        "tk::mqtt::build_climate_payload",
        "tk::mqtt::build_drive_payload",
        "tk::mqtt::build_tires_payload",
        "tk::mqtt::build_closures_payload",
        "tk::mqtt::build_vehicle_payload",
        "tk::mqtt::build_empty_payload",
        "tk::mqtt::build_device_payload",
    ):
        mutated_mqtt = mqtt_source.replace(token, "fixture_mqtt_bypass", 1)
        try:
            require_mqtt_production_seams(mutated_mqtt)
        except AssertionError:
            pass
        else:
            raise AssertionError(f"MQTT seam-removal mutation passed unexpectedly: {token}")

    mqtt_payloads = (MAIN / "mqtt_payloads.hpp").read_text(encoding="utf-8")
    mqtt_tests = (ROOT / "test/test_mqtt_json_publish.cpp").read_text(encoding="utf-8")

    for label, changed in (
        ("missing discovery document admission", mqtt_source.replace(
            "if (!document) return false;", "if (false) return false;", 1)),
        ("empty discovery document handoff", mqtt_source.replace(
            "pub_json(config_topic, std::move(document))", "pub_json(config_topic, {})", 1)),
    ):
        if changed == mqtt_source:
            raise AssertionError(label + " mutation did not apply")
        require_mutation_rejected(label, lambda changed=changed:
            require_mqtt_factory_inventory(mqtt_payloads, changed, mqtt_tests))

    # A ninth production definition and call without an OOM/success case must fail closed.
    added_payloads = mqtt_payloads + (
        "\nnamespace tk::mqtt { inline JsonOwner build_fixture_payload() noexcept "
        "{ return {}; } }\n"
    )
    added_production = mqtt_source + (
        "\nstatic void fixture_factory_call() { tk::mqtt::build_fixture_payload(); }\n"
    )
    try:
        require_mqtt_factory_inventory(added_payloads, added_production, mqtt_tests)
    except AssertionError:
        pass
    else:
        raise AssertionError("uncovered ninth MQTT payload factory passed unexpectedly")

    # Also catch an unnamed ninth live path that constructs and finishes a builder directly in
    # mqtt_ha.cpp; factory-name inventory alone would otherwise never see it.
    direct_live_payload = mqtt_source + (
        "\nstatic void fixture_live_payload() { tk::JsonBuilder canary; "
        "pub_json(state_topic(tk::mqtt::StateDomain::Device), canary.finish()); }\n"
    )
    try:
        require_mqtt_factory_inventory(mqtt_payloads, direct_live_payload, mqtt_tests)
    except AssertionError:
        pass
    else:
        raise AssertionError("direct ninth MQTT live payload passed unexpectedly")

    raw_state_publish = mqtt_source + (
        '\nstatic void fixture_raw_state() { pub("tesla/key/state/device", "{}"); }\n'
    )
    try:
        require_mqtt_factory_inventory(mqtt_payloads, raw_state_publish, mqtt_tests)
    except AssertionError:
        pass
    else:
        raise AssertionError("unreviewed raw MQTT state publish passed unexpectedly")

    removed_case = mqtt_tests.replace(
        '    {"build_device_payload", build_device_full},\n', "", 1
    )
    if removed_case == mqtt_tests:
        raise AssertionError("MQTT factory test-removal mutation did not apply")
    try:
        require_mqtt_factory_inventory(mqtt_payloads, mqtt_source, removed_case)
    except AssertionError:
        pass
    else:
        raise AssertionError("removed MQTT OOM/success case passed unexpectedly")

    miswired_case = mqtt_tests.replace(
        '{"build_charge_payload", build_charge_full}',
        '{"build_charge_payload", build_drive_full}',
        1,
    )
    if miswired_case == mqtt_tests:
        raise AssertionError("MQTT factory miswire mutation did not apply")
    try:
        require_mqtt_factory_inventory(mqtt_payloads, mqtt_source, miswired_case)
    except AssertionError:
        pass
    else:
        raise AssertionError("miswired MQTT OOM/success fixture passed unexpectedly")

    removed_call = mqtt_source.replace(
        "tk::mqtt::build_device_payload", "fixture_device_payload", 1
    )
    if removed_call == mqtt_source:
        raise AssertionError("MQTT production-call removal mutation did not apply")
    try:
        require_mqtt_factory_inventory(mqtt_payloads, removed_call, mqtt_tests)
    except AssertionError:
        pass
    else:
        raise AssertionError("removed MQTT production payload call passed unexpectedly")

    late_throw_fixture = function_body("mqtt_probe_broker").replace(
        "resources.owner.client = esp_mqtt_client_init(&cfg);",
        "std::string late_allocation;\nresources.owner.client = esp_mqtt_client_init(&cfg);",
    )
    acquire = late_throw_fixture.find("xSemaphoreCreateBinary")
    if acquire < 0 or not re.search(
        r"\b(?:std::string|new|throw)\b", scrub_cpp(late_throw_fixture[acquire:])
    ):
        raise AssertionError("MQTT post-acquire allocation mutation canary is ineffective")

    mutated_safe_mode = SOURCES["safe_mode.cpp"].replace(
        "boot_fail_parse(raw.c_str(), count)", "true", 1
    )
    try:
        require_safe_mode_source_contract(mutated_safe_mode)
    except AssertionError:
        pass
    else:
        raise AssertionError("safe-mode counter-parser bypass mutation passed unexpectedly")
    for phrase in ("and a confirmed counter reset.",
                   "a clean boot exits only if counter storage is healthy."):
        if SOURCES["safe_mode.cpp"].count(phrase) != 1:
            raise AssertionError("safe-mode operator canary requires one actual source anchor")
        mutated_safe_mode = SOURCES["safe_mode.cpp"].replace(phrase, "reboot to recover", 1)
        mutated_safe_mode += "\n// " + phrase + "\n"
        try:
            require_safe_mode_source_contract(mutated_safe_mode)
        except AssertionError:
            pass
        else:
            raise AssertionError("safe-mode operator storage condition/comment-decoy mutation passed")
    for filename, task, loop, delay in ITERATION_FAULT_TASKS:
        source = SOURCES[filename]
        declaration = "tk::IterationFaultLog fault_log{};"
        clock = "tk::report_iteration_fault(fault_log, static_cast<uint64_t>(esp_timer_get_time()) / 1000,"
        mutations = (
            source.replace(declaration, "", 1),
            source.replace(declaration, "static " + declaration, 1),
            source.replace(declaration, "static\n    " + declaration, 1),
            source.replace(declaration, "thread_local " + declaration, 1),
            source.replace(declaration, "thread_local\n    " + declaration, 1),
            source.replace(declaration, "", 1).replace(loop + " {", loop + " { " + declaration, 1),
            source.replace("tk::report_iteration_fault(", "ignored_iteration_fault(", 1)
            + "\n// tk::report_iteration_fault(\n",
            source.replace(clock, clock.replace("uint64_t", "uint32_t"), 1),
        )
        for mutant in mutations:
            try:
                require_iteration_fault_contract(mutant, task, loop, delay)
            except AssertionError:
                pass
            else:
                raise AssertionError("iteration fault lifetime/dispatch/clock mutation passed")

    provisioning_save = function_body("save_post_impl")
    provisioning_decode = function_body("url_decode")
    http_body_header = (MAIN / "logic/http_body.hpp").read_text(encoding="utf-8")
    for old, replacement, label in (
        ("kSaveBodyMaxBytes = 1024", "kSaveBodyMaxBytes = 2048", "wire limit"),
        (
            "std::array<char, kSaveBodyMaxBytes + 1> body{};",
            "std::array<char, kSaveBodyMaxBytes> body{};",
            "terminator storage",
        ),
        (
            "std::array<char, kSaveBodyMaxBytes + 1> body{};",
            "std::string body(static_cast<size_t>(len) + 1, '\\0');",
            "dynamic body",
        ),
        ("tk::http_body_fits_buffer", "fixture_body_fits_buffer", "length preflight"),
        ("got < 0 || got != len", "got < 0", "short/partial read"),
        (
            "tk::http_body_has_embedded_nul",
            "fixture_accept_embedded_nul",
            "embedded NUL",
        ),
        (
            "const std::string_view body_view(body.data(), static_cast<size_t>(got));",
            "const std::string_view body_view(body.data());",
            "length-aware view",
        ),
    ):
        mutated = provisioning_save.replace(old, replacement, 1)
        if mutated == provisioning_save:
            raise AssertionError(f"provisioning {label} mutation did not apply")
        try:
            require_provisioning_body_contract(mutated, provisioning_decode, http_body_header)
        except AssertionError:
            pass
        else:
            raise AssertionError(f"provisioning {label} mutation passed unexpectedly")

    for old, replacement, label in (
        ("total < capacity", "total <= capacity", "exact-full acceptance"),
        (
            "!http_body_fits_buffer(total, cap)",
            "total > cap",
            "shared-reader bound bypass",
        ),
        ("if (data[i] == '\\0') return true;", "if (false) return true;", "NUL scan"),
    ):
        mutated_header = http_body_header.replace(old, replacement, 1)
        if mutated_header == http_body_header:
            raise AssertionError(f"provisioning {label} header mutation did not apply")
        try:
            require_provisioning_body_contract(
                provisioning_save, provisioning_decode, mutated_header
            )
        except AssertionError:
            pass
        else:
            raise AssertionError(f"provisioning {label} mutation passed unexpectedly")

    for old, replacement, label in (
        ("if (i + 2 >= in.size()) return false;", "", "truncated percent escape"),
        ("if (high < 0 || low < 0) return false;", "", "invalid percent escape"),
        ("if (decoded == '\\0') return false;", "", "percent-decoded NUL"),
    ):
        mutated_decode = provisioning_decode.replace(old, replacement, 1)
        if mutated_decode == provisioning_decode:
            raise AssertionError(f"provisioning {label} decoder mutation did not apply")
        try:
            require_provisioning_body_contract(
                provisioning_save, mutated_decode, http_body_header
            )
        except AssertionError:
            pass
        else:
            raise AssertionError(f"provisioning {label} mutation passed unexpectedly")

    startup = SOURCES["main.cpp"]
    checked_start = ('if (!tk::heap_supervisor_start(config_store, boot_reboot_record)) {\n'
                     '        boot_fatal("heap supervisor");\n    }')
    if startup.count(checked_start) != 1:
        raise AssertionError("essential Heap startup canary seam is missing/ambiguous")
    for label, mutated in (
        ("missing essential Heap startup", startup.replace(checked_start, "", 1)),
        ("disabled essential Heap startup", startup.replace(checked_start, "if (false) {\n" + checked_start + "\n}", 1)),
        ("unbraced disabled Heap startup", startup.replace(checked_start, "if (false) " + checked_start, 1)),
        ("false conjunction Heap startup", startup.replace("if (!tk::heap_supervisor_start", "if (false && !tk::heap_supervisor_start", 1)),
        ("unchecked essential Heap startup", startup.replace(checked_start, "tk::heap_supervisor_start(config_store, boot_reboot_record);", 1)),
        ("duplicate essential Heap startup", startup.replace(checked_start, checked_start + "\n" + checked_start, 1)),
        ("wrong essential Heap reason", startup.replace("heap_supervisor_start(config_store, boot_reboot_record)", "heap_supervisor_start(config_store, {})", 1)),
        ("late essential Heap startup", startup.replace(checked_start, "", 1).replace("const bool have_wire", checked_start + "\nconst bool have_wire", 1)),
        ("nonfatal essential Heap startup", startup.replace('boot_fatal("heap supervisor");', 'ESP_LOGE(TAG, "heap supervisor");', 1)),
    ):
        if mutated == startup:
            raise AssertionError(label + ": mutation did not apply")
        require_mutation_rejected(label,
            lambda changed=mutated: require_heap_supervisor_start_contract(changed))
    require_heap_supervisor_start_contract(startup)

    supervisor = SOURCES["heap_supervisor.cpp"]
    for old, replacement, label in (
        ("if (!s.budget.may_restart())", "if (false)", "restart cap bypass"),
        ("if (!s.budget.may_restart())", "if (s.budget.may_restart())", "restart cap inversion"),
        ("boot_reason.heap_restarts, boot_reason.state", "0, boot_reason.state", "constant restart seed"),
        ("s.budget.effective + 1", "0 + 1", "overwritten restart count"),
        ("if (!s.budget.may_restart())", "if (false) /* if (!s.budget.may_restart()) */", "comment cap decoy"),
        ("attempt_heap_restart(s, now_ms, verdict);", "if (false) attempt_heap_restart(s, now_ms, verdict);", "dead nested restart"),
        ("if (!excused) ota_request_heap_abort();", "if (false) ota_request_heap_abort();", "expired grace never aborts writer"),
        ("if (writer_busy) {", "if (false) {", "busy writer restart authority stolen"),
        ("if (!owner.held())", "if (false)", "operation admission"),
        ("OtaFaultRestartGuard owner;", "OtaFaultRestartGuard owner; owner.seal();", "precommit retained lease"),
        ("if (!persist_reboot_reason(s.cfg, why.text))", "if (false)", "persistence authority"),
    ):
        if supervisor.count(old) != 1:
            raise AssertionError("heap restart mutation missing/ambiguous: " + label)
        changed = supervisor.replace(old, replacement, 1)
        require_mutation_rejected(label, lambda changed=changed: require_heap_restart_gate_contract(changed))

    diag_handler = function_body("handle_diag")
    redactor_header = (MAIN / "logic/redact.hpp").read_text(encoding="utf-8")
    diag_source = SOURCES["diag_log.cpp"]
    runtime_tests = (ROOT / "test/test_runtime_boundaries.cpp").read_text(encoding="utf-8")
    allocating_diag = diag_handler.replace(
        "const tk::FixedDiagRedaction result =",
        "std::string post_stream_allocation;\n        const tk::FixedDiagRedaction result =",
        1,
    )
    if allocating_diag == diag_handler:
        raise AssertionError("/diag allocation mutation did not apply")
    try:
        require_diag_redaction_stream_contract(
            allocating_diag, redactor_header, diag_source, runtime_tests
        )
    except AssertionError:
        pass
    else:
        raise AssertionError("/diag post-stream allocation mutation passed unexpectedly")

    fixed_redactor_at = redactor_header.find(
        "inline FixedDiagRedaction redact_diag_line_fixed"
    )
    throwing_redactor = redactor_header[:fixed_redactor_at] + redactor_header[
        fixed_redactor_at:
    ].replace(
        "std::size_t capacity) noexcept {", "std::size_t capacity) {", 1
    )
    if throwing_redactor == redactor_header:
        raise AssertionError("fixed /diag noexcept mutation did not apply")
    try:
        require_diag_redaction_stream_contract(
            diag_handler, throwing_redactor, diag_source, runtime_tests
        )
    except AssertionError:
        pass
    else:
        raise AssertionError("throw-capable fixed /diag redactor mutation passed unexpectedly")

    boundary_bypass = diag_handler.replace(
        ", DiagDumpStart::AfterWrappedLineBoundary", "", 1
    )
    if boundary_bypass == diag_handler:
        raise AssertionError("wrapped /diag boundary-mode mutation did not apply")
    try:
        require_diag_redaction_stream_contract(
            boundary_bypass, redactor_header, diag_source, runtime_tests
        )
    except AssertionError:
        pass
    else:
        raise AssertionError("wrapped /diag boundary-mode bypass passed unexpectedly")

    for before, after in (
        ('if (diag_mqtt_uri_error_(line, len))', 'if (false)'),
        ('if (!s_hook_installed)', 'if (true)'),
        ('return diag_forward_fixed_("%s", tk::kDiagMqttUriErrorLine);', 'return diag_forward_fixed_("%s", line);'),
        ('static std::atomic<SemaphoreHandle_t> s_mtx{nullptr};', 'static SemaphoreHandle_t s_mtx{nullptr};'),
        ('static std::atomic<vprintf_like_t> s_prev{nullptr};', 'static vprintf_like_t s_prev{nullptr};'),
        ('static_assert(CONFIG_LOG_TIMESTAMP_SOURCE_RTOS,', 'static_assert(true,'),
        ('#if defined(CONFIG_LOG_COLORS) && CONFIG_LOG_COLORS', '#if 0'),
        ('static_assert(CONFIG_LOG_VERSION == 1,', 'static_assert(true,'),
        ('tk::diag_library_capture_content_end({data, len})', '0'),
        ('tk::diag_capture_byte(data[i], i, library_end)', 'data[i]'),
    ):
        if diag_source.count(before) != 1:
            raise AssertionError("diagnostic capture canary anchor missing")
        require_mutation_rejected("qualified capture guard removed", lambda: require_diag_redaction_stream_contract(
            diag_handler, redactor_header, diag_source.replace(before, after, 1), runtime_tests))

    mutated_diag_source = diag_source.replace("std::memchr(chunk, '\\n', n)", "nullptr", 1)
    if mutated_diag_source == diag_source:
        raise AssertionError("wrapped /diag boundary-search mutation did not apply")
    try:
        require_diag_redaction_stream_contract(
            diag_handler, redactor_header, mutated_diag_source, runtime_tests
        )
    except AssertionError:
        pass
    else:
        raise AssertionError("wrapped /diag boundary-search bypass passed unexpectedly")

    split_line_bypass = diag_handler.replace(
        "tk::diag_line_step(frame, p[i], kLineMax)",
        "tk::DiagLineStep{tk::DiagLineAction::Append, 0}",
        1,
    )
    if split_line_bypass == diag_handler:
        raise AssertionError("overlong /diag framing mutation did not apply")
    try:
        require_diag_redaction_stream_contract(
            split_line_bypass, redactor_header, diag_source, runtime_tests
        )
    except AssertionError:
        pass
    else:
        raise AssertionError("overlong /diag split-line bypass passed unexpectedly")

    raw_overlong = diag_handler.replace(
        "httpd_resp_send_chunk(req, tk::kRedacted, tk::kRedactedLength)",
        "httpd_resp_send_chunk(req, line, kLineMax)",
        1,
    )
    if raw_overlong == diag_handler:
        raise AssertionError("overlong /diag raw-output mutation did not apply")
    try:
        require_diag_redaction_stream_contract(
            raw_overlong, redactor_header, diag_source, runtime_tests
        )
    except AssertionError:
        pass
    else:
        raise AssertionError("overlong /diag raw-output mutation passed unexpectedly")

    # Production-binding mutation canaries: each injected bypass must turn the same static gate
    # red. The executable Ping matrix above separately proves the asynchronous state machine.
    vehicle_source = SOURCES["vehicle_ctrl.cpp"]
    telemetry_source = SOURCES["vehicle_telemetry.cpp"]
    pairing_source = SOURCES["vehicle_pairing.cpp"]
    for label, mutated_vehicle, mutated_telemetry, mutated_pairing in (
        (
            "vehicle barrier begin bypass",
            vehicle_source.replace("task_start_gate_.begin()", "true", 1),
            telemetry_source,
            pairing_source,
        ),
        (
            "vehicle external task delete",
            vehicle_source.replace(
                "task_start_gate_.cancel();",
                "task_start_gate_.cancel();\n        vTaskDelete(loop_task_);",
                1,
            ),
            telemetry_source,
            pairing_source,
        ),
        (
            "vehicle cancel acknowledgement bypass",
            vehicle_source.replace(
                "task_start_gate_.cancelled_tasks_acknowledged(1)",
                "fixture_cancelled_without_ack(1)",
            ),
            telemetry_source,
            pairing_source,
        ),
        (
            "vehicle loop entry barrier bypass",
            vehicle_source,
            telemetry_source.replace("!self->await_task_start_()", "false", 1),
            pairing_source,
        ),
        (
            "auto-pair entry barrier bypass",
            vehicle_source,
            telemetry_source,
            pairing_source.replace("!self->await_task_start_()", "false", 1),
        ),
        (
            "vehicle TWDT unregister bypass",
            vehicle_source,
            telemetry_source.replace("esp_task_wdt_delete(nullptr)",
                                     "fixture_wdt_leak(nullptr)", 1),
            pairing_source,
        ),
        (
            "vehicle global Ready wait bypass",
            vehicle_source.replace("switch (tk::runtime_admission_action())",
                                   "switch (tk::RuntimeAdmissionAction::Run)", 1),
            telemetry_source,
            pairing_source,
        ),
    ):
        require_mutation_rejected(
            label,
            lambda vehicle=mutated_vehicle, telemetry=mutated_telemetry,
                   pairing=mutated_pairing: require_vehicle_task_start_contract(
                       vehicle, telemetry, pairing
                   ),
        )

    admission_logic = (MAIN / "logic/runtime_admission.hpp").read_text(encoding="utf-8")
    admission_header = (MAIN / "runtime_admission.hpp").read_text(encoding="utf-8")
    admission_source = SOURCES["runtime_admission.cpp"]
    main_source = SOURCES["main.cpp"]
    route_header = (MAIN / "logic/http_route.hpp").read_text(encoding="utf-8")
    route_dispatch = function_body("handle_all_dispatch")
    admission_mutations = (
        (
            "runtime default-open",
            admission_logic.replace("state_{runtime_admission_pack(RuntimeAdmissionState::Booting)}",
                                    "state_{runtime_admission_pack(RuntimeAdmissionState::Ready)}", 1),
            admission_header, admission_source, main_source, route_header, route_dispatch,
        ),
        (
            "late boot-fatal admission close",
            admission_logic, admission_header, admission_source,
            main_source.replace("tk::runtime_admission_mark_fatal(tk::runtime_fatal_component(component));", "", 1),
            route_header, route_dispatch,
        ),
        (
            "boot fatal component moved behind diagnostic",
            admission_logic, admission_header, admission_source,
            main_source.replace("tk::runtime_admission_mark_fatal(tk::runtime_fatal_component(component));", "", 1)
                .replace("    fatal_startup_rollback();", "    tk::runtime_admission_mark_fatal(tk::runtime_fatal_component(component));\n    fatal_startup_rollback();", 1),
            route_header, route_dispatch,
        ),
        (
            "runtime claim accepts Fatal",
            admission_logic.replace("std::uint8_t expected = runtime_admission_pack(RuntimeAdmissionState::Ready);",
                "std::uint8_t expected = runtime_admission_pack(RuntimeAdmissionState::Fatal);", 1),
            admission_header, admission_source, main_source, route_header, route_dispatch,
        ),
        (
            "HTTP Ready check bypass",
            admission_logic, admission_header, admission_source, main_source, route_header,
            route_dispatch.replace("!tk::runtime_admission_vehicle_ready()", "false", 1),
        ),
        (
            "HTTP vehicle-route omission",
            admission_logic, admission_header, admission_source, main_source,
            route_header.replace("case HttpRoute::Scan:", "case HttpRoute::OtaCheck:", 1),
            route_dispatch,
        ),
        (
            "Ready published before task start",
            admission_logic, admission_header, admission_source,
            main_source.replace(
                "if (!tk::runtime_admission_mark_ready())",
                "if (!tk::runtime_admission_mark_ready()) /* fixture */",
                1,
            ).replace("!vehicle.start_tasks()", "!tk::runtime_admission_mark_ready()", 1),
            route_header, route_dispatch,
        ),
    )
    for label, logic, facade_h, facade_cpp, app, routes, dispatch in admission_mutations:
        require_mutation_rejected(
            label,
            lambda logic=logic, facade_h=facade_h, facade_cpp=facade_cpp, app=app,
                   routes=routes, dispatch=dispatch: require_runtime_admission_contract(
                       logic, facade_h, facade_cpp, app, routes, dispatch
                   ),
        )

    nimble_gate = (MAIN / "logic/nimble_start_gate.hpp").read_text(encoding="utf-8")
    ble_header = (MAIN / "ble_client.hpp").read_text(encoding="utf-8")
    ble_source = SOURCES["ble_client.cpp"]
    logic_tests = (ROOT / "test/test_logic.cpp").read_text(encoding="utf-8")
    runtime_tests = (ROOT / "test/test_runtime_boundaries.cpp").read_text(encoding="utf-8")
    for label, gate, header, source, app in (
        (
            "NimBLE late sync resurrection",
            nimble_gate.replace(
                "NimbleStartState expected = NimbleStartState::AwaitingSync;\n"
                "        return state_.compare_exchange_strong(expected, NimbleStartState::Synced",
                "NimbleStartState expected = NimbleStartState::TimedOut;\n"
                "        return state_.compare_exchange_strong(expected, NimbleStartState::Synced",
                1,
            ),
            ble_header, ble_source, main_source,
        ),
        (
            "NimBLE acknowledgement wait bypass",
            nimble_gate, ble_header,
            ble_source.replace("switch (start_gate_.action())",
                               "switch (tk::NimbleStartAction::Ready)", 1),
            main_source,
        ),
        (
            "NimBLE acknowledgement gate-begin bypass",
            nimble_gate, ble_header,
            ble_source.replace("start_gate_.begin()", "true", 1),
            main_source,
        ),
        (
            "NimBLE timeout bound drift",
            nimble_gate, ble_header,
            ble_source.replace("pdMS_TO_TICKS(5000)", "pdMS_TO_TICKS(50000)", 1),
            main_source,
        ),
        (
            "NimBLE timeout failure bypass",
            nimble_gate, ble_header,
            ble_source.replace("start_gate_.mark_timed_out()", "false", 1),
            main_source,
        ),
        (
            "NimBLE on_sync acknowledgement bypass",
            nimble_gate, ble_header,
            ble_source.replace("start_gate_.acknowledge_sync()", "true", 1),
            main_source,
        ),
        (
            "NimBLE boot-fatal bypass",
            nimble_gate, ble_header, ble_source,
            main_source.replace("if (!ble_client.start())", "if (false)", 1),
        ),
    ):
        require_mutation_rejected(
            label,
            lambda gate=gate, header=header, source=source, app=app:
                require_nimble_start_ack_contract(
                    gate, header, source, app, logic_tests, runtime_tests
                ),
        )

    nimble_marker = "if (!safe_mode) {\n        if (!ble_client.start())"
    if main_source.count(nimble_marker) != 1:
        raise AssertionError("NimBLE SafeMode mutation target is ambiguous")
    guard_at = main_source.index(nimble_marker)
    guard_code = scrub_cpp_preserving_layout(main_source)
    _, guard_open, guard_close = if_statement_at(guard_code, guard_at, "NimBLE test guard")
    else_at = skip_space(guard_code, guard_close + 1, len(guard_code))
    if not guard_code.startswith("else", else_at):
        raise AssertionError("NimBLE SafeMode mutation needs the actual else branch")
    else_open = skip_space(guard_code, else_at + 4, len(guard_code))
    chain_end = balanced_end(guard_code, else_open, "{", "}", "NimBLE test else") + 1
    chain = main_source[guard_at:chain_end]
    for label, mutated in (
        ("SafeMode NimBLE condition bypass", main_source.replace(nimble_marker,
            "if (true) {\n        if (!ble_client.start())", 1)),
        ("SafeMode NimBLE condition inversion", main_source.replace(nimble_marker,
            "if (safe_mode) {\n        if (!ble_client.start())", 1)),
        ("SafeMode NimBLE unwrapped start", main_source[:guard_at] +
            main_source[guard_open + 1:guard_close] + main_source[chain_end:]),
        ("SafeMode NimBLE additional outside start", main_source[:guard_at] +
            "ble_client.start();\n" + main_source[guard_at:]),
        ("SafeMode NimBLE dead nested guard", main_source[:guard_at] +
            "if (false) {\n" + chain + "\n}" + main_source[chain_end:]),
        ("SafeMode NimBLE comment-only guard", main_source.replace(nimble_marker,
            "if (true) { /* if (!safe_mode) { ble_client.start(); } */\n        if (!ble_client.start())", 1)),
        ("SafeMode NimBLE fabricated input", main_source.replace(
            "const bool safe_mode = tk::safe_mode_begin(config_store,\n        tk::boot_counter_cause(tk::diag_crash_reset_info().code, boot_reboot_record.state),\n        boot_reboot_record.state, boot_reboot_record.heap_restarts);",
            "const bool safe_mode = false;", 1)),
    ):
        if mutated == main_source:
            raise AssertionError(f"{label}: mutation did not apply")
        require_mutation_rejected(label,
            lambda changed=mutated: require_nimble_safe_mode_start_contract(changed))
    require_nimble_safe_mode_start_contract(main_source)

    ota_source = SOURCES["ota_update.cpp"]
    for label, header, source, app, ota in (
        (
            "NimBLE current-health reset revocation",
            ble_header,
            ble_source.replace("host_synced_ = false;", "host_synced_ = true;", 1),
            main_source, ota_source,
        ),
        (
            "NimBLE reset-counter saturation",
            ble_header,
            ble_source.replace("resets != UINT32_MAX", "true", 1),
            main_source, ota_source,
        ),
        (
            "OTA health reset evidence at verdict",
            ble_header, ble_source,
            main_source.replace("ota_runtime_health().healthy()", "true", 1),
            ota_source,
        ),
        (
            "OTA health reset evidence after owner",
            ble_header, ble_source,
            main_source.replace("const auto commit_health = ota_runtime_health();", "const auto commit_health = tk::RuntimeHealthSample{};", 1),
            ota_source,
        ),
        (
            "explicit OTA reset evidence after owner",
            ble_header, ble_source, main_source,
            ota_source.replace("[] { return ble_host_reset_count(); }", "[] { return 0; }", 1),
        ),
    ):
        require_mutation_rejected(
            label,
            lambda header=header, source=source, app=app, ota=ota:
                require_dynamic_ble_host_health_contract(
                    header, source, app, ota, logic_tests, runtime_tests
                ),
        )

    for old, replacement, label in (
        ("s_operation_gate.state() != owner", "false", "recovery reference actual owner"),
        ("if (!old->coordinator_done.load(std::memory_order_acquire)) return false;", "", "recovery reference done acquire"),
        ("old->coordinator.can_reclaim() && s_network_slot.owns(old->job)", "true", "recovery reference real cleanup and slot"),
        ("return Result::NoMemory;", "return Result::InternalError;", "entry memory typed result"),
        ("tk::ota_recovery_required_bytes(tk::OtaRecoveryUse::OperationEntry)", "tk::kHeapCriticalBytes", "entry recovery role"),
        ("return tk::ota_recovery_required_bytes_for(use);", "return tk::kHeapCriticalBytes;", "real pure recovery selector"),
    ):
        if old not in ota_source: raise AssertionError("missing recovery-reference canary: " + label)
        require_mutation_rejected(label, lambda changed=ota_source.replace(old,replacement,1):
                                  require_operation_wrapper_contract(changed))

    health_task = function_body("ota_health_gate_task")
    for token, replacement, label in (
        ("if (!ota_recovery_reference_ready(commit_guard)) continue;", "", "OTA health owned recovery reference"),
        ("switch (tk::runtime_admission_action())", "switch (tk::RuntimeAdmissionAction::Run)",
         "OTA health initial admission"),
        ("if (!commit_health.healthy()) continue;", "",
         "OTA health post-owner shared-health refusal"),
    ):
        mutated_health = health_task.replace(token, replacement, 1)
        if mutated_health == health_task:
            raise AssertionError(f"{label} mutation did not apply")
        require_mutation_rejected(
            label, lambda body=mutated_health: require_health_commit_contract(body)
        )

    ping_header = (MAIN / "ping_probe.hpp").read_text(encoding="utf-8")
    ping_generation = (MAIN / "logic/ping_probe.hpp").read_text(encoding="utf-8")
    net_source = SOURCES["net.cpp"]
    syslog_source = SOURCES["syslog.cpp"]
    runtime_tests = (ROOT / "test/test_runtime_boundaries.cpp").read_text(encoding="utf-8")
    for label, header, generation, net, syslog in (
        (
            "ping pre-generation quarantine bypass",
            ping_header.replace(
                "if (!ping_probe_cleanup_completed(control)) return PingProbeResult::PendingEnd;",
                "ping_probe_cleanup_completed(control);",
                1,
            ),
            ping_generation, net_source, syslog_source,
        ),
        (
            "ping exact-end-before-delete bypass",
            ping_header.replace(
                "if (!control.generation.ended(control.session_generation)) return false;",
                "if (false) return false;",
                1,
            ),
            ping_generation, net_source, syslog_source,
        ),
        (
            "ping timeout stop bypass",
            ping_header.replace("esp_ping_stop(session);", "fixture_no_stop(session);", 1),
            ping_generation, net_source, syslog_source,
        ),
        (
            "ping stale-generation acceptance",
            ping_header,
            ping_generation.replace("active_.load(std::memory_order_acquire) != generation",
                                    "false", 1),
            net_source, syslog_source,
        ),
        (
            "network production ping delegation",
            ping_header, ping_generation,
            net_source.replace("ping_probe_run(", "fixture_ping_probe_run(", 1),
            syslog_source,
        ),
        (
            "ping failed-start delete retry bypass",
            ping_header.replace(
                "if (esp_ping_delete_session(control.session) != ESP_OK) return false;",
                "esp_ping_delete_session(control.session);",
                1,
            ),
            ping_generation, net_source, syslog_source,
        ),
        (
            "ping failed-start retention inversion",
            ping_header.replace(
                "if (esp_ping_delete_session(session) == ESP_OK)",
                "if (esp_ping_delete_session(session) != ESP_OK)",
                1,
            ),
            ping_generation, net_source, syslog_source,
        ),
        (
            "ping profile failure fabricated as no-reply",
            ping_header.replace(
                "if (!measurement_valid) return PingProbeResult::Unavailable;",
                "if (!measurement_valid) return PingProbeResult::NoReply;",
                1,
            ),
            ping_generation, net_source, syslog_source,
        ),
        (
            "gateway unknown treated as failure",
            ping_header, ping_generation,
            net_source.replace(
                "out.reachable = result != PingProbeResult::NoReply;",
                "out.reachable = result == PingProbeResult::Reply;",
                1,
            ),
            syslog_source,
        ),
        (
            "gateway reply attributed without same-lease check",
            ping_header, ping_generation,
            net_source.replace(
                "current_ip.gw.addr != out.identity.address) return out;",
                "current_ip.gw.addr == out.identity.address) return out;",
                1,
            ),
            syslog_source,
        ),
        (
            "gateway probe not bound to the sampled netif",
            ping_header, ping_generation,
            net_source.replace(
                "cfg.interface   = static_cast<uint32_t>(esp_netif_get_netif_impl_index(netif));",
                "",
                1,
            ),
            syslog_source,
        ),
        (
            "gateway verdict kept after a transport change",
            ping_header, ping_generation,
            net_source.replace(
                "    if (s_kind.load() != kind || net_active_netif() != netif ||\n        lease_generation(kind) != out.identity.lease_generation) return out;\n    esp_netif_ip_info_t current_ip{};",
                "    esp_netif_ip_info_t current_ip{};",
                1,
            ),
            syslog_source,
        ),
        (
            "gateway verdict kept after a same-IP lease change",
            ping_header, ping_generation,
            net_source.replace(
                "    if (s_kind.load() != kind || net_active_netif() != netif ||\n        lease_generation(kind) != out.identity.lease_generation) return out;",
                "    if (s_kind.load() != kind || net_active_netif() != netif) return out;",
                1,
            ),
            syslog_source,
        ),
        (
            "syslog production ping delegation",
            ping_header, ping_generation, net_source,
            syslog_source.replace("tk::ping_probe_run(", "fixture_ping_probe_run(", 1),
        ),
    ):
        require_mutation_rejected(
            label,
            lambda header=header, generation=generation, net=net, syslog=syslog:
                require_ping_probe_contract(
                    header, generation, net, syslog, runtime_tests
                ),
        )

    syslog_gate = (MAIN / "logic/syslog_start_gate.hpp").read_text(encoding="utf-8")
    logic_tests = (ROOT / "test/test_logic.cpp").read_text(encoding="utf-8")
    for label, source, gate, logic, runtime in (
        (
            "Syslog sender publication memory-order downgrade",
            syslog_source.replace(
                "s_queue.store(queue, std::memory_order_release);",
                "s_queue.store(queue, std::memory_order_relaxed);",
                1,
            ),
            syslog_gate, logic_tests, runtime_tests,
        ),
        (
            "Syslog hook acquisition memory-order downgrade",
            syslog_source.replace(
                "s_queue.load(std::memory_order_acquire)",
                "s_queue.load(std::memory_order_relaxed)",
                1,
            ),
            syslog_gate, logic_tests, runtime_tests,
        ),
        (
            "Syslog unpublished queue deletion bypass",
            syslog_source.replace("if (queue) vQueueDelete(queue);", "", 1),
            syslog_gate, logic_tests, runtime_tests,
        ),
        (
            "Syslog start gate commits cancellation",
            syslog_source,
            syslog_gate.replace(
                "compare_exchange_strong(expected, SyslogStartState::Running,",
                "compare_exchange_strong(expected, SyslogStartState::Cancelled,",
                1,
            ),
            logic_tests, runtime_tests,
        ),
        (
            "Syslog executable publication matrix removal",
            syslog_source, syslog_gate, logic_tests,
            runtime_tests.replace(
                "test_syslog_queue_publication_lifetime",
                "fixture_syslog_queue_publication_lifetime",
                1,
            ),
        ),
    ):
        require_mutation_rejected(
            label,
            lambda source=source, gate=gate, logic=logic, runtime=runtime:
                require_syslog_start_lifetime_contract(source, gate, logic, runtime),
        )

    for label, source, runtime in (
        (
            "Ethernet driver-uninstall cleanup bypass",
            net_source.replace("esp_eth_driver_uninstall(r.handle)",
                               "fixture_driver_uninstall(r.handle)", 1),
            runtime_tests,
        ),
        (
            "Ethernet activated graph quarantine bypass",
            net_source.replace("if (r.start_attempted) {", "if (false && r.start_attempted) {", 1),
            runtime_tests,
        ),
        (
            "Ethernet live-driver fallback teardown",
            net_source.replace(
                "err = esp_eth_start(r.handle);",
                "err = esp_eth_start(r.handle);\n"
                "    if (false) return eth_startup_fallback(r);",
                1,
            ),
            runtime_tests,
        ),
        (
            "Ethernet retired mirrored fixture restored",
            net_source,
            runtime_tests + "\nstruct EthernetStartupFixture {};\n",
        ),
    ):
        require_mutation_rejected(
            label,
            lambda source=source, runtime=runtime:
                require_ethernet_start_cleanup_contract(source, runtime),
        )

    ota_source = SOURCES["ota_update.cpp"]
    ota_logic = (MAIN / "logic/ota_contract.hpp").read_text(encoding="utf-8")
    manifest_header = (MAIN / "ota_manifest.hpp").read_text(encoding="utf-8")
    moved_release = ota_source.replace("std::string{}.swap(body);", "", 1).replace(
        "res.available.assign(available.data(), available.size());",
        "res.available.assign(available.data(), available.size());\n"
        "    std::string{}.swap(body);",
        1,
    )
    for label, source, logic, manifest in (
        (
            "OTA bounded-body gate bypass",
            ota_source.replace("out.reserve(tk::kOtaManifestMaxBytes)", "out.reserve(16384)", 1),
            ota_logic, manifest_header,
        ),
        (
            "OTA JSON materialization bypass",
            ota_source.replace("tk::json_materialize<cJSON>", "fixture_json_parse<cJSON>", 1),
            ota_logic, manifest_header,
        ),
        (
            "OTA duplicate-key inspector bypass",
            ota_source.replace("tk::inspect_ota_manifest(j.get())", "fixture_manifest(j.get())", 1),
            ota_logic, manifest_header,
        ),
        (
            "OTA validate-before-copy bypass",
            ota_source.replace("tk::canonical_ota_version(available)", "true", 1),
            ota_logic, manifest_header,
        ),
        ("OTA body-release ordering", moved_release, ota_logic, manifest_header),
        (
            "OTA manifest bound drift",
            ota_source,
            ota_logic.replace("kOtaManifestMaxBytes = 8192", "kOtaManifestMaxBytes = 16384", 1),
            manifest_header,
        ),
        (
            "OTA duplicate-root comparison bypass",
            ota_source, ota_logic,
            manifest_header.replace("std::strcmp(item->string, later->string) == 0", "false", 1),
        ),
    ):
        require_mutation_rejected(
            label,
            lambda source=source, logic=logic, manifest=manifest:
                require_ota_fetch_contract(source, logic, manifest),
        )

    status_source = SOURCES["http_status.cpp"]
    runtime_tests = (ROOT / "test/test_runtime_boundaries.cpp").read_text(encoding="utf-8")
    read_fallthrough = status_source.replace(
        "const esp_err_t read_err = esp_partition_read(source.partition, offset, buffer.bytes, bytes);",
        "const esp_err_t read_err = esp_partition_read(source.partition, offset, buffer.bytes, bytes);\n"
        "        if (read_err != ESP_OK) break;",
        1,
    )
    require_mutation_rejected(
        "coredump read failure clean-termination fallthrough",
        lambda: require_coredump_stream_contract(read_fallthrough, runtime_tests),
    )
    # F-003 countercontrol: a guard that can never fire keeps every token in order.
    read_guard_disabled = status_source.replace(
        "if (read_err != ESP_OK) {", "if (false && read_err != ESP_OK) {", 1)
    if read_guard_disabled == status_source:
        raise AssertionError("coredump disabled-guard mutation did not apply")
    require_mutation_rejected(
        "coredump read-error guard disabled",
        lambda: require_coredump_stream_contract(read_guard_disabled, runtime_tests),
    )
    for label, old, new in (
        (
            "coredump failure return behind a nested condition",
            "if (read_err != ESP_OK) {\n            return ESP_FAIL;",
            "if (read_err != ESP_OK) {\n            if (offset == 0) return ESP_FAIL;",
        ),
        (
            "coredump read result not kept in the guarded variable",
            "const esp_err_t read_err = esp_partition_read(source.partition, offset, buffer.bytes, bytes);",
            "esp_partition_read(source.partition, offset, buffer.bytes, bytes);\n        const esp_err_t read_err = ESP_OK;",
        ),
    ):
        mutated = status_source.replace(old, new, 1)
        if mutated == status_source:
            raise AssertionError(f"{label} mutation did not apply")
        require_mutation_rejected(
            label, lambda mutated=mutated: require_coredump_stream_contract(mutated, runtime_tests))

    heap_stream = (MAIN / "logic/heap_json_stream.hpp").read_text(encoding="utf-8")
    logic_tests = (ROOT / "test/test_logic.cpp").read_text(encoding="utf-8")
    for label, source, stream in (
        (
            "/heap fixed chunk bound drift",
            status_source,
            heap_stream.replace("char buffer_[192]{};", "char buffer_[1024]{};", 1),
        ),
        (
            "/heap send failure bypass",
            status_source.replace("if (!sent) return ESP_FAIL;", "if (false) return ESP_FAIL;", 1),
            heap_stream,
        ),
        (
            "/heap cJSON tree regression",
            status_source.replace(
                "esp_err_t handle_heap(GuardedReq rq) {",
                "esp_err_t handle_heap(GuardedReq rq) {\n    tk::JsonBuilder fixture_tree;",
                1,
            ),
            heap_stream,
        ),
        (
            "/heap pointer preflight bypass",
            status_source,
            heap_stream.replace(
                "if (view.count != 0 && (!view.free_samples || !view.largest_samples)) return false;",
                "if (false) return false;",
                1,
            ),
        ),
    ):
        require_mutation_rejected(
            label,
            lambda source=source, stream=stream: require_heap_json_stream_contract(
                source, stream, logic_tests
            ),
        )

    main_source = SOURCES["main.cpp"]
    net_source = SOURCES["net.cpp"]
    provisioning_source = SOURCES["provisioning.cpp"]
    for label, app, net, provisioning in (
        (
            "app boot ESP_ERROR_CHECK reintroduction",
            main_source.replace(
                'extern "C" void app_main() {',
                'extern "C" void app_main() { ESP_ERROR_CHECK(ESP_OK);',
                1,
            ),
            net_source, provisioning_source,
        ),
        (
            "network ESP_ERROR_CHECK reintroduction",
            main_source,
            net_source.replace("void net_init() {", "void net_init() { ESP_ERROR_CHECK(ESP_OK);", 1),
            provisioning_source,
        ),
        (
            "setup AP ESP_ERROR_CHECK reintroduction",
            main_source, net_source,
            provisioning_source.replace(
                "void provisioning_run(NvsStorageAdapter& config_store) {",
                "void provisioning_run(NvsStorageAdapter& config_store) { ESP_ERROR_CHECK(ESP_OK);",
                1,
            ),
        ),
        (
            "NVS boot failure bypass",
            main_source.replace("if (flash_init_err != ESP_OK)", "if (false)", 1),
            net_source, provisioning_source,
        ),
        (
            "network substrate fail-closed routing bypass",
            main_source,
            net_source.replace(
                "net_boot_require(net_init_substrate(&failed_component), failed_component);",
                "(void)net_init_substrate(&failed_component);",
                1,
            ),
            provisioning_source,
        ),
        (
            "network substrate error acceptance",
            main_source,
            net_source.replace(
                "err != ESP_OK && err != ESP_ERR_INVALID_STATE",
                "false",
                1,
            ),
            provisioning_source,
        ),
        (
            "setup HTTP stop error ignored",
            main_source, net_source,
            provisioning_source.replace("if (stop_err != ESP_OK)", "if (false)", 1),
        ),
        (
            "setup HTTP stop failure loses running handle",
            main_source, net_source,
            provisioning_source.replace("if (stop_err != ESP_OK) {",
                                        "if (stop_err != ESP_OK) { portal.server = nullptr;", 1),
        ),
        (
            "remaining event-driven retries omitted from portal ownership",
            main_source,
            net_source.replace("s_retry_num >= MAX_RETRY", "false", 1),
            provisioning_source,
        ),
    ):
        require_mutation_rejected(
            label,
            lambda app=app, net=net, provisioning=provisioning:
                require_explicit_idf_boot_error_contract(
                    app, net, provisioning, runtime_tests
                ),
        )

    diag_header = (MAIN / "diag_log.hpp").read_text(encoding="utf-8")
    diag_source = SOURCES["diag_log.cpp"]
    diag_handler = function_body("handle_diag")
    for label, handler, header, source in (
        (
            "/diag sink failure fabricated complete",
            diag_handler, diag_header,
            diag_source.replace(
                "return DiagDumpResult::SinkFailed;",
                "return DiagDumpResult::Complete;",
                1,
            ),
        ),
        (
            "/diag plain failure terminator bypass",
            diag_handler.replace(
                "if (dump != DiagDumpResult::Complete) return ESP_FAIL;",
                "if (false) return ESP_FAIL;",
                1,
            ),
            diag_header, diag_source,
        ),
        (
            "/diag redacted partial-line flush after invalidation",
            diag_handler.replace(
                "if (dump != DiagDumpResult::Complete || !ok) return ESP_FAIL;",
                "if (!ok) return ESP_FAIL;",
                1,
            ),
            diag_header, diag_source,
        ),
    ):
        require_mutation_rejected(
            label,
            lambda handler=handler, header=header, source=source:
                require_diag_dump_completion_contract(
                    handler, header, source, runtime_tests
                ),
        )

    for label, source in (
        (
            "OTA status reader allocation-failure unlocked read",
            ota_source.replace(
                "if (!lock) return unavailable_status_snapshot();",
                "if (!lock) return s_status;",
                1,
            ),
        ),
        (
            "OTA status reader guard-failure unlocked read",
            ota_source.replace(
                "if (!g) return unavailable_status_snapshot();",
                "if (!g) return s_status;",
                1,
            ),
        ),
        (
            "OTA first-lock CAS loser returns candidate",
            ota_source.replace(
                "vSemaphoreDelete(candidate);\n        return lock;",
                "vSemaphoreDelete(candidate);\n        return candidate;",
                1,
            ),
        ),
        (
            "OTA status writer lock bypass",
            ota_source.replace("if (!g) return;", "if (false) return;", 1),
        ),
    ):
        require_mutation_rejected(
            label,
            lambda source=source: require_ota_status_lock_contract(source, runtime_tests),
        )

    materialize_under_lock = ota_source.replace(
        "    }\n    // Any allocation happens after releasing the status lock.",
        "        return {snapshot.state, snapshot.progress, snapshot.message.data(),\n"
        "                snapshot.available.data(), snapshot.update_available, snapshot.current.data()};\n"
        "    }\n    // Any allocation happens after releasing the status lock.",
        1,
    )
    if materialize_under_lock == ota_source:
        raise AssertionError("OTA materialization-under-lock mutation did not apply")
    require_mutation_rejected(
        "OTA status string materialization under lock",
        lambda: require_ota_status_lock_contract(materialize_under_lock, runtime_tests),
    )

    partial_check_publish = ota_source.replace(
        "    ESP_LOGI(TAG, \"available %s — %s\", res.available.c_str(), res.reason.c_str());",
        "    set_state(OtaState::Idle, 0, res.reason.c_str());\n"
        "    ESP_LOGI(TAG, \"available %s — %s\", res.available.c_str(), res.reason.c_str());",
        1,
    )
    if partial_check_publish == ota_source:
        raise AssertionError("OTA partial-check publication mutation did not apply")
    require_mutation_rejected(
        "OTA manifest check publishes partial status before whole snapshot",
        lambda: require_ota_status_lock_contract(partial_check_publish, runtime_tests),
    )

    allocating_check_error = ota_source.replace(
        "static void set_check_error(const char* message) noexcept {\n"
        "    OtaStatusPod candidate{};",
        "static void set_check_error(const char* message) noexcept {\n"
        "    std::string fallback_allocation;\n"
        "    OtaStatusPod candidate{};",
        1,
    )
    if allocating_check_error == ota_source:
        raise AssertionError("OTA check-error allocation mutation did not apply")
    require_mutation_rejected(
        "OTA noexcept check-error fallback allocation",
        lambda: require_ota_status_lock_contract(allocating_check_error, runtime_tests),
    )
    hidden_allocating_check_error = ota_source.replace(
        "static void set_check_error(const char* message) noexcept {\n"
        "    OtaStatusPod candidate{};",
        "static void set_check_error(const char* message) noexcept {\n"
        "    auto fallback_allocation = std::to_string(42);\n"
        "    OtaStatusPod candidate{};",
        1,
    )
    if hidden_allocating_check_error == ota_source:
        raise AssertionError("OTA check-error indirect-allocation mutation did not apply")
    require_mutation_rejected(
        "OTA noexcept check-error fallback calls an unreviewed allocator",
        lambda: require_ota_status_lock_contract(hidden_allocating_check_error, runtime_tests),
    )

    partial_check_error = ota_source.replace(
        "    publish_check_status(candidate);\n}\n\n// ─── Small HTTPS GET",
        "    set_state(OtaState::Error, 0, message);\n}\n\n// ─── Small HTTPS GET",
        1,
    )
    if partial_check_error == ota_source:
        raise AssertionError("OTA check-error partial-publication mutation did not apply")
    require_mutation_rejected(
        "OTA noexcept check-error fallback publishes partial status",
        lambda: require_ota_status_lock_contract(partial_check_error, runtime_tests),
    )

    ble_callback_source = SOURCES["ble_client.cpp"]
    blocking_ble_callback = ble_callback_source.replace(
        "tk::SemGuard intent(intent_mutex_, 0);",
        "tk::SemGuard intent(intent_mutex_);",
        1,
    )
    if blocking_ble_callback == ble_callback_source:
        raise AssertionError("BLE callback blocking-lock mutation did not apply")
    require_mutation_rejected(
        "BLE timer callback uses unbounded lifecycle lock",
        lambda: require_ble_callback_lock_contract(blocking_ble_callback),
    )
    blocking_ble_disconnect = ble_callback_source.replace(
        "disconnect_from_callback_();", "disconnect();", 1
    )
    if blocking_ble_disconnect == ble_callback_source:
        raise AssertionError("BLE callback blocking-disconnect mutation did not apply")
    require_mutation_rejected(
        "BLE GATT callback delegates to blocking disconnect",
        lambda: require_ble_callback_lock_contract(blocking_ble_disconnect),
    )
    blocking_ble_scan = ble_callback_source.replace("ensure_scanning_(0);",
                                                    "ensure_scanning_(portMAX_DELAY);", 1)
    if blocking_ble_scan == ble_callback_source:
        raise AssertionError("BLE callback scan-deadline mutation did not apply")
    require_mutation_rejected(
        "BLE host callback delegates to blocking scan deadline",
        lambda: require_ble_callback_lock_contract(blocking_ble_scan),
    )
    blocking_ble_helper = ble_callback_source.replace(
        "bool BleClient::start_scan_locked_() {",
        "bool BleClient::start_scan_locked_() {\n"
        "    tk::SemGuard regression(intent_mutex_);",
        1,
    )
    if blocking_ble_helper == ble_callback_source:
        raise AssertionError("BLE transitive-helper blocking-lock mutation did not apply")
    require_mutation_rejected(
        "BLE callback transitive helper uses unbounded lifecycle lock",
        lambda: require_ble_callback_lock_contract(blocking_ble_helper),
    )
    blocking_ble_registry = ble_callback_source.replace(
        "void BleClient::note_scan_(const ble_gap_disc_desc& d, const ble_hs_adv_fields& f) {",
        "void BleClient::note_scan_(const ble_gap_disc_desc& d, const ble_hs_adv_fields& f) {\n"
        "    tk::SemGuard regression(scan_mutex_);",
        1,
    )
    if blocking_ble_registry == ble_callback_source:
        raise AssertionError("BLE registry-helper blocking-lock mutation did not apply")
    require_mutation_rejected(
        "BLE callback registry helper uses unbounded lock",
        lambda: require_ble_callback_lock_contract(blocking_ble_registry),
    )
    allocating_ble_helper = ble_callback_source.replace(
        "void BleClient::terminate_published_link_() {",
        "void BleClient::terminate_published_link_() {\n"
        "    std::string callback_allocation;",
        1,
    )
    if allocating_ble_helper == ble_callback_source:
        raise AssertionError("BLE transitive-helper allocation mutation did not apply")
    require_mutation_rejected(
        "BLE noexcept disconnect helper chain allocates",
        lambda: require_ble_callback_lock_contract(allocating_ble_helper),
    )
    hidden_allocating_ble_helper = ble_callback_source.replace(
        "void BleClient::terminate_published_link_() {",
        "void BleClient::terminate_published_link_() {\n"
        "    auto callback_allocation = std::to_string(42);",
        1,
    )
    if hidden_allocating_ble_helper == ble_callback_source:
        raise AssertionError("BLE transitive-helper indirect-allocation mutation did not apply")
    require_mutation_rejected(
        "BLE noexcept disconnect helper calls an unreviewed allocator",
        lambda: require_ble_callback_lock_contract(hidden_allocating_ble_helper),
    )
    static_allocating_ble_helper = ble_callback_source.replace(
        "void BleClient::terminate_published_link_() {",
        "static void callback_allocating_helper() { std::string value; }\n\n"
        "void BleClient::terminate_published_link_() {\n"
        "    callback_allocating_helper();",
        1,
    )
    if static_allocating_ble_helper == ble_callback_source:
        raise AssertionError("BLE translation-unit helper allocation mutation did not apply")
    require_mutation_rejected(
        "BLE callback closure reaches an unreviewed translation-unit helper",
        lambda: require_ble_callback_lock_contract(static_allocating_ble_helper),
    )
    aliased_allocating_ble_type = ble_callback_source.replace(
        "void BleClient::terminate_published_link_() {",
        "using CallbackHiddenString = std::string;\n\n"
        "void BleClient::terminate_published_link_() {\n"
        "    CallbackHiddenString hidden = \"a deliberately long callback allocation\";",
        1,
    )
    if aliased_allocating_ble_type == ble_callback_source:
        raise AssertionError("BLE aliased callback allocation mutation did not apply")
    require_mutation_rejected(
        "BLE callback executable fingerprint rejects aliased RAII allocation",
        lambda: require_ble_callback_lock_contract(aliased_allocating_ble_type),
    )
    raw_wait_ble_helper = ble_callback_source.replace(
        "void BleClient::terminate_published_link_() {",
        "void BleClient::terminate_published_link_() {\n"
        "    (void)xSemaphoreTake(intent_mutex_, portMAX_DELAY);",
        1,
    )
    if raw_wait_ble_helper == ble_callback_source:
        raise AssertionError("BLE transitive-helper raw-wait mutation did not apply")
    require_mutation_rejected(
        "BLE noexcept disconnect helper chain waits on raw semaphore",
        lambda: require_ble_callback_lock_contract(raw_wait_ble_helper),
    )

    controller_source = SOURCES["vehicle_ctrl.cpp"]
    telemetry_source = SOURCES["vehicle_telemetry.cpp"]
    ble_source = SOURCES["ble_client.cpp"]
    commands_source = SOURCES["vehicle_commands.cpp"]
    callback_mutations = (
        (
            "persistent callback allocation",
            controller_source,
            telemetry_source.replace(
                "telemetry_pending_charge_ = state;",
                "std::string callback_allocation; telemetry_pending_charge_ = state;",
                1,
            ),
            ble_source,
        ),
        (
            "telemetry callback parsing under Vehicle lock",
            controller_source,
            telemetry_source.replace(
                "telemetry_pending_charge_ = state;",
                "parse_charge_state(state, last_known_charge_);",
                1,
            ),
            ble_source,
        ),
        (
            "deferred telemetry drain removed",
            controller_source,
            telemetry_source.replace("self->process_pending_telemetry_();", "", 1),
            ble_source,
        ),
        (
            "cache_mutex_ logging canary",
            controller_source,
            telemetry_source.replace(
                "last_known_charge_ = std::move(parsed);",
                'ESP_LOGW(TAG, "canary"); last_known_charge_ = std::move(parsed);',
                1,
            ),
            ble_source,
        ),
        (
            "NimBLE host direct Vehicle RX reentry",
            controller_source,
            telemetry_source,
            ble_source.replace(
                "int BleClient::on_gap_event",
                "void callback_canary() { process_rx_frame_(payload, len); }\n\n"
                "int BleClient::on_gap_event",
                1,
            ),
        ),
    )
    for label, controller_mutated, telemetry_mutated, ble_mutated in callback_mutations:
        if (controller_mutated, telemetry_mutated, ble_mutated) == (
            controller_source, telemetry_source, ble_source
        ):
            raise AssertionError(f"{label} mutation did not apply")
        require_mutation_rejected(
            label,
            lambda controller_mutated=controller_mutated,
                   telemetry_mutated=telemetry_mutated,
                   ble_mutated=ble_mutated:
                require_tesla_cpp_callback_contract(
                    controller_mutated, telemetry_mutated, ble_mutated, commands_source
                ),
        )

    command_callback_log = commands_source.replace(
        "completion->completed = true;",
        'ESP_LOGW(TAG, "callback mutation"); completion->completed = true;',
        1,
    )
    if command_callback_log == commands_source:
        raise AssertionError("command callback logging mutation did not apply")
    require_mutation_rejected(
        "command callback logging under Vehicle lock",
        lambda: require_tesla_cpp_callback_contract(
            controller_source, telemetry_source, ble_source, command_callback_log
        ),
    )
    stale_amp_readback = commands_source.replace(
        "const ChargingAmpsFeedback feedback_after = charging_amps_feedback_snapshot_();",
        "const ChargingAmpsFeedback feedback_after = feedback_before;",
        1,
    )
    if stale_amp_readback == commands_source:
        raise AssertionError("stale charging-amps feedback mutation did not apply")
    require_mutation_rejected(
        "charging-amps verification reuses stale generation",
        lambda: require_tesla_cpp_callback_contract(
            controller_source, telemetry_source, ble_source, stale_amp_readback
        ),
    )

    crash_source = SOURCES["diag_crash.cpp"]
    for label, crash_mutated in (
        (
            "crash dismissal plain bool",
            crash_source.replace(
                "static std::atomic<bool> s_dismissed{false};",
                "static bool s_dismissed{false};",
                1,
            ),
        ),
        (
            "crash dismissal shared snapshot write",
            crash_source.replace(
                "s_dismissed.store(true, std::memory_order_release);",
                "s_ci.dismissed = true;",
                1,
            ),
        ),
    ):
        if crash_mutated == crash_source:
            raise AssertionError(f"{label} mutation did not apply")
        require_mutation_rejected(
            label,
            lambda crash_mutated=crash_mutated:
                require_crash_dismiss_atomic_contract(crash_mutated),
        )

    for old, replacement, label in (
        ("crash_finish_snapshot(info, {before, after,", "crash_finish_snapshot(info, {before, before,", "stale final generation"),
        ("const uint32_t after = s_admission.load(std::memory_order_acquire);", "const uint32_t after = before;", "missing final acquire"),
        ("const uint32_t after = s_admission.load(std::memory_order_acquire);", "const uint32_t after = before; /* const uint32_t after = s_admission.load(std::memory_order_acquire); */", "comment decoy final acquire"),
        ("crash_finish_snapshot(info, {before, after,", "/* crash_finish_snapshot(info, {before, after, */ ignored_snapshot(info, {before, after,", "comment decoy finalization"),
        ("state(after) == Current, state(after) == Foreign,", "state(before) == Current, state(before) == Foreign,", "stale final authority"),
    ):
        mutated = crash_source.replace(old, replacement, 1)
        if mutated == crash_source:
            raise AssertionError(label + " mutation did not apply")
        require_mutation_rejected(label, lambda mutated=mutated: require_crash_snapshot_contract(mutated))
    load = "const uint32_t after = s_admission.load(std::memory_order_acquire);"
    borrow = "CrashInfoView info = crash_borrow(s_boot);"
    early = crash_source.replace(load, "", 1).replace(borrow, load + "\n    " + borrow, 1)
    require_mutation_rejected("acquire before payload view", lambda: require_crash_snapshot_contract(early))

    for old, replacement, label in (
        ("return s_operation_gate.try_begin(tk::OtaIdentityGateState::FaultRestart);", "return true;", "FaultRestart begin"),
        ("finish_operation(tk::OtaIdentityGateState::FaultRestart);", "", "FaultRestart cancel"),
        ("finish_operation(tk::OtaIdentityGateState::HealthCommit);", "", "HealthCommit release"),
        ("s_operation_gate.try_begin(tk::OtaIdentityGateState::ConfigRestart);", "true;", "ConfigRestart begin"),
        ("if (!owner.owns_gate()) return Result::WrongOwner;", "", "config query actual owner"),
        ("return held_ && s_operation_gate.state() == tk::OtaIdentityGateState::ConfigRestart;", "return true;", "guard identity"),
        ("case ESP_OTA_IMG_NEW: return Result::New;", "case ESP_OTA_IMG_NEW: return Result::Pending;", "NEW raw state"),
        ("if (result != Result::Pending) return result;", "", "only raw Pending may mark"),
        ("[] { return tk::runtime_admission_vehicle_ready(); }", "[] { return true; }", "shared Ready reader"),
        ("[] { return ble_host_synced(); }", "[] { return true; }", "shared NimBLE sync reader"),
        ("[] { return ble_host_reset_count(); }", "[] { return 0; }", "shared NimBLE reset reader"),
        ("[] { return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL); }", "[] { return 4096; }", "shared fresh heap reader"),
        ("result = Result::MarkUnknown;", "result = Result::Pending;", "uncertain SDK effect"),
        ("result = Result::Confirmed;", "result = Result::Pending;", "confirmed result before logging"),
    ):
        mutated_ota = ota_source.replace(old, replacement, 1)
        if mutated_ota == ota_source: raise AssertionError(label + " mutation did not apply")
        require_mutation_rejected(label, lambda: require_operation_wrapper_contract(mutated_ota))

    health_task = function_body("ota_health_gate_task")
    for old, replacement, label in (
        ("ota_runtime_health().healthy()", "true", "shared health verdict"),
        ("OtaHealthCommitGuard commit_guard;", "bool commit_guard = true;", "operation owner"),
        ("if (!commit_health.healthy()) continue;", "", "shared health CAS recheck"),
    ):
        mutated_health = health_task.replace(old, replacement, 1)
        if mutated_health == health_task:
            raise AssertionError(f"health commit {label} mutation did not apply")
        try:
            require_health_commit_contract(mutated_health)
        except AssertionError:
            pass
        else:
            raise AssertionError(f"health commit {label} bypass passed unexpectedly")

    main_source = SOURCES["main.cpp"]
    telemetry_source = SOURCES["vehicle_telemetry.cpp"]
    pairing_source = SOURCES["vehicle_pairing.cpp"]

    # F01: VIN transition cleanup ordering mutations in app_main
    vin_cleanup_inverted = main_source.replace(
        "if (!tk::vin_transition_cleanup_ready_for_marker_removal(",
        "if (!config_store.remove(tk::nvs_contract::kVinTransition)) {\n"
        "                bootloader_random_disable();\n"
        "                boot_fatal(\"VIN transition marker cleanup\");\n"
        "            }\n"
        "            if (!tk::vin_transition_cleanup_ready_for_marker_removal(",
        1,
    )
    if vin_cleanup_inverted == main_source:
        raise AssertionError("VIN cleanup inverted mutation did not apply")
    require_mutation_rejected(
        "VIN transition marker removal before cleanup check",
        lambda: require_vin_transition_cleanup_contract(vin_cleanup_inverted),
    )

    vin_cleanup_missing_key = main_source.replace(
        "const bool vcsec_removed = tesla_store.remove(tk::nvs_contract::kSessionVcsec);",
        "const bool vcsec_removed = true;",
        1,
    )
    if vin_cleanup_missing_key == main_source:
        raise AssertionError("VIN cleanup missing key mutation did not apply")
    require_mutation_rejected(
        "VIN transition cleanup missing storage key removal",
        lambda: require_vin_transition_cleanup_contract(vin_cleanup_missing_key),
    )

    # F07: Telemetry epoch check + cache mutex mutations in process_pending_telemetry_
    telemetry_no_mutex = telemetry_source.replace(
        "tk::MutexGuard g(cache_mutex_);\n            if (tk::telemetry_epoch_matches(climate_epoch",
        "if (tk::telemetry_epoch_matches(climate_epoch",
        1,
    )
    if telemetry_no_mutex == telemetry_source:
        raise AssertionError("telemetry no-mutex mutation did not apply")
    require_mutation_rejected(
        "telemetry cache update without cache_mutex_",
        lambda: require_telemetry_epoch_cache_contract(telemetry_no_mutex),
    )

    telemetry_no_epoch = telemetry_source.replace(
        "if (tk::telemetry_epoch_matches(drive_epoch, identity_epoch_.load(std::memory_order_acquire))) {\n                last_known_drive_ = std::move(parsed);",
        "last_known_drive_ = std::move(parsed);",
        1,
    )
    if telemetry_no_epoch == telemetry_source:
        raise AssertionError("telemetry no-epoch mutation did not apply")
    require_mutation_rejected(
        "telemetry cache update without epoch check under cache_mutex_",
        lambda: require_telemetry_epoch_cache_contract(telemetry_no_epoch),
    )

    # F-003 countercontrols: each keeps the expected token order but publishes unlocked, outside
    # the epoch-checked branch, or behind a predicate that always holds.
    tires_epoch_if = ("if (tk::telemetry_epoch_matches(tires_epoch, "
                      "identity_epoch_.load(std::memory_order_acquire))) {")
    charge_epoch_if = ("if (tk::telemetry_epoch_matches(charge_epoch, "
                       "identity_epoch_.load(std::memory_order_acquire))) {")
    for label, old, new in (
        (
            "telemetry Tires publication after the lock and epoch scopes",
            tires_epoch_if + "\n                last_known_tires_ = std::move(parsed);\n"
            "                note_contact_();\n            }\n        }",
            tires_epoch_if + "}\n        }\n        last_known_tires_ = std::move(parsed);\n"
            "        note_contact_();",
        ),
        (
            "telemetry Tires epoch predicate that always holds (prefix)",
            tires_epoch_if,
            tires_epoch_if.replace("if (tk::", "if (true || tk::", 1),
        ),
        (
            "telemetry Tires epoch predicate that always holds (suffix)",
            tires_epoch_if,
            tires_epoch_if.replace(")) {", ") || true) {", 1),
        ),
        (
            "telemetry charge publication beside, not inside, the epoch branch",
            charge_epoch_if + "\n                last_known_charge_ = std::move(parsed);",
            charge_epoch_if + "}\n            {\n                last_known_charge_ = std::move(parsed);",
        ),
        (
            "telemetry Tires assignment alone moved after the scopes",
            tires_epoch_if + "\n                last_known_tires_ = std::move(parsed);\n"
            "                note_contact_();\n            }\n        }",
            tires_epoch_if + "\n                note_contact_();\n            }\n        }\n"
            "        last_known_tires_ = std::move(parsed);",
        ),
        (
            "telemetry closures note_contact_ alone moved after the scopes",
            "                last_known_closures_ = std::move(parsed);\n                note_contact_();\n"
            "            }\n        }",
            "                last_known_closures_ = std::move(parsed);\n            }\n        }\n"
            "        note_contact_();",
        ),
        (
            "telemetry drive branch without note_contact_",
            "                last_known_drive_ = std::move(parsed);\n                note_contact_();",
            "                last_known_drive_ = std::move(parsed);",
        ),
        (
            "telemetry extra note_contact_ outside every branch",
            "    if (pending & PendingClosures) {\n        ClosuresStateResult parsed{};",
            "    note_contact_();\n    if (pending & PendingClosures) {\n        ClosuresStateResult parsed{};",
        ),
        (
            "telemetry Tires guard in a sibling scope",
            "tk::MutexGuard g(cache_mutex_);\n            " + tires_epoch_if,
            "{ tk::MutexGuard g(cache_mutex_); }\n            " + tires_epoch_if,
        ),
        (
            "telemetry Tires cache written from the closures block",
            "                last_known_closures_ = std::move(parsed);",
            "                last_known_closures_ = std::move(parsed);\n"
            "                last_known_tires_ = TirePressureResult{};",
        ),
        (
            "telemetry charge generation bumped after the scopes",
            "                charge_state_generation_.fetch_add(1);\n"
            "                charge_cache_stale_reported_.store(false);\n"
            "                note_contact_();\n            }\n        }\n    }",
            "                charge_cache_stale_reported_.store(false);\n"
            "                note_contact_();\n            }\n        }\n"
            "        charge_state_generation_.fetch_add(1);\n    }",
        ),
        (
            "telemetry charge early epoch check that never skips",
            "if (!tk::telemetry_epoch_matches(charge_epoch,",
            "if (false && !tk::telemetry_epoch_matches(charge_epoch,",
        ),
    ):
        mutated = telemetry_source.replace(old, new, 1)
        if mutated == telemetry_source:
            raise AssertionError(f"{label} mutation did not apply")
        require_mutation_rejected(label, lambda mutated=mutated: require_telemetry_epoch_cache_contract(mutated))

    # F07: Pairing cleanup ordering in clear_session_and_cache_
    pairing_inverted_epoch = pairing_source.replace(
        "identity_epoch_.fetch_add(1, std::memory_order_acq_rel);\n    portENTER_CRITICAL(&telemetry_pending_mux_);\n    telemetry_pending_mask_ = 0;",
        "portENTER_CRITICAL(&telemetry_pending_mux_);\n    telemetry_pending_mask_ = 0;\n    portEXIT_CRITICAL(&telemetry_pending_mux_);\n    identity_epoch_.fetch_add(1, std::memory_order_acq_rel);\n    portENTER_CRITICAL(&telemetry_pending_mux_);",
        1,
    )
    if pairing_inverted_epoch == pairing_source:
        raise AssertionError("pairing inverted epoch mutation did not apply")
    require_mutation_rejected(
        "pairing cleanup pending reset before identity epoch bump",
        lambda: require_pairing_cleanup_epoch_contract(pairing_inverted_epoch),
    )

    pairing_missing_cache_reset = pairing_source.replace(
        "last_known_status_   = {};\n",
        "",
        1,
    )
    if pairing_missing_cache_reset == pairing_source:
        raise AssertionError("pairing missing cache reset mutation did not apply")
    require_mutation_rejected(
        "pairing cleanup missing cache field reset",
        lambda: require_pairing_cleanup_epoch_contract(pairing_missing_cache_reset),
    )

    for freshness_reset in (
        "        last_charge_ticks_.store(0);\n",
        "        charge_state_generation_.store(0);\n",
        "        last_climate_ticks_.store(0);\n",
        "        climate_state_generation_.store(0);\n",
    ):
        pairing_missing_freshness_reset = pairing_source.replace(freshness_reset, "", 1)
        if pairing_missing_freshness_reset == pairing_source:
            raise AssertionError(f"pairing freshness reset mutation did not apply: {freshness_reset}")
        require_mutation_rejected(
            f"pairing cleanup missing freshness reset {freshness_reset.strip()}",
            lambda mutated=pairing_missing_freshness_reset: require_pairing_cleanup_epoch_contract(
                mutated),
        )


def require_unconditional_statement(label: str, body: str, statement: str) -> None:
    """The statement must stand alone on its own line and run unconditionally.

    A call site that wraps it in an if/else, or in braces, can skip a case: that is exactly how an
    UNKNOWN VCSEC report used to leave the ASLEEP clock running. The previous significant line must
    therefore end a statement (';') or a block ('}'), never open or continue a condition.
    """
    lines = body.splitlines()
    for index, line in enumerate(lines):
        if line.strip() != statement:
            continue
        for previous in reversed(lines[:index]):
            stripped = previous.strip()
            if not stripped or stripped.startswith("//"):
                continue
            if stripped.endswith((";", "}")):
                return
            break
    raise AssertionError(f"{label}: {statement!r} must be an unconditional statement")


def require_vehicle_recovery_contract(pairing: str, boot: str, telemetry: str,
                                      ctrl_header: str) -> None:
    """Wiring of the VCSEC sleep clock and the key-rotation cleanup onto their host-tested logic.

    The behaviour (an UNKNOWN report ends the ASLEEP run, key_created is erased before the journal,
    a failed erase keeps the journal armed, ...) is proven by test_logic.cpp and
    test_nvs_storage.cpp, which run tk::next_asleep_since, tk::vcsec_asleep_proven,
    tk::run_key_rotation_boot_cleanup and tk::retire_key_rotation_journal themselves. What is left
    for a source contract is that the firmware shell still calls exactly those functions,
    unconditionally, and does not grow a second, untested copy of the sequence.
    """
    recovery = function_body_in(boot, "recover_pending_key_rotation_at_boot_")
    for token in ("tk::run_key_rotation_boot_cleanup(*storage_)",
                  "cleanup.state() != tk::KeyRotationBootState::Ready"):
        if token not in recovery:
            raise AssertionError(f"boot key-rotation recovery lost its tested cleanup: {token!r}")
    if "->remove(" in recovery:
        raise AssertionError("boot key-rotation recovery must erase only through "
                             "tk::run_key_rotation_boot_cleanup")

    finish = function_body_in(pairing, "finish_key_rotation_cleanup_")
    for token in ("tk::retire_key_rotation_journal(*storage_, date_untrusted)",
                  "tk::KeyRotationJournalRetire::DatePending",
                  "tk::KeyRotationJournalRetire::JournalPending"):
        if token not in finish:
            raise AssertionError(f"key-rotation journal retirement lost its tested step: {token!r}")
    if "->remove(" in finish:
        raise AssertionError("key-rotation journal retirement must erase only through "
                             "tk::retire_key_rotation_journal")
    require_before("session cleanup before journal retirement", finish,
                   "clear_session_and_cache_()", "tk::retire_key_rotation_journal(")
    require_before("journal retired before the cleanup flags clear", finish,
                   "tk::retire_key_rotation_journal(", "pairing_cleanup_pending_.store(false)")
    for outcome in ("DatePending", "JournalPending"):
        if not re.search(
            rf"==\s*tk::KeyRotationJournalRetire::{outcome}\s*\)\s*\{{[^}}]*return false;", finish
        ):
            raise AssertionError(f"a {outcome} journal retirement must refuse completion")

    clock = function_body_in(ctrl_header, "note_vcsec_sleep_")
    if "tk::next_asleep_since(" not in clock or "compare_exchange_weak(" not in clock:
        raise AssertionError("note_vcsec_sleep_ must fold the report through tk::next_asleep_since")
    proven = function_body_in(ctrl_header, "vcsec_stably_asleep_")
    if "tk::vcsec_asleep_proven(" not in proven or "vcsec_sleep_state_.load()" not in proven:
        raise AssertionError("vcsec_stably_asleep_ must decide through tk::vcsec_asleep_proven "
                             "on the CURRENT raw state")
    unknown = function_body_in(ctrl_header, "mark_vcsec_unknown_")
    require_before("raw state published Unknown before the clock is cut", unknown,
                   "vcsec_sleep_state_.store(static_cast<int>(tk::SleepState::Unknown))",
                   "note_vcsec_sleep_(tk::SleepState::Unknown)")
    for name, text in (("vehicle_ctrl.hpp", ctrl_header), ("vehicle_telemetry.cpp", telemetry),
                       ("vehicle_ctrl.cpp", boot), ("vehicle_pairing.cpp", pairing)):
        if re.search(r"note_vcsec_sleep_\(\s*(?:true|false)\s*\)", text):
            raise AssertionError(f"{name}: the sleep clock takes a SleepState, never a bool")
    require_unconditional_statement(
        "VCSEC RX path feeds the sleep clock", function_body_in(telemetry, "handle_vcsec_frame_"),
        "note_vcsec_sleep_(reported);")
    require_unconditional_statement(
        "vehicle loop sampler feeds the sleep clock", function_body_in(telemetry, "loop_task_fn_"),
        "self->note_vcsec_sleep_(st);")


def self_test_vehicle_recovery_canaries(sources: dict[str, str]) -> None:
    def mutate(key: str, old: str, new: str) -> dict[str, str]:
        if sources[key].count(old) != 1:
            raise AssertionError(f"recovery canary anchor missing or ambiguous: {old!r}")
        return {**sources, key: sources[key].replace(old, new, 1)}

    cases = (
        ("RX path feeds the clock only on ASLEEP",
         mutate("telemetry", "note_vcsec_sleep_(reported);",
                "if (reported == tk::SleepState::Asleep) note_vcsec_sleep_(reported);")),
        ("RX path reduces the report to a bool",
         mutate("telemetry", "note_vcsec_sleep_(reported);",
                "note_vcsec_sleep_(reported == tk::SleepState::Asleep);")),
        ("RX path hard-codes a bool",
         mutate("telemetry", "note_vcsec_sleep_(reported);", "note_vcsec_sleep_(false);")),
        ("sampler feeds the clock only on ASLEEP (same line)",
         mutate("telemetry", "self->note_vcsec_sleep_(st);",
                "if (st == tk::SleepState::Asleep) self->note_vcsec_sleep_(st);")),
        ("sampler feeds the clock only on ASLEEP (next line)",
         mutate("telemetry", "self->note_vcsec_sleep_(st);",
                "if (st == tk::SleepState::Asleep)\n                self->note_vcsec_sleep_(st);")),
        ("sampler skips UNKNOWN in a braced block",
         mutate("telemetry", "self->note_vcsec_sleep_(st);",
                "if (st != tk::SleepState::Unknown) {\n                self->note_vcsec_sleep_(st);\n            }")),
        ("clock stops folding through the tested rule",
         mutate("ctrl_header", "tk::next_asleep_since(since, reported, now)",
                "(reported == tk::SleepState::Asleep ? now : 0u)")),
        ("proof stops deciding through the tested rule",
         mutate("ctrl_header", "return tk::vcsec_asleep_proven(",
                "return fixture_vcsec_asleep_proven(")),
        ("Unknown no longer cuts the clock",
         mutate("ctrl_header", "        note_vcsec_sleep_(tk::SleepState::Unknown);\n", "")),
        ("boot recovery stops using the tested cleanup",
         mutate("boot", "tk::run_key_rotation_boot_cleanup(*storage_)", "fixture_cleanup(*storage_)")),
        ("boot recovery ignores the cleanup verdict",
         mutate("boot", "cleanup.state() != tk::KeyRotationBootState::Ready", "false")),
        ("boot recovery grows its own erase",
         mutate("boot", "if (cleanup.state() != tk::KeyRotationBootState::Ready) {",
                "storage_->remove(tk::nvs_contract::kKeyCreated);\n"
                "    if (cleanup.state() != tk::KeyRotationBootState::Ready) {")),
        ("journal retirement grows its own erase",
         mutate("pairing", "const bool was_untrusted = key_created_untrusted_.load();",
                "storage_->remove(tk::kKeyRotationMarker);\n"
                "    const bool was_untrusted = key_created_untrusted_.load();")),
        ("a pending date erase no longer refuses completion",
         mutate("pairing",
                "ESP_LOGE(TAG, \"key-rotation date cleanup still pending\");\n        return false;",
                "ESP_LOGE(TAG, \"key-rotation date cleanup still pending\");")),
        ("a pending journal erase no longer refuses completion",
         mutate("pairing", "if (retired == tk::KeyRotationJournalRetire::JournalPending) {",
                "if (retired == tk::KeyRotationJournalRetire::JournalPending && false) {")),
    )
    for label, mutated in cases:
        require_mutation_rejected(
            f"vehicle recovery wiring: {label}",
            lambda mutated=mutated: require_vehicle_recovery_contract(**mutated),
        )


def require_pairing_recovery_contract(pairing: str, commands: str, telemetry: str) -> None:
    """Automatic enrollment reuses verified identity and preserves bounded single-write admission."""
    supervisor = function_body_in(pairing, "auto_pair_task_fn_")
    if "self->pair(" in scrub_cpp(supervisor):
        raise AssertionError("removed manual pairing entry must not return")
    for token in ("tk::auto_enrollment_due(", "tk::auto_enrollment_started(",
                  "self->enroll_existing_key_();", "self->enrollment_session_snapshot_(pdMS_TO_TICKS(50))"):
        if token not in supervisor:
            raise AssertionError(f"automatic enrollment admission lost {token!r}")
    if not (supervisor.index("self->poll_existing_key_()") < supervisor.index("self->enrollment_session_snapshot_(") < supervisor.index("tk::auto_enrollment_due(") <
            supervisor.index("tk::auto_enrollment_started(") < supervisor.index("self->enroll_existing_key_();")):
        raise AssertionError("enrollment must probe first and reserve backoff before I/O")
    if "enrollment = {}" in scrub_cpp(supervisor):
        raise AssertionError("health/hint changes must not reset the reserved send cadence")
    recovery_at = supervisor.find("if (key_action == tk::AutomaticKeyAction::RecoveryRequired)")
    generate_at = supervisor.find("if (key_action == tk::AutomaticKeyAction::Generate)")
    if recovery_at < 0 or generate_at < 0 or recovery_at >= generate_at:
        raise AssertionError("recovery hint must be handled before the first-boot-only generator")
    recovery_branch = supervisor[recovery_at:generate_at]
    for forbidden in ("generate_key(", "clear_session_and_cache_(", "enroll_existing_key_(",
                      "storage_->remove(", "finish_key_rotation_cleanup_("):
        if forbidden in scrub_cpp(recovery_branch):
            raise AssertionError(f"recovery hint branch may not perform {forbidden!r}")
    if "self->repair_notice_.store(true)" not in recovery_branch:
        raise AssertionError("recovery hint must remain visible to the user")

    paired_branch = supervisor[supervisor.find("if ((self->has_session() || self->session_available()) && !recovery_hint)"): ]
    health = paired_branch.find("bool ok            = self->health_probe_();")
    clear_notice = paired_branch.find("self->repair_notice_.store(false);")
    if health < 0 or clear_notice < health:
        raise AssertionError("a stored session alone must not clear the recovery notice")
    if "if ((self->has_session() || self->session_available()) && !recovery_hint)" not in paired_branch[:clear_notice]:
        raise AssertionError("health confirmation must remain associated with a stored session")

    result_callback = function_body_in(commands, "make_result_cb_")
    scrubbed_callback = scrub_cpp(result_callback)
    for forbidden in ("generate_key(", "clear_session_and_cache_(", "finish_key_rotation_cleanup_(",
                      "storage_->remove(", "storage_->save(", "pair(", "enroll_existing_key_("):
        if forbidden in scrubbed_callback:
            raise AssertionError(f"remote result callback may not perform {forbidden!r}")
    for required in ("pairing_recovery_hint_.store(true, std::memory_order_release)",
                     "repair_notice_.store(true, std::memory_order_release)"):
        if required not in scrubbed_callback:
            raise AssertionError(f"remote result callback no longer publishes {required!r}")

    fault_handler = function_body_in(telemetry, "handle_signed_message_fault_")
    scrubbed_fault_handler = scrub_cpp(fault_handler)
    for forbidden in ("storage_->remove(", "storage_->save(", "generate_key(",
                      "finish_key_rotation_cleanup_(", "pair(", "enroll_existing_key_("):
        if forbidden in scrubbed_fault_handler:
            raise AssertionError(f"signed message fault may not perform {forbidden!r}")
    for required in ("peer->reset();", "on_vehicle_message_(msg);",
                     "command_runner_.notify_message_fault(decision, payload_rejection,",
                     "if (authenticated && decision.repair_session)",
                     "!response_matches_current_(msg)"):
        if required not in scrubbed_fault_handler:
            raise AssertionError(f"signed message fault handling lost {required!r}")

    pair_body = function_body_in(pairing, "enroll_existing_key_")
    for token in ("OtaIdentityMutationGuard identity_guard(tk::IdentityMutationEntry::AutomaticEnrollment)",
                  "if (!identity_guard) return false;", "tk::SemGuard cmd_guard(command_mutex_",
                  "!tk::runtime_admission_vehicle_ready()", "!has_plausible_vin()",
                  "storage_->probe_blob(tk::nvs_contract::kPrivateKey, key_present)",
                  "const auto session = enrollment_session_snapshot_(",
                  "!key_probe_ok || !key_present || !command_identity_ready_()",
                  "tk::auto_enrollment_needed(session, pairing_recovery_hint_.load())",
                  "WakePolicy::NoWakeFail, deadline, tk::ConnectOrigin::Background",
                  "tk::CompletionTimeoutPolicy::ExpectedSilent"):
        if token not in pair_body:
            raise AssertionError(f"automatic enrollment sender lost {token!r}")
    if not (pair_body.index("tk::SemGuard cmd_guard") < pair_body.index("probe_blob(") <
            pair_body.index("send_vcsec_locked_(")):
        raise AssertionError("sender must recheck storage and identity under FIFO before transmission")
    for token in ("generate_key(", "storage_->remove(", "storage_->save(", "clear_session_and_cache_("):
        if token in scrub_cpp(pair_body):
            raise AssertionError(f"automatic same-key enrollment may not perform {token!r}")
    if "ESP_LOGW(" in scrub_cpp(pair_body) or "ESP_LOGI(" in scrub_cpp(pair_body):
        raise AssertionError("ordinary unattended enrollment attempts must not bypass background log throttling")
    if "Keys_Role_ROLE_CHARGING_MANAGER" not in pair_body:
        raise AssertionError("automatic enrollment role changed from Charging Manager")
    if "VCSEC_KeyFormFactor_KEY_FORM_FACTOR_CLOUD_KEY" not in pair_body:
        raise AssertionError("automatic enrollment form factor changed from CLOUD_KEY")
    if "true /* completes_on_transmit:" not in pair_body:
        raise AssertionError("Whitelist Add result must report local transmission, not wait for car approval")
    if "0 /* max_retries:" not in pair_body:
        raise AssertionError("an ambiguous Whitelist Add write must not be replayed automatically")
    probe = function_body_in(pairing, "poll_existing_key_")
    if "get_vehicle_status(status, tk::ConnectOrigin::Background, 6000)" not in probe:
        raise AssertionError("automatic enrollment must retain the bounded non-waking status probe")
    snapshot = function_body_in(pairing, "enrollment_session_snapshot_")
    for token in ("storage_->probe_blob(tk::nvs_contract::kSessionVcsec, snapshot.stored)",
                  "tk::SemGuard vehicle_guard(vehicle_mutex_, wait_ticks)",
                  "if (!vehicle_guard || !client_) return snapshot;",
                  "runtime_session_observation_locked_(UniversalMessage_Domain_DOMAIN_VEHICLE_SECURITY)",
                  "snapshot.runtime_readable = runtime.known;", "snapshot.runtime_ready = runtime.usable;"):
        if token not in snapshot:
            raise AssertionError(f"runtime session snapshot lost {token!r}")
    runtime = function_body_in(telemetry, "runtime_session_observation_locked_")
    for token in ("!peer->is_private_key_initialized()", "peer->get_epoch()", "epoch_bits |= epoch[i]",
                  "peer->is_valid()", "peer->has_shared_secret()", "epoch_bits != 0",
                  "command_runner_.vcsec_session()", "tracker.is_authenticated()", "tk::session_runtime_observation("):
        if token not in runtime:
            raise AssertionError(f"shared runtime predicate lost {token!r}")
    for forbidden in ("storage_->", "ESP_LOG", "can_send_command(", "is_initialized(", "has_valid_epoch(",
                      "send_vcsec", "xSemaphoreTake", "vTaskDelay"):
        if forbidden in scrub_cpp(runtime):
            raise AssertionError(f"shared runtime predicate may not perform {forbidden!r}")
    for method in ("session_availability", "session_available"):
        getter = scrub_cpp(function_body_in(telemetry, method))
        for forbidden in ("storage_->", "client_->", "ESP_LOG", "send_", "xSemaphore", "vTaskDelay",
                          "persist_session_", "runtime_session_observation_locked_"):
            if forbidden in getter:
                raise AssertionError(f"cache-only {method} may not perform {forbidden!r}")
    loop = function_body_in(telemetry, "loop_task_fn_")
    if not re.search(r"identity_ready && tk::session_can_operate\(identity_ready, self->has_session\(\),\s*"
                     r"self->session_available\(\)\)", loop):
        raise AssertionError("background eligibility must use the tested identity/stored/VCSEC RAM rule")
    locked = snapshot[snapshot.index("tk::SemGuard vehicle_guard"):]
    for forbidden in ("storage_->", "ESP_LOG", "can_send_command(", "is_initialized(",
                      "has_valid_epoch(", "send_vcsec", "xSemaphoreTake", "vTaskDelay"):
        if forbidden in scrub_cpp(locked):
            raise AssertionError(f"runtime session snapshot may not perform {forbidden!r} under Vehicle lock")
    if not (pair_body.index("tk::SemGuard cmd_guard") < pair_body.index("enrollment_session_snapshot_(") <
            pair_body.index("send_vcsec_locked_(")):
        raise AssertionError("sender must repeat RAM admission under FIFO before transmission")
    if "std::min<TickType_t>(ticks_until_(deadline), pdMS_TO_TICKS(50))" not in pair_body:
        raise AssertionError("sender runtime snapshot must retain its bounded remaining wait")
    vcsec_sender = function_body_in(commands, "send_vcsec_locked_")
    if "completes_on_transmit, max_retries" not in vcsec_sender:
        raise AssertionError("VCSEC sender must preserve each command's retry budget")


def self_test_pairing_recovery_canaries(pairing: str, commands: str,
                                        telemetry: str) -> None:
    def reject(label: str, mutated_pairing: str, mutated_commands: str = commands,
               mutated_telemetry: str = telemetry) -> None:
        require_mutation_rejected(
            f"pairing recovery contract: {label}",
            lambda: require_pairing_recovery_contract(mutated_pairing, mutated_commands,
                                                       mutated_telemetry),
        )

    anchor = "void VehicleController::auto_pair_task_fn_(void* arg) {"
    if pairing.count(anchor) != 1:
        raise AssertionError("pairing supervisor canary anchor missing")
    reject("removed manual pairing entry restored",
           pairing.replace(anchor, anchor + "\n    self->pair(tk::ConnectOrigin::Background);", 1))
    reject("backoff reservation removed", pairing.replace(
        "tk::auto_enrollment_started(enrollment, now_ms);", "", 1))
    reject("hint oscillation resets cadence", pairing.replace(
        "tk::auto_enrollment_started(enrollment, now_ms);",
        "enrollment = {}; tk::auto_enrollment_started(enrollment, now_ms);", 1))
    reject("sender loses tri-state session recheck", pairing.replace(
        "tk::auto_enrollment_needed(session, pairing_recovery_hint_.load())",
        "tk::auto_enrollment_needed({true, false, true, false}, pairing_recovery_hint_.load())", 1))
    reject("RAM session ignored", pairing.replace(
        "snapshot.runtime_ready = runtime.usable;", "snapshot.runtime_ready = false;", 1))
    reject("unknown runtime accepted", pairing.replace(
        "if (!vehicle_guard || !client_) return snapshot;", "if (!client_) return snapshot;", 1))
    reject("runtime snapshot logs under lock", pairing, commands, telemetry.replace(
        "return tk::session_runtime_observation(true,", 'ESP_LOGW(TAG, "runtime"); return tk::session_runtime_observation(true,', 1))
    reject("background eligibility loses identity gate", pairing, commands, telemetry.replace(
        "tk::session_can_operate(identity_ready,", "tk::session_can_operate(true,", 1))
    reject("background storage probe loses short-circuit gate", pairing, commands, telemetry.replace(
        "identity_ready && tk::session_can_operate(", "tk::session_can_operate(", 1))
    reject("background eligibility ignores VCSEC RAM", pairing, commands, telemetry.replace(
        "self->session_available());", "false);", 1))
    reject("availability getter reads storage", pairing, commands, telemetry.replace(
        "const auto runtime = session_availability().vcsec;",
        "storage_->blob_exists(tk::nvs_contract::kSessionVcsec); const auto runtime = session_availability().vcsec;", 1))
    reject("sender can change identity", pairing.replace(
        "CommandOutcome outcome = send_vcsec_locked_(",
        "storage_->remove(tk::nvs_contract::kPrivateKey); CommandOutcome outcome = send_vcsec_locked_(", 1))
    reject("background enrollment warning storm restored", pairing.replace(
        'ESP_LOGD(TAG, "Whitelist Add transmission was not confirmed:',
        'ESP_LOGW(TAG, "Whitelist Add transmission was not confirmed:', 1))
    reject("ambiguous Whitelist Add retry restored",
           pairing.replace("0 /* max_retries:", "3 /* max_retries:", 1))
    reject("VCSEC sender discards the explicit retry budget", pairing,
           commands.replace("completes_on_transmit, max_retries", "completes_on_transmit", 1))
    recovery_start = pairing.find("if (key_action == tk::AutomaticKeyAction::RecoveryRequired)")
    generate_start = pairing.find("if (key_action == tk::AutomaticKeyAction::Generate)", recovery_start)
    if recovery_start < 0 or generate_start < 0:
        raise AssertionError("pairing recovery canary anchors missing")
    recovery_branch = pairing[recovery_start:generate_start]
    reject("hint-triggered key generation", pairing[:recovery_start] +
           recovery_branch.replace("self->repair_notice_.store(true);",
                                   "self->repair_notice_.store(true); self->generate_key();", 1) +
           pairing[generate_start:])
    reject("error callback clears persisted sessions", pairing,
           commands.replace("pairing_recovery_hint_.store(true, std::memory_order_release);",
                            "pairing_recovery_hint_.store(true, std::memory_order_release);\n"
                            "storage_->remove(tk::nvs_contract::kSessionVcsec);", 1))
    handler_start = telemetry.find("void VehicleController::handle_signed_message_fault_(")
    handler_end = telemetry.find("void VehicleController::handle_session_info_frame_(", handler_start)
    if handler_start < 0 or handler_end < 0:
        raise AssertionError("signed fault handler canary anchors missing")
    handler = telemetry[handler_start:handler_end]
    reject("signed fault clears a stored session",
           pairing, commands,
           telemetry[:handler_start] + handler.replace(
               "peer->reset();", "peer->reset(); storage_->remove(tk::nvs_contract::kSessionVcsec);", 1)
           + telemetry[handler_end:])


def require_wake_result_contract(commands: str, telemetry: str, pairing: str, ctrl_header: str) -> None:
    wake = function_body_in(commands, "wake_up")
    for token in ("CommandOutcome sent;", "sent = send_vcsec_outcome_(", "final_outcome = std::move(sent);",
                  "if (!tk::wake_confirmation_required(sent.success, sent.failure))"):
        if token not in wake:
            raise AssertionError(f"wake lost call-owned result binding {token!r}")
    if "last_error_" in scrub_cpp(wake) or wake.count("send_vcsec_outcome_(") != 1:
        raise AssertionError("wake must retain one call-owned send outcome without shared error reads")
    if "WakePolicy::NoWakeFail, capped_deadline_(deadline, 9000)" not in wake:
        raise AssertionError("wake send must retain the original absolute deadline")
    typed_sender = function_body_in(commands, "send_vcsec_outcome_")
    if "deadline_in_(" in scrub_cpp(typed_sender):
        raise AssertionError("typed send handoff must not renew an absolute deadline")
    cap = scrub_cpp(function_body_in(ctrl_header, "capped_deadline_"))
    if cap.count("xTaskGetTickCount()") != 1 or "tk::capped_command_deadline(" not in cap:
        raise AssertionError("command deadline cap must delegate with one clock snapshot")
    callback = function_body_in(commands, "make_result_cb_")
    if "if (completion->success && response_expected)" not in callback:
        raise AssertionError("TX-only result must not stamp response reachability")
    sender = function_body_in(commands, "send_vcsec_locked_")
    if "make_result_cb_(completion, generation, auth_fail_is_health_probe, !completes_on_transmit)" not in sender:
        raise AssertionError("VCSEC sender must bind response evidence to its actual completion mode")
    loop = function_body_in(telemetry, "loop_task_fn_")
    for token in ("now_ticks - last_connect_ticks > pdMS_TO_TICKS(15000)",
                  "now_ticks, pdMS_TO_TICKS(30000)"):
        if token not in loop:
            raise AssertionError(f"documented connection/rotation cadence lost {token!r}")
    idle = function_body_in(pairing, "idle_until_next_health_poll_")
    for token in ("kIdleMs = 30000", "w < kIdleMs / kStepMs", "vTaskDelay(pdMS_TO_TICKS(kStepMs))"):
        if token not in idle:
            raise AssertionError(f"documented health idle lost {token!r}")
    health = function_body_in(pairing, "health_probe_")
    if "WakePolicy::NoWakeFail" not in health:
        raise AssertionError("documented health wake policy changed")


def self_test_wake_result_contract(sources: dict[str, str]) -> None:
    cases = (
        ("TX-only reaches response branch", "commands", "completion->success && response_expected", "completion->success"),
        ("sender loses actual completion mode", "commands", "auth_fail_is_health_probe, !completes_on_transmit", "auth_fail_is_health_probe, true"),
        ("wake reads shared reason", "commands", "final_outcome = std::move(sent);", "final_outcome.error = last_error_;"),
        ("wake bypasses send-outcome rule", "commands", "if (!tk::wake_confirmation_required(sent.success, sent.failure))", "if (false)"),
        ("wake renews absolute deadline", "commands", "WakePolicy::NoWakeFail, capped_deadline_(deadline, 9000)", "WakePolicy::NoWakeFail, deadline_in_(9000)"),
        ("cap adds a second clock", "ctrl_header", "tk::capped_command_deadline(deadline, xTaskGetTickCount(), pdMS_TO_TICKS(max_ms))", "tk::capped_command_deadline(deadline, xTaskGetTickCount() + xTaskGetTickCount(), pdMS_TO_TICKS(max_ms))"),
        ("warm-up cadence drifts", "telemetry", "last_connect_ticks > pdMS_TO_TICKS(15000)", "last_connect_ticks > pdMS_TO_TICKS(1000)"),
        ("health cadence drifts", "pairing", "kIdleMs = 30000", "kIdleMs = 1000"),
    )
    for label, key, old, new in cases:
        if sources[key].count(old) != 1:
            raise AssertionError(f"wake/cadence canary anchor missing or ambiguous: {label}")
        mutated = {**sources, key: sources[key].replace(old, new, 1)}
        require_mutation_rejected(label, lambda mutated=mutated: require_wake_result_contract(**mutated))


def require_request_intake_contract(parts: dict[str, str]) -> None:
    parts = {key: strip_cpp_comments(value) for key, value in parts.items()}
    compact = lambda text: re.sub(r"\s+", "", text)
    parse_call = ('tk::json_materialize<cJSON>(body.data,req->content_len,'
                  '[](const char* text){return cJSON_Parse(text);},tk::kJsonMaxRequestValues);')
    parse_call = compact(parse_call)
    for key, function in (("api", "handle_command"), ("config", "parse_json_object_body_"),
                          ("mcp", "mcp_handle_post")):
        body = function_body_in(parts[key], function)
        if compact(body).count(parse_call) != 1:
            raise AssertionError(f"{key}: actual request materializer must receive the64value budget")
        if "JsonMaterializeStatus::TooManyValues" not in body:
            raise AssertionError(f"{key}: too-many-values rejection missing")
    for key,function,rejection in (
        ("api", "handle_command", 'if(materialized.status==tk::JsonMaterializeStatus::TooManyValues){return send_json(req,400,make_response(false,cmd,vin,"too many JSON values"));}'),
        ("config", "parse_json_object_body_", 'if(materialized.status==tk::JsonMaterializeStatus::TooManyValues){return{tk::JsonOwner{},tk::ConfigSubmissionStatus::JsonTooManyValues};}'),
    ):
        if compact(rejection) not in compact(function_body_in(parts[key],function)):
            raise AssertionError(f"{key}: value rejection must terminate before root consumption")
    if "kJsonMaxRequestValues=64;" not in compact(parts["syntax"]):
        raise AssertionError("request value limit must remain64")
    if "kJsonMaxRequestValues" in parts["ota"]:
        raise AssertionError("OTA must retain the generic unlimited value budget")
    command = function_body_in(parts["api"], "handle_command")
    rejection = '''if(tk::has_duplicate_json_keys(json.get())){return send_json(req,400,make_response(false,cmd,vin,"duplicate JSON key"));}'''
    if compact(rejection) not in compact(command):
        raise AssertionError("REST duplicate keys must terminate with400 before field consumption")
    require_before("REST duplicate rejection before argument reads", command,
                   "tk::has_duplicate_json_keys(json.get())", "json_int_arg(json.get()")
    config = function_body_in(parts["config"], "parse_json_object_body_")
    rejection = '''if(tk::has_duplicate_json_keys(json.get())){return{tk::JsonOwner{},tk::ConfigSubmissionStatus::DuplicateJsonKey};}'''
    if compact(rejection) not in compact(config):
        raise AssertionError("configuration duplicate rejection must return before a Ready tree")
    require_before("configuration duplicate rejection before Ready", config,
                   "tk::has_duplicate_json_keys(json.get())", "return {std::move(json),")
    if 'using tk::has_duplicate_json_keys;' not in parts["mcp_payloads"]:
        raise AssertionError("MCP must share the decoded duplicate-key implementation")
    envelope = function_body_in(parts["mcp_payloads"], "inspect_request_envelope")
    if "has_duplicate_json_keys(object)" not in envelope:
        raise AssertionError("MCP request envelope lost duplicate rejection")
    mcp = function_body_in(parts["mcp"], "mcp_handle_post")
    rejection = '''if(materialized.status==tk::JsonMaterializeStatus::TooManyValues){return send_rpc_error_(req,{},tk::kJsonRpcInvalidRequest,"too many JSON values");}'''
    if compact(rejection) not in compact(mcp):
        raise AssertionError("MCP value budget must reject with untrusted/null-id Invalid Request")
    require_before("MCP value rejection before envelope/notifications", mcp,
                   "JsonMaterializeStatus::TooManyValues", "inspect_request_envelope")
    rest_integer = function_body_in(parts["api"], "json_int_arg")
    if compact('if(!tk::json_command_number(j,d)){problem="invalid argument";return false;}') not in compact(rest_integer):
        raise AssertionError("REST integer adapter bypasses shared decimal input rejection")
    require_before("REST parsed number before existing range decision", rest_integer,
                   "tk::json_command_number(j, d)", "tk::strict_rest_int")
    mcp_call = function_body_in(parts["mcp"], "handle_tools_call_")
    if 'if(!tk::json_command_number(j,number)||!tk::mcp_integer_value(number,a.lo,a.hi,ival[i]))' not in compact(mcp_call):
        raise AssertionError("MCP integer adapter bypasses shared decimal/range checks")
    number = function_body_in(parts["shared"], "json_command_number")
    if 'json_canonical_safe_integer(first, first + std::strlen(first), integer)' not in number:
        raise AssertionError("shared numeric strings no longer use canonical decimal parsing")
    if any('strtod(' in compact(text) for text in (rest_integer,mcp_call,number)):
        raise AssertionError("command arguments restored permissive strtod string parsing")


def self_test_request_intake_contract(parts: dict[str, str]) -> None:
    variants = (
        ("REST missing value cap", "api", "}, tk::kJsonMaxRequestValues)", "})"),
        ("configuration missing value cap", "config", "}, tk::kJsonMaxRequestValues)", "})"),
        ("MCP missing value cap", "mcp", "}, tk::kJsonMaxRequestValues)", "})"),
        ("REST cap falls through", "api", 'return send_json(req, 400, make_response(false, cmd, vin, "too many JSON values"));', '(void)send_json(req, 400, make_response(false, cmd, vin, "too many JSON values"));'),
        ("config cap falls through", "config", 'return {tk::JsonOwner{}, tk::ConfigSubmissionStatus::JsonTooManyValues};', '(void)tk::ConfigSubmissionStatus::JsonTooManyValues;'),
        ("REST duplicate bypass", "api", "if (tk::has_duplicate_json_keys(json.get()))", "if (false)"),
        ("config duplicate bypass", "config", "if (tk::has_duplicate_json_keys(json.get()))", "if (false)"),
        ("MCP duplicate implementation fork", "mcp_payloads", "using tk::has_duplicate_json_keys;", ""),
        ("MCP cap falls through", "mcp", 'return send_rpc_error_(req, {}, tk::kJsonRpcInvalidRequest, "too many JSON values");', '(void)send_rpc_error_(req, {}, tk::kJsonRpcInvalidRequest, "too many JSON values");'),
        ("REST number parser bypass", "api", "!tk::json_command_number(j, d)", "false"),
        ("MCP number parser bypass", "mcp", "!tk::json_command_number(j, number)", "false"),
        ("OTA request cap added", "ota", "cJSON_ParseWithLengthOpts(", "tk::kJsonMaxRequestValues + cJSON_ParseWithLengthOpts("),
    )
    for label,key,before,after in variants:
        if before not in parts[key]:
            raise AssertionError(f"request intake mutation did not apply: {label}")
        changed=dict(parts);changed[key]=parts[key].replace(before,after,1)+"\n// unchanged decoy: "+before+"\n"
        require_mutation_rejected(label,lambda changed=changed:require_request_intake_contract(changed))


def require_http_security_contract(parts: dict[str, str]) -> None:
    # Ignore comments while preserving literal header/value tokens. A comment cannot restore a guard.
    parts = {key: strip_cpp_comments(value) if key != "config" else value
             for key, value in parts.items()}
    server, status = parts["server"], parts["status"]
    dispatch = function_body_in(server, "handle_all_dispatch")
    gate = "tk::mutation_origin_required(POST, req->uri) && !browser_mutation_allowed(req)"
    if gate not in dispatch or "return reject_cross_origin_mutation(req);" not in dispatch:
        raise AssertionError("HTTP origin gate must use actual POST and reject before dispatch")
    require_before("origin rejection before route classification", dispatch, gate, "tk::classify_http_route")
    compact_dispatch = " ".join(dispatch.split())
    if f"if ({gate}) {{ return reject_cross_origin_mutation(req); }}" not in compact_dispatch:
        raise AssertionError("HTTP origin failure must return from its actual conditional branch")
    header = function_body_in(server, "read_header_bounded")
    if "if (len >= capacity) return false;" not in header:
        raise AssertionError("oversized provenance header must fail closed")
    browser = function_body_in(server, "browser_mutation_allowed")
    for token in (
        "const bool is_post = req->method == HTTP_POST;",
        'read_header_bounded(req, "Origin", origin, sizeof(origin))',
        'read_header_bounded(req, "Sec-Fetch-Site", fetch_site, sizeof(fetch_site))',
        'read_header_bounded(req, "Host", host, sizeof(host))',
        'httpd_req_get_hdr_value_len(req, "X-Requested-With") > 0',
        "origin_allowed_for_owned_addresses(req, is_post, host, origin, fetch_site,",
        "has_custom_header, device_ip)",
    ):
        if token not in browser:
            raise AssertionError(f"browser origin wiring missing {token!r}")
    origin = function_body_in(server, "origin_allowed_for_owned_addresses")
    for token in ('read_header_bounded(req, "Referer", referer, sizeof(referer))',
                  "const bool host_owned = owned_host_allowed(req, host, active_netif_ip);",
                  "tk::mutation_request_allowed_for_owned_host(is_post, host_owned, host, origin,",
                  "fetch_site, referer, has_custom_header"):
        if token not in origin:
            raise AssertionError(f"Referer/actual method delegation missing {token!r}")
    owned = function_body_in(server, "owned_host_allowed")
    for token in ("getsockname(fd,", "local.sin_family == AF_INET", "inet_ntop(AF_INET,",
                  "const std::string_view device_ips[] = {active_netif_ip, socket_ip};",
                  "char mdns_hostname[MDNS_NAME_BUF_LEN] = {};",
                  "if (mdns_hostname_get(mdns_hostname) != ESP_OK) mdns_hostname[0] = '\\0';",
                  "tk::device_host_allowed(host, device_ips, ip_count, mdns_hostname)",
                  "return tk::device_host_allowed(host, device_ips, ip_count, mdns_hostname);"):
        if token not in owned:
            raise AssertionError(f"owned socket/live mDNS wiring missing {token!r}")
    if '#if defined(CONFIG_LWIP_IPV6) && CONFIG_LWIP_IPV6\n#error ' not in server:
        raise AssertionError("HTTP IPv4-only socket seam must reject IPv6 enablement at compile time")
    if not re.search(r"(?m)^CONFIG_LWIP_IPV6=n$", parts["config"]):
        raise AssertionError("IPv6 enabled without a reviewed HTTP owned-address policy")
    if re.search(r"(?m)^CONFIG_LWIP_IPV6=y$", parts["config"]):
        raise AssertionError("a target enables unsupported IPv6 owned-address checks")
    diagnostic = function_body_in(server, "diagnostic_host_allowed")
    for token in ('read_header_bounded(req, "Host", host, sizeof(host))',
                  "active_ipv4(device_ip, sizeof(device_ip))",
                  "return owned_host_allowed(req, host, device_ip);"):
        if token not in diagnostic:
            raise AssertionError(f"diagnostic Host binding missing {token!r}")
    host_gate = "tk::http_route_requires_owned_host(verb, route) && !diagnostic_host_allowed(req)"
    if host_gate not in dispatch or '"device-owned Host required"' not in dispatch:
        raise AssertionError("diagnostic routes bypass owned-Host rejection")
    require_before("diagnostic Host rejection before handlers", dispatch, host_gate, "switch (route)")
    host_rejection = (f'if ({host_gate}) {{ httpd_resp_set_status(req, "403 Forbidden"); '
                      'httpd_resp_set_type(req, "text/plain; charset=utf-8"); '
                      'return httpd_resp_sendstr(req, "device-owned Host required"); }')
    if host_rejection not in compact_dispatch:
        raise AssertionError("diagnostic Host rejection must terminate with 403 before its handler")
    status_handler = function_body_in(status, "handle_status")
    if 'const bool redact = query_param_is(req, "redact", "1");' not in status_handler:
        raise AssertionError("/status no longer passes the requested redact selector")
    if "build_status_object(redact)" not in status_handler:
        raise AssertionError("/status ignores the selected redaction")
    if 'if (!query_param_is(req, "redact", "1"))' not in function_body_in(status, "handle_diag"):
        raise AssertionError("/diag ignores the requested redact selector")
    if parts["app"].count("{'X-Requested-With':'XMLHttpRequest'}") != 2:
        raise AssertionError("both shipped JSON helpers must send X-Requested-With")
    if re.search(r"Access-Control-Allow-Origin", parts["all_cpp"], re.IGNORECASE):
        raise AssertionError("HTTP must not grant cross-origin response access")
    index = function_body_in(status, "handle_index")
    for header in ('"X-Frame-Options", "DENY"',
                   '"Content-Security-Policy", "frame-ancestors \'none\'"'):
        token = f"httpd_resp_set_hdr(req, {header}) != ESP_OK"
        if token not in index:
            raise AssertionError(f"UI anti-framing header must fail before send: {header}")
        require_before("anti-framing header before HTML", index, token, "httpd_resp_send(req,")
    frame_guard = ("if (httpd_resp_set_hdr(req, \"X-Frame-Options\", \"DENY\") != ESP_OK || "
                   "httpd_resp_set_hdr(req, \"Content-Security-Policy\", \"frame-ancestors 'none'\") != ESP_OK) "
                   "{ return ESP_FAIL; }")
    if frame_guard not in " ".join(index.split()):
        raise AssertionError("anti-framing header allocation failure must not send HTML")
    if 'mdns_hostname_set(tk::kDeviceHostname)' not in parts["boot"]:
        raise AssertionError("mDNS boot name bypasses the shared constant")
    if len(re.findall(r"esp_netif_set_hostname\([^,]+, kDeviceHostname\)", parts["net"])) != 2:
        raise AssertionError("both network transports must use the shared DHCP hostname")
    if sum(x.count('"tesla-key-esp32"') for x in (parts["boot"], parts["net"], parts["origin"], parts["hostname"])) != 1:
        raise AssertionError("device hostname must have one literal owner")


def self_test_http_security_contract(parts: dict[str, str]) -> None:
    variants = (
        ("POST gate replaced with false", "server", "mutation_origin_required(POST,", "mutation_origin_required(false,"),
        ("oversized header absent", "server", "if (len >= capacity) return false;", "if (len >= capacity) return true;"),
        ("socket address omitted", "server", "{active_netif_ip, socket_ip}", "{active_netif_ip, \"\"}"),
        ("status redaction disabled", "status", 'const bool redact = query_param_is(req, "redact", "1");', 'const bool redact = false;'),
        ("diag redaction disabled", "status", 'if (!query_param_is(req, "redact", "1"))', 'if (true)'),
        ("UI header removed", "app", "{'X-Requested-With':'XMLHttpRequest'}", "{}"),
        ("wildcard CORS added", "all_cpp", "httpd_resp_set_type(req,", 'httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); httpd_resp_set_type(req,'),
        ("diagnostic Host gate removed", "server", "!diagnostic_host_allowed(req)", "false"),
        ("diagnostic rejection falls through", "server", 'return httpd_resp_sendstr(req, "device-owned Host required");', '(void)httpd_resp_sendstr(req, "device-owned Host required");'),
        ("live mDNS name ignored", "server", "ip_count, mdns_hostname)", "ip_count)"),
        ("checked Host bit replaced with true", "server", "is_post, host_owned, host, origin,", "is_post, true, host, origin,"),
        ("IPv6 compile guard removed", "server", "#if defined(CONFIG_LWIP_IPV6) && CONFIG_LWIP_IPV6", "#if 0"),
        ("IPv6 enabled", "config", "CONFIG_LWIP_IPV6=n", "CONFIG_LWIP_IPV6=y"),
        ("frame options removed", "status", '"X-Frame-Options", "DENY"', '"X-Frame-Options", "SAMEORIGIN"'),
        ("frame CSP removed", "status", '"Content-Security-Policy", "frame-ancestors \'none\'"', '"Content-Security-Policy", "default-src *"'),
        ("hostname duplicated", "boot", "mdns_hostname_set(tk::kDeviceHostname)", 'mdns_hostname_set("tesla-key-esp32")'),
    )
    for label, key, before, after in variants:
        mutated = dict(parts)
        if before not in parts[key]:
            raise AssertionError(f"HTTP security mutation did not apply: {label}")
        mutated[key] = parts[key].replace(before, after, 1)
        if key != "config":
            mutated[key] += "\n// unchanged decoy token: " + before + "\n"
        require_mutation_rejected(label, lambda mutated=mutated: require_http_security_contract(mutated))


def require_api_reply_contract(parts: dict[str, str]) -> None:
    parts = {key: re.sub(r"\[\[.*?\]\]", "", strip_cpp_comments(value))
             for key, value in parts.items()}
    compact = lambda text: re.sub(r"\s+", "", text)
    command = function_body_in(parts["api"], "handle_command")
    for state, reason in (("TooLarge", "request body too large"),
                          ("NoMemory", "out of memory"),
                          ("ReceiveFailed", "request body read failed")):
        branch = (f'if(body.status==tk::BodyReadStatus::{state}){{'
                  'return send_json(req,tk::body_read_http_status(body.status),'
                  f'make_response(false,cmd,vin,"{reason}"));}}')
        if compact(branch) not in compact(command):
            raise AssertionError(f"REST body status must use actual tested policy: {state}")
    sender = compact(function_body_in(parts["api"], "send_command_result"))
    expected = ('return send_effect_json(rq,effective_ok?200:502,make_response(effective_ok,cmd,vin,'
                'tk::command_result_text(effective_ok,err)));')
    if compact(expected) not in sender:
        raise AssertionError("REST command failure status/envelope drift")
    call = compact(function_body_in(parts["mcp"], "handle_tools_call_"))
    if compact('if(!info){request.reset();return send_rpc_error_(req,id,tk::kJsonRpcInvalidParams,"unknown tool");}') not in call:
        raise AssertionError("unknown MCP tool must be a terminal InvalidParams reply")
    for route, handler, name in (("SetTime", "handle_set_time", "set_time"),
                                 ("SetMqtt", "handle_set_mqtt", "set_mqtt"),
                                 ("SetSyslog", "handle_set_syslog", "set_syslog"),
                                 ("SetWifi", "handle_set_wifi", "set_wifi"),
                                 ("SetOta", "handle_set_ota", "set_ota")):
        body = compact(function_body_in(parts["config"], handler))
        branch = (f'if(tk::http_route_blocks_during_ota_update_409(tk::HttpRoute::{route})&&'
                  'ota_is_updating()){return send_json(req,static_cast<int>(tk::HttpRouteOtaConflict::Reject409),'
                  f'make_response(false,"{name}","",tk::kOtaUpdateInProgressReason.data()));}}')
        if compact(branch) not in body:
            raise AssertionError(f"{handler}: actual OTA409 policy/terminal guard drift")
    state = function_body_in(parts["mcp"], "vehicle_state_result_")
    state_c = compact(state)
    if (compact('const bool charge_available = g_vehicle->get_charge_state(cs);') not in state_c or
            compact('state.charge_available = charge_available;') not in state_c or
            compact('if (charge_available) {') not in state_c or 'get_cached_charge' in state):
        raise AssertionError("MCP charge availability bypasses the REST cache-only gate")
    for forbidden in ("connect(", "wake(", "get_vehicle_status(", "poll_charge_state("):
        if forbidden in state:
            raise AssertionError("MCP cached-state read gained live work")
    if compact('return send_json(req,200,json.release());') not in compact(function_body_in(parts["api"], "handle_body_controller")):
        raise AssertionError("body-controller HTTP200 compatibility policy drift")
    post = function_body_in(parts["mcp"], "mcp_handle_post")
    notification = ('if(id_status==tk::mcp_json::RpcIdStatus::Missing){'
                    'const bool is_notification=(method!=nullptr);msg.reset();'
                    'return is_notification?send_accepted_(req):'
                    'send_rpc_error_(req,{},tk::kJsonRpcInvalidRequest,"missing method");}')
    if compact(notification) not in compact(post):
        raise AssertionError("id-less method notification policy drift")
    server = compact(function_body_in(parts["server"], "handle_all"))
    exception = compact(function_body_in(parts["common"], "send_effect_exception"))
    if (server.count('result=send_effect_exception(rq);') != 2 or
            'httpd_resp_set_type(rq.req(),"text/plain")' not in exception or
            'transport.set_status(503)' not in exception):
        raise AssertionError("documented shared exception transport fallback drift")


def self_test_api_reply_contract(parts: dict[str, str]) -> None:
    variants = (
        ("REST413mapping", "api", 'tk::body_read_http_status(body.status)', '400'),
        ("RESTfailedcommand503", "api", 'effective_ok ? 200 : 502', 'effective_ok ? 200 : 503'),
        ("MCPunknowntoolcode", "mcp", 'tk::kJsonRpcInvalidParams, "unknown tool"', 'tk::kJsonRpcMethodNotFound, "unknown tool"'),
        ("MQTTOTAguardremoved", "config", 'tk::http_route_blocks_during_ota_update_409(tk::HttpRoute::SetMqtt)', 'false'),
        ("WiFiOTAguardfallthrough", "config", 'return send_json(req, static_cast<int>(tk::HttpRouteOtaConflict::Reject409),\n                         make_response(false, "set_wifi", "", tk::kOtaUpdateInProgressReason.data()));', '(void)send_json(req, static_cast<int>(tk::HttpRouteOtaConflict::Reject409),\n                         make_response(false, "set_wifi", "", tk::kOtaUpdateInProgressReason.data()));'),
        ("MCPcachedchargereadrestored", "mcp", 'g_vehicle->get_charge_state(cs)', 'g_vehicle->get_cached_charge().valid'),
        ("MCPchargegateremoved", "mcp", 'if (charge_available) {', 'if (true) {'),
        ("MCPchargeflagforged", "mcp", 'state.charge_available = charge_available;', 'state.charge_available = true;'),
        ("bodycontrollerstatuschanged", "api", 'return send_json(req, 200, json.release());', 'return send_json(req, 503, json.release());'),
        ("notificationdispatchpolicychanged", "mcp", 'const bool is_notification = (method != nullptr);', 'const bool is_notification = false;'),
        ("exceptionjsonpromisechanged", "common", 'httpd_resp_set_type(rq.req(), "text/plain")', 'httpd_resp_set_type(rq.req(), "application/json")'),
    )
    for label, key, before, after in variants:
        mutated = dict(parts)
        if before not in mutated[key]:
            raise AssertionError(f"API reply mutation did not apply: {label}")
        mutated[key] = mutated[key].replace(before, after, 1) + "\n// unchanged decoy token: " + before + "\n"
        require_mutation_rejected(label, lambda mutated=mutated: require_api_reply_contract(mutated))



def require_config_integrity_contract(parts: dict[str, str]) -> None:
    # These are call-site pins, complementary to the real config/NVS fault tests. Match executable
    # bodies after comment removal; a retained comment cannot substitute for a refusing branch.
    def body(key: str, name: str) -> str:
        code = normalize_preprocessor_text(parts[key])
        code = re.sub(r"\[\[gnu::noinline\]\]\s*|__attribute__\(\(noinline\)\)\s*", "", code)
        return re.sub(r"\s+", " ", function_body_in(code, name)).strip()

    routes = (
        ("handle_set_vin", "current", "set_vin", ', vin.c_str()'),
        ("handle_set_mqtt", "cfg", "set_mqtt", ''),
        ("handle_set_syslog", "cfg", "set_syslog", ''),
        ("apply_wifi_submission_", "cfg", "set_wifi", ''),
        ("handle_set_ota", "cfg", "set_ota", ''),
    )
    effects = re.compile(r"\b(?:cfg_save|reset_for_new_vehicle|apply_mqtt_submission_|"
                         r"apply_syslog_submission_|stage_wifi_change|ota_set_channel)\s*\(")
    for name, variable, command, suffix in routes:
        code = body("config", name)
        condition = f'if (!tk::cfg_load_for_update(*g_config, {variable}))'
        response = f'return send_config_unreadable_(req, "{command}"{suffix});'
        guard = re.search(re.escape(condition) + r"\s*(?:\{\s*)?" + re.escape(response), code)
        if not guard:
            raise AssertionError(f"{name}: authoritative load must refuse before configuration effects")
        effect = effects.search(code)
        if not effect or guard.end() >= effect.start():
            raise AssertionError(f"{name}: update guard no longer precedes its mutation/helper")
    unreadable = body("config", "send_config_unreadable_")
    if 'return send_json(req, 503, make_response(false, command, vin,' not in unreadable:
        raise AssertionError("unreadable config must return503/resultfalse before a write")

    portal = body("portal", "save_post_impl")
    guard = re.search(r'if \(!tk::cfg_load_for_update\(\*g_cfg, cfg\)\) \{([^{}]*)\}', portal)
    if not guard or not guard.group(1).strip().startswith('return portal_save_send(req, context,'):
        raise AssertionError("setup save must terminate on unreadable configuration")
    if 'return portal_save_send(req, context, "503 Service Unavailable", "text/html",' not in guard.group(1):
        raise AssertionError("setup unreadable configuration must report503")
    if any(token in guard.group(1) for token in ('cfg_save(', 'save_str(', 'esp_restart(')):
        raise AssertionError("setup unreadable branch must not persist/restart")
    effect = portal.find('tk::cfg_save(')
    if effect < 0 or guard.end() >= effect:
        raise AssertionError("setup authoritative load must precede its config save")

    boot = body("boot", "load_config_blob_verified")
    fatal = re.search(r'if \(cfg_blob_state == tk::ConfigLoadState::Error\) \{([^{}]*)\}', boot)
    if not fatal or not fatal.group(1).strip().endswith('boot_fatal("configuration verification");'):
        raise AssertionError("boot configuration read/decode errors must remain fatal")
    if 'tk::cfg_load_state(config_store, cfg_blob)' not in boot or 'retry < 3' not in boot:
        raise AssertionError("boot must retry the authoritative tri-state configuration load")


def self_test_config_integrity_contract(parts: dict[str, str]) -> None:
    changes = [
        ("VINload", "config", 'cfg_load_for_update(*g_config, current)', 'cfg_load(*g_config, current)'),
        ("MQTTload", "config", 'cfg_load_for_update(*g_config, cfg)) return send_config_unreadable_(req, "set_mqtt")', 'cfg_load(*g_config, cfg)) return send_config_unreadable_(req, "set_mqtt")'),
        ("Syslogload", "config", 'cfg_load_for_update(*g_config, cfg)) return send_config_unreadable_(req, "set_syslog")', 'cfg_load(*g_config, cfg)) return send_config_unreadable_(req, "set_syslog")'),
        ("WiFiload", "config", 'cfg_load_for_update(*g_config, cfg)) return send_config_unreadable_(req, "set_wifi")', 'cfg_load(*g_config, cfg)) return send_config_unreadable_(req, "set_wifi")'),
        ("OTAload", "config", 'cfg_load_for_update(*g_config, cfg)) return send_config_unreadable_(req, "set_ota")', 'cfg_load(*g_config, cfg)) return send_config_unreadable_(req, "set_ota")'),
        ("Portalload", "portal", 'cfg_load_for_update(*g_cfg, cfg)', 'cfg_load(*g_cfg, cfg)'),
        ("MQTTfallthrough", "config", 'return send_config_unreadable_(req, "set_mqtt");', '(void)send_config_unreadable_(req, "set_mqtt");'),
        ("Bootnotfatal", "boot", 'boot_fatal("configuration verification");', '(void)0;'),
        ("Unreadable200", "config", 'return send_json(req, 503,\n                     make_response(false, command, vin,', 'return send_json(req, 200,\n                     make_response(false, command, vin,'),
    ]
    for label, key, before, after in changes:
        changed = dict(parts)
        if before not in changed[key]:
            raise AssertionError(f"configuration integrity canary did not apply: {label}")
        changed[key] = changed[key].replace(before, after, 1) + '\n// decoy: ' + before + '\n'
        require_mutation_rejected(label, lambda changed=changed: require_config_integrity_contract(changed))



def require_vin_marker_format_contract(parts: dict[str, str]) -> None:
    config = normalize_preprocessor_text(parts["config"])
    callback = re.sub(r"\s+", " ", function_body_in(config, "handle_set_vin"))
    refusal = ('tk::make_vin_transition_marker(current.vin, previous_key_id); '
               'if (marker.empty()) return false; '
               'if (!g_config->save_str(tk::nvs_contract::kVinTransition, marker)) return false;')
    if refusal not in callback:
        raise AssertionError("VIN staging must refuse an empty marker before any journal/config write")
    pairing = normalize_preprocessor_text(parts["pairing"])
    pairing = re.sub(r'__attribute__\(\(noinline\)\)\s*', '', pairing)
    formatter = re.sub(r"\s+", " ", function_body_in(pairing, "compute_key_fingerprint_"))
    if ('const auto formatted = tk::key_fingerprint_text(sha, sizeof(sha)); '
            'fp = formatted.data();') not in formatter:
        raise AssertionError("IDF fingerprint output must delegate to the actual tested formatter")


def self_test_vin_marker_format_contract(parts: dict[str, str]) -> None:
    for label, key, before, after in (
        ("emptyVINmarkerwritten", "config", 'if (marker.empty()) return false;', '(void)marker.empty();'),
        ("fingerprintformatterbypassed", "pairing", 'tk::key_fingerprint_text(sha, sizeof(sha))', 'std::array<char, 12>{}'),
    ):
        changed = dict(parts)
        if before not in changed[key]:
            raise AssertionError(f"VIN format canary did not apply: {label}")
        changed[key] = changed[key].replace(before, after, 1) + '\n// decoy: ' + before + '\n'
        require_mutation_rejected(label, lambda changed=changed: require_vin_marker_format_contract(changed))


def require_clock_integration_contract(parts: dict[str, str]) -> None:
    def body(key: str, name: str) -> str:
        code = re.sub(r"__attribute__\(\(noinline\)\)\s*", "", normalize_preprocessor_text(parts[key]))
        return re.sub(r"\s+", " ", function_body_in(code, name))
    startup = body("boot", "app_main")
    require_before("clock before HTTP", startup, "initialize_clock_gate_();", "http_server_start(")
    require_before("clock before SNTP", startup, "initialize_clock_gate_();", "esp_sntp_init();")
    require_before("explicit IMMED", startup, "sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);", "esp_sntp_init();")
    if "sntp_set_time_sync_notification_cb" in startup:
        raise AssertionError("strong clock hook must not also register a notification writer")
    for name in ("apply_browser_clock", "apply_browser_clock_sample"):
        code = body("boot", name)
        if any(token in code for token in ("save_str(", "checkpoint_ntp_time_(", ".store(true", "mark_clock_authoritative")):
            raise AssertionError("browser estimate must not persist or establish authority")
    rotation = body("pairing", "generate_key_locked_")
    for token in ("tk::stamp_or_retire_key_date(", "tk::historical_date_valid(now, clock_plausibility_ceiling())",
                  "storage_->save_str(tk::nvs_contract::kKeyCreated", "storage_->remove(tk::nvs_contract::kKeyCreated)"):
        if token not in rotation:
            raise AssertionError("key date must use the tested bounded stamp-or-retire path")
    require_before("key authority before time", rotation, "clock_is_authoritative()", "time(nullptr)")
    if 'return clock_synced_via_ntp();' not in body("boot", "clock_is_authoritative"):
        raise AssertionError("only NTP establishes durable-date authority")


def self_test_clock_integration_contract(parts: dict[str, str]) -> None:
    variants = (
        ("clockinitremoved", "boot", "    initialize_clock_gate_();", "    ;"),
        ("smoothmodeenabled", "boot", "sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);", "sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);"),
        ("keydatetemplatebypassed", "pairing", "tk::stamp_or_retire_key_date(", "unreviewed_stamp_or_retire("),
        ("keydateboundsbypassed", "pairing", "tk::historical_date_valid(now, clock_plausibility_ceiling())", "true"),
        ("browserauthorityrestored", "boot", "BrowserClockResult apply_browser_clock(double milliseconds) noexcept {",
         "BrowserClockResult apply_browser_clock(double milliseconds) noexcept { s_ntp_synced.store(true);"),
    )
    for label, key, before, after in variants:
        mutated = dict(parts)
        if before not in mutated[key]: raise AssertionError("clock canary did not apply: " + label)
        mutated[key] = mutated[key].replace(before, after, 1) + "\n// decoy: " + before + "\n"
        require_mutation_rejected(label, lambda mutated=mutated: require_clock_integration_contract(mutated))


def require_config_encode_lease_contract(parts: dict[str, str]) -> None:
    blob = normalize_preprocessor_text(parts["blob"])
    save = re.sub(r"\s+", " ", function_body_in(blob, "cfg_save"))
    require_before("encode operation lease", save, "ConfigEncodeLease lease;", "if (!lease.held) return false;")
    require_before("busy before encoder", save, "if (!lease.held) return false;", "config_blob_encode(")
    if "auto& buf = s_encode_buffer;" not in save or "ConfigBlobBuffer buf" in save:
        raise AssertionError("config serialization must use the private static buffer")
    start = blob.index("struct ConfigEncodeLease")
    opening = blob.index("{", start)
    end = balanced_end(scrub_cpp_preserving_layout(blob), opening, "{", "}", "config encode lease")
    lease = re.sub(r"\s+", " ", blob[start:end+1])
    for token in ("std::atomic_flag s_encode_active = ATOMIC_FLAG_INIT;", "ConfigBlobBuffer s_encode_buffer{};"):
        if token not in blob: raise AssertionError("config buffer/permit must remain private static owners")
    for token in ("held(!s_encode_active.test_and_set(std::memory_order_acquire))", "if (!held) return;",
                  "volatile uint8_t* bytes = s_encode_buffer.data();", "i < s_encode_buffer.size()", "bytes[i] = 0;"):
        if token not in lease: raise AssertionError("config lease admission/full volatile wipe drift")
    require_before("wipe before publication", lease, "bytes[i] = 0;", "s_encode_active.clear(std::memory_order_release);")
    if any(token in lease for token in ("ESP_LOG", "save_", "xSemaphore", "vTaskDelay", "new ")):
        raise AssertionError("config lease construction/destruction must not call out or wait")


def self_test_config_encode_lease_contract(parts: dict[str, str]) -> None:
    variants = (
        ("configbusyfallthrough", "blob", "if (!lease.held) return false;", "if (!lease.held) {}"),
        ("configwipeomitted", "blob", "bytes[i] = 0;", "(void)bytes[i];"),
        ("configwipenonvolatile", "blob", "volatile uint8_t* bytes", "uint8_t* bytes"),
        ("configreleasebeforewipe", "blob", "for (size_t i = 0; i < s_encode_buffer.size(); ++i) bytes[i] = 0;\n        s_encode_active.clear(std::memory_order_release);", "s_encode_active.clear(std::memory_order_release);\n        for (size_t i = 0; i < s_encode_buffer.size(); ++i) bytes[i] = 0;"),
    )
    for label, key, before, after in variants:
        mutated = dict(parts)
        if before not in mutated[key]: raise AssertionError("config lease canary not applied: " + label)
        mutated[key] = mutated[key].replace(before, after, 1) + "\n// decoy: " + before + "\n"
        require_mutation_rejected(label, lambda mutated=mutated: require_config_encode_lease_contract(mutated))


def require_long_uptime_clocks(telemetry: str, trend: str, supervisor: str) -> None:
    """PKG-13: long-lived clocks stay at their actual migrated consumers."""
    loop = scrub_cpp(function_body_in(telemetry, "loop_task_fn_"))
    clock = "tk::uptime_seconds(static_cast<uint64_t>(esp_timer_get_time()))"
    if f"const uint32_t now_s = {clock};" not in " ".join(loop.split()):
        raise AssertionError("bootstrap seconds must use the production 64-bit timer conversion")
    iteration = scrub_cpp(function_body_in(supervisor, "supervisor_iteration"))
    calls = call_arguments(iteration, "heap_trend_record")
    if len(calls) != 1 or calls[0][0].strip() != "uptime_seconds(static_cast<uint64_t>(esp_timer_get_time()))":
        raise AssertionError("independent heap history must use the long-lived timer conversion")
    monotonic = " ".join(scrub_cpp(function_body_in(supervisor, "monotonic_ms")).split())
    if monotonic != "{ return static_cast<uint64_t>(esp_timer_get_time()) / 1000; }":
        raise AssertionError("supervisor maintenance clock must preserve 64-bit monotonic milliseconds")
    short = "{largest, static_cast<uint32_t>(now_ms), ota_is_busy()}"
    if short not in " ".join(iteration.split()):
        raise AssertionError("critical hold must retain wrap-safe low-32-bit millisecond differences")
    adoption = " ".join(scrub_cpp(function_body_in(trend, "adopt_or_reset")).split())
    if "heap_persist_valid(persist()) && heap_carry_fits(persist().ring)" not in adoption:
        raise AssertionError("retained history adoption must reject an overflowing carry")
    record = " ".join(scrub_cpp(function_body_in(trend, "heap_trend_record")).split())
    if "if (!heap_timeline_fits(monotonic_s, s_carry_s)) { persist().ring.reset(); s_carry_s = 0;" not in record:
        raise AssertionError("a retained history sum overflow must start an empty timeline")


def self_test_long_uptime_clocks() -> None:
    parts = {name: SOURCES[name + ".cpp"] for name in
             ("vehicle_telemetry", "heap_trend", "heap_supervisor")}
    variants = (
        ("bootstrap tick rollover", "vehicle_telemetry", "const uint32_t now_s = tk::uptime_seconds(static_cast<uint64_t>(esp_timer_get_time()));", "const uint32_t now_s = now_ticks / configTICK_RATE_HZ;"),
        ("heap millisecond rollover", "heap_supervisor", "heap_trend_record(uptime_seconds(static_cast<uint64_t>(esp_timer_get_time())),", "heap_trend_record(static_cast<uint32_t>(now_ms) / 1000,"),
        ("supervisor millisecond rollover", "heap_supervisor", "return static_cast<uint64_t>(esp_timer_get_time()) / 1000;", "return static_cast<uint32_t>(esp_timer_get_time() / 1000);"),
        ("unchecked retained carry", "heap_trend", " && heap_carry_fits(persist().ring)", ""),
        ("unchecked retained sum", "heap_trend", "if (!heap_timeline_fits(monotonic_s, s_carry_s))", "if (false)"),
    )
    for label, owner, before, after in variants:
        changed = dict(parts)
        if changed[owner].count(before) != 1:
            raise AssertionError("PKG-13 canary missing/ambiguous: " + label)
        changed[owner] = changed[owner].replace(before, after, 1) + "\n// decoy: " + before + "\n"
        require_mutation_rejected("PKG-13 " + label, lambda c=changed: require_long_uptime_clocks(
            c["vehicle_telemetry"], c["heap_trend"], c["heap_supervisor"]))


def ota_program_contract():
    return runpy.run_path(str(ROOT / "test/ota_operation_contract.py"))


def require_ota_download_chain(source: str) -> None:
    ota_program_contract()["check_source"](source)


def require_ota_https_configuration(source: str) -> None:
    code = strip_cpp_comments(source)
    depth = 0
    assertions = set()
    for line in code.splitlines():
        directive = re.match(r"\s*#\s*(if|ifdef|ifndef|endif)\b", line)
        if directive:
            depth += -1 if directive[1] == "endif" else 1
        if depth == 0:
            match = re.fullmatch(r"\s*static_assert\(tk::ota_https_url\((CONFIG_TESLA_OTA_(?:MANIFEST_URL|FIRMWARE_BASE_URL))\),\s*", line)
            if match:assertions.add(match[1])
    if assertions != {"CONFIG_TESLA_OTA_MANIFEST_URL", "CONFIG_TESLA_OTA_FIRMWARE_BASE_URL"}:
        raise AssertionError("both configured OTA URLs require unconditional compile-time HTTPS assertions")


def self_test_ota_download_chain(source: str) -> None:
    contract=ota_program_contract()
    contract["check_source"](source)
    contract["self_test"]()
    for macro in ("CONFIG_TESLA_OTA_MANIFEST_URL", "CONFIG_TESLA_OTA_FIRMWARE_BASE_URL"):
        assertion=f"static_assert(tk::ota_https_url({macro}),"
        if assertion not in source:raise AssertionError("missing HTTPS assertion")
        require_mutation_rejected("OTA configured HTTPS refusal",lambda changed=source.replace(assertion,"static_assert(true,",1):require_ota_https_configuration(changed))


# F116 closed source programs: fixed POD/critical-section helpers only. Update after an
# independent source review; the extractor never writes these expected identities at runtime.
BLE_DEFERRED_PROGRAMS = {'ble_link_event_cb_': {'owner': 'ctrl', 'sha256': 'edc6860b2cb2e65cc7e642533be606d1a1a48d47395505d6ce94ee189d284733'}, 'ble_rx_event_cb_': {'owner': 'ctrl', 'sha256': '9f8462cd54ef462d6454ba9ca02cdd4a1997bcb3211de9bb32c67d0648cae6ac'}, 'enqueue_ble_link_event_': {'owner': 'ctrl', 'sha256': 'fa4976fc3cfc73f7915314e67ca7b2c85f8bf9e244d71e77b0ea0ed887f1e056'}, 'enqueue_ble_rx_event_': {'owner': 'ctrl', 'sha256': 'e7e705125cab17d42e83546a1d46915a961c3300097f7b2b6ee5459cc1e45af6'}, 'ble_host_events_ready_': {'owner': 'ctrl', 'sha256': 'e8289f7fa61e7f2c9003f368d8d7537a8d5f5fdc59be566c18a2e4bbcda29183'}, 'pop_ble_host_event_': {'owner': 'ctrl', 'sha256': 'f7807f5ba8cee806ba6739e78040f403481ca5561dd1c048ced2cada2360cb6a'}, 'clear_ble_host_events_': {'owner': 'ctrl', 'sha256': 'b9d23918bd68e77d85c448c6fea5a69527c6d6a8ea31a1d57fa1cff228bb1d90'}, 'process_ble_host_events_': {'owner': 'telemetry', 'sha256': '9371140efaaa009bd9f6e740c186dc3c444a3e60b25446ed21260ba2f678286f'}}
BLE_DEFERRED_QUEUE_SHA256 = "3513279776711593ae9eeee7bc672e38f380c0440e0ee2165ce5876aceefc7c2"


def require_ble_deferred_program_contract(parts: dict[str, str], queue: str) -> None:
    if hashlib.sha256(queue.encode()).hexdigest() != BLE_DEFERRED_QUEUE_SHA256:
        raise AssertionError("deferred FIFO pure program drift requires review")
    for name, expected in BLE_DEFERRED_PROGRAMS.items():
        body = function_body_in(parts[expected["owner"]], name)
        if hashlib.sha256(body.encode()).hexdigest() != expected["sha256"]:
            raise AssertionError("deferred mailbox complete program drift: " + name)
    init = scrub_cpp(function_body_in(parts["ctrl"], "init"))
    require_before("deferred publication", init, "ble_events_.queue.clear()", "ble_events_.ready = true")
    require_before("deferred callback lifetime", init, "ble_events_.ready = true", "set_connected_cb")
    require_before("deferred RX lifetime", init, "ble_events_.ready = true", "set_rx_data_cb")


def self_test_ble_deferred_program_contract(parts: dict[str, str], queue: str) -> None:
    # Lexical source-gate controls are distinct from the compiled runtime omission controls.
    for name, expected in BLE_DEFERRED_PROGRAMS.items():
        body = function_body_in(parts[expected["owner"]], name)
        changed = {**parts, expected["owner"]: parts[expected["owner"]].replace(body, body[:-1] + " std::string forbidden; }", 1)}
        require_mutation_rejected("deferred whole-program " + name,
            lambda changed=changed: require_ble_deferred_program_contract(changed, queue))
    require_mutation_rejected("deferred pure program append",
        lambda: require_ble_deferred_program_contract(parts, queue + "\n// unreviewed source\n"))


def require_concurrency_regression_contract(parts: dict[str, str]) -> None:
    """Pin reviewed shell seams; this lexical contract is not a concurrency/stack proof."""
    common = scrub_cpp(parts["common"])
    if not re.search(r"static\s+constexpr\s+size_t\s+MAX_BODY_LEN\s*=\s*2048\s*;", common):
        raise AssertionError("HTTP shell body allocation cap must remain 2048")
    registrations = [args for args in call_arguments(scrub_cpp(parts["ctrl"]), "xTaskCreate")
                     if args and args[0].strip() == "loop_task_fn_"]
    if len(registrations) != 1 or registrations[0][2].strip() != "10240":
        raise AssertionError("vehicle loop stack allocation must remain reviewed 10240 bytes")

    for owner, name in (("header", "copy_locked_"), ("commands", "set_last_command_error"),
                        ("commands", "publish_command_outcome_")):
        if owner == "header":
            code = scrub_cpp_preserving_layout(parts[owner])
            matches = list(re.finditer(r"\bcopy_locked_\s*\([^{};]*\)\s*\{", code))
            if len(matches) != 1: raise AssertionError("cache copy template definition is ambiguous")
            opening = code.index("{", matches[0].start())
            body = code[opening:balanced_end(code, opening, "{", "}", name) + 1]
        else:
            body = scrub_cpp(function_body_in(parts[owner], name))
        if re.search(r"\bESP_LOG[A-Z]*\s*\(", body):
            raise AssertionError(f"leaf result/cache helper {name} may not log")

    for owner, name in (("commands", "send_vcsec_locked_"),
                        ("commands", "send_infotainment_locked_"),
                        ("commands", "await_completion_"),
                        ("telemetry", "get_vehicle_status")):
        body = scrub_cpp_preserving_layout(function_body_in(parts[owner], name))
        for match in re.finditer(r"\btk::(?:SemGuard|MutexGuard)\s+\w+\s*\(\s*vehicle_mutex_\s*[,)]", body):
            opened = []
            for index, char in enumerate(body[:match.start()]):
                if char == "{": opened.append(index)
                elif char == "}": opened.pop()
            if not opened: raise AssertionError("Vehicle guard lost its enclosing scope")
            closing = balanced_end(body, opened[-1], "{", "}", name)
            tail = body[match.end():closing]
            if re.search(r"\bawait_completion_\s*\(|\bxSemaphoreTake\s*\(\s*completion->sem", tail):
                raise AssertionError(f"{name} waits for completion under Vehicle ownership")
        if re.search(r"\bxSemaphoreTake\s*\(\s*vehicle_mutex_", body):
            raise AssertionError(f"{name} bypasses reviewed Vehicle RAII ownership")

    events = scrub_cpp_preserving_layout(function_body_in(parts["telemetry"], "process_ble_host_events_"))
    prefix = (r"for\s*\(\s*;\s*;\s*\)\s*\{\s*"
              r"if\s*\(\s*!pop_ble_host_event_\(event,\s*&empty_ms\)\s*\)\s*break\s*;\s*"
              r"const\s+uint32_t\s+current_generation\s*=\s*ble_->lifecycle_generation\(\)\s*;\s*"
              r"if\s*\(\s*!tk::ble_deferred_event_may_apply\(event.kind,\s*event.generation,\s*current_generation\)\s*\)\s*\{\s*continue\s*;\s*\}")
    if not re.search(prefix, events):
        raise AssertionError("deferred BLE event must reject stale generation first in each queue iteration")
    status = scrub_cpp(function_body_in(parts["telemetry"], "get_vehicle_status"))
    if not re.search(r"if\s*\(\s*cmd\s*&&\s*cmd->id\s*==\s*completion->cmd_id\s*\)\s*\{[^{}]*command_runner_\.pop_current\(\);", status):
        raise AssertionError("status cleanup must pop only its own command")
    callback = scrub_cpp(function_body_in(parts["commands"], "make_result_cb_"))
    if not re.search(r"return\s*\[[^\]]+\]\s*\([^)]*\)\s*\{\s*if\s*\(\s*command_generation_\.load\(\)\s*!=\s*generation\s*\)\s*return\s*;", callback):
        raise AssertionError("result callback must reject stale generation before state changes")


def self_test_concurrency_regression_contract(parts: dict[str, str]) -> None:
    cases = (
        ("HTTP body cap increase", "common", "MAX_BODY_LEN = 2048", "MAX_BODY_LEN = 16777216"),
        ("vehicle stack decrease", "ctrl", '"vehicle_loop", 10240', '"vehicle_loop", 8192'),
        ("logging under cache lock", "header", "tk::SemGuard g(cache_mutex_);\n        return src;",
         'tk::SemGuard g(cache_mutex_);\n        ESP_LOGW(TAG, "mutant"); return src;'),
        ("logging under result lock", "commands", "tk::SemGuard g(result_mutex_);\n    last_error_ = err;",
         'tk::SemGuard g(result_mutex_);\n    ESP_LOGW(TAG, "mutant"); last_error_ = err;'),
        ("completion wait under Vehicle lock", "commands",
         "out = await_completion_(completion, generation, deadline, name.c_str(), timeout_policy);",
         "tk::SemGuard regression(vehicle_mutex_);\n    out = await_completion_(completion, generation, deadline, name.c_str(), timeout_policy);"),
        ("waiter directly owns Vehicle while waiting", "commands", "    const bool signalled =",
         "    tk::SemGuard regression(vehicle_mutex_);\n    const bool signalled ="),
        ("stale BLE event applied", "telemetry", "if (!tk::ble_deferred_event_may_apply(event.kind, event.generation,",
         "if (false && !tk::ble_deferred_event_may_apply(event.kind, event.generation,"),
        ("idle expiry loses its empty-queue clock", "telemetry", "if (!pop_ble_host_event_(event, &empty_ms)) break;",
         "if (!pop_ble_host_event_(event)) break;"),
        ("status pops unrelated command", "telemetry", "if (cmd && cmd->id == completion->cmd_id)", "if (cmd)"),
        ("late result mutates newer request", "commands", "      if (command_generation_.load() != generation) return;",
         "      if (false && command_generation_.load() != generation) return;"),
    )
    for label, owner, old, new in cases:
        if parts[owner].count(old) != 1: raise AssertionError(f"{label} mutation does not bind exactly once")
        changed = {**parts, owner: parts[owner].replace(old, new, 1)}
        require_mutation_rejected(label, lambda mutated=changed: require_concurrency_regression_contract(mutated))


def require_http_ota_deferred_contract(parts: dict[str, str]) -> None:
    """The plain capture is valid only on one server's serialized request/control turns."""
    references = {
        "ota_check_start_after_http": {"ota_update.cpp": 1, "ota_update.hpp": 1, "http_ota.cpp": 1},
        "ota_check_after_http_work": {"ota_update.cpp": 2},
        "handle_ota_check": {"http_ota.cpp": 1, "http_handlers.hpp": 1, "http_server.cpp": 1},
        "handle_all_dispatch": {"http_server.cpp": 2},
        "handle_all": {"http_server.cpp": 3},
    }
    for symbol, expected in references.items():
        actual = {name: len(re.findall(r"\b" + symbol + r"\b", scrub_cpp(source)))
                  for name, source in parts.items()}
        if {name: count for name, count in actual.items() if count} != expected:
            raise AssertionError(f"deferred OTA HTTP-only caller surface changed: {symbol}")
    handler = " ".join(scrub_cpp(function_body_in(parts["http_ota.cpp"], "handle_ota_check")).split())
    if "const auto result = ota_check_start_after_http(pr, req->handle);" not in handler:
        raise AssertionError("deferred OTA must copy this request's server handle")
    dispatch = " ".join(scrub_cpp(function_body_in(parts["http_server.cpp"], "handle_all_dispatch")).split())
    if "case tk::HttpRoute::OtaCheck: return handle_ota_check(rq);" not in dispatch:
        raise AssertionError("deferred OTA must remain in the synchronous wildcard dispatch")
    if "httpd_req_async_handler_begin" in scrub_cpp(parts["http_server.cpp"]):
        raise AssertionError("deferred OTA cannot inherit asynchronous request dispatch")
    guard = ('#if defined(CONFIG_HTTPD_QUEUE_WORK_BLOCKING) && CONFIG_HTTPD_QUEUE_WORK_BLOCKING\n'
             '#error "Deferred OTA metadata admission requires nonblocking HTTP work submission"\n'
             '#endif')
    if guard not in strip_cpp_comments(parts["ota_update.cpp"]):
        raise AssertionError("deferred OTA lost its nonblocking SDK configuration guard")


def self_test_http_ota_deferred_contract(parts: dict[str, str]) -> None:
    cases = (
        ("non-HTTP caller", "extra.cpp", "void f() { ota_check_start_after_http(0, nullptr); }"),
        ("concurrent callback caller", "extra.cpp", "void f() { ota_check_after_http_work(nullptr); }"),
        ("direct handler caller", "extra.cpp", "void f() { handle_ota_check({}); }"),
        ("foreign server", "http_ota.cpp", parts["http_ota.cpp"].replace("pr, req->handle", "pr, nullptr")),
        ("inline admission", "http_ota.cpp", parts["http_ota.cpp"].replace("ota_check_start_after_http(pr, req->handle)", "ota_check_start(pr)")),
        ("async dispatch", "http_server.cpp", parts["http_server.cpp"] + "\nvoid f() { httpd_req_async_handler_begin(0,0); }"),
        ("blocking queue", "ota_update.cpp", parts["ota_update.cpp"].replace("#if defined(CONFIG_HTTPD_QUEUE_WORK_BLOCKING) && CONFIG_HTTPD_QUEUE_WORK_BLOCKING", "#if 0")),
    )
    for label, name, source in cases:
        require_mutation_rejected(label, lambda changed={**parts, name: source}:
                                  require_http_ota_deferred_contract(changed))
        require_http_ota_deferred_contract(parts)


def main() -> int:
    tasks = task_inventory(ALL_CODE)
    callbacks = callback_inventory(ALL_CODE)
    check_contract(tasks, callbacks)
    self_test_canaries(tasks, callbacks)
    # Include untracked/new C++ files as well as the closed CMake/header inventory. An added
    # non-HTTP caller must not evade the single-server premise before CMake enrolment.
    deferred_parts = {**MAIN_CODE, **{p.relative_to(MAIN).as_posix(): p.read_text()
                      for p in MAIN.rglob("*") if p.is_file() and p.suffix in (".cpp", ".cc", ".cxx")}}
    require_http_ota_deferred_contract(deferred_parts)
    self_test_http_ota_deferred_contract(deferred_parts)
    self_test_f135_programs(f135_program_parts())
    self_test_f135_ping_targets(MAIN_CODE["ping_probe.hpp"], SOURCES["net.cpp"], SOURCES["syslog.cpp"])
    self_test_f137_source_contract({"mqtt": SOURCES["mqtt_ha.cpp"], "net": SOURCES["net.cpp"],
                                  "ble": SOURCES["ble_client.cpp"], "class": (MAIN / "ble_client.hpp").read_text(),
                                  "wdt": (MAIN / "task_watchdog_subscription.hpp").read_text()})
    self_test_long_uptime_clocks()
    concurrency_parts = {"common": SOURCES["http_common.cpp"], "ctrl": SOURCES["vehicle_ctrl.cpp"],
                         "commands": SOURCES["vehicle_commands.cpp"], "telemetry": SOURCES["vehicle_telemetry.cpp"],
                         "header": (MAIN / "vehicle_ctrl.hpp").read_text()}
    deferred_queue_source = (MAIN / "logic/ble_deferred_queue.hpp").read_text(encoding="utf-8")
    require_ble_deferred_program_contract(concurrency_parts, deferred_queue_source)
    self_test_ble_deferred_program_contract(concurrency_parts, deferred_queue_source)
    require_concurrency_regression_contract(concurrency_parts)
    self_test_concurrency_regression_contract(concurrency_parts)
    require_ota_download_chain(SOURCES["ota_update.cpp"])
    require_ota_https_configuration(SOURCES["ota_update.cpp"])
    self_test_ota_download_chain(SOURCES["ota_update.cpp"])
    recovery_sources = {
        "pairing": SOURCES["vehicle_pairing.cpp"],
        "boot": SOURCES["vehicle_ctrl.cpp"],
        "telemetry": SOURCES["vehicle_telemetry.cpp"],
        "ctrl_header": (MAIN / "vehicle_ctrl.hpp").read_text(encoding="utf-8"),
    }
    require_vehicle_recovery_contract(**recovery_sources)
    self_test_vehicle_recovery_canaries(recovery_sources)
    pairing_sources = {
        "pairing": SOURCES["vehicle_pairing.cpp"],
        "commands": SOURCES["vehicle_commands.cpp"],
        "telemetry": SOURCES["vehicle_telemetry.cpp"],
    }
    require_pairing_recovery_contract(**pairing_sources)
    self_test_pairing_recovery_canaries(**pairing_sources)
    wake_sources = {**pairing_sources, "ctrl_header": recovery_sources["ctrl_header"]}
    require_wake_result_contract(**wake_sources)
    self_test_wake_result_contract(wake_sources)
    intake_parts = {
        "api": SOURCES["http_api.cpp"], "config": SOURCES["http_config.cpp"],
        "mcp": SOURCES["mcp_server.cpp"], "ota": SOURCES["ota_update.cpp"],
        "mcp_payloads": (MAIN / "mcp_json_payloads.hpp").read_text(),
        "syntax": (MAIN / "logic/json_syntax.hpp").read_text(),
        "shared": (MAIN / "json_input_validation.hpp").read_text(),
    }
    reply_parts = {**intake_parts, "server": SOURCES["http_server.cpp"], "common": SOURCES["http_common.cpp"]}
    require_api_reply_contract(reply_parts)
    self_test_api_reply_contract(reply_parts)
    require_request_intake_contract(intake_parts)
    self_test_request_intake_contract(intake_parts)
    http_parts = {
        "server": SOURCES["http_server.cpp"], "status": SOURCES["http_status.cpp"],
        "boot": SOURCES["main.cpp"], "net": SOURCES["net.cpp"],
        "app": (MAIN / "www/app.js").read_text(),
        "config": "\n".join(p.read_text() for p in sorted(ROOT.glob("sdkconfig.defaults*"))),
        "all_cpp": "\n".join(SOURCES.values()),
        "origin": (MAIN / "logic/http_origin.hpp").read_text(),
        "hostname": (MAIN / "logic/device_hostname.hpp").read_text(),
    }
    require_http_security_contract(http_parts)
    self_test_http_security_contract(http_parts)
    config_parts = {"config": SOURCES["http_config.cpp"], "portal": SOURCES["provisioning.cpp"], "boot": SOURCES["main.cpp"]}
    require_config_integrity_contract(config_parts)
    self_test_config_integrity_contract(config_parts)
    vin_parts = {"config": SOURCES["http_config.cpp"], "pairing": SOURCES["vehicle_pairing.cpp"]}
    require_vin_marker_format_contract(vin_parts)
    self_test_vin_marker_format_contract(vin_parts)
    clock_parts = {"boot": SOURCES["main.cpp"], "pairing": SOURCES["vehicle_pairing.cpp"]}
    require_clock_integration_contract(clock_parts)
    self_test_clock_integration_contract(clock_parts)
    encode_parts = {"blob": SOURCES["config_blob.cpp"], "http": SOURCES["http_config.cpp"]}
    require_config_encode_lease_contract(encode_parts)
    self_test_config_encode_lease_contract(encode_parts)
    print(f"OK runtime C-boundary inventory ({len(tasks)} tasks, {len(callbacks)} callbacks)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
