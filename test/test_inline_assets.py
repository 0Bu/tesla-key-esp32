#!/usr/bin/env python3
"""Byte controls and actual CMake generation/dependency tests; no SDK or board."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
WWW = ROOT / "main/www"
SPEC = importlib.util.spec_from_file_location("css_comments", WWW / "prune_css_comments.py")
CSS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CSS)


class ScannerTests(unittest.TestCase):
    def test_only_whole_line_comment_bytes_change(self):
        raw = b'.x { color: red; }\n/* note\n\tX\r\n */\n.y {}'
        self.assertEqual(CSS.prune(raw), b'.x { color: red; }\n       \n\t \r\n   \n.y {}')
        for raw in (b'.x { color: /* note */ red; }', b'/* note */ .x {}', b'.x {} /* note */'):
            self.assertEqual(CSS.prune(raw), raw)

    def test_strings_escapes_and_parentheses_are_opaque(self):
        for raw in (b'"/* note */"', b"'/* note */'", b'"a\\\"(/* note */)"',
                    b'"a\\\r\nb"', b'"a\\\nb"', b'"a\\\rb"', b'"a\\\fb"', b'"a\\\\b"',
                    b'url(\n/* note */\n)', b'calc(\n/* note */\n1px)', b'f(g(\n/* note */\n))',
                    b'\\(\n/* note */ x'):
            with self.subTest(raw=raw):
                self.assertEqual(CSS.prune(raw), raw)

    def test_legal_annotations_hacks_and_html_context_are_preserved(self):
        for raw in (b'/*! notice */', b'/* SPDX-License-Identifier: MIT */', b'/* @preserve */',
                    b'/* @LICENSE */', b'/* Copyright */', b'/*# sourceMappingURL=x.map */',
                    b'/*@ sourceURL=x */', b'/* \\ legacy hack */', b'/* </StYlE> */'):
            self.assertEqual(CSS.prune(raw + b'\r\n.x {}'), raw + b'\r\n.x {}')

    def test_utf8_and_whitespace_are_preserved(self):
        raw = '/* Erklärung */\r\n.x {content:"ä"}\f'.encode()
        result = CSS.prune(raw)
        self.assertEqual(len(result), len(raw))
        self.assertEqual(result, b' ' * len('/* Erklärung */'.encode()) + '\r\n.x {content:"ä"}\f'.encode())
        self.assertEqual(CSS.prune(b'\xef\xbb\xbf.x {}'), b'\xef\xbb\xbf.x {}')

    def test_malformed_or_unsupported_input_fails(self):
        for raw in (b'/* open', b'"open', b'f(', b')', b'\\', b'"x\ny"', b'\0', b'\xff',
                    b'x' * (CSS.MAX_BYTES + 1)):
            with self.subTest(raw=raw), self.assertRaises(ValueError):
                CSS.prune(raw)

    def test_actual_css_is_bounded_and_idempotent(self):
        raw = (WWW / "style.css").read_bytes()
        result = CSS.prune(raw)
        self.assertEqual(len(result), len(raw))
        self.assertNotEqual(result, raw)
        self.assertEqual(CSS.prune(result), result)


class CMakeTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="tesla-inline-assets-")
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        self.www = self.work / "www"
        shutil.copytree(WWW, self.www)
        self.output = self.work / "page.html"

    def generate(self):
        return subprocess.run([
            "cmake", f"-DHTML={self.www / 'index.html'}", f"-DCSS={self.www / 'style.css'}",
            f"-DJS={self.www / 'app.js'}", f"-DOUT={self.output}", f"-DPYTHON={sys.executable}",
            "-P", str(self.www / "inline_assets.cmake"),
        ], capture_output=True, timeout=15)

    def test_actual_cli_preserves_markup_js_and_exact_css_bytes(self):
        raw = b'/* note */\r\n.x{content:"${KEEP};@KEEP@"}\r\n'
        (self.www / "style.css").write_bytes(raw)
        page = (self.www / "index.html").read_bytes()
        js = (self.www / "app.js").read_bytes()
        expected = page.replace(b'/*@@INLINE:style.css@@*/\n', CSS.prune(raw)).replace(b'//@@INLINE:app.js@@\n', js)
        result = self.generate()
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertEqual(self.output.read_bytes(), expected)
        self.assertFalse(self.output.with_suffix('.html.css-pruned').exists())

    def test_raw_marker_cannot_be_hidden_by_pruning(self):
        for marker in (b'/*@@INLINE:style.css@@*/', b'//@@INLINE:app.js@@'):
            (self.www / "style.css").write_bytes(b'/*\n' + marker + b'\n*/\n')
            result = self.generate()
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b'marker text', result.stderr)
            self.assertFalse(self.output.exists())

    def test_failed_or_missing_helper_cannot_succeed_with_stale_output(self):
        helper = self.www / "prune_css_comments.py"
        for replacement in (b'print("partial")\nraise SystemExit(23)\n', None):
            self.output.write_bytes(b'stale')
            if replacement is None:
                helper.unlink()
            else:
                helper.write_bytes(replacement)
            result = self.generate()
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b'CSS comment pruning failed', result.stderr)
            self.assertEqual(self.output.read_bytes(), b'stale')

    def test_prune_failure_removes_previously_validated_stale_output(self):
        self.output.write_bytes(b'stale')
        (self.www / 'style.css').write_bytes(b'/* unfinished')
        result = self.generate()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.output.exists())

    def test_output_cannot_truncate_or_remove_an_input_alias(self):
        css = self.www / 'style.css'
        original = css.read_bytes()
        for kind in ('same-path', 'symlink', 'hardlink'):
            if kind == 'same-path':
                self.output = css
            else:
                self.output = self.work / kind
                if kind == 'symlink':
                    self.output.symlink_to(css)
                else:
                    os.link(css, self.output)
            result = self.generate()
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(css.read_bytes(), original)

    def test_cli_refuses_symlink_directory_and_oversized_file(self):
        source = self.work / 'source.css'
        source.write_bytes(b'x')
        link = self.work / 'link.css'
        link.symlink_to(source)
        for candidate in (link, self.work, source):
            if candidate == source:
                source.write_bytes(b'x' * (CSS.MAX_BYTES + 1))
            result = subprocess.run([sys.executable, '-I', '-B', str(WWW / 'prune_css_comments.py'), str(candidate)],
                                    capture_output=True, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, b'')

    def test_real_custom_command_depends_on_helper(self):
        # Use the actual production custom-command block with only the SDK target
        # stubbed. Changing helper contents must cause a second real invocation.
        source = (ROOT / 'main/CMakeLists.txt').read_text()
        block = source[source.index('add_custom_command(', source.index('# Assemble')):
                       source.index('# Pre-compress the captive-portal')]
        (self.work / 'CMakeLists.txt').write_text(
            'cmake_minimum_required(VERSION 3.20)\nproject(inline_fixture NONE)\n'
            'add_custom_target(fixture_component)\nset(COMPONENT_LIB fixture_component)\n'
            + f'set(PYTHON "{sys.executable}")\n' + block)
        helper = self.www / 'prune_css_comments.py'
        helper.write_text('from pathlib import Path\n'
                          'with Path(__file__).with_suffix(".calls").open("a") as out: out.write("run\\n")\n'
                          + helper.read_text())
        build = self.work / 'build'
        def run(*args):
            result = subprocess.run(['cmake', *map(str, args)], capture_output=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr.decode())
        run('-S', self.work, '-B', build)
        run('--build', build, '--target', 'gen_index_gz')
        self.assertEqual(helper.with_suffix('.calls').read_text().splitlines(), ['run'])
        helper.write_text(helper.read_text() + '\n# Dependency input changed.\n')
        # Make generators may compare timestamps at whole-second resolution.
        changed = max(helper.stat().st_mtime, (build / 'index_inlined.html').stat().st_mtime) + 2
        os.utime(helper, (changed, changed))
        run('--build', build, '--target', 'gen_index_gz')
        self.assertEqual(helper.with_suffix('.calls').read_text().splitlines(), ['run', 'run'])


if __name__ == '__main__':
    unittest.main()
