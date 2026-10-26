#!/usr/bin/env python3
"""Compile actual supervisor/facade/owner bodies against passive platform/storage doubles."""
from pathlib import Path
import re
import runpy
import sys

root = Path(__file__).resolve().parents[1]
helpers = runpy.run_path(str(root / 'test/test_runtime_boundary_contract.py'))
body_in = helpers['function_body_in']
scrub = helpers['scrub_cpp_preserving_layout']
balanced = helpers['balanced_end']
ota = (root / 'main/ota_update.cpp').read_text()
start = ota.index('OtaFaultRestartGuard::OtaFaultRestartGuard()')
end = ota.index('tk::RuntimeHealthSample ota_runtime_health()', start)
owners = ota[start:end]
if owners.count('OtaFaultRestartGuard::OtaFaultRestartGuard()') != 1:
    raise AssertionError('ambiguous production recovery ownership surface')
body = body_in(ota, 'ota_verification_state')
match = re.search(r'^tk::OtaVerificationState ota_verification_state\(', ota, re.M)
if not match:
    raise AssertionError('missing actual raw OTA-state query')
query = ota[match.start():ota.index(body, match.start())] + body

def complete_body(name):
    text = (root / 'main' / name).read_text()
    # Only includes are substituted by the host translation unit. No decision/function body is
    # rewritten, selected, mirrored or replaced; new platform calls therefore fail compilation.
    return re.sub(r'^#include[^\n]*\n', '', text, flags=re.M)

main = (root / 'main/main.cpp').read_text()
fatal_body = body_in(main, 'fatal_startup_rollback')
fatal_start = main.index('static void fatal_startup_rollback()')
fatal = main[fatal_start:main.index(fatal_body, fatal_start)] + fatal_body

def unique_block(marker):
    if main.count(marker) != 1:
        raise AssertionError('ambiguous bootstrap extraction marker: ' + marker)
    start = main.index(marker)
    opening = main.index('{', start)
    end = balanced(scrub(main), opening, '{', '}', marker)
    return main[opening:end + 1]

vin_body = unique_block('const char* recovery_failure = [&]() -> const char*')
vin = ('const char* actual_vin_repair(tk::VinTransitionRecovery recovery, '
       'tk::ConfigBlob& cfg_blob, const tk::VinTransitionMarker& marker, '
       'NvsStorageAdapter& config_store, NvsStorageAdapter& tesla_store) {\n'
       'return [&]() -> const char* ' + vin_body + '();\n}')
wifi_marker = ('            {\n                OtaConfigRestartGuard owner;\n'
               '                if (owner.try_begin()) {\n'
               '                    cfg_blob.wifi_ssid = cfg_blob.wifi_ssid_backup;')
wifi = ('void actual_wifi_rollback(tk::ConfigBlob& cfg_blob, '
        'NvsStorageAdapter& config_store) ' + unique_block(wifi_marker))

vehicle = (root / 'main/vehicle_telemetry.cpp').read_text()
progress = re.findall(r'        if \(tk::vehicle_iteration_successful\(runner_failed, parse_fault\)\)\n'
                      r'            tk::runtime_vehicle_progress_publish\([^;]+;', vehicle)
if len(progress) != 1 or vehicle.count('runtime_vehicle_progress_publish(') != 1:
    raise AssertionError('missing/duplicate actual successful-iteration publication site')
start = vehicle.index('        self->process_ble_host_events_();')
end = vehicle.index('        // While NOT paired', start)
caller = vehicle[start:end]
if caller.count('ble_fault_.exchange(false)') != 2:
    raise AssertionError('actual caller must expose both separate fault-consumption sites')
pending = caller.index('self->process_pending_telemetry_();')
capture = caller.index('const bool parse_fault = self->ble_fault_.exchange(false);')
if pending >= capture or caller.count('const bool parse_fault') != 1:
    raise AssertionError('successful progress must bind the later actual telemetry parse fault')
# Keep the complete event/runner-catch/late-parse flow and actual publisher. The single-pass loop
# preserves the production early-continue behavior. Only unrelated later polling is absent.
progress_site = ('void actual_progress_caller(ProgressSelf* self) {\n'
                 'for (int once = 0; once != 1; ++once) {\n'
                 + caller + progress[0] + '\n}\n}')

# Compile the complete real bootstrap sequence, including reason consumption, actual reset
# classification, shared admission publication and essential task creation, in firmware order.
boot_start = main.index('    const tk::RebootReasonRecord boot_reboot_record = VehicleController::take_reboot_reason(config_store);')
boot_end = main.index('    // UDP Syslog forwarder', boot_start)
bootstrap = 'void actual_recovery_bootstrap(NvsStorageAdapter& config_store) {\n' + main[boot_start:boot_end] + '\n}'

# Execute the complete actual init activity-seed if/else, preserving the production member
# store and predicate. The rest of init (library, key, task creation) is deliberately absent.
init_source = (root / 'main/vehicle_ctrl.cpp').read_text()
init_body = body_in(init_source, 'init')
seed_anchor = '// Seed only a known clean boot with verified absent reboot evidence.'
if init_body.count(seed_anchor) != 1:
    raise AssertionError('ambiguous activity seed anchor inside actual Vehicle init')
# Locate the complete conditional by its reviewed site, not the guard's implementation.
# Omitting that guard/store in an isolated mutant must still compile and fail its numeric oracle.
seed_start = init_body.index('    if (', init_body.index(seed_anchor))
seed_open = init_body.index('{', seed_start)
seed_end = balanced(scrub(init_body), seed_open, '{', '}', 'actual activity seed') + 1
if init_body[seed_end:seed_end + 7] != ' else {':
    raise AssertionError('actual activity seed must retain its complete else branch')
seed_end = balanced(scrub(init_body), seed_end + 6, '{', '}', 'actual activity refusal') + 1
activity_seed = ('void ActivitySeedFixture::run() {\n' + init_body[seed_start:seed_end] + '\n}')

Path(sys.argv[1]).write_text('\n\n'.join([
    query, owners, complete_body('runtime_admission.cpp'),
    complete_body('heap_supervisor.cpp'), fatal, vin, wifi, progress_site, bootstrap, activity_seed]) + '\n')
