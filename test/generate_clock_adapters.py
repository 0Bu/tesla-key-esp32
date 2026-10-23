#!/usr/bin/env python3
"""Compile the shipped clock shell/getters with passive host I/O, never a live board."""
import pathlib
import re
import runpy
import sys
root = pathlib.Path(__file__).resolve().parents[1]
helpers = runpy.run_path(str(root / 'test/test_runtime_boundary_contract.py'))
body_in = helpers['function_body_in']
main = (root / 'main/main.cpp').read_text()
block = main[main.index('// ─── Wall clock'):main.index('// ─── app_main')]
pairing = (root / 'main/vehicle_pairing.cpp').read_text()
parts = [block]
for name in ('key_created_at', 'paired_at'):
    declaration = re.search(r'^time_t VehicleController::' + name + r'\(\)\s*', pairing, re.M)
    if not declaration:
        raise AssertionError('missing production date getter: ' + name)
    body = body_in(pairing, name)
    parts.append(pairing[declaration.start():declaration.end()] + body)
pathlib.Path(sys.argv[1]).write_text('\n\n'.join(parts))
