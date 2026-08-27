#!/usr/bin/env python3
"""Runner-neutral repository hooks.

Hook payloads are read from stdin. Payload-sensitive guards inspect `cwd`,
`tool_name`, and `tool_input`; repository-scoped lifecycle, context, and formatting
actions stay anchored to the versioned hook core's own worktree.
"""

from __future__ import annotations

import argparse
import ast
import fnmatch
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
from typing import Any


FILE_TOOLS = {"read", "edit", "multiedit", "write"}
PATCH_TOOLS = {"apply_patch"}
SHELL_TOOLS = {"bash", "exec_command", "shell", "shell_command"}
HOOK_ROOT = Path(__file__).resolve().parents[2]


def normalized_tool(value: object) -> str:
    tool = str(value or "").strip()
    for separator in (".", "::"):
        if separator in tool:
            tool = tool.rsplit(separator, 1)[-1]
    return tool.lower()


def read_payload(*, fail_closed: bool) -> tuple[dict[str, Any] | None, str | None]:
    raw = sys.stdin.read()
    if not raw.strip():
        return (None, "hook payload is empty") if fail_closed else ({}, None)
    try:
        payload = json.loads(raw)
    except (TypeError, ValueError) as exc:
        return (None, f"hook payload is not valid JSON: {exc}") if fail_closed else ({}, None)
    if not isinstance(payload, dict):
        return (None, "hook payload must be a JSON object") if fail_closed else ({}, None)
    return payload, None


def tool_input(payload: dict[str, Any]) -> dict[str, Any]:
    value = payload.get("tool_input", {})
    return value if isinstance(value, dict) else {"command": value if isinstance(value, str) else ""}


def command_from(payload: dict[str, Any]) -> str:
    ti = tool_input(payload)
    values: list[str] = []
    for key in ("command", "cmd", "patch"):
        value = ti.get(key)
        if isinstance(value, str) and value:
            values.append(value)
    distinct = list(dict.fromkeys(values))
    return distinct[0] if len(distinct) == 1 else ""


def payload_cwd(payload: dict[str, Any]) -> Path:
    value = payload.get("cwd")
    if not isinstance(value, str) or not value:
        value = os.environ.get("AGENT_PROJECT_DIR") or os.environ.get("PROJECT_DIR") or os.getcwd()
    return Path(value).expanduser().resolve(strict=False)


def project_root(payload: dict[str, Any]) -> Path:
    cwd = payload_cwd(payload)
    probe = cwd if cwd.is_dir() else cwd.parent
    try:
        found = subprocess.run(
            ["git", "-C", str(probe), "rev-parse", "--show-toplevel"],
            check=True,
            capture_output=True,
            text=True,
            timeout=5,
        ).stdout.strip()
        if found:
            return Path(found).resolve(strict=False)
    except (FileNotFoundError, subprocess.SubprocessError):
        pass
    for candidate in (probe, *probe.parents):
        if (candidate / ".git").exists():
            return candidate
    return probe


def path_targets(payload: dict[str, Any]) -> list[str]:
    ti = tool_input(payload)
    values: list[str] = []
    for key in ("file_path", "path"):
        value = ti.get(key)
        if isinstance(value, str) and value:
            values.append(value)
    files = ti.get("files")
    if isinstance(files, list):
        for item in files:
            if isinstance(item, str) and item:
                values.append(item)
            elif isinstance(item, dict):
                for key in ("file_path", "path"):
                    value = item.get(key)
                    if isinstance(value, str) and value:
                        values.append(value)
    edits = ti.get("edits")
    if isinstance(edits, list):
        for item in edits:
            if isinstance(item, dict):
                value = item.get("file_path") or item.get("path")
                if isinstance(value, str) and value:
                    values.append(value)
    return values


def patch_targets(patch: str) -> list[str]:
    targets: list[str] = []
    patterns = (
        r"^\*\*\* (?:Add|Update|Delete) File:\s*(.+?)\s*$",
        r"^\*\*\* Move to:\s*(.+?)\s*$",
        r"^\+\+\+\s+(?:b/)?(.+?)\s*$",
    )
    for line in patch.splitlines():
        for pattern in patterns:
            match = re.match(pattern, line)
            if match:
                target = match.group(1)
                if target != "/dev/null":
                    targets.append(target)
                break
    return targets


def basename(path: str) -> str:
    return path.replace("\\", "/").rstrip("/").rsplit("/", 1)[-1].lower()


def is_sensitive_path(path: str) -> bool:
    normalized = path.strip().strip("'\"").replace("\\", "/").lower()
    base = basename(normalized)
    if not normalized:
        return False
    if base.endswith((".pem", ".key", ".p12", ".pfx", ".jks", ".keystore")):
        return True
    if base in {
        ".git-credentials",
        ".netrc",
        ".npmrc",
        ".pypirc",
        "credentials",
        "credentials.json",
        "secrets.env",
        "sdkconfig.local",
        "ota_signing_key.pem",
        "id_rsa",
        "id_dsa",
        "id_ecdsa",
        "id_ed25519",
        "id_ed25519_sk",
        "private_key",
        "credentials.yml",
        "credentials.yaml",
        "nvs.bin",
        "nvs_dump.bin",
        "nvs-backup.bin",
        "vehicle_private_key.bin",
        "ble_sessions.bin",
    }:
        return True
    if re.search(r"(?:^|[-_.])nvs(?:[-_.].*)?(?:dump|backup)?\.bin$", base):
        return True
    if re.search(r"(?:vehicle|tesla|ble)[-_.].*(?:private[-_.]?key|session).*(?:\.bin|\.json|\.dump)?$", base):
        return True
    if base.startswith("id_") and not base.endswith(".pub"):
        return True
    if base == ".env" or (base.startswith(".env.") and base not in {".env.example", ".env.sample", ".env.template"}):
        return True
    if "/.aws/credentials" in normalized or normalized.endswith(".aws/credentials"):
        return True
    if "/.config/gh/hosts.yml" in normalized or normalized.endswith(".config/gh/hosts.yml"):
        return True
    if "/.docker/config.json" in normalized or normalized.endswith(".docker/config.json"):
        return True
    if "/.gem/credentials" in normalized or normalized.endswith(".gem/credentials"):
        return True
    if "/.kube/config" in normalized or normalized.endswith(".kube/config"):
        return True
    if "/.ssh/" in normalized and not base.endswith(".pub") and base not in {"config", "known_hosts", "authorized_keys"}:
        return base.startswith("id_") or "private" in base
    return False


SENSITIVE_COMMAND_PATTERNS = (
    r"(?:^|[\s'\"=:/])(?:[^\s'\"]*/)?[^\s'\"]*\.(?:pem|key|p12|pfx|jks|keystore)(?:[\s'\";&|<>]|$)",
    r"(?:^|[\s'\"=/])(?:\.git-credentials|\.netrc|\.npmrc|\.pypirc|secrets\.env|sdkconfig\.local)(?:[\s'\";&|<>]|$)",
    r"(?:^|[\s'\"=/])(?:id_rsa|id_dsa|id_ecdsa|id_ed25519)(?:[\s'\";&|<>]|$)",
    r"\.aws/credentials(?:[\s'\";&|<>]|$)",
    r"\.config/gh/hosts\.yml(?:[\s'\";&|<>]|$)",
    r"\.docker/config\.json(?:[\s'\";&|<>]|$)",
    r"\.gem/credentials(?:[\s'\";&|<>]|$)",
    r"\.kube/config(?:[\s'\";&|<>]|$)",
    r"(?:^|[\s'\"=/])\.env(?:\.[A-Za-z0-9_-]+)?(?:[\s'\";&|<>]|$)",
    r"(?:^|[\s'\"=/])(?:nvs(?:[-_.][A-Za-z0-9_-]+)*(?:dump|backup)?\.bin|vehicle[_-]private[_-]key(?:\.[A-Za-z0-9_-]+)?|ble[_-]sessions?(?:\.[A-Za-z0-9_-]+)?)(?:[\s'\";&|<>]|$)",
)

SENSITIVE_ENV_REFERENCE = re.compile(
    r"\$(?:\{)?(?:"
    r"GH_TOKEN|GITHUB_TOKEN|OPENAI_API_KEY|ANTHROPIC_API_KEY|"
    r"AWS_SESSION_TOKEN|AWS_[A-Z0-9_]*KEY[A-Z0-9_]*|"
    r"NPM_TOKEN|PYPI_TOKEN|GITLAB_TOKEN|DOCKER_PASSWORD|"
    r"SLACK_TOKEN|STRIPE_SECRET_KEY|SENTRY_AUTH_TOKEN|CLOUDFLARE_API_TOKEN|"
    r"OTA_SIGNING_KEY_FILE|OTA_SIGNING_KEY|TESLA_PRIVATE_KEY|VEHICLE_PRIVATE_KEY|BLE_SESSION_KEY"
    r")(?:\})?",
)

SHELL_EXTGLOB = re.compile(r"(?<!\\)[@+?!*]\([^)]*\)")


def shell_mentions_sensitive(command: str) -> bool:
    return any(re.search(pattern, command, flags=re.IGNORECASE) for pattern in SENSITIVE_COMMAND_PATTERNS)


def shell_mentions_sensitive_env(command: str) -> bool:
    return SENSITIVE_ENV_REFERENCE.search(command) is not None


SHELL_ASSIGNMENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*=.*$", re.DOTALL)

SUDO_OPTIONS_WITH_VALUE = {
    "-C",
    "--chdir",
    "-D",
    "--chroot",
    "-g",
    "--group",
    "-h",
    "--host",
    "-p",
    "--prompt",
    "-R",
    "--role",
    "-T",
    "--type",
    "-u",
    "--user",
}


def consume_sudo_options(tokens: list[str]) -> list[str]:
    index = 1
    while index < len(tokens):
        token = tokens[index]
        if token == "--":
            return tokens[index + 1 :]
        if token in SUDO_OPTIONS_WITH_VALUE:
            index += 2
            continue
        if any(token.startswith(option + "=") for option in SUDO_OPTIONS_WITH_VALUE if option.startswith("--")):
            index += 1
            continue
        if token.startswith("-"):
            index += 1
            continue
        break
    return tokens[index:]


def effective_shell_tokens(segment: str) -> tuple[list[str], bool]:
    """Return the command after wrappers/env; bool means env itself would print its state."""
    try:
        tokens = shlex.split(segment, posix=True)
    except ValueError:
        return [], False
    while tokens and SHELL_ASSIGNMENT.fullmatch(tokens[0]):
        tokens.pop(0)
    for _ in range(8):
        if not tokens:
            break
        executable = Path(tokens[0]).name
        if executable == "sudo":
            tokens = consume_sudo_options(tokens)
            continue
        if executable in {"command", "builtin", "exec"}:
            tokens = tokens[1:]
            while tokens and tokens[0].startswith("-"):
                tokens = tokens[1:]
            continue
        if executable in {"!", "{", "}", "if", "then", "elif", "else", "do", "while", "until"}:
            tokens = tokens[1:]
            continue
        if executable in {"time", "nohup"}:
            tokens = tokens[1:]
            while tokens and tokens[0].startswith("-"):
                option = tokens.pop(0)
                if option in {"-f", "--format", "-o", "--output"} and tokens:
                    tokens.pop(0)
            continue
        if executable == "nice":
            tokens = tokens[1:]
            while tokens and tokens[0].startswith("-"):
                option = tokens.pop(0)
                if option in {"-n", "--adjustment"} and tokens:
                    tokens.pop(0)
            continue
        if executable == "env":
            args = tokens[1:]
            index = 0
            while index < len(args):
                token = args[index]
                if token == "--":
                    index += 1
                    break
                if token in {"-u", "--unset", "-C", "--chdir", "-S", "--split-string"}:
                    index += 2
                    continue
                if token.startswith("-"):
                    index += 1
                    continue
                break
            while index < len(args) and SHELL_ASSIGNMENT.fullmatch(args[index]):
                index += 1
            tokens = args[index:]
            if not tokens:
                return [], True
            continue
        break
    return tokens, bool(tokens and Path(tokens[0]).name == "env")


def normalize_ansi_c_quotes(command: str) -> str:
    def decode(match: re.Match[str]) -> str:
        try:
            return bytes(match.group(1), "utf-8").decode("unicode_escape")
        except UnicodeDecodeError:
            return match.group(0)

    command = re.sub(r"\\\r?\n", "", command)
    normalized = re.sub(r"\$'((?:[^'\\]|\\.)*)'", decode, command)
    return re.sub(r'\$"((?:[^"\\]|\\.)*)"', r'"\1"', normalized)


def shell_token_sets(command: str, depth: int = 0) -> list[tuple[list[str], bool]]:
    """Tokenize top-level commands and recursively inspect shell/eval string wrappers."""
    if depth > 4:
        return [([], True)]
    command = normalize_ansi_c_quotes(command)
    results: list[tuple[list[str], bool]] = []
    for substitution in re.findall(r"(?<!\\)`([^`]*)`", command, flags=re.DOTALL):
        results.extend(shell_token_sets(substitution, depth + 1))
    for substitution in re.findall(r"(?<!\\)\$\(([^()]*)\)", command, flags=re.DOTALL):
        results.extend(shell_token_sets(substitution, depth + 1))
    try:
        lexer = shlex.shlex(command.replace("\n", " ; "), posix=True, punctuation_chars=";&|()!<>")
        lexer.whitespace_split = True
        lexer.commenters = ""
        raw_tokens = list(lexer)
    except ValueError:
        return [([], True)]
    segments: list[list[str]] = [[]]
    for token in raw_tokens:
        if re.fullmatch(r"[;&|()!<>]+", token):
            if segments[-1]:
                segments.append([])
            continue
        segments[-1].append(token)
    for segment_tokens in (segment for segment in segments if segment):
        tokens, env_without_command = effective_shell_tokens(shlex.join(segment_tokens))
        results.append((tokens, env_without_command))
        if not tokens:
            continue
        executable = Path(tokens[0]).name
        if executable in {"bash", "dash", "sh", "zsh"}:
            command_flag = next(
                (
                    index
                    for index, token in enumerate(tokens[1:], 1)
                    if token == "--command"
                    or (token.startswith("-") and not token.startswith("--") and "c" in token[1:])
                ),
                None,
            )
            if command_flag is not None and command_flag + 1 < len(tokens):
                results.extend(shell_token_sets(tokens[command_flag + 1], depth + 1))
        elif executable == "eval" and len(tokens) > 1:
            results.extend(shell_token_sets(" ".join(tokens[1:]), depth + 1))
    return results


def shell_dumps_environment(command: str) -> bool:
    if re.search(r"\bprintenv\b", command):
        return True
    if re.search(r"/proc/(?:self|[0-9]+)/environ(?:[\s'\";&|<>]|$)", command):
        return True
    if re.search(r"(?:^|[;&|]\s*)ps\s+e(?:w{0,2})(?:\s|$)", command):
        return True
    if re.search(r"\b(?:os\.(?:environ|getenv)|process\.env|ENVIRON)\b", command):
        return True
    for segment in shell_segments(command):
        for index, token in enumerate(segment):
            executable = Path(token).name
            if executable == "printenv":
                return True
            if executable == "env" and all(
                following in {";", "+", "{}"} for following in segment[index + 1 :]
            ):
                return True
    for tokens, env_without_command in shell_token_sets(command):
        if env_without_command:
            return True
        if not tokens:
            continue
        executable = Path(tokens[0]).name
        if executable == "ps" and any(
            (argument.startswith("-") and "E" in argument.lstrip("-"))
            or (not argument.startswith("-") and re.search(r"[eE]", argument))
            for argument in tokens[1:]
            if re.fullmatch(r"-?[A-Za-z]+", argument)
        ):
            return True
        if executable == "printenv":
            return True
        if executable == "set" and len(tokens) == 1:
            return True
        # All three builtins have output-producing forms whose option semantics differ between
        # bash and zsh (`declare GH_TOKEN`, `typeset +x`, naked `readonly`, ...).  Agents do not
        # need these declaration builtins for repository work, so fail closed instead of trying to
        # maintain a shell-version-specific list of printing combinations.
        if executable in {"declare", "typeset", "readonly"}:
            return True
        if executable == "export":
            args = tokens[1:]
            if (
                not args
                or any(token in {"-p", "--print"} for token in args)
                or not any(not token.startswith("-") for token in args)
            ):
                return True
    return False


def shell_dumps_credentials(command: str) -> bool:
    shell = r"(?:bash|dash|sh|zsh)"
    if any(
        re.search(pattern, command, flags=re.IGNORECASE)
        for pattern in (
            r"\bauth\s+(?:application-default\s+)?(?:token|print-(?:access|identity)-token)\b",
            r"\b(?:git|docker)-credential-[A-Za-z0-9_-]+\b[^;&|\n]*\bget\b",
            r"\bconfig\s+view\b[^;&|\n]*--raw(?:=|\b)",
            r"\bconfigure\s+(?:export-credentials|get\b[^;&|\n]*(?:access_key|secret|session_token))",
        )
    ):
        return True
    if (
        re.search(rf"\|[^|;\n]*\b{shell}\b", command)
        or re.search(rf"\b{shell}\b[^;&|\n]*(?:<<<|(?<!<)<(?!<))", command)
        or re.search(rf"\b{shell}\b(?:\s+-[^;&|\n]*)*\s+-s(?:\s|$)", command)
    ):
        return True
    for tokens, _ in shell_token_sets(command):
        if not tokens:
            continue
        for command_index, token in enumerate(tokens):
            executable = Path(token).name
            args = tokens[command_index + 1 :]
            if executable == "gh" and (
                any(args[index : index + 2] == ["auth", "token"] for index in range(len(args) - 1))
                or "--show-token" in args
                or (
                    any(args[index : index + 2] == ["auth", "status"] for index in range(len(args) - 1))
                    and "-t" in args
                )
            ):
                return True
            if executable == "git" and (
                any(args[index : index + 2] == ["credential", "fill"] for index in range(len(args) - 1))
                or any(
                    args[index].startswith("credential-") and args[index + 1] == "get"
                    for index in range(len(args) - 1)
                )
            ):
                return True
            if executable.startswith("git-credential-") and args[:1] == ["get"]:
                return True
            if executable.startswith("docker-credential-") and args[:1] == ["get"]:
                return True
            if executable == "aws" and any(
                args[index : index + 2] == ["configure", "export-credentials"]
                or (
                    args[index : index + 2] == ["configure", "get"]
                    and index + 2 < len(args)
                    and args[index + 2].split(".")[-1]
                    in {"aws_access_key_id", "aws_secret_access_key", "aws_session_token"}
                )
                for index in range(len(args))
            ):
                return True
            if executable == "gcloud" and any(
                args[index : index + 2]
                in (["auth", "print-access-token"], ["auth", "print-identity-token"])
                or args[index : index + 3] == ["auth", "application-default", "print-access-token"]
                for index in range(len(args))
            ):
                return True
            if executable == "kubectl" and any(
                args[index : index + 2] == ["config", "view"] for index in range(len(args))
            ) and any(arg == "--raw" or arg.startswith("--raw=") for arg in args):
                return True
            if executable == "security" and any(arg.startswith("find-") for arg in args) and any(
                arg in {"-g", "-w"} for arg in args
            ):
                return True
            if executable == "security" and "export" in args:
                return True
    return False


def expand_static_braces(token: str, depth: int = 0, budget: list[int] | None = None) -> list[str]:
    ambiguous = "__AGENT_AMBIGUOUS_BRACE__"
    if budget is None:
        budget = [256]
    if depth > 32:
        return [ambiguous]
    match = re.search(r"\{([^{}]+)\}", token)
    if not match:
        return [token]
    body = match.group(1)
    choices: list[str]
    if "," in body:
        choices = list(dict.fromkeys(body.split(",")))
    else:
        parts = body.split("..")
        if len(parts) not in {2, 3}:
            return [token]
        start, end = parts[:2]
        step_text = parts[2] if len(parts) == 3 else ""
        try:
            if len(start) == len(end) == 1 and not start.isdigit() and not end.isdigit():
                start_value, end_value = ord(start), ord(end)
                default_step = 1 if start_value <= end_value else -1
                step = int(step_text) if step_text else default_step
                if step == 0 or (end_value - start_value) * step < 0:
                    return [token]
                values = range(start_value, end_value + (1 if step > 0 else -1), step)
                choices = [chr(value) for value in values]
            elif re.fullmatch(r"-?\d+", start) and re.fullmatch(r"-?\d+", end):
                start_value, end_value = int(start), int(end)
                default_step = 1 if start_value <= end_value else -1
                step = int(step_text) if step_text else default_step
                if step == 0 or (end_value - start_value) * step < 0:
                    return [token]
                values = range(start_value, end_value + (1 if step > 0 else -1), step)
                choices = [str(value) for value in values]
            else:
                return [token]
        except (ValueError, OverflowError):
            return [token]
        if len(choices) > 256:
            return [ambiguous]
    # Charge the global expansion budget before descending.  Checking only after a complete child
    # subtree makes a short token containing many `{a,b}` groups exponential and can outlive the
    # hook timeout.  Exhaustion is policy ambiguity, so callers fail closed.
    if len(choices) > budget[0]:
        return [ambiguous]
    budget[0] -= len(choices)
    expanded: list[str] = []
    for choice in choices:
        replacement = token[: match.start()] + choice + token[match.end() :]
        child = expand_static_braces(replacement, depth + 1, budget)
        if ambiguous in child:
            return [ambiguous]
        expanded.extend(child)
        if len(expanded) > 256:
            return [ambiguous]
    return list(dict.fromkeys(expanded))


def token_can_resolve_sensitive(token: str) -> bool:
    for expanded in expand_static_braces(token):
        if expanded == "__AGENT_AMBIGUOUS_BRACE__":
            return True
        candidates = [expanded]
        if "=" in expanded:
            candidates.append(expanded.split("=", 1)[1])
        for candidate in candidates:
            if is_sensitive_path(candidate):
                return True
            base_pattern = basename(candidate)
            if not any(character in base_pattern for character in "*?["):
                continue
            representative_basenames = {
                "ota_signing_key.pem",
                "private.pem",
                "private.key",
                "private.p12",
                "private.pfx",
                "private.jks",
                "private.keystore",
                ".git-credentials",
                ".netrc",
                ".npmrc",
                ".pypirc",
                "secrets.env",
                "sdkconfig.local",
                "credentials",
                "credentials.json",
                ".env",
                ".env.production",
                "id_rsa",
                "id_dsa",
                "id_ecdsa",
                "id_ed25519",
                "id_ed25519_sk",
                "private_key",
            }
            if any(fnmatch.fnmatchcase(name, base_pattern) for name in representative_basenames):
                return True
            normalized = candidate.replace("\\", "/").lower()
            path_specific = {
                "/.aws/": "credentials",
                "/.gem/": "credentials",
                "/.config/gh/": "hosts.yml",
                "/.docker/": "config.json",
                "/.kube/": "config",
            }
            if any(marker in normalized and fnmatch.fnmatchcase(name, base_pattern) for marker, name in path_specific.items()):
                return True
    return False


def shell_mentions_sensitive_token_path(command: str) -> bool:
    for tokens, _ in shell_token_sets(command):
        for token in tokens:
            if token_can_resolve_sensitive(token):
                return True
    return False


def is_exact_espsecure_sign(command: str) -> bool:
    if "\n" in command or re.search(r"[;&|`<>]|\$\(", command):
        return False
    try:
        tokens = shlex.split(command, posix=True)
    except ValueError:
        return False
    if not tokens:
        return False
    if Path(tokens[0]).name in {"python", "python3"}:
        if len(tokens) < 4 or tokens[1:3] != ["-m", "espsecure"]:
            return False
        tokens = tokens[2:]
    executable = Path(tokens[0]).name
    if executable not in {"espsecure", "espsecure.py"} or len(tokens) < 3:
        return False
    if tokens[1] not in {"sign_data", "sign-data"}:
        return False
    key_flags = [index for index, token in enumerate(tokens) if token == "--keyfile"]
    if len(key_flags) != 1 or key_flags[0] + 1 >= len(tokens):
        return False
    key_index = key_flags[0] + 1
    key_path = tokens[key_index]
    key_reference = key_path in {"$OTA_SIGNING_KEY_FILE", "${OTA_SIGNING_KEY_FILE}"}
    if basename(key_path) != "ota_signing_key.pem" and not key_reference:
        return False

    # The private key path is allowed in exactly one semantic position: the value of --keyfile.
    # Reject it (or any other sensitive path) as DATAFILE, output, a second key, or an option value.
    # Otherwise espsecure could be abused as the copy primitive that the surrounding guard forbids.
    for index, token in enumerate(tokens):
        if index == key_index:
            continue
        if is_sensitive_path(token) or shell_mentions_sensitive_env(token):
            return False
    return True


def policy_violation_reason(payload: dict[str, Any]) -> str | None:
    raw_tool = payload.get("tool_name")
    if not isinstance(raw_tool, str) or not raw_tool.strip():
        return "hook payload has no non-empty string tool_name"
    tool = normalized_tool(raw_tool)
    if tool not in FILE_TOOLS | PATCH_TOOLS | SHELL_TOOLS:
        return "hook payload names an unsupported matched tool; its value was redacted"
    if tool in FILE_TOOLS:
        targets = path_targets(payload)
        if not targets:
            return "cannot determine the file-tool target from the hook payload"
        for target in targets:
            if is_sensitive_path(target):
                return "the requested path is credential or private-key material and must not enter agent context"
        return None
    if tool in PATCH_TOOLS:
        command = command_from(payload)
        targets = patch_targets(command)
        if not command or not targets:
            return "cannot determine apply_patch targets from the hook payload"
        for target in targets:
            if is_sensitive_path(target):
                return "apply_patch targets sensitive credential or private-key material; the path was redacted"
        return None
    if tool in SHELL_TOOLS:
        command = command_from(payload)
        if not command:
            return "cannot determine the shell-tool command from the hook payload"
        if SHELL_EXTGLOB.search(command):
            return "shell extglob expansion is not statically bounded by the credential/partition guard"
        if shell_dumps_environment(command):
            return "the command would dump process environment values, which may include credentials"
        if shell_dumps_credentials(command):
            return "the command would print an authentication token or credential value"
        if (
            shell_mentions_sensitive_env(command)
            or shell_mentions_sensitive_token_path(command)
            or shell_mentions_sensitive(command)
        ) and not is_exact_espsecure_sign(command):
            return "the command reads, copies, stages, or otherwise exposes credential/private-key material"
        return None
    return None


def is_partitions_path(path: str) -> bool:
    return basename(path) == "partitions.csv"


def command_tokens(segment: str) -> list[str]:
    try:
        tokens = shlex.split(segment, posix=True)
    except ValueError:
        return []
    while tokens and ("=" in tokens[0] and not tokens[0].startswith(("=", "-"))):
        tokens.pop(0)
    if tokens and tokens[0] in {"sudo", "command", "builtin"}:
        tokens.pop(0)
    return tokens


PARTITIONS_MENTION = re.compile(
    r"(?:^|[^A-Za-z0-9_.-])partitions\.csv(?:$|[^A-Za-z0-9_.-])", re.IGNORECASE
)


def shell_segments(command: str) -> list[list[str]]:
    command = normalize_ansi_c_quotes(command)
    try:
        lexer = shlex.shlex(command.replace("\n", " ; "), posix=True, punctuation_chars=";&|()!<>")
        lexer.whitespace_split = True
        lexer.commenters = ""
        tokens = list(lexer)
    except ValueError:
        return []
    segments: list[list[str]] = [[]]
    for token in tokens:
        if re.fullmatch(r"[;&|()!]+", token):
            if segments[-1]:
                segments.append([])
            continue
        segments[-1].append(token)
    return [segment for segment in segments if segment]


def token_can_resolve_partitions(token: str) -> bool:
    for expanded in expand_static_braces(token):
        if expanded == "__AGENT_AMBIGUOUS_BRACE__":
            return True
        for piece in re.findall(r"[A-Za-z0-9_.?*\[\]-]+", expanded.lower()):
            if piece == "partitions.csv" or fnmatch.fnmatchcase("partitions.csv", piece):
                return True
    return False


def partition_segment_is_read_only(tokens: list[str]) -> bool:
    for index, token in enumerate(tokens):
        if token_can_resolve_partitions(token) and index > 0 and re.fullmatch(r"\d*>+|>+", tokens[index - 1]):
            return False
    tokens, _ = effective_shell_tokens(shlex.join(tokens))
    if not tokens:
        return False
    executable = Path(tokens[0]).name.lower()
    args = tokens[1:]
    if executable in {
        "cat",
        "cmp",
        "diff",
        "file",
        "grep",
        "egrep",
        "fgrep",
        "head",
        "ls",
        "rg",
        "sha256sum",
        "shasum",
        "stat",
        "tail",
        "wc",
    }:
        if executable in {"cmp", "diff"} and any(
            arg == "--output" or arg.startswith("--output=") for arg in args
        ):
            return False
        if executable == "rg" and any(arg == "--pre" or arg.startswith("--pre=") for arg in args):
            return False
        return True
    if executable == "git" and args:
        if args[0] not in {"diff", "grep", "log", "show", "status"}:
            return False
        return not any(
            arg in {"--output", "--ext-diff", "--textconv"} or arg.startswith("--output=")
            for arg in args[1:]
        )
    return False


def shell_writes_partitions(command: str) -> bool:
    if any(is_partitions_path(target) for target in patch_targets(command)):
        return True
    if re.search(r"\bpartitions\b", command, re.IGNORECASE) and ".csv" in command and re.search(
        r"[$+]|\b(?:python|python3|node|awk)\b", command
    ):
        return True
    # A normally read-only tool becomes a writer when the protected path is its redirection target.
    # Check this before the executable allowlist so `cat source > partitions.csv` cannot pass as a
    # harmless cat invocation.
    if re.search(
        r">{1,2}\s*['\"]?[^\s'\"]*partitions\.csv(?:[\s'\";&|]|$)",
        command,
        re.IGNORECASE,
    ):
        return True
    segments = shell_segments(command)
    if not segments:
        return bool(PARTITIONS_MENTION.search(command) or "*.csv" in command or "partitions.*" in command)
    if not any(token_can_resolve_partitions(token) for segment in segments for token in segment):
        return False
    mentioned = False
    for tokens in segments:
        if not any(token_can_resolve_partitions(token) for token in tokens):
            continue
        mentioned = True
        if not partition_segment_is_read_only(tokens):
            return True
    # A mention that disappeared during tokenization is ambiguous and therefore blocked.
    return not mentioned


def partition_violation(payload: dict[str, Any], *, shell_only: bool = False) -> bool:
    tool = normalized_tool(payload.get("tool_name"))
    if tool in SHELL_TOOLS:
        return shell_writes_partitions(command_from(payload))
    if shell_only:
        return False
    if tool in FILE_TOOLS:
        return tool in {"edit", "multiedit", "write"} and any(
            is_partitions_path(path) for path in path_targets(payload)
        )
    if tool in PATCH_TOOLS:
        command = command_from(payload)
        return any(is_partitions_path(path) for path in patch_targets(command))
    return False


def emit_permission(decision: str, reason: str) -> None:
    print(
        json.dumps(
            {
                "hookSpecificOutput": {
                    "hookEventName": "PreToolUse",
                    "permissionDecision": decision,
                    "permissionDecisionReason": reason,
                }
            },
            separators=(",", ":"),
        )
    )


def guard_secrets(payload: dict[str, Any] | None, error: str | None) -> bool:
    reason = error or (policy_violation_reason(payload or {}) if payload is not None else "unknown payload error")
    if not reason:
        return False
    emit_permission(
        "deny",
        "Blocked by the repository secret guard: "
        + reason
        + ". Do not read or copy the value. The sole key-path exception is an unchained espsecure sign_data invocation.",
    )
    return True


def guard_partitions(payload: dict[str, Any], *, shell_only: bool = False) -> bool:
    if not partition_violation(payload, shell_only=shell_only):
        return False
    invariant = (
        "partitions.csv is protected: NVS must remain at offset 0x9000 with size 0x6000, and "
        "otadata/OTA slot offsets must not move; a lexical hook cannot prove those invariants"
    )
    emit_permission(
        "deny",
        invariant
        + ". Do not retry through a wrapper or alternate tool; an explicitly authorized maintainer must review "
        "and apply any partition-table change outside this agent hook path.",
    )
    return True


def run_pre_tool_guards(args: argparse.Namespace) -> int:
    payload, error = read_payload(fail_closed=True)
    if guard_secrets(payload, error):
        return 0
    assert payload is not None
    guard_partitions(payload, shell_only=args.partition_shell_only)
    return 0


def eligible_format_path(root: Path, target: str) -> Path | None:
    path = Path(target).expanduser()
    if not path.is_absolute():
        path = root / path
    path = path.resolve(strict=False)
    try:
        relative = path.relative_to(root)
    except ValueError:
        return None
    parts = relative.parts
    if not parts or path.suffix not in {".cpp", ".hpp"}:
        return None
    if parts[0] == "main" and len(parts) >= 2 and parts[1] != "def":
        return path
    if parts[0] == "test":
        return path
    return None


def run_format(_: argparse.Namespace) -> int:
    payload, _ = read_payload(fail_closed=False)
    if not payload or shutil.which("clang-format") is None:
        return 0
    tool = normalized_tool(payload.get("tool_name"))
    targets = path_targets(payload)
    if tool in PATCH_TOOLS:
        targets.extend(patch_targets(command_from(payload)))
    root = HOOK_ROOT
    for target in dict.fromkeys(targets):
        path = eligible_format_path(root, target)
        if path is not None and path.is_file():
            subprocess.run(["clang-format", "-i", str(path)], check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return 0


def run_capabilities(_: argparse.Namespace) -> int:
    payload, _ = read_payload(fail_closed=False)
    root = HOOK_ROOT
    print(f"tesla-key-esp32 capabilities ({root}):")
    docker = shutil.which("docker")
    docker_ok = False
    if docker:
        try:
            docker_ok = subprocess.run(
                [docker, "info"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5, check=False
            ).returncode == 0
        except subprocess.SubprocessError:
            docker_ok = False
    print("  + Docker daemon: firmware builds available" if docker_ok else "  - No Docker daemon: firmware build unavailable")
    esptool = shutil.which("esptool") or shutil.which("esptool.py")
    ports = []
    for pattern in ("cu.usbmodem*", "ttyUSB*", "ttyACM*"):
        ports.extend(str(path) for path in Path("/dev").glob(pattern))
    if esptool and ports:
        print("  + esptool and serial device present: local USB work may be possible after explicit authorization")
    elif esptool:
        print("  ~ esptool present, no serial device detected")
    else:
        print("  - No esptool: USB flashing unavailable")
    host = any(shutil.which(tool) for tool in ("cmake", "g++", "clang++"))
    print("  + Host C++ toolchain: mock tests available" if host else "  - No host C++ toolchain: rely on CI")
    return 0


def run_subagent_context(_: argparse.Namespace) -> int:
    payload, _ = read_payload(fail_closed=False)
    payload = payload or {}
    root = HOOK_ROOT
    agent_type = str(payload.get("agent_type") or "subagent")
    print("<repository-subagent-context>")
    print(f"Project root: {root}")
    print(f"Agent type: {agent_type}")
    print("Work only inside the scope assigned by the parent. Treat other worktree changes as another agent's work.")
    print("Review agents remain read-only; writing agents use explicit file ownership and report changed files plus checks.")
    print("Never read signing, vehicle, pairing, BLE-session, credential, or NVS-dump material; never edit partitions.csv.")
    print("Do not wake the vehicle, send commands, flash, OTA, erase, restore, publish, or merge without explicit authority.")
    print("Keep hardware-free logic in main/logic with host tests and separate host, CI, signing, USB, vehicle, and UI evidence.")
    print("</repository-subagent-context>")
    return 0


def verify_build_efficiency_report_only() -> tuple[bool, str]:
    """Prove the SessionStart handler itself contains only context I/O, never mutation calls."""
    tree = ast.parse(Path(__file__).read_text(encoding="utf-8"))
    function = next(
        node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == "run_build_efficiency"
    )
    allowed = {"print", "read_payload", "verify_build_efficiency_report_only"}
    calls = {
        node.func.id
        for node in ast.walk(function)
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Name)
    }
    attribute_calls = {
        node.func.attr
        for node in ast.walk(function)
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
    }
    unexpected = sorted((calls - allowed) | attribute_calls)
    helper = next(
        node
        for node in tree.body
        if isinstance(node, ast.FunctionDef) and node.name == "verify_build_efficiency_report_only"
    )
    helper_names = {
        node.func.id
        for node in ast.walk(helper)
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Name)
    }
    helper_attributes = {
        node.func.attr
        for node in ast.walk(helper)
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
    }
    helper_allowed_names = {"Path", "isinstance", "next", "sorted"}
    helper_allowed_attributes = {"extend", "join", "parse", "read_text", "walk"}
    unexpected.extend(sorted(helper_names - helper_allowed_names))
    unexpected.extend(sorted(helper_attributes - helper_allowed_attributes))
    return (not unexpected, ", ".join(unexpected))


def run_build_efficiency(args: argparse.Namespace) -> int:
    if args.self_test:
        safe, unexpected = verify_build_efficiency_report_only()
        if not safe:
            print(f"build-efficiency report-only self-test failed: unexpected calls: {unexpected}", file=sys.stderr)
            return 2
        print("build-efficiency report-only self-test: PASS")
        return 0
    read_payload(fail_closed=False)
    print("<build-efficiency-context>")
    print("Report-only: inspect the latest completed main build for cache hit rate, duration, artifacts, and target sizes.")
    print("SessionStart must not create issues, branches, commits, or draft PRs; any mutation needs an explicit user request.")
    print("Use the pinned ESP-IDF 5.5.5 Docker entrypoints and keep esp32/esp32s3/esp32c3/esp32c6 evidence separate.")
    print("</build-efficiency-context>")
    return 0


def run_stop_logic_tests(_: argparse.Namespace) -> int:
    payload, _ = read_payload(fail_closed=False)
    payload = payload or {}
    if payload.get("stop_hook_active") is True:
        return 0
    root = HOOK_ROOT
    if shutil.which("git"):
        unstaged = subprocess.run(
            ["git", "-C", str(root), "diff", "--quiet", "--", "main/", "test/"],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        ).returncode
        staged = subprocess.run(
            ["git", "-C", str(root), "diff", "--cached", "--quiet", "--", "main/", "test/"],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        ).returncode
        untracked = subprocess.run(
            ["git", "-C", str(root), "ls-files", "--others", "--exclude-standard", "--", "main/", "test/"],
            check=False,
            capture_output=True,
            text=True,
        )
        if unstaged == 0 and staged == 0 and untracked.returncode == 0 and not untracked.stdout.strip():
            return 0
    def block(reason: str) -> int:
        message = ("Host logic tests could not prove the changed main/test boundary:\n" + reason)[:4000]
        print(json.dumps({"decision": "block", "reason": message}, separators=(",", ":")))
        return 0

    missing = [tool for tool in ("git", "python3", "cmake", "node") if shutil.which(tool) is None]
    if not any(shutil.which(tool) for tool in ("g++", "clang++")):
        missing.append("g++ or clang++")
    if missing:
        return block("required --require-all tools are unavailable: " + ", ".join(missing))
    script = root / "scripts/run-mock-tests.sh"
    if not script.is_file() or script.is_symlink() or not os.access(script, os.X_OK):
        return block("scripts/run-mock-tests.sh is missing, a symlink, or not executable")
    try:
        result = subprocess.run(
            [str(script), "--require-all"],
            cwd=root,
            check=False,
            capture_output=True,
            text=True,
            timeout=540,
        )
        if result.returncode == 0:
            return 0
        output = (result.stdout + result.stderr).strip()
    except subprocess.TimeoutExpired as exc:
        output = f"host logic tests exceeded 540 seconds: {exc}"
    except OSError as exc:
        output = f"host logic tests could not be started: {exc}"
    return block(output or f"host logic tests exited {result.returncode} without diagnostics")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    guards = subparsers.add_parser("pre-tool-guards")
    guards.add_argument("--partition-shell-only", action="store_true")
    guards.set_defaults(func=run_pre_tool_guards)
    formatter = subparsers.add_parser("format")
    formatter.set_defaults(func=run_format)
    capabilities = subparsers.add_parser("capabilities")
    capabilities.set_defaults(func=run_capabilities)
    subagent = subparsers.add_parser("subagent-context")
    subagent.set_defaults(func=run_subagent_context)
    build_efficiency = subparsers.add_parser("build-efficiency")
    build_efficiency.add_argument("--self-test", action="store_true")
    build_efficiency.set_defaults(func=run_build_efficiency)
    stop_tests = subparsers.add_parser("stop-logic-tests")
    stop_tests.set_defaults(func=run_stop_logic_tests)
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main())
