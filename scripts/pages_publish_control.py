#!/usr/bin/env python3
"""Own the empty root marker that keeps already-built Pages payloads out of Jekyll."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import stat

MARKER = ".nojekyll"


def check_marker(path: Path, *, create: bool = False) -> None:
    """Create only an absent marker; never follow, truncate or replace an existing entry."""
    root = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    descriptor = None
    try:
        try:
            descriptor = os.open(MARKER, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=root)
        except FileNotFoundError:
            if not create:
                return
            descriptor = os.open(MARKER, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                                 0o644, dir_fd=root)
        info = os.fstat(descriptor)
        if not stat.S_ISREG(info.st_mode) or info.st_size != 0 or info.st_nlink != 1:
            raise ValueError("Pages control marker must be an empty, regular, unaliased file")
    finally:
        if descriptor is not None:
            os.close(descriptor)
        os.close(root)


def ensure_static_site(path: Path) -> None:
    check_marker(path, create=True)


def expected_site(files: dict[str, bytes]) -> dict[str, bytes]:
    """Preserve the entire authenticated payload, with exactly one empty control marker."""
    if MARKER in files and files[MARKER] != b"":
        raise ValueError("original Pages control marker is not empty")
    return {**files, MARKER: b""}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("site", type=Path)
    arguments = parser.parse_args()
    try:
        ensure_static_site(arguments.site)
    except (OSError, ValueError) as error:
        parser.exit(1, f"Pages control marker refused: {error}\n")
