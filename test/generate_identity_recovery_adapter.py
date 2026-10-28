#!/usr/bin/env python3
"""Extract complete production recovery/facade/owner/payload bodies for passive host tests."""
from pathlib import Path
import re
import runpy
import sys

home = Path(__file__).resolve().parents[1]
root = Path(sys.argv[1]).resolve() if len(sys.argv) == 3 else home
destination = Path(sys.argv[-1])
helpers = runpy.run_path(str(home / 'test/test_runtime_boundary_contract.py'))
body_in = helpers['function_body_in']
balanced = helpers['balanced_end']
scrub = helpers['scrub_cpp_preserving_layout']

def function(source, name, prefix):
    matches = list(re.finditer(prefix, source, re.M))
    if len(matches) != 1:
        raise AssertionError('missing/ambiguous actual recovery body: ' + name)
    start = matches[0].start()
    body = body_in(source, name)
    return source[start:source.index(body, start)] + body

ota = (root / 'main/ota_update.cpp').read_text()
pairing = (root / 'main/vehicle_pairing.cpp').read_text()
facade = (root / 'main/runtime_admission.cpp').read_text()
payload = (root / 'main/mqtt_payloads.hpp').read_text()
parts = [function(ota, 'ota_verification_state', r'^tk::OtaVerificationState ota_verification_state\(')]
first = ota.index('OtaIdentityMutationGuard::OtaIdentityMutationGuard(')
parts.append(ota[first:ota.index('bool ota_fault_restart_begin()', first)])
first = ota.index('OtaConfigRestartGuard::~OtaConfigRestartGuard()')
parts.append(ota[first:ota.index('tk::RuntimeHealthSample ota_runtime_health()', first)])
parts.append(function(ota, 'ota_config_image_state', r'^tk::ConfigImageResult ota_config_image_state\('))
parts.append(re.sub(r'^#include[^\n]*\n', '', facade, flags=re.M))
parts.append(function(pairing, 'automatic_identity_reload_once',
                      r'^template <typename Recheck>\nstatic bool automatic_identity_reload_once\('))
parts.append(function(pairing, 'generate_key', r'^bool VehicleController::generate_key\('))

supervisor = body_in(pairing, 'auto_pair_task_fn_')
marker = 'if (self->key_reload_required_.load())'
if supervisor.count(marker) != 1:
    raise AssertionError('ambiguous actual reload branch')
start = supervisor.index(marker)
opening = supervisor.index('{', start)
end = balanced(scrub(supervisor), opening, '{', '}', marker)
parts.append('void actual_reload_caller(VehicleController* self, tk::IdentityReloadOnce& identity_reload) {\n'
             'for (int pass = 0; pass != 1; ++pass) ' + supervisor[start:end + 1] + '\n}\n')

start = payload.index('struct DevicePayload {')
end = payload.index('\n};', start) + 3
present = function(payload, 'present', r'^inline bool present\(')
build = function(payload, 'build_device_payload', r'^inline JsonOwner build_device_payload\(')
parts.append('namespace tk::mqtt {\n' + present + '\n' + payload[start:end] + '\n' + build + '\n}\n')
destination.write_text('\n\n'.join(parts) + '\n')
