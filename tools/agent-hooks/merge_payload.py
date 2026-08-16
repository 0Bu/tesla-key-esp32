#!/usr/bin/env python3
"""Parse merge-tool hook payloads into a repository-bound, unambiguous target."""

from __future__ import annotations

import json
import fnmatch
import os
import re
import shlex
import sys
from typing import Any


ASSIGNMENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*=(.*)$", re.DOTALL)
SHELLS = {"bash", "dash", "sh", "zsh"}
SHELL_EXTGLOB = re.compile(r"(?<!\\)[@+?!*]\([^)]*\)")
CANONICAL_REPOSITORY = "github.com/0Bu/tesla-key-esp32"
MCP_BLOCKED_ACTIONS = {
    "merge_pull_request",
    "enable_auto_merge",
    "enable_pull_request_auto_merge",
    "enqueue_pull_request",
}

GH_BUILTIN_COMMANDS = {
    "alias",
    "api",
    "auth",
    "browse",
    "codespace",
    "completion",
    "config",
    "extension",
    "gist",
    "help",
    "issue",
    "org",
    "pr",
    "project",
    "release",
    "repo",
    "run",
    "search",
    "secret",
    "ssh-key",
    "status",
    "variable",
    "workflow",
}


def executable(token: str) -> str:
    return token.rsplit("/", 1)[-1]


def normalize_shell_source(source: str) -> str:
    def decode(match: re.Match[str]) -> str:
        try:
            return bytes(match.group(1), "utf-8").decode("unicode_escape")
        except UnicodeDecodeError:
            return match.group(0)

    normalized = re.sub(r"\\\r?\n", "", source)
    normalized = re.sub(r"\$'((?:[^'\\]|\\.)*)'", decode, normalized)
    normalized = re.sub(r'\$"((?:[^"\\]|\\.)*)"', r'"\1"', normalized)
    previous = None
    while previous != normalized:
        previous = normalized
        normalized = re.sub(r"\{([^{}]+)\.\.\1\}", r"\1", normalized)
    return normalized


def tokenize(source: str) -> list[str]:
    lexer = shlex.shlex(source.replace("\n", " ; "), posix=True, punctuation_chars=";&|(){}!<>")
    lexer.whitespace_split = True
    lexer.commenters = ""
    return list(lexer)


def split_segments(tokens: list[str]) -> list[list[str]]:
    segments: list[list[str]] = [[]]
    for token in tokens:
        if re.fullmatch(r"[;&|(){}!<>]+", token):
            if segments[-1]:
                segments.append([])
            continue
        segments[-1].append(token)
    return [segment for segment in segments if segment]


def assignments_before(tokens: list[str], end: int) -> dict[str, str]:
    values: dict[str, str] = {}
    for token in tokens[:end]:
        match = ASSIGNMENT.fullmatch(token)
        if match:
            values[token.split("=", 1)[0]] = match.group(1)
    return values


def split_repo_spec(spec: str, explicit_host: str = "") -> tuple[str, str, str]:
    value = spec.strip().removesuffix(".git")
    host = explicit_host.strip()
    if not value:
        return "", host, ""
    if re.search(r"[\s$`{};&|<>]", value) or "://" in value:
        return "", host, f"repository target is not a static [HOST/]OWNER/REPO value: {spec}"
    parts = [part for part in value.split("/") if part]
    if len(parts) == 3:
        repo_host, owner, name = parts
        if host and host.lower() != repo_host.lower():
            return "", host, "conflicting repository host selectors"
        host = repo_host
    elif len(parts) == 2:
        owner, name = parts
    else:
        return "", host, f"repository target is not OWNER/REPO: {spec}"
    return f"{owner}/{name}", host, ""


def normalize_gh_args(args: list[str], assignments: dict[str, str]) -> tuple[list[str], str, str, str]:
    repo_spec = assignments.get("GH_REPO", "")
    host = assignments.get("GH_HOST", "")
    normalized: list[str] = []
    index = 0
    while index < len(args):
        token = args[index]
        if token in {"-R", "--repo", "--hostname"}:
            if index + 1 >= len(args):
                return [], "", host, f"{token} has no value"
            value = args[index + 1]
            if token == "--hostname":
                host = value
            else:
                repo_spec = value
            index += 2
            continue
        if token.startswith("--repo="):
            repo_spec = token.split("=", 1)[1]
            index += 1
            continue
        if token.startswith("-R") and token != "-R":
            repo_spec = token[2:]
            index += 1
            continue
        if token.startswith("--hostname="):
            host = token.split("=", 1)[1]
            index += 1
            continue
        normalized.append(token)
        index += 1
    repo, host, error = split_repo_spec(repo_spec, host)
    return normalized, repo, host, error


def parse_pr_selector(args: list[str]) -> tuple[str, str, str, str, str]:
    positional: list[str] = []
    expected_head = ""
    squash_count = 0
    index = 0
    while index < len(args):
        token = args[index]
        if token == "--":
            return "", "", "", "gh pr merge does not permit an option terminator", expected_head
        if token == "--match-head-commit" or token.startswith("--match-head-commit="):
            if expected_head:
                return "", "", "", "gh pr merge needs exactly one --match-head-commit", expected_head
            if token == "--match-head-commit":
                if index + 1 >= len(args):
                    return "", "", "", f"{token} has no value", ""
                value = args[index + 1]
                index += 2
            else:
                value = token.split("=", 1)[1]
                index += 1
            if not re.fullmatch(r"[0-9a-fA-F]{40}", value):
                return "", "", "", "--match-head-commit must be a full static 40-hex SHA", ""
            expected_head = value.lower()
            continue
        if token == "--squash":
            squash_count += 1
            if squash_count > 1:
                return "", "", "", "gh pr merge needs exactly one --squash", expected_head
            index += 1
            continue
        if token.startswith("-"):
            return "", "", "", f"unsupported gh pr merge option: {token}", expected_head
        positional.append(token)
        index += 1
    if len(positional) > 1:
        return "", "", "", "gh pr merge has more than one positional target", expected_head
    if not positional:
        return "", "", "", "gh pr merge needs one explicit numeric pull request", expected_head
    selector = positional[0]
    if not re.fullmatch(r"\d+", selector):
        return "", "", "", f"merge selector must be a static numeric pull request: {selector}", expected_head
    if not expected_head:
        return selector, "", "", "gh pr merge needs exactly one full --match-head-commit", ""
    if squash_count != 1:
        return selector, "", "", "gh pr merge needs exactly one --squash", expected_head
    return selector, "", "", "", expected_head


def canonical_repo_option(args: list[str]) -> str:
    """Require exactly the documented, host-qualified --repo argument on the gh argv."""
    values: list[str] = []
    index = 0
    while index < len(args):
        token = args[index]
        if token == "--repo":
            if index + 1 >= len(args):
                return "--repo has no value"
            values.append(args[index + 1])
            index += 2
            continue
        if token.startswith("--repo=") or token == "-R" or (token.startswith("-R") and token != "-R"):
            return "merge must use the canonical separate --repo argument"
        if token == "--hostname" or token.startswith("--hostname="):
            return "merge must bind github.com through the canonical --repo argument"
        index += 1
    if values != [CANONICAL_REPOSITORY]:
        return f"merge must use exactly --repo {CANONICAL_REPOSITORY}"
    return ""


def token_may_be_gh(token: str) -> bool:
    base = executable(token)
    return base == "gh" or (
        any(character in base for character in "*?[") and fnmatch.fnmatchcase("gh", base)
    )


def execution_context_is_ambiguous(source: str) -> bool:
    """Reject merge wrappers that can change argv or cwd after static target binding."""
    normalized = normalize_shell_source(source)
    try:
        tokens = tokenize(normalized)
    except ValueError:
        return True
    if re.search(
        r"(?:^|[^A-Za-z0-9_./-])(?:command\s+)?(?:cd|pushd|popd)(?:[^A-Za-z0-9_./-]|$)",
        normalized,
    ):
        return True
    if re.search(r"\bgit\b[\s\S]*\b(?:push|remote|config)\b", normalized):
        return True
    if re.search(
        r"\b(?:printf|read|declare|typeset|export|unset)\b[\s\S]*\bGH_(?:REPO|HOST)\b",
        normalized,
    ):
        return True
    for index, token in enumerate(tokens):
        base = executable(token)
        if token in {"GH_REPO", "GH_HOST"}:
            return True
        if base in {"source", "."}:
            return True
        if base == "git" and any(
            candidate in {"push", "remote", "config"} for candidate in tokens[index + 1 :]
        ):
            return True
        if ".git/config" in token.replace("\\", "/"):
            return True
        if base in {"cd", "pushd", "popd"}:
            return True
        if base == "env":
            following = tokens[index + 1 :]
            if any(
                option in {"-C", "--chdir"}
                or option.startswith("--chdir=")
                or (option.startswith("-C") and option != "-C")
                for option in following
            ):
                return True
        if base == "sudo":
            following = tokens[index + 1 :]
            if any(
                option in {"-D", "--chdir"}
                or option.startswith("--chdir=")
                or (option.startswith("-D") and option != "-D")
                for option in following
            ):
                return True
    if re.search(r"\bGH_(?:REPO|HOST)=", normalized) and re.search(r"[;&|]|\n", normalized):
        return True
    mergeish = re.search(r"\bpr\s+merge\b|mergePullRequest|/pulls?/.*/merge", normalized)
    if mergeish and (
        re.search(r"(?:^|[\s;&|(){}!])xargs(?:[\s;&|(){}!]|$)", normalized)
        or re.search(r"(?:^|[\s;&|(){}!])find\b[^;\n]*\s-exec(?:dir)?\b", normalized)
        or re.search(r"(?:^|[\s;&|(){}!])parallel(?:[\s;&|(){}!]|$)", normalized)
    ):
        return True
    return False


def merge_action_count(source: str) -> int:
    normalized = normalize_shell_source(source)
    return (
        len(re.findall(r"\bpr\s+merge\b", normalized))
        + len(re.findall(r"mergePullRequest", normalized))
        + len(re.findall(r"/?repos/[^\s'\"]+/pulls/[^\s'\"]+/merge", normalized))
    )


def parse_gh(tokens: list[str], index: int, inherited: dict[str, str]) -> dict[str, str] | None:
    assignments = dict(inherited)
    assignments.update(assignments_before(tokens, index))
    raw_args = tokens[index + 1 :]
    normalized, repo, host, error = normalize_gh_args(raw_args, assignments)
    if error:
        return {"action": "gh merge", "selector": "", "repo": repo, "host": host, "error": error}
    if normalized[:2] == ["pr", "merge"]:
        selector, _, _, error, expected_head = parse_pr_selector(normalized[2:])
        error = error or canonical_repo_option(raw_args)
        return {
            "action": "gh pr merge",
            "selector": selector,
            "repo": repo,
            "host": host,
            "error": error,
            "expected_head": expected_head,
        }
    if normalized and normalized[0] == "api":
        joined = " ".join(normalized[1:])
        if "graphql" in normalized[1:] and (
            "--input" in normalized[1:]
            or any(token.startswith("--input=") for token in normalized[1:])
            or any("@" in token for token in normalized[1:])
        ):
            return {
                "action": "gh api graphql input",
                "selector": "",
                "repo": repo,
                "host": host,
                "error": "file-backed GraphQL input cannot be inspected or bound to one reviewed pull request",
            }
        if "graphql" in normalized[1:] and "mergePullRequest" in joined:
            return {
                "action": "gh api graphql merge",
                "selector": "",
                "repo": repo,
                "host": host,
                "error": "GraphQL mergePullRequest target cannot be bound from a node id",
            }
        for token in normalized[1:]:
            match = re.fullmatch(r"/?repos/([^/]+)/([^/]+)/pulls/(\d+)/merge(?:\?[^\s]*)?", token)
            if match:
                api_repo = f"{match.group(1)}/{match.group(2)}"
                if repo and repo.lower() != api_repo.lower():
                    error = "conflicting --repo and gh api merge targets"
                return {
                    "action": "gh api merge",
                    "selector": match.group(3),
                    "repo": api_repo,
                    "host": host,
                    "error": error or "direct gh api merge cannot bind the reviewed head SHA",
                }
            if "repos/" in token and "/pulls/" in token and re.search(r"/merge(?:\?|$)", token):
                return {
                    "action": "gh api merge",
                    "selector": "",
                    "repo": repo,
                    "host": host,
                    "error": "gh api merge endpoint contains a dynamic or invalid target",
                }
    if normalized and not normalized[0].startswith("-") and normalized[0] not in GH_BUILTIN_COMMANDS:
        return {
            "action": "gh alias or extension",
            "selector": "",
            "repo": repo,
            "host": host,
            "error": f"unknown gh subcommand may be a configured merge alias: {normalized[0]}",
        }
    return None


def shell_executes_stdin(source: str) -> bool:
    shell = r"(?:bash|dash|sh|zsh)"
    if re.search(rf"\|[^|;\n]*\b{shell}\b", source):
        return True
    if re.search(rf"\b{shell}\b[^;&|\n]*(?:<<<|(?<!<)<(?!<))", source):
        return True
    return re.search(rf"\b{shell}\b(?:\s+-[^;&|\n]*)*\s+-s(?:\s|$)", source) is not None


def find_merge(source: str, depth: int = 0) -> dict[str, str] | None:
    if depth > 5:
        return {"action": "shell merge", "selector": "", "repo": "", "host": "", "error": "shell nesting exceeds parser limit"}
    source = normalize_shell_source(source)
    graphql_action = next(
        (
            action
            for action in ("mergePullRequest", "enqueuePullRequest", "enablePullRequestAutoMerge")
            if action in source
        ),
        "",
    )
    if graphql_action:
        return {
            "action": "GraphQL merge activation",
            "selector": "",
            "repo": "",
            "host": "",
            "error": f"GraphQL {graphql_action} uses an unbindable node-id target",
        }
    if re.search(r"\bcurl\b", source) and re.search(r"https?://(?:api\.)?github\.com/graphql", source) and re.search(
        r"(?:^|\s)(?:-d|--data(?:-ascii|-binary|-raw|-urlencode)?|-X|--request)(?:\s|=)", source
    ):
        return {
            "action": "curl GraphQL mutation-capable request",
            "selector": "",
            "repo": "",
            "host": "",
            "error": "POST/data GraphQL input cannot be bound to one reviewed pull request",
        }
    if SHELL_EXTGLOB.search(source) and re.search(
        r"\bpr\s+merge\b|mergePullRequest|/pulls?/.*/merge", source
    ):
        return {
            "action": "shell merge",
            "selector": "",
            "repo": "",
            "host": "",
            "error": "shell extglob prevents static merge executable/target binding",
        }
    if shell_executes_stdin(source):
        return {"action": "shell stdin", "selector": "", "repo": "", "host": "", "error": "stdin-executed shell cannot be bound to one merge target"}
    substitutions = re.findall(r"(?<!\\)`([^`]*)`", source, flags=re.DOTALL)
    substitutions += re.findall(r"(?<!\\)\$\(([^()]*)\)", source, flags=re.DOTALL)
    for substitution in substitutions:
        nested = find_merge(substitution, depth + 1)
        if nested is not None:
            return nested
    try:
        segments = split_segments(tokenize(source))
    except ValueError:
        return {"action": "shell merge", "selector": "", "repo": "", "host": "", "error": "shell command is not parseable"}
    for segment in segments:
        for index, token in enumerate(segment):
            base = executable(token)
            if base in SHELLS:
                for flag_index in range(index + 1, len(segment)):
                    flag = segment[flag_index]
                    is_command = flag == "--command" or (
                        flag.startswith("-") and not flag.startswith("--") and "c" in flag[1:]
                    )
                    if is_command and flag_index + 1 < len(segment):
                        nested = find_merge(segment[flag_index + 1], depth + 1)
                        if nested is not None:
                            return nested
                        break
            if base == "env":
                for env_index in range(index + 1, len(segment)):
                    option = segment[env_index]
                    if option in {"-S", "--split-string"} and env_index + 1 < len(segment):
                        nested = find_merge(segment[env_index + 1], depth + 1)
                        if nested is not None:
                            return nested
                    elif option.startswith("-S") and option != "-S":
                        nested = find_merge(option[2:], depth + 1)
                        if nested is not None:
                            return nested
                    elif option.startswith("--split-string="):
                        nested = find_merge(option.split("=", 1)[1], depth + 1)
                        if nested is not None:
                            return nested
            if base == "eval" and index + 1 < len(segment):
                nested = find_merge(" ".join(segment[index + 1 :]), depth + 1)
                if nested is not None:
                    return nested
            if token_may_be_gh(token):
                parsed = parse_gh(segment, index, {})
                if parsed is not None:
                    return parsed
    if (
        re.search(r"\bpr\s+merge\b|mergePullRequest|/?repos/[^\s'\"]+/pulls/[^\s'\"]+/merge", source)
        or (
            re.search(r"(?:^|[^A-Za-z0-9_])gh(?:[^A-Za-z0-9_]|$)", source)
            and re.search(r"\bpr\b", source)
            and re.search(r"\bmerge\b", source)
        )
    ):
        return {
            "action": "shell merge",
            "selector": "",
            "repo": "",
            "host": "",
            "error": "literal merge operation is present but its executable or target is dynamic",
        }
    return None


def blocked_mcp_activation(tool: str) -> dict[str, str]:
    action = tool.rsplit("__", 1)[-1]
    return {
        "action": f"MCP {action}",
        "selector": "",
        "repo": "",
        "host": "",
        "error": "all MCP merge, auto-merge, and queue forms are blocked; use the canonical expected-head gh CLI path",
        "expected_head": "",
    }


def main() -> int:
    try:
        data = json.load(sys.stdin)
    except Exception:
        return 2
    if not isinstance(data, dict):
        return 2
    tool = str(data.get("tool_name") or "")
    tool_input = data.get("tool_input") or {}
    if not isinstance(tool_input, dict):
        return 2
    parsed: dict[str, str] | None = None
    tool_lower = tool.lower()
    serialized = json.dumps(data, sort_keys=True, separators=(",", ":"))
    mcp_mergeish = tool_lower.startswith("mcp__") and (
        any(marker in tool_lower for marker in ("merge", "enqueue", "queue"))
        or any(marker in serialized for marker in ("mergePullRequest", "enablePullRequestAutoMerge", "enqueuePullRequest"))
    )
    if mcp_mergeish or (
        tool_lower.startswith("mcp__") and any(tool_lower.endswith(action) for action in MCP_BLOCKED_ACTIONS)
    ):
        parsed = blocked_mcp_activation(tool)
    elif tool.lower() in {"bash", "exec_command", "shell", "shell_command"}:
        command_values = [
            str(tool_input[key])
            for key in ("command", "cmd")
            if tool_input.get(key) not in (None, "")
        ]
        distinct_commands = list(dict.fromkeys(command_values))
        command = distinct_commands[0] if len(distinct_commands) == 1 else ""
        parsed = (
            {
                "action": "ambiguous shell tool input",
                "selector": "",
                "repo": "",
                "host": "",
                "error": "conflicting command/cmd fields in merge hook payload",
            }
            if len(distinct_commands) > 1
            else find_merge(command)
        )
        if parsed is not None:
            if parsed.get("action") == "gh pr merge":
                canonical = (
                    f"gh --repo {CANONICAL_REPOSITORY} pr merge {parsed.get('selector', '')} "
                    f"--match-head-commit {parsed.get('expected_head', '')} --squash"
                )
                if command.strip() != canonical:
                    parsed["error"] = parsed.get("error") or f"merge command must be exactly: {canonical}"
            if merge_action_count(command) > 1:
                parsed["error"] = parsed["error"] or "multiple merge actions cannot be bound atomically"
            elif execution_context_is_ambiguous(command):
                parsed["error"] = parsed["error"] or "merge command changes argv or working directory after target binding"
    if parsed is None:
        parsed = {"action": "", "selector": "", "repo": "", "host": "", "error": ""}
    raw_payload_cwd = data.get("cwd")
    payload_cwd = str(raw_payload_cwd or "")
    if parsed["action"] and tool.lower() in {"bash", "exec_command", "shell", "shell_command"} and (
        not isinstance(raw_payload_cwd, str) or not raw_payload_cwd.strip()
    ):
        parsed["error"] = parsed["error"] or "shell merge hook payload has no execution cwd"
    workdir_values = [
        str(tool_input[key])
        for key in ("workdir", "cwd")
        if tool_input.get(key) not in (None, "")
    ]
    distinct_workdirs = list(dict.fromkeys(workdir_values))
    if len(distinct_workdirs) > 1:
        parsed["action"] = parsed["action"] or "ambiguous shell workdir input"
        parsed["error"] = parsed["error"] or "conflicting workdir/cwd fields in merge hook payload"
    tool_workdir = distinct_workdirs[0] if len(distinct_workdirs) == 1 else ""
    if tool_workdir:
        payload_cwd = os.path.abspath(os.path.join(payload_cwd or os.getcwd(), tool_workdir))
    fields = (
        parsed["action"],
        parsed["selector"],
        payload_cwd,
        parsed["repo"],
        parsed["host"],
        parsed["error"],
        parsed.get("expected_head", ""),
    )
    sys.stdout.write("\0".join(fields) + "\0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
