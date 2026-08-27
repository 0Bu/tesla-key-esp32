#!/usr/bin/env python3
"""Exact, mutation-tested namespace/key/owner/API/retention contract for shipped NVS access."""

from __future__ import annotations

import argparse
from collections import Counter
from pathlib import Path
import re
import shutil
import tempfile


class ContractError(RuntimeError):
    pass


SOURCE_SUFFIXES = {
    ".c", ".cc", ".cpp", ".cxx", ".c++",
    ".h", ".hh", ".hpp", ".hxx", ".h++",
    ".inc", ".inl", ".ipp", ".tpp", ".txx",
}

EXPECTED_DIRECT_CALLS = {
    "main/nvs_storage.cpp": Counter({
        "get_blob": 6, "commit": 4, "get_str": 4, "set_blob": 2,
        "close": 1, "open": 1, "erase_key": 1, "set_str": 1,
    }),
    "main/display.cpp": Counter({
        "open": 2, "get_u8": 2, "close": 2, "set_u8": 1, "commit": 1,
    }),
    # Boot performs one non-destructive partition initialization. It deliberately has no
    # erase-and-retry fallback: this partition owns WiFi, VIN, the vehicle private key and BLE
    # sessions, so format/capacity errors are recovery evidence rather than erase authority.
    "main/main.cpp": Counter({"flash_init": 1}),
}
NVS_FUNCTION_IDENTIFIERS = frozenset(
    name for calls in EXPECTED_DIRECT_CALLS.values() for name in calls
)
# These are repository C++ identifiers/header stems, not callable NVS APIs. Every other nvs_*
# identifier is rejected, even without a following '(', so a new API cannot hide behind a simple
# function pointer, alias or wrapper. This is deliberately lexical; the mutation suite pins the
# boundary and exact direct-call tuples below pin every allowed function use.
NON_FUNCTION_NVS_IDENTIFIERS = frozenset({"contract", "flash", "handle_t"})


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ContractError(message)


def cpp_code_only(text: str) -> str:
    """Blank comments and literals so examples cannot satisfy or inflate call inventories."""
    out: list[str] = []
    i = 0
    state = "code"
    quote = ""
    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""
        if state == "code":
            if ch == "/" and nxt == "/":
                out.extend("  ")
                i += 2
                state = "line"
                continue
            if ch == "/" and nxt == "*":
                out.extend("  ")
                i += 2
                state = "block"
                continue
            if ch in {'"', "'"}:
                quote = ch
                out.append(" ")
                i += 1
                state = "literal"
                continue
            out.append(ch)
            i += 1
            continue
        if state == "line":
            if ch == "\n":
                out.append("\n")
                state = "code"
            else:
                out.append(" ")
            i += 1
            continue
        if state == "block":
            if ch == "*" and nxt == "/":
                out.extend("  ")
                i += 2
                state = "code"
            else:
                out.append("\n" if ch == "\n" else " ")
                i += 1
            continue
        if ch == "\\" and nxt:
            out.extend("  ")
            i += 2
        elif ch == quote:
            out.append(" ")
            i += 1
            state = "code"
        else:
            out.append("\n" if ch == "\n" else " ")
            i += 1
    return "".join(out)


def cpp_tokens(text: str) -> list[tuple[str, str]]:
    """Minimal C++ lexer for member-call and exact argument-shape contracts.

    Comments and the contents of string/character literals are never returned as identifiers.
    String literals are emitted as one opaque STRING token so a real `save_str("literal", ...)`
    remains distinguishable from the same characters inside a comment or diagnostic string.
    """
    tokens: list[tuple[str, str]] = []
    i = 0
    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""
        if ch.isspace():
            i += 1
            continue
        if ch == "/" and nxt == "/":
            newline = text.find("\n", i + 2)
            i = len(text) if newline < 0 else newline + 1
            continue
        if ch == "/" and nxt == "*":
            end = text.find("*/", i + 2)
            i = len(text) if end < 0 else end + 2
            continue
        if ch in {'"', "'"}:
            quote = ch
            i += 1
            while i < len(text):
                if text[i] == "\\" and i + 1 < len(text):
                    i += 2
                elif text[i] == quote:
                    i += 1
                    break
                else:
                    i += 1
            tokens.append(("string" if quote == '"' else "char", "STRING"))
            continue
        if ch.isalpha() or ch == "_":
            end = i + 1
            while end < len(text) and (text[end].isalnum() or text[end] == "_"):
                end += 1
            tokens.append(("identifier", text[i:end]))
            i = end
            continue
        if text.startswith("->", i) or text.startswith("::", i):
            tokens.append(("symbol", text[i:i + 2]))
            i += 2
            continue
        tokens.append(("symbol", ch))
        i += 1
    return tokens


def token_signature(text: str) -> str:
    return "".join(value for _, value in cpp_tokens(text))


def literal_adapter_calls(text: str) -> list[str]:
    methods = {
        "load", "save", "remove", "blob_exists", "probe_blob", "load_str",
        "load_str_state", "save_str", "load_blob", "load_blob_state", "save_blob",
    }
    tokens = cpp_tokens(text)
    found: list[str] = []
    for i in range(len(tokens) - 4):
        if (tokens[i][0] == "identifier" and tokens[i + 1][1] in {".", "->"} and
                tokens[i + 2][0] == "identifier" and tokens[i + 2][1] in methods and
                tokens[i + 3][1] == "(" and tokens[i + 4][0] == "string"):
            found.append(tokens[i + 2][1])
    return found


def literal_namespace_constructions(text: str) -> int:
    tokens = cpp_tokens(text)
    count = 0
    for i, token in enumerate(tokens):
        if token != ("identifier", "NvsStorageAdapter"):
            continue
        cursor = i + 1
        if cursor < len(tokens) and tokens[cursor][0] == "identifier":
            cursor += 1
        if (cursor + 1 < len(tokens) and tokens[cursor][1] in {"(", "{"} and
                tokens[cursor + 1][0] == "string"):
            count += 1
    return count


def extract_method_body(text: str, method: str) -> str:
    code = cpp_code_only(text)
    matches = list(re.finditer(rf"\bNvsStorageAdapter::{re.escape(method)}\s*\(", code))
    require(len(matches) == 1, f"NVS adapter method definition drifted: {method}")
    opening = code.find("{", matches[0].end())
    require(opening >= 0, f"NVS adapter method has no body: {method}")
    depth = 0
    for index in range(opening, len(code)):
        if code[index] == "{":
            depth += 1
        elif code[index] == "}":
            depth -= 1
            if depth == 0:
                return text[opening + 1:index]
    raise ContractError(f"NVS adapter method body is unbalanced: {method}")


def extract_free_function_body(text: str, function: str) -> str:
    code = cpp_code_only(text)
    match = re.search(rf"\b{re.escape(function)}\s*\(\s*(?:int\s+\w+)?\s*\)\s*\{{", code)
    require(match is not None, f"direct NVS owner function drifted: {function}")
    opening = code.find("{", match.start())
    depth = 0
    for index in range(opening, len(code)):
        if code[index] == "{":
            depth += 1
        elif code[index] == "}":
            depth -= 1
            if depth == 0:
                return text[opening + 1:index]
    raise ContractError(f"direct NVS owner function body is unbalanced: {function}")


EXPECTED = (
    ("Config", "cfg", "cfg", "RawBlob", "ConfigHttp", "DurableAcrossOta", True),
    ("Config", "wifi_ssid", "wifi_ssid", "String", "LegacyConfigMirror", "LegacyDowngradeMirror", True),
    ("Config", "wifi_pass", "wifi_pass", "String", "LegacyConfigMirror", "LegacyDowngradeMirror", True),
    ("Config", "vin", "vin", "String", "LegacyConfigMirror", "LegacyDowngradeMirror", True),
    ("Config", "mqtt_uri", "mqtt_uri", "String", "LegacyConfigMirror", "LegacyDowngradeMirror", True),
    ("Config", "syslog_uri", "syslog_uri", "String", "LegacyConfigMirror", "LegacyDowngradeMirror", True),
    ("Config", "last_time", "last_time", "String", "Clock", "ReplaceableCache", False),
    ("Config", "vin_txn", "vin_txn", "String", "VinTransition", "RecoveryJournal", True),
    ("Config", "ble_mac", "ble_mac", "String", "BleDiscovery", "ReplaceableCache", True),
    ("Config", "reboot_why", "reboot_why", "String", "HeapWatchdog", "RecoveryJournal", False),
    ("Config", "boot_fails", "boot_fails", "String", "BootGuard", "RecoveryJournal", False),
    ("Config", "disp_rot", "disp_rot", "DirectU8", "Display", "DurableAcrossOta", False),
    ("Config", "disp_flip", "disp_flip", "DirectU8", "Display", "MigrationOnly", False),
    ("TeslaBle", "private_key", "private_key", "Blob", "TeslaBleLibrary", "DurableAcrossOta", True),
    ("TeslaBle", "session_vcsec", "sess_vcsec", "Blob", "TeslaBleLibrary", "ReplaceableCache", True),
    ("TeslaBle", "session_infotainment", "sess_info", "Blob", "TeslaBleLibrary", "ReplaceableCache", True),
    ("TeslaBle", "paired_at", "paired_at", "String", "Pairing", "ReplaceableCache", False),
    ("TeslaBle", "key_created", "key_created", "String", "Pairing", "DurableAcrossOta", False),
    ("TeslaBle", "key_rotate", "key_rotate", "Blob", "KeyRotation", "RecoveryJournal", True),
)


def parse_registry(text: str) -> tuple[tuple[object, ...], ...]:
    constants = dict(re.findall(r'inline constexpr char (k\w+)\[\] = "([^"]*)";', text))

    def value(token: str) -> str:
        token = token.strip()
        if token.startswith('"') and token.endswith('"'):
            return token[1:-1]
        require(token in constants, f"NVS registry uses unresolved key token: {token}")
        return constants[token]

    pattern = re.compile(
        r"\{Namespace::(\w+),\s*([^,]+),\s*([^,]+),\s*StorageApi::(\w+),\s*"
        r"Owner::(\w+),\s*Retention::(\w+),\s*(true|false)\}",
        re.S,
    )
    return tuple(
        (namespace, value(logical), value(stored), api, owner, retention, secret == "true")
        for namespace, logical, stored, api, owner, retention, secret in pattern.findall(text)
    )


def validate_physical_name(namespace: str, logical_key: str, stored_key: str) -> None:
    require(bool(stored_key), f"NVS stored key is empty: {namespace}/{logical_key}")
    require(len(stored_key.encode("utf-8")) <= 15,
            f"NVS stored key exceeds 15 bytes: {namespace}/{logical_key} -> {stored_key}")


def validate_adapter_methods(adapter: str) -> None:
    blob = "tk::nvs_contract::StorageApi::Blob"
    string = "tk::nvs_contract::StorageApi::String"
    raw_blob = "tk::nvs_contract::StorageApi::RawBlob"
    specs = {
        "initialize": (None, Counter({"open": 1})),
        "load": (f"map_key(key,{blob})", Counter({"get_blob": 2})),
        "blob_exists": (f"map_key(key,{blob})", Counter({"get_blob": 1})),
        "probe_blob": (f"map_key(key,{blob})", Counter({"get_blob": 1})),
        "save": (f"map_key(key,{blob})", Counter({"set_blob": 1, "commit": 1})),
        "remove": (f"map_key(key,{blob},true)", Counter({"erase_key": 1, "commit": 1})),
        "load_str": (f"map_key(key?key:STRING,{string})", Counter({"get_str": 2})),
        "load_str_state": (f"map_key(key?key:STRING,{string})", Counter({"get_str": 2})),
        "save_str": (f"map_key(key?key:STRING,{string})", Counter({"set_str": 1, "commit": 1})),
        "load_blob": (None, Counter()),
        "load_blob_state": (f"map_key(key?key:STRING,{raw_blob})", Counter({"get_blob": 2})),
        "save_blob": (f"map_key(key?key:STRING,{raw_blob})",
                      Counter({"set_blob": 1, "commit": 1})),
    }
    direct_re = re.compile(r"\bnvs_([A-Za-z0-9_]+)\s*\(")
    for method, (mapping, expected_calls) in specs.items():
        body = extract_method_body(adapter, method)
        signature = token_signature(body)
        actual_calls = Counter(direct_re.findall(cpp_code_only(body)))
        require(actual_calls == expected_calls,
                f"NVS adapter method {method} direct-call contract drifted: {actual_calls}")
        if mapping is None:
            require("map_key(" not in signature,
                    f"NVS adapter method {method} gained an unclassified map_key call")
        else:
            require(signature.count(mapping) == 1 and signature.count("map_key(") == 1,
                    f"NVS adapter method {method} has the wrong Method-to-StorageApi binding")

    initialize = token_signature(extract_method_body(adapter, "initialize"))
    require(initialize.count("nvs_open(ns_,NVS_READWRITE,&handle_)") == 1,
            "NVS initialize no longer opens its classified namespace READWRITE")
    load_blob = token_signature(extract_method_body(adapter, "load_blob"))
    require("returnload_blob_state(key,out)==tk::NvsBlobLoadState::Present;" in load_blob,
            "NVS load_blob no longer delegates to the fail-closed RawBlob state reader")
    ordinary_string = token_signature(extract_method_body(adapter, "load_str"))
    for token in ("try{", "catch(...)", "std::stringcandidate(", "out.swap(candidate)"):
        require(token in ordinary_string,
                f"NVS load_str lost atomic exception containment: {token}")


def validate_display_direct_u8(display: str) -> None:
    load = token_signature(extract_free_function_body(display, "load_disp_rot"))
    save = token_signature(extract_free_function_body(display, "save_disp_rot"))
    load_tuples = (
        "nvs_open(tk::nvs_contract::kConfigNamespace,NVS_READONLY,&h)",
        "nvs_get_u8(h,tk::nvs_contract::kDisplayRotation,&rot)",
        "nvs_get_u8(h,tk::nvs_contract::kLegacyDisplayFlip,&flip)",
        "nvs_close(h)",
    )
    save_tuples = (
        "nvs_open(tk::nvs_contract::kConfigNamespace,NVS_READWRITE,&h)",
        "nvs_set_u8(h,tk::nvs_contract::kDisplayRotation,",
        "nvs_commit(h)",
        "nvs_close(h)",
    )
    for value in load_tuples:
        require(load.count(value) == 1,
                f"display load DirectU8 operation/namespace/key/mode tuple drifted: {value}")
    for value in save_tuples:
        require(save.count(value) == 1,
                f"display save DirectU8 operation/namespace/key/mode tuple drifted: {value}")
    require([load.index(value) for value in load_tuples] ==
            sorted(load.index(value) for value in load_tuples),
            "display load DirectU8 operations are out of order")
    require([save.index(value) for value in save_tuples] ==
            sorted(save.index(value) for value in save_tuples),
            "display save DirectU8 operations are out of order")


def validate_direct_call_inventory(main: Path) -> None:
    direct_re = re.compile(r"\bnvs_([A-Za-z0-9_]+)\s*\(")
    identifier_re = re.compile(r"\bnvs_([A-Za-z0-9_]+)\b")
    actual_calls: dict[str, Counter[str]] = {}
    for path in sorted(main.rglob("*")):
        if not path.is_file() or path.suffix.lower() not in SOURCE_SUFFIXES:
            continue
        code = cpp_code_only(path.read_text(encoding="utf-8"))
        identifiers = identifier_re.findall(code)
        unknown = sorted(set(identifiers) - NVS_FUNCTION_IDENTIFIERS -
                         NON_FUNCTION_NVS_IDENTIFIERS)
        require(not unknown,
                "unknown or indirectly aliased shipped nvs_* identifier in "
                f"{path.relative_to(main.parent)}: {unknown}")
        function_identifiers = Counter(
            name for name in identifiers if name in NVS_FUNCTION_IDENTIFIERS
        )
        calls = Counter(direct_re.findall(code))
        require(function_identifiers == calls,
                "indirect/aliased nvs_* use is outside the lexical gate policy in "
                f"{path.relative_to(main.parent)}: identifiers={function_identifiers}, calls={calls}")
        if calls:
            actual_calls[path.relative_to(main.parent).as_posix()] = calls
    require(actual_calls == EXPECTED_DIRECT_CALLS,
            "exact shipped nvs_* call inventory drifted or contains an unknown API: "
            f"{actual_calls}")


def validate(root: Path) -> None:
    header = (root / "main/logic/nvs_contract.hpp").read_text(encoding="utf-8")
    adapter = (root / "main/nvs_storage.cpp").read_text(encoding="utf-8")
    rows = parse_registry(header)
    logical = [(row[0], row[1]) for row in rows]
    stored = [(row[0], row[2]) for row in rows]
    require(len(set(logical)) == len(logical), "NVS logical key collision")
    require(len(set(stored)) == len(stored), "NVS stored-key collision")
    for namespace, logical_key, stored_key, *_ in rows:
        validate_physical_name(namespace, logical_key, stored_key)
    require(rows == EXPECTED,
            "NVS registry differs from the exact 19-entry namespace/key/owner/API/retention contract")
    require('kConfigNamespace[] = "tesla_cfg"' in header and
            'kTeslaBleNamespace[] = "tesla_ble"' in header,
            "NVS namespace spelling drifted")

    require("nvs_contract::find(ns_kind_, key)" in adapter,
            "NVS adapter does not resolve every access through the production registry")
    require("entry->api != api" in adapter and "StorageApi::DirectU8" in adapter,
            "NVS adapter does not reject wrong API/direct-U8 access")
    require("return nullptr;" in adapter and "Namespace::Unknown" in adapter,
            "NVS adapter is not fail-closed for unknown keys/namespaces")
    require("entry.stored_key.size() > 15" in cpp_code_only(header),
            "C++ NVS registry no longer accepts exactly 15 bytes and rejects only longer keys")
    for forbidden in ("substr(0, 15)", "key.length() <= 15", "last resort"):
        require(forbidden not in adapter, f"NVS truncation fallback returned: {forbidden}")
    validate_adapter_methods(adapter)

    main = root / "main"
    for path in sorted(main.rglob("*")):
        if not path.is_file() or path.suffix.lower() not in SOURCE_SUFFIXES:
            continue
        text = path.read_text(encoding="utf-8")
        require(not literal_adapter_calls(text),
                f"production NVS access bypasses registry constants: {path.relative_to(root)}")
        require(literal_namespace_constructions(text) == 0,
                f"production NVS namespace bypasses registry constants: {path.relative_to(root)}")
    display = (main / "display.cpp").read_text(encoding="utf-8")
    validate_display_direct_u8(display)
    validate_direct_call_inventory(main)


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    require(old in text, f"self-test fixture text missing: {old}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


def self_test(root: Path) -> None:
    validate(root)
    # ESP-IDF's limit is fifteen payload bytes (sixteen including NUL): exercise both sides rather
    # than relying on today's shorter production names to distinguish >15 from >=15.
    validate_physical_name("Canary", "exactly_fifteen", "123456789012345")
    try:
        validate_physical_name("Canary", "sixteen", "1234567890123456")
    except ContractError as exc:
        require("15 bytes" in str(exc), f"16-byte boundary failed for wrong reason: {exc}")
    else:
        raise ContractError("NVS physical-name validator accepted sixteen bytes")

    mutations = (
        ("main/logic/nvs_contract.hpp",
         '{Namespace::Config, kLastTime, kLastTime, StorageApi::String,',
         '{Namespace::Config, kLastTime, "last_time_2", StorageApi::String,', "exact 19-entry"),
        ("main/logic/nvs_contract.hpp", "kPrivateKey, kPrivateKey, StorageApi::Blob",
         'kPrivateKey, "private_key_1234", StorageApi::Blob', "15 bytes"),
        ("main/logic/nvs_contract.hpp", "Namespace::TeslaBle, kPrivateKey",
         "Namespace::Config, kPrivateKey", "exact 19-entry"),
        ("main/logic/nvs_contract.hpp", 'kSessionInfotainment, "sess_info"',
         'kSessionInfotainment, "sess_other"', "exact 19-entry"),
        ("main/logic/nvs_contract.hpp", "entry.stored_key.size() > 15",
         "entry.stored_key.size() > 14", "exactly 15 bytes"),
        ("main/nvs_storage.cpp", "return nullptr;", "return key.data();", "fail-closed"),
        ("main/nvs_storage.cpp",
         "bool NvsStorageAdapter::save_str(const char* key, const std::string& value) {\n"
         "    if (!initialized_) return false;\n"
         "    const char* nvskey = map_key(key ? key : \"\", "
         "tk::nvs_contract::StorageApi::String);",
         "bool NvsStorageAdapter::save_str(const char* key, const std::string& value) {\n"
         "    if (!initialized_) return false;\n"
         "    const char* nvskey = map_key(key ? key : \"\", "
         "tk::nvs_contract::StorageApi::Blob);",
         "Method-to-StorageApi"),
        ("main/nvs_storage.cpp", "        out.swap(candidate);",
         "        out = candidate;", "atomic exception containment"),
        ("main/display.cpp",
         "nvs_open(tk::nvs_contract::kConfigNamespace, NVS_READONLY, &h)",
         "nvs_open(tk::nvs_contract::kTeslaBleNamespace, NVS_READONLY, &h)",
         "display load DirectU8"),
        ("main/display.cpp",
         "nvs_set_u8(h, tk::nvs_contract::kDisplayRotation,",
         "nvs_set_u8(h, tk::nvs_contract::kLegacyDisplayFlip,",
         "display save DirectU8"),
        ("main/display.cpp",
         "        nvs_set_u8(h, tk::nvs_contract::kDisplayRotation, (uint8_t)(rot & 3));\n"
         "        nvs_commit(h);",
         "        nvs_commit(h);\n"
         "        nvs_set_u8(h, tk::nvs_contract::kDisplayRotation, (uint8_t)(rot & 3));",
         "out of order"),
        ("main/display.cpp", "nvs_commit(h);", "nvs_commit(h);\n        nvs_commit(h);",
         "display save DirectU8"),
        ("main/main.cpp", "tk::nvs_contract::kLastTime,",
         '"rogue_time",', "bypasses registry"),
    )
    for relative, old, new, expected in mutations:
        with tempfile.TemporaryDirectory(prefix="nvs-contract-") as directory:
            fixture = Path(directory)
            shutil.copytree(root / "main", fixture / "main")
            replace_once(fixture / relative, old, new)
            try:
                validate(fixture)
            except ContractError as exc:
                require(expected in str(exc), f"mutation failed for wrong reason: {exc}")
            else:
                raise ContractError(f"NVS contract accepted mutation: {relative}: {old}")
    additions = (
        ("main/nested/rogue.cc", 'void rogue() { nvs_set_str(0, "rogue", "x"); }',
         "exact shipped nvs_* call inventory"),
        ("main/nested/rogue.hpp", "inline void rogue() { nvs_open(nullptr, 0, nullptr); }",
         "exact shipped nvs_* call inventory"),
        ("main/nested/rogue.inc", "nvs_register_callback(nullptr);",
         "unknown or indirectly aliased"),
        ("main/nested/alias.tpp",
         "inline void rogue() { auto fn = &nvs_set_u32; fn(0, nullptr, 1); }",
         "unknown or indirectly aliased"),
        ("main/nested/known_alias.h++",
         "inline void rogue() { auto fn = &nvs_set_str; fn(0, nullptr, nullptr); }",
         "indirect/aliased nvs_* use"),
        ("main/nested/literal.ipp",
         'inline void rogue(NvsStorageAdapter& s) { std::string x; s.load_str("last_time", x); }',
         "bypasses registry constants"),
        ("main/nested/namespace.cxx", 'void rogue() { NvsStorageAdapter s("tesla_cfg"); }',
         "namespace bypasses registry constants"),
    )
    for relative, source, expected in additions:
        with tempfile.TemporaryDirectory(prefix="nvs-contract-add-") as directory:
            fixture = Path(directory)
            shutil.copytree(root / "main", fixture / "main")
            path = fixture / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(source, encoding="utf-8")
            try:
                validate(fixture)
            except ContractError as exc:
                require(expected in str(exc),
                        f"NVS addition failed for wrong reason: {exc}")
            else:
                raise ContractError(f"NVS contract accepted new direct API: {relative}")

    accepted_non_code = (
        ("main/nested/comment_only.tpp",
         '// cfg.load_str("future_key", value); nvs_set_u32(0, "x", 1);\n'
         '// NvsStorageAdapter rogue("tesla_cfg");\n'),
        ("main/nested/string_only.h++",
         'inline const char* example() { return "cfg.load_str(\\\"future_key\\\", value); '
         'nvs_set_u32(0, x, 1); NvsStorageAdapter rogue(\\\"tesla_cfg\\\");"; }\n'),
    )
    for relative, source in accepted_non_code:
        with tempfile.TemporaryDirectory(prefix="nvs-contract-non-code-") as directory:
            fixture = Path(directory)
            shutil.copytree(root / "main", fixture / "main")
            path = fixture / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(source, encoding="utf-8")
            validate(fixture)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    try:
        validate(args.root.resolve())
        if args.self_test:
            self_test(args.root.resolve())
    except (OSError, UnicodeError, ContractError) as exc:
        print(f"nvs-contract: {exc}")
        return 1
    print("nvs-contract: PASS" + (" (mutation canaries)" if args.self_test else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
