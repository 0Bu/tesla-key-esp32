#!/usr/bin/env python3
"""Fail closed when a new C/RTOS callback boundary appears without review."""

from __future__ import annotations

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
    "healthy_timer_task",
    "ota_check_task",
    "ota_task",
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
    "on_sync_cb",
    "on_reset_cb",
    "gap_event_cb",
    "svc_disc_cb",
    "chr_disc_cb",
    "dsc_disc_cb",
    "subscribe_write_cb",
    "scan_timeout_cb",
    "form_get",
    "save_post",
    "wifi_event_handler",
    "eth_event_handler",
    "mqtt_event_handler",
    "on_mqtt_probe",
    "ping_probe_on_end",
    "on_time_sync",
    "handle_all",
    "diag_vprintf_",
    "app_main",
    "ble_link_event_cb_",
    "ble_rx_event_cb_",
}

# These boundaries contain their own catch-all in the registered function/task body. The gate
# inspects the body rather than trusting this list alone, so deleting the catch makes it red.
CONTAINED = {
    "publisher_task",
    "healthy_timer_task",
    "ota_check_task",
    "ota_task",
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
    "on_time_sync",
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
}

DELEGATE_ALLOWED_CALLS = {
    "gap_event_cb": {"on_gap_event"},
    "svc_disc_cb": {"ble_client_instance", "on_svc_disc"},
    "chr_disc_cb": {"ble_client_instance", "on_chr_disc"},
    "dsc_disc_cb": {"ble_client_instance", "on_dsc_disc"},
    "subscribe_write_cb": {"ble_client_instance", "on_subscribe_write"},
}

# Reviewed exceptions are deliberately restricted to fixed-buffer/C/atomic paths. Adding another
# exception is a code-review event because it weakens the default rule that C frames need a catch.
REVIEWED_NON_THROWING = {
    "dns_task": "fixed stack buffer and socket C calls only",
    "nimble_host_task": "NimBLE run/deinit trampoline only",
    "on_mqtt_probe": "plain-data probe verdict plus semaphore give only",
    "ping_probe_on_end": "generation-atomic ping result plus semaphore give only",
    "diag_vprintf_": "fixed-buffer/C/queue-only",
    "ble_link_event_cb_": "POD cast plus fixed queue delegate only",
    "ble_rx_event_cb_": "POD cast plus fixed queue delegate only",
}

REVIEWED_ALLOWED_CALLS = {
    "dns_task": {
        "socket", "ESP_LOGW", "vTaskDelete", "htonl", "htons", "bind", "close",
        "recvfrom", "memcpy", "sendto",
    },
    "nimble_host_task": {"nimble_port_run", "nimble_port_freertos_deinit"},
    "on_mqtt_probe": {"xSemaphoreGive"},
    "ping_probe_on_end": {"esp_ping_get_profile", "complete", "xSemaphoreGive"},
    "diag_vprintf_": {
        "va_copy", "vsnprintf", "va_end", "diag_append_", "syslog_send", "s_prev", "vprintf",
    },
    "ble_link_event_cb_": {"static_cast", "enqueue_ble_link_event_"},
    "ble_rx_event_cb_": {"static_cast", "enqueue_ble_rx_event_"},
}

REVIEWED_DELEGATED_HELPERS = {
    "diag_vprintf_": {
        "diag_append_": {"g", "pdMS_TO_TICKS"},  # SemGuard variable construction + C macro
        "syslog_send": {"sv", "find", "memcpy", "load", "xQueueSend"},
    },
    "ble_link_event_cb_": {
        "enqueue_ble_link_event_": {"fetch_add", "store", "xQueueSend"},
    },
    "ble_rx_event_cb_": {
        "enqueue_ble_rx_event_": {"store", "static_cast", "data", "memcpy", "xQueueSend"},
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
        "install_state_callbacks_",
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
    found.update(
        assignment_callbacks(
            code,
            r"\b\w+\.(?:on_ping_success|on_ping_timeout|on_ping_end)\s*=\s*([^,;}\n]+)",
            "esp_ping callback",
        )
    )

    # ESP-IDF invokes app_main through a C startup frame even though it is not registered by a
    # source-level function call.
    if re.search(r'extern\s+"C"\s+void\s+app_main\s*\(', text):
        found.add("app_main")
    return found


def function_body_in(text: str, name: str) -> str:
    definition = re.compile(
        rf"^[ \t]*(?:extern\s+\"C\"\s+)?(?:static\s+)?"
        rf"(?:[A-Za-z_]\w*(?:::\w+)*(?:[<>,*&]+)?[ \t]+)+"
        rf"(?:[A-Za-z_]\w*::)*{re.escape(name)}\s*\([^;{{}}]*\)\s*"
        rf"(?:noexcept\s*)?\{{",
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


def require_catch_all(name: str, body: str) -> None:
    """Require one structural outer try/catch chain to cover the whole boundary body.

    Auditing only known throwing syntax is not fail closed: an ordinary-looking helper call can
    throw today or become throwing tomorrow. Requiring `try` as the first body token and the final
    catch body as the last token makes every executable statement part of the protected region or
    its exception handler, independent of the statement spelling.
    """
    code = scrub_cpp_preserving_layout(body)
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
        "cJSON_GetObjectItemCaseSensitive": 2, "cJSON_IsBool": 2, "cJSON_IsNumber": 1,
        "cJSON_IsObject": 1, "cJSON_IsString": 1, "cJSON_IsTrue": 2, "cJSON_Parse": 1,
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
    "json_http_reply.hpp": {"cJSON_PrintUnformatted": 1, "cJSON_free": 1},
    "mcp_json_payloads.hpp": {
        "cJSON_GetObjectItemCaseSensitive": 2, "cJSON_IsArray": 1, "cJSON_IsNull": 1,
        "cJSON_IsNumber": 1, "cJSON_IsObject": 4, "cJSON_IsString": 2,
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
    "status_json_emitter.hpp": {"cJSON_IsArray": 5},
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
    if owner_declarations != ["tk::JsonOwner root"]:
        raise AssertionError(
            f"mqtt_ha.cpp JsonOwner construction/parameter inventory drift: {owner_declarations}"
        )

    expected_pub_json = (
        "pub_json(config_topic,tk::mqtt::build_discovery_payload(payload))",
        "pub_json(state_topic(tk::mqtt::StateDomain::Charge),tk::mqtt::build_charge_payload(payload))",
        "pub_json(state_topic(tk::mqtt::StateDomain::Climate),tk::mqtt::build_climate_payload(payload))",
        "pub_json(state_topic(tk::mqtt::StateDomain::Drive),tk::mqtt::build_drive_payload(payload))",
        "pub_json(state_topic(tk::mqtt::StateDomain::Tires),tk::mqtt::build_tires_payload(payload))",
        "pub_json(state_topic(tk::mqtt::StateDomain::Closures),tk::mqtt::build_closures_payload(payload))",
        "pub_json(state_topic(tk::mqtt::StateDomain::Vehicle),tk::mqtt::build_vehicle_payload(ss))",
        "pub_json(state_topic(tk::mqtt::StateDomain::Device),tk::mqtt::build_device_payload(payload))",
    )
    for call in expected_pub_json:
        if compact_production.count(call) != 1:
            raise AssertionError(f"MQTT pub_json callsite/argument inventory drift: {call}")
    if len(re.findall(r"\bpub_json\s*\(", production_code)) != len(expected_pub_json) + 1:
        raise AssertionError("MQTT pub_json callsite count drift")

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


def require_status_production_seams(producer: str, handler: str) -> None:
    for token in ("tk::StatusJsonEmitter", "tk::status::emit_status", "return e.release();"):
        if token not in producer:
            raise AssertionError(f"/status producer bypasses tested production seam {token!r}")
    for token in ("build_status_object", "send_json"):
        if token not in handler:
            raise AssertionError(f"/status handler bypasses tested production seam {token!r}")


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
        "if (!s_mtx) return DiagDumpResult::SnapshotInvalidated;",
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
        r"OtaHealthCommitGuard::OtaHealthCommitGuard\s*\(\s*\)\s*:\s*"
        r"held_\s*\(\s*s_operation_gate\.try_begin\s*\(\s*"
        r"tk::OtaIdentityGateState::HealthCommit\s*\)\s*\)\s*\{\s*\}",
        ota_source,
        re.DOTALL,
    )
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

    confirm_start = ota_source.find("void ota_confirm_pending_image")
    confirm_end = ota_source.find("// Short per-target image suffix", confirm_start)
    if confirm_start < 0 or confirm_end < 0:
        raise AssertionError("OTA user-confirmation production body is missing")
    confirm = ota_source[confirm_start:confirm_end]
    require_before("user OTA confirmation owner", confirm,
                   "OtaHealthCommitGuard commit_guard;",
                   "esp_ota_mark_app_valid_cancel_rollback()")
    require_before("user OTA confirmation admission", confirm,
                   "if (!commit_guard)",
                   "esp_ota_mark_app_valid_cancel_rollback()")
    denied = re.search(
        r"if\s*\(\s*!commit_guard\s*\)\s*\{(?P<body>.*?)\}",
        confirm,
        re.DOTALL,
    )
    if not denied or not re.search(r"\breturn\s*;", scrub_cpp(denied.group("body"))):
        raise AssertionError("user OTA confirmation does not fail closed when owner admission fails")

    ready_denied = re.search(
        r"if\s*\(\s*!tk::runtime_admission_vehicle_ready\s*\(\s*\)\s*\)\s*"
        r"\{(?P<body>.*?)\}",
        confirm,
        re.DOTALL,
    )
    if not ready_denied or not re.search(
        r"\breturn\s*;", scrub_cpp(ready_denied.group("body"))
    ):
        raise AssertionError(
            "user OTA confirmation does not fail closed outside RuntimeAdmission Ready"
        )
    if confirm.count("tk::runtime_admission_vehicle_ready()") != 1:
        raise AssertionError("user OTA confirmation must re-check global Ready exactly once")
    require_before("user OTA confirmation owner before Ready", confirm,
                   "OtaHealthCommitGuard commit_guard;",
                   "tk::runtime_admission_vehicle_ready()")
    require_before("user OTA confirmation Ready before heap", confirm,
                   "tk::runtime_admission_vehicle_ready()", "commit_largest")
    require_before("user OTA confirmation Ready before mark-valid", confirm,
                   "tk::runtime_admission_vehicle_ready()",
                   "esp_ota_mark_app_valid_cancel_rollback()")
    host_denied = re.search(
        r"if\s*\(\s*!ble_host_synced\s*\(\s*\)\s*\)\s*"
        r"\{(?P<body>.*?)\}", confirm, re.DOTALL,
    )
    if not host_denied or not re.search(
        r"\breturn\s*;", scrub_cpp(host_denied.group("body"))
    ):
        raise AssertionError("user OTA confirmation does not fail closed while NimBLE is reset")
    if confirm.count("ble_host_synced()") != 1:
        raise AssertionError("user OTA confirmation must re-check current NimBLE health once")
    require_before("user OTA confirmation Ready before BLE health", confirm,
                   "tk::runtime_admission_vehicle_ready()", "ble_host_synced()")
    require_before("user OTA confirmation BLE health before heap", confirm,
                   "ble_host_synced()", "commit_largest")
    require_before("user OTA confirmation BLE health before mark-valid", confirm,
                   "ble_host_synced()", "esp_ota_mark_app_valid_cancel_rollback()")
    reset_denied = re.search(
        r"if\s*\(\s*ble_host_reset_count\s*\(\s*\)\s*!=\s*0\s*\)\s*"
        r"\{(?P<body>.*?)\}", confirm, re.DOTALL,
    )
    if not reset_denied or not re.search(
        r"\breturn\s*;", scrub_cpp(reset_denied.group("body"))
    ):
        raise AssertionError(
            "user OTA confirmation does not fail closed after a NimBLE host reset"
        )
    if confirm.count("ble_host_reset_count()") != 1:
        raise AssertionError("user OTA confirmation must re-check sticky NimBLE reset evidence once")
    require_before("user OTA confirmation BLE sync before sticky reset evidence", confirm,
                   "ble_host_synced()", "ble_host_reset_count()")
    require_before("user OTA confirmation reset evidence before heap", confirm,
                   "ble_host_reset_count()", "commit_largest")
    require_before("user OTA confirmation reset evidence before mark-valid", confirm,
                   "ble_host_reset_count()", "esp_ota_mark_app_valid_cancel_rollback()")
    for token in (
        "MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL",
        "commit_largest < tk::kHeapCriticalBytes",
    ):
        if token not in confirm:
            raise AssertionError(f"user OTA confirmation heap recheck missing {token!r}")
    require_before("user OTA confirmation heap recheck", confirm,
                   "commit_largest < tk::kHeapCriticalBytes",
                   "esp_ota_mark_app_valid_cancel_rollback()")
    heap_denied = re.search(
        r"if\s*\(\s*commit_largest\s*<\s*tk::kHeapCriticalBytes\s*\)\s*"
        r"\{(?P<body>.*?)\}",
        confirm,
        re.DOTALL,
    )
    if not heap_denied or not re.search(
        r"\breturn\s*;", scrub_cpp(heap_denied.group("body"))
    ):
        raise AssertionError("user OTA confirmation does not fail closed on critical heap")

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


def require_health_commit_contract(health_task: str) -> None:
    for token in (
        "MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL",
        "largest >= tk::kHeapCriticalBytes",
        "runtime_healthy",
        "OtaHealthCommitGuard commit_guard;",
        "if (!commit_guard) continue;",
        "commit_largest < tk::kHeapCriticalBytes",
        "esp_ota_mark_app_valid_cancel_rollback()",
    ):
        if token not in health_task:
            raise AssertionError(f"OTA health commit contract missing {token!r}")
    commit_branch = health_task[health_task.find("if (v == tk::HealthVerdict::Commit)") :]
    require_before("health heap verdict", commit_branch,
                   "OtaHealthCommitGuard commit_guard;", "commit_largest")
    require_before("health owner before mark-valid", commit_branch,
                   "OtaHealthCommitGuard commit_guard;",
                   "esp_ota_mark_app_valid_cancel_rollback()")
    require_before("health heap recheck before mark-valid", commit_branch,
                   "commit_largest < tk::kHeapCriticalBytes",
                   "esp_ota_mark_app_valid_cancel_rollback()")

    # Connectivity and heap are insufficient while app_main is still Booting, has latched Safe
    # Mode, or has hit a late essential failure. Runtime admission is checked both before forming
    # the verdict and again after acquiring the irreversible HealthCommit owner.
    for token in (
        "switch (tk::runtime_admission_action())",
        "case tk::RuntimeAdmissionAction::Wait:",
        "case tk::RuntimeAdmissionAction::Stop:",
        "case tk::RuntimeAdmissionAction::Run:",
        "if (!tk::runtime_admission_vehicle_ready()) continue;",
        "if (!ble_host_synced()) continue;",
        "if (ble_host_reset_count() != 0) continue;",
    ):
        if token not in health_task:
            raise AssertionError(f"OTA health runtime-admission contract missing {token!r}")
    pre_verdict = health_task[:health_task.find("tk::health_gate_decide")]
    if not re.search(
        r"largest\s*>=\s*tk::kHeapCriticalBytes\s*&&\s*"
        r"ble_host_synced\s*\(\s*\)\s*&&\s*"
        r"ble_host_reset_count\s*\(\s*\)\s*==\s*0",
        pre_verdict,
    ):
        raise AssertionError(
            "OTA health verdict is not gated by current sync and sticky zero-reset evidence"
        )
    require_before("health admission before verdict", health_task,
                   "switch (tk::runtime_admission_action())", "tk::health_gate_decide")
    require_before("health current BLE state before verdict", health_task,
                   "ble_host_synced()", "tk::health_gate_decide")
    require_before("health Ready recheck after owner", commit_branch,
                   "OtaHealthCommitGuard commit_guard;",
                   "if (!tk::runtime_admission_vehicle_ready()) continue;")
    require_before("health Ready recheck before mark-valid", commit_branch,
                   "if (!tk::runtime_admission_vehicle_ready()) continue;",
                   "esp_ota_mark_app_valid_cancel_rollback()")
    require_before("health Ready before BLE recheck", commit_branch,
                   "if (!tk::runtime_admission_vehicle_ready()) continue;",
                   "if (!ble_host_synced()) continue;")
    require_before("health BLE recheck before heap", commit_branch,
                   "if (!ble_host_synced()) continue;", "commit_largest")
    require_before("health BLE recheck before mark-valid", commit_branch,
                   "if (!ble_host_synced()) continue;",
                   "esp_ota_mark_app_valid_cancel_rollback()")
    require_before("health BLE sync before sticky reset evidence", commit_branch,
                   "if (!ble_host_synced()) continue;",
                   "if (ble_host_reset_count() != 0) continue;")
    require_before("health reset evidence before heap", commit_branch,
                   "if (ble_host_reset_count() != 0) continue;", "commit_largest")
    require_before("health reset evidence before mark-valid", commit_branch,
                   "if (ble_host_reset_count() != 0) continue;",
                   "esp_ota_mark_app_valid_cancel_rollback()")


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
        code = scrub_cpp(entry)
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


def require_runtime_admission_contract(logic_header: str, facade_header: str,
                                       facade_source: str, main_source: str,
                                       http_route_header: str, dispatch: str) -> None:
    for token in (
        "std::atomic<RuntimeAdmissionState> state_{RuntimeAdmissionState::Booting};",
        "RuntimeAdmissionState expected = RuntimeAdmissionState::Booting;",
        "RuntimeAdmissionState::Ready",
        "RuntimeAdmissionState::SafeMode",
        "state_.store(RuntimeAdmissionState::Fatal, std::memory_order_release);",
        "== RuntimeAdmissionState::Ready",
        "case RuntimeAdmissionState::Booting:  return RuntimeAdmissionAction::Wait;",
        "case RuntimeAdmissionState::Ready:    return RuntimeAdmissionAction::Run;",
        "case RuntimeAdmissionState::SafeMode: return RuntimeAdmissionAction::Stop;",
        "case RuntimeAdmissionState::Fatal:    return RuntimeAdmissionAction::Stop;",
    ):
        if token not in logic_header:
            raise AssertionError(f"global runtime admission is not default-false: missing {token!r}")
    if logic_header.count("compare_exchange_strong") != 2:
        raise AssertionError("Ready/SafeMode must be the only Booting CAS transitions")

    if facade_header.count("runtime_admission_vehicle_ready() noexcept;") != 1:
        raise AssertionError("runtime admission facade declaration drifted")
    if facade_source.count("RuntimeAdmissionGate s_runtime_admission;") != 1:
        raise AssertionError("runtime admission must have exactly one process-global gate")
    for token in (
        "s_runtime_admission.mark_ready()",
        "s_runtime_admission.mark_safe_mode()",
        "s_runtime_admission.mark_fatal()",
        "s_runtime_admission.vehicle_ready()",
        "s_runtime_admission.action()",
    ):
        if token not in facade_source:
            raise AssertionError(f"runtime admission facade bypasses the global gate: {token!r}")

    if main_source.count("tk::runtime_admission_mark_ready()") != 1:
        raise AssertionError("app_main must publish Ready exactly once")
    require_before("late essential failure closes admission", main_source,
                   "tk::runtime_admission_mark_fatal();",
                   "esp_ota_get_running_partition()")
    require_before("vehicle tasks allocated before Ready", main_source,
                   "vehicle.start_tasks()", "tk::runtime_admission_mark_ready()")
    require_before("safe-mode latch before Ready branch", main_source,
                   "tk::runtime_admission_mark_safe_mode()",
                   "tk::runtime_admission_mark_ready()")

    route_body = function_body_in(http_route_header,
                                  "http_route_requires_vehicle_runtime")
    groups = re.findall(
        r"((?:\s*case\s+HttpRoute::[A-Za-z0-9_]+\s*:\s*)+)"
        r"return\s+(true|false)\s*;", scrub_cpp(route_body), re.DOTALL,
    )
    classified: dict[bool, set[str]] = {True: set(), False: set()}
    for case_group, verdict in groups:
        classified[verdict == "true"].update(
            re.findall(r"case\s+HttpRoute::([A-Za-z0-9_]+)\s*:", case_group)
        )
    expected_active = {
        "Command", "VehicleData", "BodyController", "GenKeys", "SendKey", "SetVin",
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
        "callbacks.on_ping_end = ping_probe_on_end;",
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
    for token in (
        "const bool ok = result == PingProbeResult::Reply;",
        "if (ok) gw_baseline(s_kind.load()).store(true);",
        "return result == PingProbeResult::NoReply ? false : true;",
    ):
        if token not in gateway:
            raise AssertionError(f"gateway unknown-vs-failure policy missing {token!r}")
    require_before("gateway reply establishes baseline", gateway,
                   "const bool ok = result == PingProbeResult::Reply;",
                   "if (ok) gw_baseline(s_kind.load()).store(true);")
    require_before("gateway baseline before reset verdict", gateway,
                   "if (ok) gw_baseline(s_kind.load()).store(true);",
                   "return result == PingProbeResult::NoReply ? false : true;")


def require_ota_fetch_contract(ota_source: str, ota_logic: str,
                               manifest_header: str) -> None:
    fetch = function_body_in(ota_source, "http_get_to_buffer")
    check = function_body_in(ota_source, "ota_check")
    for token in (
        "tk::BoundedHttpBodyGate body_gate(content_length, chunked);",
        "body_gate.valid_headers()",
        "tk::kOtaManifestMaxBytes",
        "out.reserve(chunked ? tk::kOtaManifestMaxBytes",
        "body_gate.next_read_size(sizeof(buf))",
        "esp_http_client_read(c, buf, static_cast<int>(requested))",
        "body_gate.accept_read(",
        "esp_http_client_is_complete_data_received(c)",
        "out.append(buf, static_cast<std::size_t>(read))",
    ):
        if token not in fetch:
            raise AssertionError(f"bounded OTA transport contract missing {token!r}")
    require_before("OTA header validation before reserve", fetch,
                   "body_gate.valid_headers()", "out.reserve(")
    require_before("OTA bounded request before transport read", fetch,
                   "body_gate.next_read_size(sizeof(buf))", "esp_http_client_read")
    require_before("OTA read verdict before append", fetch,
                   "body_gate.accept_read(", "out.append(")
    if re.search(r"\b(?:malloc|calloc|realloc|new)\b", scrub_cpp(fetch)):
        raise AssertionError("OTA manifest transport added an unbounded secondary allocation")

    required_check_tokens = (
        "http_get_to_buffer(CONFIG_TESLA_OTA_MANIFEST_URL, body)",
        "tk::json_materialize<cJSON>",
        "cJSON_ParseWithLengthOpts(",
        "tk::inspect_ota_manifest(j.get())",
        "parse_end != body.c_str() + body.size()",
        "std::string{}.swap(body);",
        "const std::string_view available(manifest.value);",
        "tk::canonical_ota_version(available)",
        "tk::canonical_ota_version(res.current)",
        "res.available.assign(available.data(), available.size());",
        "tk::compare_ota_versions(res.available, res.current)",
    )
    for token in required_check_tokens:
        if token not in check:
            raise AssertionError(f"OTA manifest integration contract missing {token!r}")
    ordered = (
        "http_get_to_buffer(CONFIG_TESLA_OTA_MANIFEST_URL, body)",
        "tk::json_materialize<cJSON>",
        "tk::inspect_ota_manifest(j.get())",
        "parse_end != body.c_str() + body.size()",
        "std::string{}.swap(body);",
        "tk::canonical_ota_version(available)",
        "res.available.assign(available.data(), available.size());",
        "tk::compare_ota_versions(res.available, res.current)",
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
    loop_start = handler.find("while (off < size)")
    terminal = handler.find("httpd_resp_send_chunk(req, nullptr, 0)", loop_start)
    if loop_start < 0 or terminal < 0:
        raise AssertionError("coredump fixed-chunk stream/terminator is missing")
    loop = handler[loop_start:terminal]
    read_at = loop.find("esp_partition_read(part, off, buf, n)")
    send_at = loop.find("httpd_resp_send_chunk(req, buf, n)", read_at)
    if read_at < 0 or send_at < 0 or read_at >= send_at:
        raise AssertionError("coredump flash read is not checked before sending its chunk")
    read_failure_region = scrub_cpp(loop[read_at:send_at])
    if "return ESP_FAIL;" not in read_failure_region:
        raise AssertionError("coredump flash-read failure does not abort the HTTP stream")
    if re.search(r"\bbreak\s*;", read_failure_region):
        raise AssertionError("coredump flash-read failure can fall through to a clean terminator")
    if handler.count("httpd_resp_send_chunk(req, nullptr, 0)") != 1:
        raise AssertionError("coredump stream must have exactly one success-only terminating chunk")
    require_before("coredump final data chunk before terminator", handler,
                   "httpd_resp_send_chunk(req, buf, n)",
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
        "esp_eth_del_netif_glue(r.glue)", "esp_netif_destroy(r.netif)",
        "esp_eth_driver_uninstall(r.handle)", "r.phy->del(r.phy)",
        "r.mac->del(r.mac)", "vEventGroupDelete(r.events)", "eth_release_spi_bus()",
    ):
        if token not in cleanup:
            raise AssertionError(f"Ethernet reverse cleanup missing {token!r}")
    ordered = (
        "esp_eth_stop(r.handle)", "IP_EVENT, IP_EVENT_ETH_GOT_IP, eth_event_handler",
        "ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler", "if (r.published)",
        "esp_eth_del_netif_glue(r.glue)", "esp_netif_destroy(r.netif)",
        "esp_eth_driver_uninstall(r.handle)", "r.phy->del(r.phy)",
        "r.mac->del(r.mac)", "vEventGroupDelete(r.events)", "eth_release_spi_bus()",
    )
    positions = [cleanup.find(token) for token in ordered]
    if min(positions) < 0 or positions != sorted(positions):
        raise AssertionError("Ethernet startup cleanup is not in reverse ownership order")
    require_before("Ethernet uninstall failure retains MAC/PHY/SPI", cleanup,
                   "driver_released = false", "if (driver_released)")

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

    runtime_matrix = function_body_in(runtime_tests, "test_ethernet_partial_start_cleanup")
    for token in (
        "event_group_failure", "netif_failure",
        "missing_phy", "missing_mac", "driver_install_failure", "glue_allocation_failure",
        "attach_failure", "ip_handler_failure", "start_failure", "uninstall_failure",
        "no_link_fallback", '"stop", "ip_unregister", "eth_unregister", "withdraw_globals"',
        '"destroy_netif", "uninstall_driver", "delete_phy", "delete_mac"',
        "!uninstall_failure.cleanup()",
    ):
        if token not in runtime_matrix:
            raise AssertionError(f"Ethernet executable partial-acquire matrix missing {token!r}")
    if "test_ethernet_partial_start_cleanup();" not in function_body_in(runtime_tests, "main"):
        raise AssertionError("Ethernet executable partial-acquire matrix is not invoked")


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
    setup = function_body_in(provisioning_source, "provisioning_run")
    if setup.count("!= ESP_OK &&") < 2 or setup.count("!= ESP_ERR_INVALID_STATE") < 2:
        raise AssertionError("setup AP substrate does not handle only idempotent INVALID_STATE")
    for token in (
        'provisioning_boot_require(netif_err, "setup network interface substrate")',
        'provisioning_boot_require(loop_err, "setup network event loop")',
        'if (!ap_netif) boot_fatal("setup AP network interface");',
        'provisioning_boot_require(esp_wifi_init(&cfg), "setup WiFi driver initialization");',
        'provisioning_boot_require(esp_wifi_set_mode(WIFI_MODE_AP), "setup AP mode");',
        'provisioning_boot_require(esp_wifi_set_config(WIFI_IF_AP, &ap), "setup AP configuration");',
        'provisioning_boot_require(esp_wifi_start(), "setup AP start");',
        'if (httpd_start(&server, &hcfg) != ESP_OK)',
        'boot_fatal("setup HTTP server");',
        "httpd_register_uri_handler(server, &save) != ESP_OK",
        "httpd_register_uri_handler(server, &form) != ESP_OK",
        "httpd_stop(server);",
        'boot_fatal("setup HTTP handlers");',
    ):
        if token not in setup:
            raise AssertionError(f"setup AP fail-closed route missing {token!r}")
    require_before("setup handler failure stops partial server", setup,
                   "httpd_stop(server);", 'boot_fatal("setup HTTP handlers");')

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
    if 'return {OtaState::Error, 0, "unavailable", "", false, ""};' not in unavailable:
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
    check = scrub_cpp(function_body_in(ota_source, "ota_check"))
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
    callback_reachable = (
        "on_scan_timeout",
        "on_sync",
        "on_reset",
        "ensure_scanning_",
        "disconnect_from_callback_",
        "on_gap_event",
        "on_svc_disc",
        "on_chr_disc",
        "subscribe_notify_",
        "on_dsc_disc",
        "on_subscribe_write",
    )
    for name in callback_reachable:
        body = scrub_cpp(function_body_in(ble_source, name))
        if re.search(r"tk::SemGuard\s+\w+\s*\(\s*intent_mutex_\s*\)\s*;", body):
            raise AssertionError(
                f"BLE callback path {name} uses the blocking intent-mutex guard"
            )
        if name not in ("ensure_scanning_", "disconnect_from_callback_"):
            if re.search(r"\bdisconnect\s*\(", body):
                raise AssertionError(
                    f"BLE callback path {name} delegates to blocking disconnect()"
                )
            for deadline in re.findall(r"\bensure_scanning_\s*\(([^)]*)\)", body):
                if deadline.strip() != "0":
                    raise AssertionError(
                        f"BLE callback path {name} delegates to a non-zero scan-lock deadline"
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
    persistent = {
        "set_charge_state_callback": "on_charge_state_",
        "set_climate_state_callback": "on_climate_state_",
        "set_drive_state_callback": "on_drive_state_",
        "set_tire_pressure_state_callback": "on_tire_pressure_state_",
        "set_closures_state_callback": "on_closures_state_",
        "set_message_callback": "on_vehicle_message_",
    }
    combined = controller + "\n" + telemetry
    for setter, helper in persistent.items():
        registrations = call_arguments(combined, setter)
        if len(registrations) != 1 or len(registrations[0]) != 1:
            raise AssertionError(
                f"{setter}: expected one single-argument persistent registration"
            )
        adapter = scrub_cpp(registrations[0][0])
        compact = re.sub(r"\s+", " ", adapter).strip()
        if not re.fullmatch(
            rf"\[this\]\s*\([^)]*\)\s*\{{\s*{re.escape(helper)}\s*\(\s*\w+\s*\)\s*;\s*\}}",
            compact,
        ):
            raise AssertionError(f"{setter}: adapter must delegate only to {helper}")
        for forbidden in (
            "std::string", "std::vector", "tk::SemGuard", "tk::MutexGuard",
            "save_str", "ESP_LOG", "throw", "new ",
        ):
            if forbidden in adapter:
                raise AssertionError(f"{setter}: adapter contains forbidden {forbidden!r}")

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
    if "pairing_lost_.store(true, std::memory_order_release);" not in message:
        raise AssertionError("on_vehicle_message_: pairing loss is not an atomic publication")

    deferred = function_body_in(telemetry, "process_pending_telemetry_")
    if "vehicle_mutex_" in scrub_cpp(deferred):
        raise AssertionError("deferred telemetry parser reacquires vehicle_mutex_")
    for parser in (
        "parse_charge_state", "parse_climate_state", "parse_drive_state",
        "parse_tire_pressure", "parse_closures_state",
    ):
        if deferred.count(parser + "(") != 1:
            raise AssertionError(f"deferred telemetry parser inventory drift: {parser}")

    loop = scrub_cpp(function_body_in(telemetry, "loop_task_fn_"))
    require_before("BLE host drain before tesla-ble pump", loop,
                   "process_ble_host_events_();", "vehicle_->loop();")
    require_before("telemetry parse after tesla-ble pump", loop,
                   "vehicle_->loop();", "process_pending_telemetry_();")
    require_before("telemetry parse after Vehicle guard scope", loop,
                   "vcsec_sleep_state_.store", "process_pending_telemetry_();")

    if combined.count("vehicle_->on_rx_data(") != 1:
        raise AssertionError("Vehicle::on_rx_data must have one deferred task-owned callsite")
    if "vehicle_->on_rx_data(" in scrub_cpp(ble_source):
        raise AssertionError("NimBLE host callback directly reentered Vehicle::on_rx_data")
    ble_events = function_body_in(telemetry, "process_ble_host_events_")
    if "vehicle_->on_rx_data(data);" not in ble_events:
        raise AssertionError("Vehicle::on_rx_data escaped the deferred event processor")
    if ble_source.count("BleClient::complete_ready(") != 1:
        raise AssertionError("BleClient ready publication definition inventory drift")
    subscribe = function_body_in(ble_source, "on_subscribe_write")
    if "complete_ready" in scrub_cpp(subscribe) or "ready_generation_.store(generation)" in scrub_cpp(subscribe):
        raise AssertionError("NimBLE subscribe callback publishes readiness before Vehicle ack")
    if "ble_->complete_ready(event.conn_handle, event.generation)" not in ble_events:
        raise AssertionError("deferred Vehicle LinkUp acknowledgement does not publish readiness")

    # The request-scoped VCSEC callback is dynamic, but still executes from Vehicle::loop while its
    # mutex is held. It may publish POD and signal a pre-created semaphore only; string shaping is
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
                   "vehicle_->set_vehicle_status_callback(nullptr);", "out.valid = true;")

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
        "std::memcpy(completion->error.data(), msg.data(), error_size);",
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


def require_crash_dismiss_atomic_contract(crash_source: str) -> None:
    for token in (
        "static std::atomic<bool> s_dismissed{false};",
        "s_dismissed.store(false, std::memory_order_release);",
        "c.dismissed = s_dismissed.load(std::memory_order_acquire);",
        "s_dismissed.store(true, std::memory_order_release);",
    ):
        if token not in crash_source:
            raise AssertionError(f"crash dismissal atomic contract missing {token!r}")
    code = scrub_cpp(crash_source)
    if "s_ci.dismissed =" in code or re.search(r"static\s+bool\s+s_dismissed", code):
        raise AssertionError("crash dismissal reintroduced a racy shared bool/string snapshot")
    dismiss = function_body_in(crash_source, "diag_crash_dismiss")
    require_before("crash evidence erase before atomic dismissal", dismiss,
                   "esp_core_dump_image_erase();", "s_dismissed.store(true")


def require_heap_restart_gate_contract(telemetry: str) -> None:
    restart_marker = "if (v.action == tk::HeapAction::Restart)"
    restart_at = telemetry.find(restart_marker)
    if restart_at < 0:
        raise AssertionError("heap restart action branch is missing")
    restart_branch = telemetry[restart_at:]
    require_before(
        "heap restart operation admission",
        restart_branch,
        "if (!ota_fault_restart_begin())",
        "self->persist_reboot_reason_(why.text)",
    )
    busy_branch = restart_branch[
        restart_branch.find("if (!ota_fault_restart_begin())") :
        restart_branch.find("if (!self->persist_reboot_reason_(why.text))")
    ]
    if "continue;" not in busy_branch:
        raise AssertionError("occupied heap restart gate no longer postpones the reboot")
    persist_gate = "if (!self->persist_reboot_reason_(why.text))"
    persist_at = restart_branch.find(persist_gate)
    if persist_at < 0:
        raise AssertionError("heap reboot persistence authority is missing")
    persist_branch = restart_branch[persist_at:]
    require_before("heap reboot persistence release", persist_branch,
                   "ota_fault_restart_cancel();", "continue;")
    require_before("heap reboot persistence", persist_branch, persist_gate, "continue;")
    require_before("heap reboot action", persist_branch, "continue;", "esp_restart();")
    success_tail = persist_branch[persist_branch.find("continue;") + len("continue;"):]
    if "ota_fault_restart_cancel" in success_tail:
        raise AssertionError("successful heap restart releases FaultRestart before esp_restart")


HTTP_ROUTE_DISPATCH = {
    "Command": "handle_command",
    "VehicleData": "handle_vehicle_data",
    "BodyController": "handle_body_controller",
    "OtaCheck": "handle_ota_check",
    "OtaUpdate": "handle_ota_update",
    "OtaStatus": "handle_ota_status",
    "GenKeys": "handle_gen_keys",
    "SendKey": "handle_send_key",
    "SetTime": "handle_set_time",
    "SetVin": "handle_set_vin",
    "SetMqtt": "handle_set_mqtt",
    "SetSyslog": "handle_set_syslog",
    "SetWifi": "handle_set_wifi",
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
            rf"return\s+{re.escape(handler)}\s*\(\s*\{{\s*req\s*\}}\s*\)\s*;"
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
    if "The latch remains until a non-fault reset" not in source:
        raise AssertionError("safe-mode operator text no longer describes the real latch")


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


def require_runtime_source_contracts() -> None:
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
        function_body("build_status_object"), function_body("handle_status")
    )
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
    telemetry = function_body("loop_task_fn_")
    require_heap_restart_gate_contract(telemetry)

    require_safe_mode_source_contract(SOURCES["safe_mode.cpp"])
    require_provisioning_body_contract(
        function_body("save_post_impl"),
        function_body("url_decode"),
        (MAIN / "logic/http_body.hpp").read_text(encoding="utf-8"),
    )


def check_contract(tasks: set[str], callbacks: set[str]) -> None:
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
    for token in ("tk::StatusJsonEmitter", "tk::status::emit_status", "return e.release();"):
        mutated_status = status_producer.replace(token, "fixture_status_bypass", 1)
        try:
            require_status_production_seams(mutated_status, status_handler)
        except AssertionError:
            pass
        else:
            raise AssertionError(f"/status seam-removal mutation passed unexpectedly: {token}")

    route_dispatch = function_body("handle_all_dispatch")
    route_mutations = (
        route_dispatch.replace("tk::classify_http_route", "fixture_route_classifier", 1),
        route_dispatch.replace("return handle_ota_update({req});", "return handle_status({req});", 1),
        route_dispatch.replace("case tk::HttpRoute::SetWifi:", "case tk::HttpRoute::Fixture:", 1),
        route_dispatch.replace(
            "case tk::HttpRoute::NotFound:       break;",
            "case tk::HttpRoute::Fixture:        return handle_status({req});\n"
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

    telemetry = function_body("loop_task_fn_")
    for old, replacement, label in (
        ("if (!ota_fault_restart_begin())", "if (false)", "operation admission"),
        ("ota_fault_restart_cancel();", "", "persistence-failure release"),
        ("if (!self->persist_reboot_reason_(why.text))", "if (false)", "persistence authority"),
    ):
        mutated_telemetry = telemetry.replace(old, replacement, 1)
        if mutated_telemetry == telemetry:
            raise AssertionError(f"heap restart {label} mutation did not apply")
        try:
            require_heap_restart_gate_contract(mutated_telemetry)
        except AssertionError:
            pass
        else:
            raise AssertionError(f"heap restart {label} bypass passed unexpectedly")

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
            admission_logic.replace("state_{RuntimeAdmissionState::Booting}",
                                    "state_{RuntimeAdmissionState::Ready}", 1),
            admission_header, admission_source, main_source, route_header, route_dispatch,
        ),
        (
            "late boot-fatal admission close",
            admission_logic, admission_header, admission_source,
            main_source.replace("tk::runtime_admission_mark_fatal();", "", 1),
            route_header, route_dispatch,
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
            main_source.replace("ble_host_reset_count() == 0", "true", 1),
            ota_source,
        ),
        (
            "OTA health reset evidence after owner",
            ble_header, ble_source,
            main_source.replace("if (ble_host_reset_count() != 0) continue;", "", 1),
            ota_source,
        ),
        (
            "explicit OTA reset evidence after owner",
            ble_header, ble_source, main_source,
            ota_source.replace("if (ble_host_reset_count() != 0)", "if (false)", 1),
        ),
    ):
        require_mutation_rejected(
            label,
            lambda header=header, source=source, app=app, ota=ota:
                require_dynamic_ble_host_health_contract(
                    header, source, app, ota, logic_tests, runtime_tests
                ),
        )

    health_task = function_body("ota_health_gate_task")
    for token, replacement, label in (
        ("switch (tk::runtime_admission_action())", "switch (tk::RuntimeAdmissionAction::Run)",
         "OTA health initial admission"),
        ("if (!tk::runtime_admission_vehicle_ready()) continue;", "",
         "OTA health post-owner Ready recheck"),
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
                "return result == PingProbeResult::NoReply ? false : true;",
                "return result == PingProbeResult::Reply;",
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
            "Ethernet uninstall failure frees owned tail",
            net_source.replace("if (driver_released) {", "if (true) {", 1),
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
            "Ethernet executable partial-acquire matrix removal",
            net_source,
            runtime_tests.replace(
                "test_ethernet_partial_start_cleanup",
                "fixture_ethernet_partial_start_cleanup",
                1,
            ),
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
            ota_source.replace("tk::BoundedHttpBodyGate body_gate", "FixtureBodyGate body_gate", 1),
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
        "const esp_err_t read_err = esp_partition_read(part, off, buf, n);",
        "const esp_err_t read_err = esp_partition_read(part, off, buf, n);\n"
        "        if (read_err != ESP_OK) break;",
        1,
    )
    require_mutation_rejected(
        "coredump read failure clean-termination fallthrough",
        lambda: require_coredump_stream_contract(read_fallthrough, runtime_tests),
    )

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

    controller_source = SOURCES["vehicle_ctrl.cpp"]
    telemetry_source = SOURCES["vehicle_telemetry.cpp"]
    ble_source = SOURCES["ble_client.cpp"]
    commands_source = SOURCES["vehicle_commands.cpp"]
    callback_mutations = (
        (
            "persistent callback allocation",
            controller_source,
            telemetry_source.replace(
                "on_charge_state_(state);",
                "std::string callback_allocation; on_charge_state_(state);",
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
            "NimBLE host direct Vehicle RX reentry",
            controller_source,
            telemetry_source,
            ble_source.replace(
                "int BleClient::on_gap_event",
                "void callback_canary() { vehicle_->on_rx_data(data); }\n\n"
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
        (
            "return s_operation_gate.try_begin(tk::OtaIdentityGateState::FaultRestart);",
            "return true;",
            "FaultRestart begin",
        ),
        (
            "finish_operation(tk::OtaIdentityGateState::FaultRestart);",
            "",
            "FaultRestart cancel",
        ),
        (
            "finish_operation(tk::OtaIdentityGateState::HealthCommit);",
            "",
            "HealthCommit release",
        ),
        (
            "OtaHealthCommitGuard commit_guard;\n    if (!commit_guard)",
            "bool commit_guard = true;\n    if (!commit_guard)",
            "user confirmation owner",
        ),
        (
            "tk::runtime_admission_vehicle_ready()",
            "true",
            "user confirmation RuntimeAdmission Ready",
        ),
        (
            "commit_largest < tk::kHeapCriticalBytes",
            "commit_largest < 0",
            "user confirmation heap recheck",
        ),
    ):
        mutated_ota = ota_source.replace(old, replacement, 1)
        if mutated_ota == ota_source:
            raise AssertionError(f"{label} wrapper mutation did not apply")
        try:
            require_operation_wrapper_contract(mutated_ota)
        except AssertionError:
            pass
        else:
            raise AssertionError(f"{label} wrapper bypass passed unexpectedly")

    confirm_start = ota_source.find("void ota_confirm_pending_image")
    confirm_end = ota_source.find("// Short per-target image suffix", confirm_start)
    confirm = ota_source[confirm_start:confirm_end]
    denied = re.search(
        r"if\s*\(\s*!commit_guard\s*\)\s*\{(?P<body>.*?)\}",
        confirm,
        re.DOTALL,
    )
    if not denied or "return;" not in denied.group("body"):
        raise AssertionError("user confirmation return mutation fixture is missing")
    mutated_denied = denied.group(0).replace("return;", "", 1)
    mutated_confirm = confirm[: denied.start()] + mutated_denied + confirm[denied.end() :]
    mutated_ota = ota_source[:confirm_start] + mutated_confirm + ota_source[confirm_end:]
    try:
        require_operation_wrapper_contract(mutated_ota)
    except AssertionError:
        pass
    else:
        raise AssertionError("user confirmation fail-closed return mutation passed unexpectedly")

    ready_denied = re.search(
        r"if\s*\(\s*!tk::runtime_admission_vehicle_ready\s*\(\s*\)\s*\)\s*"
        r"\{(?P<body>.*?)\}",
        confirm,
        re.DOTALL,
    )
    if not ready_denied or "return;" not in ready_denied.group("body"):
        raise AssertionError("user confirmation RuntimeAdmission return fixture is missing")
    mutated_ready_denied = ready_denied.group(0).replace("return;", "", 1)
    mutated_confirm = (
        confirm[: ready_denied.start()] + mutated_ready_denied + confirm[ready_denied.end() :]
    )
    mutated_ota = ota_source[:confirm_start] + mutated_confirm + ota_source[confirm_end:]
    require_mutation_rejected(
        "user confirmation RuntimeAdmission fail-closed return",
        lambda: require_operation_wrapper_contract(mutated_ota),
    )

    heap_denied = re.search(
        r"if\s*\(\s*commit_largest\s*<\s*tk::kHeapCriticalBytes\s*\)\s*"
        r"\{(?P<body>.*?)\}",
        confirm,
        re.DOTALL,
    )
    if not heap_denied or "return;" not in heap_denied.group("body"):
        raise AssertionError("user confirmation heap return mutation fixture is missing")
    mutated_heap_denied = heap_denied.group(0).replace("return;", "", 1)
    mutated_confirm = (
        confirm[: heap_denied.start()] + mutated_heap_denied + confirm[heap_denied.end() :]
    )
    mutated_ota = ota_source[:confirm_start] + mutated_confirm + ota_source[confirm_end:]
    try:
        require_operation_wrapper_contract(mutated_ota)
    except AssertionError:
        pass
    else:
        raise AssertionError("user confirmation critical-heap return mutation passed unexpectedly")

    health_task = function_body("ota_health_gate_task")
    for old, replacement, label in (
        ("largest >= tk::kHeapCriticalBytes", "true", "heap verdict"),
        ("OtaHealthCommitGuard commit_guard;", "bool commit_guard = true;", "operation owner"),
        ("commit_largest < tk::kHeapCriticalBytes", "false", "heap CAS recheck"),
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


def main() -> int:
    tasks = task_inventory(ALL_CODE)
    callbacks = callback_inventory(ALL_CODE)
    check_contract(tasks, callbacks)
    self_test_canaries(tasks, callbacks)
    print(f"OK runtime C-boundary inventory ({len(tasks)} tasks, {len(callbacks)} callbacks)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
