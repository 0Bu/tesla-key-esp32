#!/usr/bin/env python3
"""Validate an operator-declared, vehicle-free hardware-bench report.

The script is intentionally a report gate, not a bench controller: collecting the
report may reboot, flash or OTA a board and therefore remains a separately approved
operation. CI validates only the closed schema, internal plausibility and equality to
explicit expected inputs. It does not independently prove source/artifact provenance,
signature verification, NVS preservation or physical execution. The closed schema keeps
device addresses, NVS contents, vehicle identity, signing keys and raw logs out of the
uploaded report.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import re
import sys
from pathlib import Path
from typing import Any


TARGETS = {"esp32", "esp32s3", "esp32c3", "esp32c6"}
# Runtime high-water marks must retain a deliberate reserve rather than merely be
# non-zero.  These floors are one eighth of the task stacks configured in the
# firmware (httpd/vehicle/auto_pair: 8192 bytes; mqtt: 6144 bytes).  They are an
# acceptance-policy margin, not a claim that hardware has proved a universal alarm
# threshold.  A stack-size change therefore needs this policy reviewed with it.
STACK_MINIMUM_FREE_BYTES = {
    "httpd": 1024,
    "vehicle": 1024,
    "auto_pair": 1024,
    "mqtt": 768,
}
STACK_TASKS = set(STACK_MINIMUM_FREE_BYTES)
NORMAL_REQUIRED_STACK_TASKS = {"httpd", "vehicle", "mqtt"}
MAX_REPORT_BYTES = 16 * 1024
MAX_INTERNAL_BYTES = 1024 * 1024
HEX40 = re.compile(r"^[0-9a-f]{40}$")
HEX64 = re.compile(r"^[0-9a-f]{64}$")
DISPLAY_VERSION = re.compile(
    r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z.-]+)?$"
)

TOP_LEVEL_FIELDS = {
    "schemaVersion",
    "evidenceBasis",
    "profile",
    "target",
    "sourceSha",
    "displayVersion",
    "signedArtifactSha256",
    "operatorDeclaredSignedArtifactVerified",
    "operatorDeclaredVehicleContacted",
    "operatorDeclaredNvsPreserved",
    "startedAt",
    "endedAt",
    "durationSeconds",
    "coverageSeconds",
    "sampleCount",
    "unexpectedRebootCount",
    "plannedRebootCount",
    "unexpectedCrashCount",
    "plannedFaultResetCount",
    "initialBootFailCount",
    "finalBootFailCount",
    "initialLargestInternalBlockBytes",
    "minimumLargestInternalBlockBytes",
    "finalLargestInternalBlockBytes",
    "stackMinimumFreeBytes",
    "checks",
}

PROFILE_POLICY = {
    "smoke": {
        "minimumDurationSeconds": 15 * 60,
        "minimumSamples": 60,
        "minimumPlannedReboots": 0,
        "maximumPlannedReboots": 0,
        "minimumPlannedFaultResets": 0,
        "maximumPlannedFaultResets": 0,
        "requiredStackTasks": NORMAL_REQUIRED_STACK_TASKS,
        "requiredChecks": {
            "booted",
            "statusReadable",
            "diagReadable",
            "heapReadable",
            "mqttReconnect",
        },
    },
    "soak": {
        "minimumDurationSeconds": 8 * 60 * 60,
        "minimumSamples": 960,
        "minimumPlannedReboots": 0,
        "maximumPlannedReboots": 0,
        "minimumPlannedFaultResets": 0,
        "maximumPlannedFaultResets": 0,
        "requiredStackTasks": NORMAL_REQUIRED_STACK_TASKS,
        "requiredChecks": {
            "booted",
            "statusReadable",
            "diagReadable",
            "heapReadable",
            "mqttReconnect",
            "networkChurnRecovered",
        },
    },
    "ota": {
        "minimumDurationSeconds": 5 * 60,
        "minimumSamples": 20,
        "minimumPlannedReboots": 2,
        "maximumPlannedReboots": 8,
        "minimumPlannedFaultResets": 0,
        "maximumPlannedFaultResets": 0,
        "requiredStackTasks": NORMAL_REQUIRED_STACK_TASKS,
        "requiredChecks": {
            "signedCandidateAccepted",
            "unsignedCandidateRejected",
            "wrongTargetRejected",
            "healthGateCommitted",
            "failedHealthRolledBack",
            "nvsIdentityPreserved",
        },
    },
    "recovery": {
        "minimumDurationSeconds": 5 * 60,
        "minimumSamples": 20,
        "minimumPlannedReboots": 5,
        "maximumPlannedReboots": 8,
        "minimumPlannedFaultResets": 4,
        "maximumPlannedFaultResets": 7,
        # Recovery deliberately omits vehicle/MQTT while safe mode is latched, but
        # its required separate non-fault reboot is the final normal boot.  The
        # report's current-boot stack snapshot must cover that final workload.
        "requiredStackTasks": NORMAL_REQUIRED_STACK_TASKS,
        "requiredChecks": {
            "crashBootsLatchedSafeMode",
            "safeModeSkippedVehicle",
            "safeModeSkippedMqtt",
            "recoveryHttpAvailable",
            "nonFaultResetClearedCounter",
        },
    },
}


class InvalidReport(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise InvalidReport(message)


def parse_time(value: Any, field: str) -> dt.datetime:
    require(isinstance(value, str), f"{field} must be an ISO-8601 string")
    require(value.endswith("Z"), f"{field} must be UTC and end in Z")
    try:
        parsed = dt.datetime.fromisoformat(value[:-1] + "+00:00")
    except ValueError as exc:
        raise InvalidReport(f"{field} is not valid ISO-8601") from exc
    return parsed


def integer(report: dict[str, Any], field: str, minimum: int = 0) -> int:
    value = report.get(field)
    require(isinstance(value, int) and not isinstance(value, bool), f"{field} must be an integer")
    require(value >= minimum, f"{field} must be >= {minimum}")
    return value


def validate(report: dict[str, Any]) -> None:
    require(isinstance(report, dict), "report root must be an object")
    missing_fields = sorted(TOP_LEVEL_FIELDS - report.keys())
    extra_fields = sorted(report.keys() - TOP_LEVEL_FIELDS)
    require(not missing_fields, f"missing top-level fields: {', '.join(missing_fields)}")
    require(not extra_fields, f"unexpected top-level fields: {', '.join(extra_fields)}")
    require(report.get("schemaVersion") == 2, "schemaVersion must be 2")
    require(report.get("evidenceBasis") == "operator-declared-bench-observations",
            "evidenceBasis must identify operator-declared bench observations")
    profile = report.get("profile")
    require(profile in PROFILE_POLICY, "profile must be smoke, soak, ota or recovery")
    target = report.get("target")
    require(target in TARGETS, "target is not one of the four supported chips")
    require(isinstance(report.get("sourceSha"), str) and HEX40.fullmatch(report["sourceSha"]),
            "sourceSha must be a lowercase full commit SHA")
    require(isinstance(report.get("signedArtifactSha256"), str)
            and HEX64.fullmatch(report["signedArtifactSha256"]),
            "signedArtifactSha256 must be a lowercase SHA-256")
    require(isinstance(report.get("displayVersion"), str)
            and len(report["displayVersion"].encode("utf-8")) <= 31
            and DISPLAY_VERSION.fullmatch(report["displayVersion"]),
            "displayVersion must be a firmware SemVer that fits the app descriptor")

    # This repository-wide gate is deliberately vehicle-free. A vehicle E2E report
    # belongs to the separately authorized live acceptance boundary.
    require(report.get("operatorDeclaredVehicleContacted") is False,
            "operatorDeclaredVehicleContacted must be false")
    require(report.get("operatorDeclaredNvsPreserved") is True,
            "operatorDeclaredNvsPreserved must be true")
    require(report.get("operatorDeclaredSignedArtifactVerified") is True,
            "operatorDeclaredSignedArtifactVerified must be true")

    started = parse_time(report.get("startedAt"), "startedAt")
    ended = parse_time(report.get("endedAt"), "endedAt")
    require(ended > started, "endedAt must be after startedAt")
    observed_duration = int((ended - started).total_seconds())
    declared_duration = integer(report, "durationSeconds", 1)
    require(abs(observed_duration - declared_duration) <= 2,
            "durationSeconds does not match timestamps")

    policy = PROFILE_POLICY[profile]
    require(declared_duration >= policy["minimumDurationSeconds"],
            f"{profile} duration is below policy")
    samples = integer(report, "sampleCount", policy["minimumSamples"])
    coverage = integer(report, "coverageSeconds", 1)
    require(coverage <= declared_duration + 2, "coverageSeconds exceeds duration")
    require(coverage * 100 >= declared_duration * 90, "telemetry coverage is below 90%")
    require(samples * 30 >= coverage, "sample cadence is sparser than one sample per 30 seconds")

    require(integer(report, "unexpectedRebootCount") == 0, "unexpected reboot observed")
    planned_reboots = integer(report, "plannedRebootCount")
    require(policy["minimumPlannedReboots"] <= planned_reboots
            <= policy["maximumPlannedReboots"],
            f"plannedRebootCount is outside the {profile} policy")
    require(integer(report, "unexpectedCrashCount") == 0, "unexpected crash observed")
    planned_fault_resets = integer(report, "plannedFaultResetCount")
    require(policy["minimumPlannedFaultResets"] <= planned_fault_resets
            <= policy["maximumPlannedFaultResets"],
            f"plannedFaultResetCount is outside the {profile} policy")
    require(planned_fault_resets <= planned_reboots,
            "plannedFaultResetCount cannot exceed plannedRebootCount")
    initial_boot_fails = integer(report, "initialBootFailCount")
    final_boot_fails = integer(report, "finalBootFailCount")
    if profile == "recovery":
        require(initial_boot_fails == 0,
                "recovery profile must start from a clean zero boot-failure counter")
        require(planned_fault_resets >= 4,
                "recovery profile needs four fault resets to latch safe mode")
        require(planned_reboots >= planned_fault_resets + 1,
                "recovery profile needs a separate non-fault reboot after fault resets")
        require(final_boot_fails == 0,
                "recovery profile must finish with the boot-failure counter cleared")
    else:
        require(initial_boot_fails == 0 and final_boot_fails == 0,
                f"{profile} profile must start and finish with a clear boot-failure counter")
    minimum = integer(report, "minimumLargestInternalBlockBytes", 4096)
    initial = integer(report, "initialLargestInternalBlockBytes", 4096)
    final = integer(report, "finalLargestInternalBlockBytes", 4096)
    require(max(minimum, initial, final) <= MAX_INTERNAL_BYTES,
            "largest INTERNAL block exceeds the physical plausibility envelope")
    require(minimum <= initial and minimum <= final,
            "minimumLargestInternalBlockBytes exceeds an endpoint sample")
    require(final + 2048 >= initial, "largest INTERNAL block regressed by more than 2 KiB")

    checks = report.get("checks")
    require(isinstance(checks, dict), "checks must be an object")
    missing = sorted(policy["requiredChecks"] - checks.keys())
    require(not missing, f"missing required checks: {', '.join(missing)}")
    extra = sorted(checks.keys() - policy["requiredChecks"])
    require(not extra, f"unexpected checks for {profile}: {', '.join(extra)}")
    failed = sorted(name for name in policy["requiredChecks"] if checks.get(name) is not True)
    require(not failed, f"failed required checks: {', '.join(failed)}")

    stack = report.get("stackMinimumFreeBytes")
    require(isinstance(stack, dict) and stack, "stackMinimumFreeBytes must be a non-empty object")
    missing_stack_tasks = sorted(policy["requiredStackTasks"] - stack.keys())
    require(not missing_stack_tasks,
            f"missing required stack tasks: {', '.join(missing_stack_tasks)}")
    for task, free_bytes in stack.items():
        require(task in STACK_TASKS,
                "stackMinimumFreeBytes contains an unknown task")
        require(isinstance(free_bytes, int) and not isinstance(free_bytes, bool)
                and 0 < free_bytes <= MAX_INTERNAL_BYTES,
                f"stack minimum for {task} is outside the plausibility envelope")
        require(free_bytes >= STACK_MINIMUM_FREE_BYTES[task],
                f"stack minimum for {task} must be >= {STACK_MINIMUM_FREE_BYTES[task]} bytes")

    # The exact top-level/check schemas above are intentionally closed: raw logs, endpoints and
    # hardware identifiers cannot be smuggled into an otherwise valid artifact.


def valid_fixture(profile: str = "smoke") -> dict[str, Any]:
    policy = PROFILE_POLICY[profile]
    duration = policy["minimumDurationSeconds"]
    started = dt.datetime(2026, 1, 1, tzinfo=dt.timezone.utc)
    ended = started + dt.timedelta(seconds=duration)
    return {
        "schemaVersion": 2,
        "evidenceBasis": "operator-declared-bench-observations",
        "profile": profile,
        "target": "esp32s3",
        "sourceSha": "1" * 40,
        "displayVersion": "1.4.0-test",
        "signedArtifactSha256": "2" * 64,
        "operatorDeclaredSignedArtifactVerified": True,
        "operatorDeclaredVehicleContacted": False,
        "operatorDeclaredNvsPreserved": True,
        "startedAt": started.isoformat().replace("+00:00", "Z"),
        "endedAt": ended.isoformat().replace("+00:00", "Z"),
        "durationSeconds": duration,
        "coverageSeconds": duration,
        "sampleCount": max(policy["minimumSamples"], (duration + 29) // 30),
        "unexpectedRebootCount": 0,
        "plannedRebootCount": policy["minimumPlannedReboots"],
        "unexpectedCrashCount": 0,
        "plannedFaultResetCount": policy["minimumPlannedFaultResets"],
        "initialBootFailCount": 0,
        "finalBootFailCount": 0,
        "initialLargestInternalBlockBytes": 32768,
        "minimumLargestInternalBlockBytes": 16384,
        "finalLargestInternalBlockBytes": 31744,
        "stackMinimumFreeBytes": {"httpd": 2048, "vehicle": 2048, "mqtt": 1536},
        "checks": {name: True for name in policy["requiredChecks"]},
    }


def self_test() -> None:
    for profile in PROFILE_POLICY:
        validate(valid_fixture(profile))

        # The exact policy floors are accepted for the three mandatory final-normal
        # tasks; auto_pair is optional but must meet its own floor when reported.
        boundary = valid_fixture(profile)
        boundary["stackMinimumFreeBytes"] = {
            task: STACK_MINIMUM_FREE_BYTES[task]
            for task in PROFILE_POLICY[profile]["requiredStackTasks"]
        }
        boundary["stackMinimumFreeBytes"]["auto_pair"] = \
            STACK_MINIMUM_FREE_BYTES["auto_pair"]
        validate(boundary)

    mutations = {
        "vehicle contact": ("operatorDeclaredVehicleContacted", True),
        "NVS loss": ("operatorDeclaredNvsPreserved", False),
        "unverified signature": ("operatorDeclaredSignedArtifactVerified", False),
        "misrepresented evidence basis": ("evidenceBasis", "cryptographically-proven"),
        "unexpected reboot": ("unexpectedRebootCount", 1),
        "unplanned smoke reboot": ("plannedRebootCount", 1),
        "low heap": ("minimumLargestInternalBlockBytes", 4095),
        "impossible heap": ("initialLargestInternalBlockBytes", MAX_INTERNAL_BYTES + 1),
        "minimum above endpoint": ("minimumLargestInternalBlockBytes", 32769),
        "wrong target": ("target", "esp32c5"),
        "non-canonical version": ("displayVersion", "01.2.3"),
        "weak SHA": ("sourceSha", "abc"),
        "sparse coverage": ("coverageSeconds", 1),
        "unexpected crash": ("unexpectedCrashCount", 1),
    }
    for name, (field, value) in mutations.items():
        fixture = valid_fixture()
        fixture[field] = value
        try:
            validate(fixture)
        except InvalidReport:
            continue
        raise AssertionError(f"self-test accepted mutation: {name}")

    for profile, policy in PROFILE_POLICY.items():
        for task in policy["requiredStackTasks"]:
            missing_stack = valid_fixture(profile)
            missing_stack["stackMinimumFreeBytes"].pop(task)
            try:
                validate(missing_stack)
            except InvalidReport:
                pass
            else:
                raise AssertionError(
                    f"self-test accepted missing {profile} stack task: {task}"
                )

            one_byte_stack = valid_fixture(profile)
            one_byte_stack["stackMinimumFreeBytes"][task] = 1
            try:
                validate(one_byte_stack)
            except InvalidReport:
                pass
            else:
                raise AssertionError(
                    f"self-test accepted one-byte {profile} stack headroom: {task}"
                )

    optional_one_byte = valid_fixture()
    optional_one_byte["stackMinimumFreeBytes"]["auto_pair"] = 1
    try:
        validate(optional_one_byte)
    except InvalidReport:
        pass
    else:
        raise AssertionError("self-test accepted one-byte optional auto_pair stack headroom")

    impossible_stack = valid_fixture()
    impossible_stack["stackMinimumFreeBytes"]["httpd"] = MAX_INTERNAL_BYTES + 1
    try:
        validate(impossible_stack)
    except InvalidReport:
        pass
    else:
        raise AssertionError("self-test accepted impossible stack headroom")

    missing = valid_fixture("ota")
    missing["checks"].pop("unsignedCandidateRejected")
    try:
        validate(missing)
    except InvalidReport:
        pass
    else:
        raise AssertionError("self-test accepted missing OTA rejection check")

    extra_field = valid_fixture()
    extra_field["logs"] = ["private diagnostic text"]
    try:
        validate(extra_field)
    except InvalidReport:
        pass
    else:
        raise AssertionError("self-test accepted an unexpected top-level field")

    extra_check = valid_fixture()
    extra_check["checks"]["rawEndpointReachable"] = True
    try:
        validate(extra_check)
    except InvalidReport:
        pass
    else:
        raise AssertionError("self-test accepted an unexpected check field")

    unknown_task = valid_fixture()
    unknown_task["stackMinimumFreeBytes"]["private-hostname"] = 1024
    try:
        validate(unknown_task)
    except InvalidReport:
        pass
    else:
        raise AssertionError("self-test accepted an unknown task-name field")

    recovery_mutations = {
        "three fault resets": {"plannedFaultResetCount": 3},
        "no separate clear reboot": {
            "plannedFaultResetCount": 5,
            "plannedRebootCount": 5,
        },
        "dirty initial counter": {"initialBootFailCount": 1},
        "uncleared final counter": {"finalBootFailCount": 1},
    }
    for name, edits in recovery_mutations.items():
        fixture = valid_fixture("recovery")
        fixture.update(edits)
        try:
            validate(fixture)
        except InvalidReport:
            continue
        raise AssertionError(f"self-test accepted recovery mutation: {name}")
    print("bench acceptance report self-test: PASS")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("report", nargs="?", type=Path)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--expect-source-sha")
    parser.add_argument("--expect-artifact-sha256")
    parser.add_argument("--expect-profile", choices=sorted(PROFILE_POLICY))
    parser.add_argument("--expect-target", choices=sorted(TARGETS))
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if args.report is None:
        parser.error("report is required unless --self-test is used")
    try:
        require(args.report.stat().st_size <= MAX_REPORT_BYTES,
                "report exceeds the closed-schema 16 KiB input limit")
        raw = args.report.read_text(encoding="utf-8")
        report = json.loads(raw)
        validate(report)
        require(args.expect_source_sha is None or report["sourceSha"] == args.expect_source_sha,
                "sourceSha does not match the expected build")
        require(args.expect_artifact_sha256 is None
                or report["signedArtifactSha256"] == args.expect_artifact_sha256,
                "signedArtifactSha256 does not match the expected artifact")
        require(args.expect_profile is None or report["profile"] == args.expect_profile,
                "profile does not match the requested gate")
        require(args.expect_target is None or report["target"] == args.expect_target,
                "target does not match the requested gate")
    except (OSError, json.JSONDecodeError, InvalidReport) as exc:
        print(f"bench acceptance report rejected: {exc}", file=sys.stderr)
        return 1
    print(f"bench acceptance report schema/plausibility accepted: {report['profile']} "
          f"{report['target']} {report['sampleCount']} samples/{report['coverageSeconds']}s; "
          "bench observations and identity inputs are operator-declared")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
