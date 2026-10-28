#!/usr/bin/env python3
"""Extract complete actual OTA status/entry/application bodies; no substituted owner logic."""
import pathlib
import runpy
import sys
root=pathlib.Path(__file__).resolve().parents[1]
body_in=runpy.run_path(str(root/'test/test_runtime_boundary_contract.py'))['function_body_in']
source=(root/'main/ota_update.cpp').read_text()
parts=['static const char* TAG = "ota-native";',
       'static tk::OtaIdentityOperationGate s_operation_gate;',
       'static std::atomic<tk::OtaVerificationState> s_verification{tk::OtaVerificationState::Unknown};']
for name, signature in [('try_begin_ota_operation','static bool try_begin_ota_operation()'),
                        ('finish_operation','static void finish_operation(tk::OtaIdentityGateState owner)'),
                        ('ota_verification_state','tk::OtaVerificationState ota_verification_state()')]:
    body=body_in(source,name)
    parts.append(signature+' '+body)
start=source.index('static constexpr size_t kOtaStatusMessageCapacity')
# The complete remaining family includes real status storage, locks, gate/lease/operation,
# all entry points, async wrappers and blocking check. No per-function implementation mirror.
parts.append(source[start:])
pathlib.Path(sys.argv[1]).write_text('\n\n'.join(parts)+'\n')
