#!/usr/bin/env python3
"""Extract complete actual OTA status/entry/application bodies; no substituted owner logic."""
import pathlib
import runpy
import re
import sys
root=pathlib.Path(__file__).resolve().parents[1]
helpers=runpy.run_path(str(root/'test/test_runtime_boundary_contract.py'))
body_in=helpers['function_body_in']
source=(root/'main/ota_update.cpp').read_text()
parts=['static const char* TAG = "ota-native";',
       'static tk::OtaIdentityOperationGate s_operation_gate;',
       'static std::atomic<tk::OtaVerificationState> s_verification{tk::OtaVerificationState::Unknown};']
for name, signature in [('try_begin_ota_operation','static bool try_begin_ota_operation()'),
                        ('finish_operation','static void finish_operation(tk::OtaIdentityGateState owner)'),
                        ('ota_verification_state','tk::OtaVerificationState ota_verification_state()')]:
    body=body_in(source,name)
    parts.append(signature+' '+body)
d=source.index('OtaConfigRestartGuard::~OtaConfigRestartGuard() noexcept')
opening=source.index('{',d)
closing=helpers['balanced_end'](helpers['scrub_cpp_preserving_layout'](source),opening,'{','}','config destructor')
parts.append(source[d:closing+1])
start=source.index('static constexpr size_t kOtaStatusMessageCapacity')
# The complete remaining family includes real status storage, locks, gate/lease/operation,
# all entry points, async wrappers and blocking check. No per-function implementation mirror.
parts.append(source[start:])
pathlib.Path(sys.argv[1]).write_text('\n\n'.join(parts)+'\n')

# Real HTTP handlers share the real application above. Only request/config storage and reply
# transport are passive seams; no copied candidate, invalidation or installation implementation.
http=[]
for file,names in (
 ('http_ota.cpp',('ota_boot_id_', 'ota_state_str', 'parse_pr_param_', 'send_ota_error_',
                  'handle_ota_update', 'handle_ota_status', 'handle_ota_changelog')),
 ('http_config.cpp',('handle_set_ota',))):
 text=(root/'main'/file).read_text()
 for name in names:
  matches=list(re.finditer(r'^(?:static )?(?:esp_err_t|bool|const char\*|const std::array<char, 17>&) '+name+r'\(',text,re.M))
  if len(matches)!=1:raise AssertionError('ambiguous actual HTTP function '+name)
  start=matches[0].start();opening=text.index('{',matches[0].end())
  closing=helpers['balanced_end'](helpers['scrub_cpp_preserving_layout'](text),
                                  opening,'{','}',name)
  http.append(text[start:closing+1])
pathlib.Path(sys.argv[1]).with_name('ota_offer_http.generated.hpp').write_text('\n\n'.join(http)+'\n')
