#!/usr/bin/env python3
"""Gate repository-owned firmware stack frames from GCC ``-fstack-usage`` output."""

from __future__ import annotations

import argparse
import json
import re
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any


TARGETS = ("esp32", "esp32s3", "esp32c3", "esp32c6")
STACK_LINE = re.compile(r"^(.*?):([0-9]+):(?:([0-9]+):)?(.*?)\t([0-9]+)\t(.+)$")
ABSOLUTE_MAX_BYTES = 4096
REVIEW_THRESHOLD_BYTES = 256


class StackUsageError(ValueError):
    pass


@dataclass(frozen=True)
class Frame:
    bytes: int
    source: str
    function: str
    qualifier: str


FrameInventory = dict[str, dict[str, Frame]]


def _index_frames(frames: list[Frame]) -> dict[str, Frame]:
    counts: dict[str, int] = {}
    for frame in frames:
        base = f"{frame.source} :: {frame.function}"
        counts[base] = counts.get(base, 0) + 1
    seen: dict[str, int] = {}
    indexed: dict[str, Frame] = {}
    for frame in frames:
        base = f"{frame.source} :: {frame.function}"
        if counts[base] == 1:
            identity = base
        else:
            seen[base] = seen.get(base, 0) + 1
            identity = f"{base} [occurrence {seen[base]}/{counts[base]}]"
        if identity in indexed:
            raise StackUsageError(f"duplicate stack-frame identity: {identity}")
        indexed[identity] = frame
    return indexed


def collect(root: Path) -> FrameInventory:
    if root.is_symlink() or not root.is_dir():
        raise StackUsageError(f"stack-usage root is missing, non-directory or symlinked: {root}")
    result: FrameInventory = {}
    files = sorted(path for path in root.rglob("*.su") if path.is_file() and not path.is_symlink())
    if not files:
        raise StackUsageError("no .su files found; the firmware was not compiled with -fstack-usage")
    for path in files:
        relative = path.relative_to(root).as_posix()
        parsed: list[Frame] = []
        for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            if not line:
                continue
            match = STACK_LINE.fullmatch(line)
            if match is None:
                raise StackUsageError(f"cannot parse {relative}:{line_number}: {line!r}")
            size = int(match.group(5))
            qualifier = match.group(6).strip()
            if "dynamic" in qualifier and "bounded" not in qualifier:
                raise StackUsageError(
                    f"unbounded dynamic stack frame in {relative}: {match.group(4)} ({qualifier})"
                )
            parsed.append(Frame(size, match.group(1), match.group(4), qualifier))
        if not parsed:
            raise StackUsageError(f"empty stack-usage file: {relative}")
        result[relative] = _index_frames(parsed)
    return result


def observed_document(target: str, frames: FrameInventory) -> dict[str, Any]:
    return {
        "target": target,
        "files": {
            name: {
                identity: {"bytes": frame.bytes, "qualifier": frame.qualifier}
                for identity, frame in sorted(file_frames.items())
            }
            for name, file_frames in sorted(frames.items())
        },
    }


def frames_from_document(path: Path, target: str) -> FrameInventory:
    if path.is_symlink() or not path.is_file():
        raise StackUsageError(f"observed stack-usage snapshot is missing or symlinked: {path}")
    root = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(root, dict) or set(root) != {"target", "files"}:
        raise StackUsageError("observed stack-usage fields must be exactly target and files")
    if root["target"] != target:
        raise StackUsageError(
            f"observed stack-usage target mismatch: expected {target}, got {root['target']}"
        )
    files = root["files"]
    if not isinstance(files, dict) or not files:
        raise StackUsageError("observed stack-usage snapshot contains no files")
    frames: FrameInventory = {}
    for name, records in files.items():
        if (
            not isinstance(name, str)
            or not name
            or not isinstance(records, dict)
            or not records
        ):
            raise StackUsageError(f"invalid observed stack-usage file record: {name!r}")
        file_frames: dict[str, Frame] = {}
        for identity, record in records.items():
            if (
                not isinstance(identity, str)
                or not identity
                or not isinstance(record, dict)
                or set(record) != {"bytes", "qualifier"}
                or isinstance(record["bytes"], bool)
                or not isinstance(record["bytes"], int)
                or record["bytes"] < 0
                or not isinstance(record["qualifier"], str)
            ):
                raise StackUsageError(
                    f"invalid observed stack-frame record: {name}/{identity!r}"
                )
            if "dynamic" in record["qualifier"] and "bounded" not in record["qualifier"]:
                raise StackUsageError(
                    f"unbounded dynamic stack frame in observed snapshot: {name}/{identity}"
                )
            source, separator, function = identity.partition(" :: ")
            if not separator or not source or not function:
                raise StackUsageError(f"invalid observed stack-frame identity: {identity!r}")
            file_frames[identity] = Frame(
                record["bytes"], source, function, record["qualifier"]
            )
        frames[name] = file_frames
    return frames


def baseline_target_from_frames(frames: FrameInventory) -> dict[str, Any]:
    return {
        "files": {
            name: {
                identity: frame.bytes
                for identity, frame in sorted(reviewed_frames(file_frames).items())
            }
            for name, file_frames in sorted(frames.items())
        }
    }


def baseline_document(targets: dict[str, FrameInventory]) -> dict[str, Any]:
    if set(targets) != set(TARGETS):
        raise StackUsageError("baseline generation requires exactly all four supported targets")
    return {
        "schemaVersion": 2,
        "baselineKind": "reviewed-large-frames",
        "toolchain": "ESP-IDF v6.1",
        "absoluteMaxBytes": ABSOLUTE_MAX_BYTES,
        "reviewThresholdBytes": REVIEW_THRESHOLD_BYTES,
        "targets": {
            target: baseline_target_from_frames(targets[target]) for target in TARGETS
        },
    }


UPDATE_MODES = ("raise", "tighten")


def updated_baseline_document(
    old_path: Path, targets: dict[str, FrameInventory], mode: str
) -> tuple[dict[str, Any], list[str]]:
    """Return the reviewed baseline after a measured four-target build, plus one line per change.

    raise:   every reviewed frame keeps max(old, measured), so no maximum of a frame that stays
             reviewed is lowered; frames newly at or above the review threshold are added and
             frames or translation units that disappeared are dropped.
    tighten: no frame may grow above its old maximum, and no newly reviewed frame or translation
             unit may be added. Removed units and removed/below-threshold frames are reductions
             and may be dropped even though ordinary compare() rejects their set drift. Every
             remaining maximum becomes the measured value, so no maximum can grow.
    The old baseline is never rewritten wholesale: each change is listed for its review record.
    """
    if mode not in UPDATE_MODES:
        raise StackUsageError(f"stack baseline update mode must be one of {UPDATE_MODES}, got {mode!r}")
    if set(targets) != set(TARGETS):
        raise StackUsageError("baseline updates require exactly all four supported targets")
    document = baseline_document(targets)
    changes: list[str] = []
    for target in TARGETS:
        old_files = load_baseline(old_path, target)["files"]
        frames = targets[target]
        failures = compare(frames)  # the absolute frame policy holds in both modes
        if mode == "tighten":
            # Dropped frames and translation units are reductions; growth, newly reviewed frames and
            # new translation units are not.
            for name in sorted(set(frames) - set(old_files)):
                failures.append(f"{name} is a new translation unit")
            for name, file_frames in sorted(frames.items()):
                for identity, frame in sorted(reviewed_frames(file_frames).items()):
                    before = old_files.get(name, {}).get(identity)
                    if before is None:
                        failures.append(f"{name}/{identity} is a new reviewed frame ({frame.bytes} B)")
                    elif frame.bytes > before:
                        failures.append(f"{name}/{identity} grew {before} -> {frame.bytes} B")
        if failures:
            raise StackUsageError(
                f"refusing to {mode} the stack baseline for {target}: " + "; ".join(failures)
            )
        new_files = document["targets"][target]["files"]
        for name in sorted(set(old_files) | set(new_files)):
            old_entries = old_files.get(name, {})
            new_entries = new_files.get(name)
            if new_entries is None:
                changes.append(f"{target} {name}: translation unit dropped")
                continue
            if name not in old_files:
                changes.append(f"{target} {name}: new translation unit")
            for identity in sorted(set(old_entries) | set(new_entries)):
                before = old_entries.get(identity)
                if identity not in new_entries:
                    changes.append(f"{target} {name}/{identity}: {before} B -> dropped (below review threshold)")
                    continue
                measured = new_entries[identity]
                after = measured if mode == "tighten" or before is None else max(before, measured)
                new_entries[identity] = after
                if before is None:
                    changes.append(f"{target} {name}/{identity}: new reviewed frame {after} B")
                elif after != before:
                    changes.append(f"{target} {name}/{identity}: {before} -> {after} B ({after - before:+d})")
    return document, changes


def load_baseline(path: Path, target: str) -> dict[str, Any]:
    root = json.loads(path.read_text(encoding="utf-8"))
    expected_top = {
        "schemaVersion",
        "baselineKind",
        "toolchain",
        "absoluteMaxBytes",
        "reviewThresholdBytes",
        "targets",
    }
    if not isinstance(root, dict) or set(root) != expected_top:
        raise StackUsageError(f"stack baseline fields must be exactly {sorted(expected_top)}")
    if root.get("schemaVersion") != 2 or root.get("baselineKind") != "reviewed-large-frames":
        raise StackUsageError("stack baseline must be schemaVersion 2 reviewed-large-frames")
    if root.get("toolchain") != "ESP-IDF v6.1":
        raise StackUsageError("stack baseline must be bound to ESP-IDF v6.1")
    if root.get("absoluteMaxBytes") != ABSOLUTE_MAX_BYTES:
        raise StackUsageError(
            f"stack baseline absolute maximum must be {ABSOLUTE_MAX_BYTES} bytes"
        )
    if root.get("reviewThresholdBytes") != REVIEW_THRESHOLD_BYTES:
        raise StackUsageError(
            f"stack baseline review threshold must be {REVIEW_THRESHOLD_BYTES} bytes"
        )
    targets = root.get("targets")
    if not isinstance(targets, dict) or set(targets) != set(TARGETS):
        raise StackUsageError("stack baseline must contain exactly the four supported targets")
    baseline = targets.get(target)
    if not isinstance(baseline, dict) or set(baseline) != {"files"}:
        raise StackUsageError(f"stack baseline for {target} has invalid fields")
    files = baseline["files"]
    if not isinstance(files, dict) or not files:
        raise StackUsageError(f"stack baseline for {target} contains no files")
    for name, reviewed in files.items():
        if not isinstance(name, str) or not name or not isinstance(reviewed, dict):
            raise StackUsageError(f"stack baseline has invalid file record for {target}/{name}")
        for identity, maximum in reviewed.items():
            if (
                not isinstance(identity, str)
                or not identity
                or isinstance(maximum, bool)
                or not isinstance(maximum, int)
                or maximum < REVIEW_THRESHOLD_BYTES
                or maximum > ABSOLUTE_MAX_BYTES
            ):
                raise StackUsageError(
                    f"stack baseline has invalid reviewed frame for {target}/{name}/{identity}"
                )
    return baseline


def reviewed_frames(frames: dict[str, Frame]) -> dict[str, Frame]:
    return {
        identity: frame
        for identity, frame in frames.items()
        if frame.bytes >= REVIEW_THRESHOLD_BYTES
    }


def compare(frames: FrameInventory, baseline: dict[str, Any] | None = None) -> list[str]:
    failures: list[str] = []
    for name, file_frames in sorted(frames.items()):
        for identity, frame in sorted(file_frames.items()):
            if frame.bytes > ABSOLUTE_MAX_BYTES:
                failures.append(
                    f"{name}/{identity} exceeds the absolute {ABSOLUTE_MAX_BYTES}-byte frame policy: "
                    f"actual={frame.bytes}"
                )
    if baseline is None:
        return failures
    expected = baseline["files"]
    if set(frames) != set(expected):
        missing = sorted(set(expected) - set(frames))
        unexpected = sorted(set(frames) - set(expected))
        failures.append(f"stack-usage translation-unit set drifted: missing={missing} unexpected={unexpected}")
    for name in sorted(set(frames) & set(expected)):
        actual_reviewed = reviewed_frames(frames[name])
        expected_reviewed = expected[name]
        if set(actual_reviewed) != set(expected_reviewed):
            missing = sorted(set(expected_reviewed) - set(actual_reviewed))
            unexpected = sorted(set(actual_reviewed) - set(expected_reviewed))
            failures.append(
                f"{name} reviewed stack-frame set drifted: missing={missing} unexpected={unexpected}"
            )
        for identity in sorted(set(actual_reviewed) & set(expected_reviewed)):
            frame = actual_reviewed[identity]
            maximum = expected_reviewed[identity]
            if frame.bytes > maximum:
                failures.append(
                    f"{name}/{identity} stack frame grew beyond reviewed baseline: "
                    f"max={maximum} actual={frame.bytes}"
                )
    return failures


def self_test() -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        stack = root / "stack"
        stack.mkdir()
        (stack / "one.su").write_text(
            "/project/main/one.cpp:10:3:first()\t320\tstatic\n"
            "/project/main/one.cpp:20:3:second()\t512\tstatic\n",
            encoding="utf-8",
        )
        (stack / "two.su").write_text(
            "/project/main/two.cpp:4:bounded()\t256\tdynamic,bounded\n",
            encoding="utf-8",
        )
        frames = collect(stack)
        first_id = "/project/main/one.cpp :: first()"
        second_id = "/project/main/one.cpp :: second()"
        assert frames["one.su"][first_id].bytes == 320
        assert frames["one.su"][second_id].bytes == 512
        observed_path = root / "observed.json"
        observed_path.write_text(
            json.dumps(observed_document("esp32", frames)), encoding="utf-8"
        )
        assert frames_from_document(observed_path, "esp32") == frames
        wrong_observed = json.loads(observed_path.read_text(encoding="utf-8"))
        wrong_observed["target"] = "esp32s3"
        observed_path.write_text(json.dumps(wrong_observed), encoding="utf-8")
        try:
            frames_from_document(observed_path, "esp32")
        except StackUsageError:
            pass
        else:
            raise AssertionError("wrong observed stack target was accepted")
        observed_path.write_text(
            json.dumps(observed_document("esp32", frames)), encoding="utf-8"
        )
        baseline_target = baseline_target_from_frames(frames)
        assert compare(frames, baseline_target) == []
        # Dropping a complete translation unit must fail even if every remaining frame fits.
        missing_unit = {name: records for name, records in frames.items() if name != "two.su"}
        missing_failures = compare(missing_unit, baseline_target)
        assert any("translation-unit set drifted" in failure and "two.su" in failure
                   for failure in missing_failures), "missing translation unit was accepted"
        extra_unit = {**frames, "extra.su": frames["two.su"]}
        assert any("translation-unit set drifted" in failure and "extra.su" in failure
                   for failure in compare(extra_unit, baseline_target)), "unexpected unit was accepted"

        baseline_path = root / "baseline.json"
        all_targets = {target: frames for target in TARGETS}
        baseline_path.write_text(
            json.dumps(baseline_document(all_targets)),
            encoding="utf-8",
        )
        assert load_baseline(baseline_path, "esp32") == baseline_target
        wrong_toolchain = json.loads(baseline_path.read_text(encoding="utf-8"))
        wrong_toolchain["toolchain"] = "ESP-IDF v6.0"
        baseline_path.write_text(json.dumps(wrong_toolchain), encoding="utf-8")
        try:
            load_baseline(baseline_path, "esp32")
        except StackUsageError:
            pass
        else:
            raise AssertionError("wrong stack baseline toolchain was accepted")

        # A non-maximum large frame must not hide below the translation-unit maximum.
        (stack / "one.su").write_text(
            "/project/main/one.cpp:10:3:first()\t480\tstatic\n"
            "/project/main/one.cpp:20:3:second()\t512\tstatic\n",
            encoding="utf-8",
        )
        grown = collect(stack)
        assert any("first()" in failure and "grew beyond" in failure
                   for failure in compare(grown, baseline_target))

        # A previously small/new frame crossing the review threshold needs explicit baseline review.
        (stack / "one.su").write_text(
            "/project/main/one.cpp:10:3:first()\t320\tstatic\n"
            "/project/main/one.cpp:20:3:second()\t512\tstatic\n"
            "/project/main/one.cpp:30:3:new_large()\t300\tstatic\n",
            encoding="utf-8",
        )
        added = collect(stack)
        assert any("unexpected=" in failure and "new_large" in failure
                   for failure in compare(added, baseline_target))

        # Repeated compiler names are retained as distinct deterministic occurrences.
        (stack / "one.su").write_text(
            "/project/main/one.cpp:10:3:lambda()\t320\tstatic\n"
            "/project/main/one.cpp:20:3:lambda()\t384\tstatic\n",
            encoding="utf-8",
        )
        repeated = collect(stack)["one.su"]
        assert sorted(repeated) == [
            "/project/main/one.cpp :: lambda() [occurrence 1/2]",
            "/project/main/one.cpp :: lambda() [occurrence 2/2]",
        ]

        (stack / "one.su").write_text(
            f"/project/main/one.cpp:10:3:first()\t{ABSOLUTE_MAX_BYTES + 1}\tstatic\n",
            encoding="utf-8",
        )
        oversized = collect(stack)
        assert "absolute" in compare(oversized)[0]

        # Reviewed updates: raise never lowers a still-reviewed frame, tighten never lets one grow,
        # and every change is listed for the review record.
        def su(*lines: str) -> FrameInventory:
            (stack / "one.su").write_text("".join(f"{line}\n" for line in lines), encoding="utf-8")
            return collect(stack)

        update_path = root / "update-baseline.json"
        old_frames = su("/project/main/one.cpp:10:3:first()\t320\tstatic",
                        "/project/main/one.cpp:20:3:second()\t512\tstatic")
        update_path.write_text(json.dumps(baseline_document({t: old_frames for t in TARGETS})),
                               encoding="utf-8")
        shrunk = su("/project/main/one.cpp:10:3:first()\t288\tstatic",
                    "/project/main/one.cpp:20:3:second()\t200\tstatic")
        tightened, changes = updated_baseline_document(update_path, {t: shrunk for t in TARGETS}, "tighten")
        assert tightened["targets"]["esp32"]["files"]["one.su"] == {first_id: 288}
        assert any("first()" in c and "320 -> 288 B (-32)" in c for c in changes)
        assert any("second()" in c and "dropped" in c for c in changes)
        raised, changes = updated_baseline_document(update_path, {t: shrunk for t in TARGETS}, "raise")
        assert raised["targets"]["esp32"]["files"]["one.su"] == {first_id: 320}
        grown_more = su("/project/main/one.cpp:10:3:first()\t352\tstatic",
                        "/project/main/one.cpp:20:3:second()\t512\tstatic",
                        "/project/main/one.cpp:30:3:new_large()\t300\tstatic")
        raised, changes = updated_baseline_document(update_path, {t: grown_more for t in TARGETS}, "raise")
        assert raised["targets"]["esp32"]["files"]["one.su"][first_id] == 352
        assert any("320 -> 352 B (+32)" in c for c in changes)
        assert any("new reviewed frame 300 B" in c for c in changes)
        for label, measured in (("grown frame", su("/project/main/one.cpp:10:3:first()\t352\tstatic",
                                                   "/project/main/one.cpp:20:3:second()\t512\tstatic")),
                                ("new reviewed frame", su("/project/main/one.cpp:10:3:first()\t320\tstatic",
                                                          "/project/main/one.cpp:20:3:second()\t512\tstatic",
                                                          "/project/main/one.cpp:30:3:new_large()\t300\tstatic"))):
            try:
                updated_baseline_document(update_path, {t: measured for t in TARGETS}, "tighten")
            except StackUsageError as exc:
                assert "refusing to tighten" in str(exc)
            else:
                raise AssertionError(f"tighten accepted a {label}, i.e. a raise outside raise mode")
        # A new translation unit is listed in raise mode and refused in tighten mode.
        su("/project/main/one.cpp:10:3:first()\t320\tstatic",
           "/project/main/one.cpp:20:3:second()\t512\tstatic")
        (stack / "three.su").write_text("/project/main/three.cpp:1:1:small()\t64\tstatic\n", encoding="utf-8")
        with_unit = collect(stack)
        _, changes = updated_baseline_document(update_path, {t: with_unit for t in TARGETS}, "raise")
        assert any("three.su: new translation unit" in c for c in changes)
        try:
            updated_baseline_document(update_path, {t: with_unit for t in TARGETS}, "tighten")
        except StackUsageError as exc:
            assert "new translation unit" in str(exc)
        else:
            raise AssertionError("tighten accepted a new translation unit")
        (stack / "three.su").unlink()
        (stack / "two.su").unlink()
        without_unit = collect(stack)
        tightened, changes = updated_baseline_document(
            update_path, {t: without_unit for t in TARGETS}, "tighten"
        )
        assert set(tightened["targets"]["esp32"]["files"]) == {"one.su"}
        assert any("two.su: translation unit dropped" in c for c in changes)
        assert any("translation-unit set drifted" in c
                   for c in compare(without_unit, load_baseline(update_path, "esp32")))
        try:
            updated_baseline_document(update_path, {t: shrunk for t in TARGETS}, "rewrite")
        except StackUsageError:
            pass
        else:
            raise AssertionError("an unreviewed baseline rewrite mode was accepted")

        for mode in UPDATE_MODES:
            try:
                updated_baseline_document(update_path, {t: oversized for t in TARGETS}, mode)
            except StackUsageError as exc:
                assert "absolute" in str(exc)
            else:
                raise AssertionError(f"{mode} accepted a frame above the absolute limit")
            try:
                updated_baseline_document(update_path, {t: shrunk for t in TARGETS[:-1]}, mode)
            except StackUsageError as exc:
                assert "exactly all four" in str(exc)
            else:
                raise AssertionError(f"{mode} accepted a missing target")

        (stack / "one.su").write_text(
            "/project/main/one.cpp:10:3:first()\t16\tdynamic\n", encoding="utf-8"
        )
        try:
            collect(stack)
        except StackUsageError:
            pass
        else:
            raise AssertionError("unbounded dynamic frame was accepted")
    print("firmware stack-usage gate self-test: PASS (per-frame growth/addition canaries)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", choices=TARGETS)
    parser.add_argument("--stack-root", type=Path)
    parser.add_argument("--observed-json", type=Path)
    parser.add_argument("--observed-dir", type=Path)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--write-observed", type=Path)
    parser.add_argument("--write-baseline", type=Path)
    parser.add_argument("--mode", choices=UPDATE_MODES,
                        help="with --write-baseline: raise (reviewed growth) or tighten (lock in a reduction)")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if args.observed_dir is not None or args.write_baseline is not None or args.mode is not None:
        if (
            args.observed_dir is None
            or args.write_baseline is None
            or args.mode is None
            or args.target is not None
            or args.stack_root is not None
            or args.observed_json is not None
            or args.baseline is not None
            or args.write_observed is not None
        ):
            parser.error(
                "--observed-dir, --write-baseline and --mode raise|tighten must be used together "
                "without target mode"
            )
        try:
            snapshots = {
                target: frames_from_document(
                    args.observed_dir / target / f"stack-usage-{target}.json", target
                )
                for target in TARGETS
            }
            if args.write_baseline.is_symlink():
                raise StackUsageError("refusing to overwrite a symlinked stack baseline")
            document, changes = updated_baseline_document(args.write_baseline, snapshots, args.mode)
            args.write_baseline.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
        except (OSError, UnicodeError, json.JSONDecodeError, StackUsageError) as exc:
            print(f"firmware stack-usage baseline update failed: {exc}", file=sys.stderr)
            return 1
        print(f"firmware stack-usage baseline: {args.mode} wrote {args.write_baseline} "
              f"({len(changes)} change(s); record each with its measured cause in the PR)")
        for change in changes:
            print(f"  {change}")
        return 0
    if args.target is None:
        parser.error("--target is required")
    if (args.stack_root is None) == (args.observed_json is None):
        parser.error("exactly one of --stack-root and --observed-json is required")
    try:
        frames = (
            collect(args.stack_root)
            if args.stack_root is not None
            else frames_from_document(args.observed_json, args.target)
        )
        if args.write_observed is not None:
            args.write_observed.write_text(
                json.dumps(observed_document(args.target, frames), indent=2) + "\n",
                encoding="utf-8",
            )
        baseline = load_baseline(args.baseline, args.target) if args.baseline is not None else None
        failures = compare(frames, baseline)
    except (OSError, UnicodeError, json.JSONDecodeError, StackUsageError) as exc:
        print(f"firmware stack-usage gate failed: {exc}", file=sys.stderr)
        return 1
    if failures:
        for failure in failures:
            print(f"firmware stack-usage gate failed: {failure}", file=sys.stderr)
        return 1
    state = "reviewed baseline" if args.baseline is not None else "observed snapshot"
    flattened = [frame for file_frames in frames.values() for frame in file_frames.values()]
    largest = max(frame.bytes for frame in flattened)
    reviewed = sum(len(reviewed_frames(file_frames)) for file_frames in frames.values())
    print(
        f"firmware stack-usage gate: PASS ({args.target}, {len(frames)} files, "
        f"{len(flattened)} frames, {reviewed} reviewed >= {REVIEW_THRESHOLD_BYTES} B, "
        f"max={largest} B, {state})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
