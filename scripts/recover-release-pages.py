#!/usr/bin/env python3
"""Verify, or explicitly republish, the original site of the latest immutable stable Release.

Run from a clean, current main checkout. The original signed Actions artifact must still exist.
Historical firmware/site bytes are inert inputs; only current reviewed helpers execute. No key,
build, signing or GitHub Release mutation is used. Ordinary first publication is unchanged.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import importlib.util
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = "0Bu/tesla-key-esp32"
SHA = re.compile(r"[0-9a-f]{40}")
VERSION = re.compile(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)")


class RecoveryError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RecoveryError(message)


def sibling(name: str):
    spec = importlib.util.spec_from_file_location("recovery_" + name.replace("-", "_"), ROOT / "scripts" / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def command(*args: str, timeout: int = 120) -> str:
    return subprocess.check_output(args, cwd=ROOT, text=True, timeout=timeout).strip()


def api(endpoint: str, *, pages: bool = False):
    args = ["gh", "api", "--hostname", "github.com", f"repos/{REPOSITORY}/{endpoint}"]
    if pages:
        args += ["--paginate", "--slurp"]
    return json.loads(command(*args))


def validate_environment() -> None:
    require(os.environ.get("GH_HOST", "github.com") == "github.com", "foreign GitHub API host")
    require(os.environ.get("GITHUB_SERVER_URL", "https://github.com") == "https://github.com",
            "foreign publication server")
    require(not os.environ.get("TEST_REMOTE"), "recovery forbids publication remote overrides")
    require(os.environ.get("GITHUB_REPOSITORY", REPOSITORY) == REPOSITORY, "foreign publication repository")


def archive_files(path: Path, expected_digest: str) -> dict[str, bytes]:
    require(re.fullmatch(r"sha256:[0-9a-f]{64}", expected_digest) is not None,
            "original artifact has no authenticated archive digest")
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, "rb") as stream:
        info = os.fstat(stream.fileno())
        require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1 and
                0 < info.st_size <= 512 * 1024 * 1024, "unsafe or oversized artifact archive")
        data = stream.read(512 * 1024 * 1024 + 1)
    require(len(data) == info.st_size and "sha256:" + hashlib.sha256(data).hexdigest() == expected_digest,
            "artifact archive differs from the authenticated GitHub digest")
    files: dict[str, bytes] = {}
    total = 0
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        seen = set()
        for entry in archive.infolist():
            name = entry.filename.rstrip("/")
            require(name and name not in seen and "\\" not in name and
                    not name.startswith("/") and not {"..", "."} & set(name.split("/")),
                    "unsafe or duplicate artifact ZIP member")
            seen.add(name)
            mode = entry.external_attr >> 16
            require(not stat.S_ISLNK(mode) and not entry.flag_bits & 1, "linked/encrypted artifact member")
            if entry.is_dir():
                continue
            require(stat.S_IFMT(mode) in {0, stat.S_IFREG} and
                    0 <= entry.file_size <= 128 * 1024 * 1024, "unsafe artifact member type/size")
            total += entry.file_size
            require(total <= 512 * 1024 * 1024 and len(files) < 2048, "artifact extraction exceeds bound")
            files[name] = archive.read(entry)
    return files


def snapshot_tree(path: Path, excluded: frozenset[str] = frozenset()) -> dict[str, bytes]:
    """One bounded no-follow snapshot; reject aliases and special files before any publication."""
    files: dict[str, bytes] = {}
    total = 0
    flags = os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK

    def visit(directory: int, prefix: str = "") -> None:
        nonlocal total
        for name in sorted(os.listdir(directory)):
            if not prefix and name in excluded:
                continue
            info = os.stat(name, dir_fd=directory, follow_symlinks=False)
            relative = prefix + name
            require(not stat.S_ISLNK(info.st_mode), f"symlink in recovery input: {relative}")
            if stat.S_ISDIR(info.st_mode):
                child = os.open(name, flags | os.O_DIRECTORY, dir_fd=directory)
                try:
                    require(os.fstat(child).st_ino == info.st_ino, "recovery directory changed")
                    visit(child, relative + "/")
                finally:
                    os.close(child)
            else:
                require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1,
                        f"non-regular or hard-linked recovery input: {relative}")
                require(0 <= info.st_size <= 128 * 1024 * 1024, "recovery file exceeds bound")
                fd = os.open(name, flags, dir_fd=directory)
                with os.fdopen(fd, "rb") as stream:
                    before = os.fstat(stream.fileno())
                    require((before.st_dev, before.st_ino) == (info.st_dev, info.st_ino),
                            "recovery input changed before open")
                    data = stream.read(128 * 1024 * 1024 + 1)
                    after = os.fstat(stream.fileno())
                identity = lambda entry: (entry.st_dev, entry.st_ino, entry.st_size,
                                           entry.st_mtime_ns, entry.st_ctime_ns, entry.st_nlink)
                require(identity(before) == identity(after) and len(data) == info.st_size,
                        "recovery input changed while read")
                total += len(data)
                require(total <= 512 * 1024 * 1024 and len(files) < 2048, "recovery tree exceeds bound")
                files[relative] = data

    fd = os.open(path, flags | os.O_DIRECTORY)
    try:
        visit(fd)
    finally:
        os.close(fd)
    return files


def write_snapshot(files: dict[str, bytes], destination: Path) -> None:
    destination.mkdir(mode=0o700)
    for relative, data in files.items():
        path = destination / relative
        path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        with path.open("xb") as stream:
            stream.write(data)


def validate_provenance(run: dict, artifacts: list, jobs: list, source: str, version: str) -> dict:
    require(run.get("head_sha") == source and run.get("head_branch") == "main" and
            run.get("path") == ".github/workflows/build.yml" and run.get("status") == "completed" and
            run.get("conclusion") in {"success", "failure"} and
            run.get("event") in {"push", "workflow_dispatch"}, "original build provenance is ineligible")
    name = f"tesla-key-esp32-{version}-{source}"
    eligible = [a for a in artifacts if a.get("name") == name and a.get("expired") is False]
    require(len(eligible) == 1, "exact original signed artifact is absent, expired or ambiguous")
    by_name = {job.get("name"): job for job in jobs}
    required = ["logic-test", "logic-harness", "build", "independent-rebuild"]
    for target in ("esp32", "esp32s3", "esp32c3", "esp32c6"):
        required += [f"build-target ({target}, primary)", f"independent-rebuild-target ({target})"]
    require(all(sum(job.get("name") == name for job in jobs) == 1 for name in required),
            "original job evidence is absent or ambiguous")
    require(all(by_name.get(name, {}).get("conclusion") == "success" for name in required),
            "original required build/publication gates did not all succeed")
    require(sum(job.get("name") == "publish" for job in jobs) == 1 and
            by_name["publish"].get("conclusion") in {"success", "failure"},
            "original publication evidence is absent, ambiguous or cancelled")
    for job, step in [("logic-test", "Run host-side logic tests"),
                      ("logic-harness", "Run tesla-ble integration harness"),
                      ("logic-harness", "Prove and run Linux memory-safety instrumentation"),
                      ("publish", "Validate build provenance"),
                      ("publish", "Bind all 16 local Pages parts before any publication"),
                      ("publish", "Upload signed firmware artifacts"),
                      ("publish", "Verify production signing authority for all staged apps")]:
        matches = [s for s in by_name[job].get("steps", []) if s.get("name") == step]
        require(len(matches) == 1 and matches[0].get("conclusion") == "success",
                f"original gate missing, failed or ambiguous: {job}/{step}")
    publication_steps = by_name["publish"].get("steps", [])
    for name in ("Provision OTA signing key", "Sign and stage release artifacts", "Remove OTA signing key"):
        require(sum(step.get("name") == name for step in publication_steps) == 1,
                "original key/signing evidence is ambiguous")
    require(sum(step.get("name") == "Reuse and stage exact immutable Release bytes"
                for step in publication_steps) <= 1, "original reuse evidence is ambiguous")
    steps = {step["name"]: step.get("conclusion") for step in publication_steps}
    signing = [steps.get(name) for name in ("Provision OTA signing key", "Sign and stage release artifacts", "Remove OTA signing key")]
    require(signing == ["success"] * 3 or
            (signing == ["skipped"] * 3 and steps.get("Reuse and stage exact immutable Release bytes") == "success"),
            "original signing/key-removal or strictly key-free reuse evidence is incomplete")
    return eligible[0]


def verify(artifact: Path, run_id: int, candidate: Path | None = None,
           expected_id: int | None = None, version: str | None = None, source: str | None = None,
           expected_pin: bytes | None = None, archive_digest: str | None = None) -> tuple[dict, str, str, str]:
    validate_environment()
    require(run_id > 0, "invalid original run ID")
    require(command("git", "remote", "get-url", "origin") in {
        f"https://github.com/{REPOSITORY}.git", f"git@github.com:{REPOSITORY}.git"}, "foreign checkout origin")
    require(not command("git", "-c", "core.fsmonitor=false", "status", "--porcelain"),
            "recovery requires a clean trusted checkout")
    main = api("git/ref/heads/main")["object"]["sha"]
    require(SHA.fullmatch(main) is not None and command("git", "rev-parse", "HEAD") == main,
            "recovery helpers must come from current main")
    release = api("releases/latest")
    source = source or release.get("target_commitish", "")
    version = version or release.get("tag_name", "").removeprefix("v")
    require(SHA.fullmatch(source) is not None and VERSION.fullmatch(version) is not None,
            "invalid immutable Release source/version")
    require(type(release.get("id")) is int and release["id"] > 0 and
            (expected_id is None or release["id"] == expected_id), "latest Release changed")
    release_contract = sibling("check-release-assets")
    tagged = api(f"releases/tags/v{version}")
    def authority(value):
        assets = release_contract.validate_full_metadata(value, version, source, "published-immutable")
        return (value.get("id"), sorted((name, a["id"], a["size"], a["digest"]) for name, a in assets.items()))
    require(authority(tagged) == authority(release), "tagged Release differs from latest")
    tag = api(f"git/ref/tags/v{version}")["object"]
    for _ in range(4):
        if tag.get("type") != "tag":
            break
        tag = api("git/tags/" + tag["sha"])["object"]
    require(tag.get("type") == "commit" and tag.get("sha") == source, "Release tag/source mismatch")
    command("git", "merge-base", "--is-ancestor", source, main)
    run = api(f"actions/runs/{run_id}")
    artifacts = [a for page in api(f"actions/runs/{run_id}/artifacts", pages=True) for a in page["artifacts"]]
    jobs = [j for page in api(f"actions/runs/{run_id}/jobs", pages=True) for j in page["jobs"]]
    require(run.get("id") == run_id, "original run ID mismatch")
    original_artifact = validate_provenance(run, artifacts, jobs, source, version)
    require(archive_digest is not None and original_artifact.get("digest") == archive_digest,
            "original artifact archive authority changed")
    metadata = dict(line.split("=", 1) for line in (artifact / "dist/build-metadata.txt").read_text().splitlines())
    require(metadata == {"head_sha": source, "display_version": version}, "signed artifact source/version mismatch")
    sibling("check-signed-root-inventory").validate_inventory(artifact, version)
    if expected_pin is None:
        expected_pin = sibling("check-firmware-artifacts").read_public_key_digest(
            ROOT / "scripts/ota-signing-public-key.sha256")
    release_contract.validate_full_release(release, artifact, version, source,
                                           "published-immutable", expected_pin)
    original = artifact / "_site"
    try:
        control = sibling("pages_publish_control")
        control.check_marker(original)
        expected_site = control.expected_site(snapshot_tree(original))
    except (OSError, ValueError) as error:
        raise RecoveryError(str(error)) from error
    sibling("check-pages-manifest").validate(original, source, version)
    sibling("check-release-pages-bytes").verify(original, artifact, version)
    sibling("generate-ota-changelog").validate_document_bytes((original / "changelog.json").read_bytes(), version)
    if candidate is not None:
        require(snapshot_tree(candidate, frozenset({".git", "PR", "dev"})) == expected_site,
                "recovery candidate differs from the original signed site")
    pages_url = sibling("check-pages-source").validate(api("pages"))
    return release, source, version, pages_url


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifact-archive", type=Path,
                        help="previously downloaded ZIP; its GitHub archive digest is always verified")
    parser.add_argument("--run-id", type=int, required=True)
    parser.add_argument("--release-id", type=int)
    parser.add_argument("--version")
    parser.add_argument("--source-sha")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--verify-candidate", type=Path)
    mode.add_argument("--publish", action="store_true")
    args = parser.parse_args()
    try:
        validate_environment()
        with tempfile.TemporaryDirectory(prefix="tesla-release-pages-recovery-") as temporary:
            release = api("releases/latest")
            version = args.version or release.get("tag_name", "").removeprefix("v")
            source = args.source_sha or release.get("target_commitish", "")
            require(VERSION.fullmatch(version) is not None and SHA.fullmatch(source) is not None,
                    "invalid Release source/version")
            sibling("check-release-assets").validate_full_metadata(release, version, source, "published-immutable")
            artifacts = [a for page in api(f"actions/runs/{args.run_id}/artifacts", pages=True) for a in page["artifacts"]]
            name = f"tesla-key-esp32-{version}-{source}"
            eligible = [a for a in artifacts if a.get("name") == name and a.get("expired") is False]
            require(len(eligible) == 1 and type(eligible[0].get("id")) is int and eligible[0]["id"] > 0,
                    "exact original artifact is unavailable or ambiguous")
            original = eligible[0]
            archive = args.artifact_archive or Path(temporary) / "original.zip"
            if args.artifact_archive is None:
                with archive.open("xb") as output:
                    subprocess.run(["gh", "api", "--hostname", "github.com",
                        f"repos/{REPOSITORY}/actions/artifacts/{original['id']}/zip"],
                        cwd=ROOT, stdout=output, check=True, timeout=120)
            files = archive_files(archive, original.get("digest", ""))
            frozen = Path(temporary) / "artifact"
            write_snapshot(files, frozen)
            release, source, version, pages_url = verify(frozen, args.run_id, args.verify_candidate,
                args.release_id, version, source, archive_digest=original["digest"])
            if args.publish:
                environment = os.environ.copy()
                environment.update(GITHUB_REPOSITORY=REPOSITORY, SOURCE_SHA=source,
                    RELEASE_RECOVERY_RUN_ID=str(args.run_id), RELEASE_RECOVERY_ID=str(release["id"]),
                    RELEASE_RECOVERY_ARCHIVE=str(archive), RELEASE_RECOVERY_VERSION=version)
                subprocess.run(["bash", str(ROOT / "scripts/publish-pages-branch.sh"),
                    "root-recovery", str(frozen / "_site")], cwd=ROOT, env=environment, check=True)
                command("python3", str(ROOT / "scripts/wait-pages-build.py"), "--repository", REPOSITORY,
                    "--attempts", "32", "--interval", "10", timeout=900)
                metadata = Path(temporary) / "release.json"
                metadata.write_text(json.dumps(api("releases/latest")))
                command("python3", str(ROOT / "scripts/check-published-release.py"),
                    "--pages-base-url", pages_url, "--release-base-url", f"https://github.com/{REPOSITORY}/releases/download/v{version}/",
                    "--release-json", str(metadata), "--version", version, "--source-sha", source,
                    "--expected-changelog-file", str(frozen / "_site/changelog.json"),
                    "--timeout", "15", timeout=600)
        print("release Pages recovery: PASS (immutable latest, exact original artifact and site; no signing)")
        return 0
    except (RecoveryError, OSError, ValueError, TypeError, KeyError, zipfile.BadZipFile,
            subprocess.SubprocessError, RuntimeError) as error:
        print(f"release Pages recovery: FAIL: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
