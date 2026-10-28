#!/usr/bin/env python3
"""Bind daemon reuse to exact mounts, including read-only Git metadata in both layouts."""
import argparse
import copy
import hashlib
import json
import os
import sys

POLICY = "tesla-key-idf-mounts-v1"


def expected_mounts(repo, common, volume):
    repo = os.path.normpath(repo)
    if not os.path.isabs(repo) or not volume:
        raise ValueError("absolute checkout path and build volume are required")
    mounts = [
        {"Type": "bind", "Source": repo, "Destination": "/project", "RW": True},
        {"Type": "bind", "Source": repo + "/.ccache", "Destination": "/project/.ccache", "RW": True},
        {"Type": "volume", "Name": volume, "Destination": "/build_cache", "RW": True},
    ]
    if common:
        common = os.path.normpath(common)
        if not os.path.isabs(common):
            raise ValueError("absolute Git common directory is required")
        destination = (os.path.normpath("/project/" + os.path.relpath(common, repo))
                       if os.path.commonpath((repo, common)) == repo else common)
        mounts.append({"Type": "bind", "Source": common, "Destination": destination, "RW": False})
    return sorted(mounts, key=lambda m: m["Destination"])


def mount_label(expected):
    material = json.dumps([POLICY, expected], sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(material).hexdigest()


def validate(actual, expected):
    if not isinstance(actual, list) or len(actual) != len(expected):
        raise ValueError("unexpected container mount inventory")
    observed = []
    for item in actual:
        if not isinstance(item, dict) or type(item.get("RW")) is not bool:
            raise ValueError("invalid mount description")
        kind = item.get("Type")
        key = "Source" if kind == "bind" else "Name" if kind == "volume" else None
        if key is None or not isinstance(item.get(key), str) or not item[key]:
            raise ValueError("unexpected mount type or source")
        if not isinstance(item.get("Destination"), str) or not os.path.isabs(item["Destination"]):
            raise ValueError("invalid mount destination")
        observed.append({"Type": kind, key: item[key], "Destination": item["Destination"], "RW": item["RW"]})
    if sorted(observed, key=lambda m: m["Destination"]) != expected:
        raise ValueError("container mounts differ from the reviewed checkout contract")


def self_test():
    repo, common = "/work tree/selected", "/primary tree/.git"
    expected = expected_mounts(repo, common, "idf-build-test")
    validate(list(reversed(expected)), expected)
    primary = expected_mounts(repo, repo + "/.git", "v")
    assert len(primary) == 4
    assert {"Type": "bind", "Source": repo + "/.git", "Destination": "/project/.git", "RW": False} in primary
    validate(primary, primary)
    for remove_overlay in (False, True):
        altered = copy.deepcopy(primary)
        metadata = next(m for m in altered if m["Destination"] == "/project/.git")
        if remove_overlay:
            altered.remove(metadata)
        else:
            metadata["RW"] = True
        try:
            validate(altered, primary)
        except ValueError:
            continue
        raise AssertionError("writable primary Git metadata accepted")
    assert len(expected_mounts(repo, "", "v")) == 3
    assert len(expected) == 4
    assert mount_label(expected) != mount_label(expected_mounts(repo, "/other/.git", "idf-build-test"))
    cases = [None, {}, [], expected + [expected[0]], expected[:-1]]
    for key, value in (("RW", True), ("Source", "/primary tree"), ("Destination", "/elsewhere")):
        altered = copy.deepcopy(expected)
        external = next(m for m in altered if m.get("Source") == common)
        external[key] = value
        cases.append(altered)
    for key, value in (("RW", False), ("Source", "/other checkout"), ("Type", "tmpfs"), ("RW", 1)):
        altered = copy.deepcopy(expected)
        project = next(m for m in altered if m["Destination"] == "/project")
        project[key] = value
        cases.append(altered)
    duplicate = copy.deepcopy(expected)
    duplicate[0] = copy.deepcopy(duplicate[1])
    cases.append(duplicate)
    wrong_volume = copy.deepcopy(expected)
    next(m for m in wrong_volume if m["Type"] == "volume")["Name"] = "foreign-volume"
    cases.append(wrong_volume)
    for candidate in cases:
        try:
            validate(candidate, expected)
        except ValueError:
            continue
        raise AssertionError("unsafe container mount inventory accepted")
    print(f"IDF mount contract: PASS (exact inventory, {len(cases)} rejection controls)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root")
    parser.add_argument("--git-common", default="")
    parser.add_argument("--build-volume")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--label", action="store_true")
    mode.add_argument("--expected", action="store_true")
    mode.add_argument("--inspect", action="store_true")
    mode.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    if not args.repo_root or not args.build_volume:
        parser.error("--repo-root and --build-volume are required")
    expected = expected_mounts(args.repo_root, args.git_common, args.build_volume)
    if args.label:
        print(mount_label(expected))
    elif args.expected:
        print(json.dumps(expected))
    else:
        validate(json.load(sys.stdin), expected)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError) as exc:
        print(f"idf-mount-contract: {exc}", file=sys.stderr)
        sys.exit(1)
