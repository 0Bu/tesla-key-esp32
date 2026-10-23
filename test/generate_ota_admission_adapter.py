#!/usr/bin/env python3
"""Extract actual OTA admission/cache bodies; platform doubles never start a worker."""
import pathlib
import re
import runpy
import sys

root = pathlib.Path(__file__).resolve().parents[1]
body_in = runpy.run_path(str(root / 'test/test_runtime_boundary_contract.py'))['function_body_in']
source = (root / 'main/ota_update.cpp').read_text()
parts = []
for name in ('ota_verification_state', 'ota_cached_verification_state',
             'ota_refresh_verification_cache', 'ota_check_start', 'ota_start'):
    start = re.search(r'^tk::\w+ ' + name + r'\(', source, re.M)
    if not start:
        raise AssertionError('missing production OTA seam: ' + name)
    body = body_in(source, name)
    parts.append(source[start.start():source.index(body, start.start())] + body)
pathlib.Path(sys.argv[1]).write_text('\n\n'.join(parts) + '\n')
