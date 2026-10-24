"""Blank whole-line CSS documentation comments without rewriting tokens or whitespace.

Only the repository's UTF-8, NUL-free CSS subset is supported. Strings, escapes,
parenthesized regions, inline comments, legal/tooling notices and HTML style-end
sequences remain opaque. Unsupported unfinished syntax fails instead of guessing.
"""
from pathlib import Path
import os
import sys
import tempfile

MAX_BYTES = 128 * 1024
SPACE = b' \t\r\n\f'


def prune(source: bytes) -> bytes:
    if not isinstance(source, bytes) or len(source) > MAX_BYTES:
        raise ValueError('CSS input exceeds the bounded byte budget')
    source.decode('utf-8', errors='strict')
    if b'\x00' in source:
        raise ValueError('CSS input must not contain NUL')
    output = bytearray()
    quote = None
    depth = 0
    i = 0
    while i < len(source):
        value = source[i]
        if value == 92:
            if i + 1 >= len(source):
                raise ValueError('unfinished CSS escape')
            width = 3 if source[i + 1:i + 3] == b'\r\n' else 2
            output.extend(source[i:i + width])
            i += width
            continue
        if quote is not None:
            if value == quote:
                quote = None
            elif value in (10, 13, 12):
                raise ValueError('unescaped newline in CSS string')
            output.append(value)
            i += 1
            continue
        if source[i:i + 2] == b'/*':
            end = source.find(b'*/', i + 2)
            if end < 0:
                raise ValueError('unfinished CSS comment')
            end += 2
            comment = source[i:end]
            start_line = max(source.rfind(bytes([c]), 0, i) for c in (10, 13, 12)) + 1
            endings = [source.find(bytes([c]), end) for c in (10, 13, 12)]
            end_line = min((n for n in endings if n >= 0), default=len(source))
            whole_line = not source[start_line:i].strip(SPACE) and not source[end:end_line].strip(SPACE)
            lowered = comment.lower()
            legal = (comment.startswith((b'/*!', b'/*#', b'/*@')) or b'\\' in comment or
                     any(marker in lowered for marker in
                         (b'@license', b'copyright', b'@preserve', b'spdx', b'sourcemappingurl', b'sourceurl', b'</style')))
            if whole_line and depth == 0 and not legal:
                output.extend(value if value in SPACE else 32 for value in comment)
            else:
                output.extend(comment)
            i = end
            continue
        if value in (34, 39):
            quote = value
        elif value == 40:
            depth += 1
        elif value == 41:
            if depth == 0:
                raise ValueError('unbalanced CSS parentheses')
            depth -= 1
        output.append(value)
        i += 1
    if quote is not None or depth:
        raise ValueError('unfinished CSS string or parentheses')
    return bytes(output)


def main() -> int:
    try:
        if len(sys.argv) == 2:
            output = prune(read_source(Path(sys.argv[1])))
        elif len(sys.argv) == 6 and sys.argv[1] == '--inline':
            destination = Path(sys.argv[2])
            inputs = [Path(name) for name in sys.argv[3:]]
            validate_output(destination, [*inputs, Path(__file__)])
            destination.unlink(missing_ok=True)
            page, css, js = (read_source(file) for file in inputs)
            style_marker = b'/*@@INLINE:style.css@@*/\n'
            script_marker = b'//@@INLINE:app.js@@\n'
            if any(page.count(marker) != 1 for marker in (style_marker, script_marker)) or any(
                    marker.rstrip(b'\n') in asset for asset in (css, js)
                    for marker in (style_marker, script_marker)):
                raise ValueError('raw inline marker contract failed')
            output = page.replace(style_marker, prune(css)).replace(script_marker, js)
            # Publish complete bytes without truncating an input or leaving a
            # partial page when generation fails. CMake never reserializes them.
            temporary = None
            try:
                with tempfile.NamedTemporaryFile(dir=destination.parent, prefix='.inline-', delete=False) as handle:
                    temporary = Path(handle.name)
                    handle.write(output)
                os.replace(temporary, destination)
            finally:
                if temporary is not None:
                    temporary.unlink(missing_ok=True)
            return 0
        else:
            raise ValueError('expected CSS path, or --inline OUT HTML CSS JS')
        sys.stdout.buffer.write(output)
        return 0
    except (OSError, ValueError) as error:
        print(f'CSS comment pruning failed: {error}', file=sys.stderr)
        return 1


def validate_output(destination: Path, inputs: list[Path]) -> None:
    if destination.is_symlink() or not destination.parent.is_dir() or (
            destination.exists() and not destination.is_file()):
        raise ValueError('inline output must be a regular build path')
    for source in inputs:
        if destination.resolve() == source.resolve() or (
                destination.exists() and source.exists() and destination.samefile(source)):
            raise ValueError('inline output must not alias an input')


def read_source(file: Path) -> bytes:
    if not file.is_file() or file.is_symlink() or file.stat().st_size > MAX_BYTES:
        raise ValueError('asset input must be a bounded regular file')
    with file.open('rb') as handle:
        raw = handle.read(MAX_BYTES + 1)
    if len(raw) > MAX_BYTES or b'\x00' in raw:
        raise ValueError('asset input exceeds text/byte bounds')
    raw.decode('utf-8', errors='strict')
    return raw


if __name__ == '__main__':
    raise SystemExit(main())
