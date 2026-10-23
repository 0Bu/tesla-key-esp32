#!/usr/bin/env python3
"""Extract the actual discovery round and address snapshot; platform I/O stays passive."""
import pathlib
import re
import runpy
import sys

root = pathlib.Path(__file__).resolve().parents[1]
body_in = runpy.run_path(str(root / 'test/test_runtime_boundary_contract.py'))['function_body_in']
source = (root / 'main/mqtt_ha.cpp').read_text()
parts = []
for name in ('pub_json', 'current_configuration_url', 'publish_discovery_rows', 'publish_discovery'):
    start = re.search(r'^(?:\[\[gnu::noinline\]\] )?static (?:bool|void) ' + name + r'\(', source, re.M)
    if not start:
        raise AssertionError('missing production MQTT seam: ' + name)
    body = body_in(source, name)
    parts.append(source[start.start():source.index(body, start.start())] + body)
pathlib.Path(sys.argv[1]).write_text('\n\n'.join(parts) + '\n')
