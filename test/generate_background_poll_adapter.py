#!/usr/bin/env python3
"""Extract the shipped background enqueue seam; host adapters perform no BLE I/O."""
import pathlib
import re
import runpy
import sys

root = pathlib.Path(__file__).resolve().parents[1]
body_in = runpy.run_path(str(root / 'test/test_runtime_boundary_contract.py'))['function_body_in']
source = (root / 'main/vehicle_telemetry.cpp').read_text()
parts = []
for name, pattern in (
    ('build_drive_state_poll', r'^int build_drive_state_poll\('),
    ('enqueue_background_poll_', r'^bool VehicleController::enqueue_background_poll_\('),
    ('enqueue_telemetry_round_', r'^\[\[gnu::noinline\]\] void VehicleController::enqueue_telemetry_round_\('),
):
    start = re.search(pattern, source, re.M)
    if not start:
        raise AssertionError('missing production background seam: ' + name)
    body = body_in(source, name)
    prefix = source[start.start():source.index(body, start.start())]
    parts.append(prefix.replace('VehicleController::', 'BackgroundPollFixture::') + body)
pathlib.Path(sys.argv[1]).write_text('\n\n'.join(parts) + '\n')
