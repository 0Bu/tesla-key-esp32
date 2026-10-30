#!/usr/bin/env python3
"""Exact F135 owner/hook source contract. Unexecuted author draft; not runtime HWM proof.

Run only after applying Root's core, these .cpp postimages and both exact delta patches to
an owned complete source tree: python3 test/test_stack_context_hooks.py ROOT [--self-test].
The runtime-boundary contract remains a separate mandatory gate. Its mapped noexcept sampling
prefixes and typed ping delegation are checked transitively, not accepted by name or checksum.
"""
from pathlib import Path
import copy
import json
import re
import runpy
import sys

TASKS = {
    'publisher_task': ('main/mqtt_ha.cpp', 'Mqtt', 'while (true) {'),
    'ota_health_gate_task': ('main/main.cpp', 'OtaGate', 'for (;;) {'),
    'heap_supervisor_task': ('main/heap_supervisor.cpp', 'HeapSupervisor', 'for (;;) {'),
    'ota_check_task': ('main/ota_update.cpp', 'OtaCheck', None),
    'ota_task': ('main/ota_update.cpp', 'OtaWriter', None),
    'syslog_task': ('main/syslog.cpp', 'Syslog', 'while (true) {'),
    'dns_task': ('main/provisioning.cpp', 'CaptiveDns', 'while (!s_dns_stop.load()) {'),
    'loop_task_fn_': ('main/vehicle_telemetry.cpp', 'Vehicle', 'while (true) {'),
    'auto_pair_task_fn_': ('main/vehicle_pairing.cpp', 'AutoPair', 'while (true) {'),
    'display_task': ('main/display.cpp', 'Display', 'for (;;) {'),
    'net_watchdog_task': ('main/net.cpp', 'NetWatchdog', 'for (;;) {'),
    'led_task': ('main/led_status.cpp', 'Led', 'for (int frame = 0;; ++frame) {'),
    'nimble_host_task': ('main/ble_client.cpp', 'NimbleHost', None),
    'ota_network_task': ('main/ota_transport.cpp', 'OtaNetwork', None),
}
CALLBACKS = {
    'ota_check_after_http_work': ('main/ota_update.cpp', 'Httpd'),
    'on_sync_cb': ('main/ble_client.cpp', 'NimbleHost'),
    'on_reset_cb': ('main/ble_client.cpp', 'NimbleHost'),
    'gap_event_cb': ('main/ble_client.cpp', 'NimbleHost'),
    'svc_disc_cb': ('main/ble_client.cpp', 'NimbleHost'),
    'chr_disc_cb': ('main/ble_client.cpp', 'NimbleHost'),
    'dsc_disc_cb': ('main/ble_client.cpp', 'NimbleHost'),
    'subscribe_write_cb': ('main/ble_client.cpp', 'NimbleHost'),
    'scan_timeout_cb': ('main/ble_client.cpp', 'BleScanTimer'),
    'form_get': ('main/provisioning.cpp', 'CaptiveHttp'),
    'save_post': ('main/provisioning.cpp', 'CaptiveHttp'),
    'wifi_event_handler': ('main/net.cpp', 'WifiEvent'),
    'eth_event_handler': ('main/net.cpp', 'EthEvent'),
    'mqtt_event_handler': ('main/mqtt_ha.cpp', 'MqttEvent'),
    'on_mqtt_probe': ('main/http_config.cpp', 'MqttProbeEvent'),
    'sntp_sync_time': ('main/main.cpp', 'SntpSync'),
    'handle_all': ('main/http_server.cpp', 'Httpd'),
    'app_main': ('main/main.cpp', 'Main'),
    'ping_net_on_end': ('main/net.cpp', 'PingNet'),
    'ping_syslog_on_end': ('main/syslog.cpp', 'PingSyslog'),
    'parser_begin': ('main/ota_http.cpp', 'OtaParser'),
    'parser_field': ('main/ota_http.cpp', 'OtaParser'),
    'parser_value': ('main/ota_http.cpp', 'OtaParser'),
    'parser_headers': ('main/ota_http.cpp', 'OtaParser'),
    'parser_body': ('main/ota_http.cpp', 'OtaParser'),
    'parser_complete': ('main/ota_http.cpp', 'OtaParser'),
    'parser_chunk': ('main/ota_http.cpp', 'OtaParser'),
    'parser_chunk_done': ('main/ota_http.cpp', 'OtaParser'),

}
NESTED = {'ble_link_event_cb_', 'ble_rx_event_cb_'}
CONTEXTS = ['Httpd', 'Vehicle', 'AutoPair', 'Mqtt', 'Main', 'HeapSupervisor', 'Display',
            'Led', 'OtaCheck', 'OtaWriter', 'OtaGate', 'NetWatchdog', 'Syslog',
            'CaptiveDns', 'CaptiveHttp', 'WifiEvent', 'EthEvent', 'MqttEvent',
            'MqttProbeEvent', 'NimbleHost', 'BleScanTimer', 'DiagLogCallback',
            'SntpSync', 'PingNet', 'PingSyslog', 'OtaNetwork', 'OtaParser']


def require(ok, label):
    if not ok:
        raise AssertionError(label)


def contract(parts, owners, helpers):
    body_in, scrub = helpers['function_body_in'], helpers['scrub_cpp_preserving_layout']
    helpers['require_f135_sampling_programs'](parts)
    # Discover registrations with the existing independent source inventory, not name suffixes.
    task_names = set()
    callback_names = set()
    for path, text in parts.items():
        if path.endswith('.cpp'):
            task_names.update(helpers['task_inventory'](text))
            callback_names.update(helpers['callback_inventory'](text))
    require(task_names == set(TASKS), 'exact project task inventory/mapping')
    ping = parts['main/ping_probe.hpp']
    # Resolve only this reviewed typed common completion delegation, with two fixed consumers.
    require('void (*on_end)(esp_ping_handle_t, void*) = ping_probe_on_end' in ping,
            'typed ping callback compatibility/default')
    require(ping.count('callbacks.on_ping_end = on_end;') == 1, 'single shared ping delegation')
    require('if (!control.done || !on_end) return PingProbeResult::Unavailable;' in ping,
            'null ping callback is refused before any session effect')
    analyzed_ping = ping.replace('callbacks.on_ping_end = on_end;',
                                'callbacks.on_ping_end = ping_probe_on_end;', 1)
    callback_names.update(helpers['callback_inventory'](analyzed_ping))
    require('ping_probe_on_end' in callback_names, 'common completion is discoverable')
    callback_names.remove('ping_probe_on_end')
    all_ping_calls = {path: helpers['call_arguments'](scrub(text), 'ping_probe_run')
                      for path, text in parts.items() if path.endswith('.cpp')}
    require({path: len(calls) for path, calls in all_ping_calls.items() if calls} ==
            {'main/net.cpp': 1, 'main/syslog.cpp': 1},
            'exact two fixed project ping consumers; no unassigned default caller')
    for path, wrapper in [('main/net.cpp', 'ping_net_on_end'),
                          ('main/syslog.cpp', 'ping_syslog_on_end')]:
        calls = helpers['call_arguments'](scrub(parts[path]), 'ping_probe_run')
        require(len(calls) == 1 and len(calls[0]) == 5 and calls[0][-1].strip() == wrapper,
                'fixed ping role registration: ' + path)
        callback_names.add(wrapper)
    require(callback_names == set(CALLBACKS) | NESTED | {'diag_vprintf_'},
            'exact callback owners; new or omitted registration requires review')
    expected = {(entry, data[0], data[1]) for entry, data in TASKS.items()} | {
        (entry, data[0], data[1]) for entry, data in CALLBACKS.items()} | {
        ('diag_vprintf_', 'main/diag_log.cpp', 'DiagLogCallback'),
        ('ble_link_event_cb_', 'main/vehicle_ctrl.cpp', 'NimbleHost'),
        ('ble_rx_event_cb_', 'main/vehicle_ctrl.cpp', 'NimbleHost')}
    actual = {(r['entry'], r['path'], r['context']) for r in owners['rows']}
    require(actual == expected and len(owners['rows']) == len(expected) == 45,
            'exact 45 owner rows, no duplicate/missing route')
    require({r['context'] for r in owners['rows']} == set(CONTEXTS), 'all 27 lanes mapped')
    enum = re.search(r'enum class StackContext[^\{]*\{(.*?)\};',
                     parts['main/logic/stack_context.hpp'], re.S)
    require(enum is not None, 'actual core enum')
    require([x.strip() for x in enum[1].split(',') if x.strip()] == CONTEXTS + ['Count'],
            'exact enum order/aliases and capacity')
    for entry, (path, context, loop) in TASKS.items():
        body = scrub(body_in(parts[path], entry))
        prefix = ('{ tk::stack_watch_sample(tk::StackWatch::' + context + '); '
                  'tk::StackWatchScope f135_task_exit(tk::StackWatch::' + context + ');')
        require(re.sub(r'\s+', '', body).startswith(re.sub(r'\s+', '', prefix)),
                'task entry/boundary sampling first: ' + entry)
        if loop:
            needle = loop + ' tk::StackWatchScope f135_iteration_exit(tk::StackWatch::' + context + ');'
            require(re.sub(r'\s+', '', needle) in re.sub(r'\s+', '', body),
                    'actual outer iteration exit sampling: ' + entry)
        for match in re.finditer(r'vTaskDelete\s*\(\s*nullptr\s*\)\s*;', body):
            before = body[:match.start()].rstrip()
            require(before.endswith('tk::stack_watch_sample(tk::StackWatch::' + context + ');'),
                    'sample before no-unwind task delete: ' + entry)
    for entry, (path, context) in CALLBACKS.items():
        body = scrub(body_in(parts[path], entry))
        prefix = '{ tk::StackWatchScope f135_callback_exit(tk::StackWatch::' + context + ');'
        require(re.sub(r'\s+', '', body).startswith(re.sub(r'\s+', '', prefix)),
                'callback scope first/before local locks: ' + entry)
    for entry in NESTED:
        body = scrub(body_in(parts['main/vehicle_ctrl.cpp'], entry))
        require('stack_watch' not in body and 'StackWatchScope' not in body,
                'inner enqueue callback stays fixed POD only: ' + entry)
    for path, context in [('main/main.cpp', 'Main'), ('main/heap_supervisor.cpp', 'HeapSupervisor'),
                          ('main/ota_update.cpp', 'OtaWriter'), ('main/provisioning.cpp', 'CaptiveHttp'),
                          ('main/http_common.cpp', 'Httpd')]:
        code = scrub(parts[path])
        for match in re.finditer(r'(?m)^\s*(?:(?:const\s+)?esp_err_t\s+\w+\s*=\s*)?(?:esp_restart|esp_ota_mark_app_invalid_rollback_and_reboot)\([^\n;]*\);', code):
            before = code[:match.start()].rstrip()
            require(before.endswith('tk::stack_watch_sample(tk::StackWatch::' + context + ');'),
                    'sample immediately before restart effect: ' + path)
    nimble = scrub(body_in(parts['main/ble_client.cpp'], 'nimble_host_task'))
    require(re.search(r'stack_watch_sample\(tk::StackWatch::NimbleHost\);\s*nimble_port_freertos_deinit\(\)', nimble),
            'sample before SDK host self-deletion')
    log = scrub(body_in(parts['main/diag_log.cpp'], 'diag_stack_sample_safe_'))
    tokens = ['if (xPortInIsrContext() || !xPortCanYield()) return;',
              'if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING ||',
              'xTaskGetCurrentTaskHandle() == nullptr) return;',
              'tk::stack_watch_sample(tk::StackWatch::DiagLogCallback);']
    indexes = [log.find(t) for t in tokens]
    require(all(x >= 0 for x in indexes) and indexes == sorted(indexes), 'safe logger predicates precede HWM')
    require(not re.search(r'ESP_LOG|malloc|new\s|thread_local|xSemaphore|portENTER', log),
            'logger sampler does not recurse, allocate, wait or add state')
    hook = scrub(body_in(parts['main/diag_log.cpp'], 'diag_vprintf_'))
    require(re.search(r'^\{\s*struct F135SafeTaskSampleOnExit', hook), 'logger exit guard before local work')
    require('~F135SafeTaskSampleOnExit() noexcept { diag_stack_sample_safe_(); }' in hook,
            'safe logger rechecks current context at actual exit')


def main(root, self_test):
    # Loading definitions performs reads only; the existing gate's __main__ is not invoked here.
    helpers = runpy.run_path(str(root / 'test/test_runtime_boundary_contract.py'))
    paths = ['main/' + name for name in helpers['SOURCES']]
    paths += ['main/ping_probe.hpp', 'main/logic/stack_context.hpp',
              'main/stack_watch.hpp', 'main/logic/stack_sample_codec.hpp']
    parts = {path: (root / path).read_text(encoding='utf-8') for path in paths}
    owners = json.loads((root / 'test/stack_context_hook_owners.json').read_text())
    contract(parts, owners, helpers)
    if self_test:
        # Source/inventory controls only, never called runtime HWM measurements.
        mutations = []
        for entry, (path, context) in CALLBACKS.items():
            old = 'tk::StackWatchScope f135_callback_exit(tk::StackWatch::' + context + ');'
            body = helpers['function_body_in'](parts[path], entry)
            for label, replacement in [('omit', ''), ('comment', '/* ' + old + ' */'),
                                       ('false', 'if (false) { ' + old + ' }')]:
                changed = body.replace(old, replacement, 1)
                mutations.append((entry + ':' + label, {**parts, path: parts[path].replace(body, changed, 1)}, owners))
        for entry, (path, context, loop) in TASKS.items():
            body = helpers['function_body_in'](parts[path], entry)
            entry_sample = 'tk::stack_watch_sample(tk::StackWatch::' + context + ');'
            entry_scope = 'tk::StackWatchScope f135_task_exit(tk::StackWatch::' + context + ');'
            for label, old, replacement in (
                ('entry-sample-omitted', entry_sample, ''),
                ('entry-scope-omitted', entry_scope, ''),
                ('entry-wrong-lane', entry_sample,
                 'tk::stack_watch_sample(tk::StackWatch::Count);'),
                ('entry-extra-work', entry_sample, 'throwing_helper(); ' + entry_sample),
            ):
                require(body.count(old) >= 1, 'mapped task canary source: ' + entry)
                changed = body.replace(old, replacement, 1)
                mutations.append((entry + ':' + label,
                                  {**parts, path: parts[path].replace(body, changed, 1)}, owners))
            if not loop:
                continue
            old = 'tk::StackWatchScope f135_iteration_exit(tk::StackWatch::' + context + ');'
            body = helpers['function_body_in'](parts[path], entry)
            changed = body.replace(old, '', 1)
            mutations.append((entry + ':iteration-omitted', {**parts, path: parts[path].replace(body, changed, 1)}, owners))
        extra_ping = parts['main/net.cpp'] + (
            '\nvoid f135_unassigned_ping_probe(tk::PingProbeControl& control, '
            'const esp_ping_config_t& config) {\n'
            '    (void)tk::ping_probe_run(control, config, 1, 1);\n}\n')
        mutations.append(('extra-default-ping-consumer',
                          {**parts, 'main/net.cpp': extra_ping}, owners))
        missing = copy.deepcopy(owners); missing['rows'].pop()
        mutations.append(('owner-row-omitted', parts, missing))
        log_path = 'main/diag_log.cpp'
        for old in ['xPortInIsrContext()', '!xPortCanYield()',
                    'xTaskGetSchedulerState() != taskSCHEDULER_RUNNING',
                    'xTaskGetCurrentTaskHandle() == nullptr']:
            mutations.append(('logger-predicate:' + old, {**parts, log_path: parts[log_path].replace(old, 'false', 1)}, owners))
        for label, altered, mapping in mutations:
            try:
                contract(altered, mapping, helpers)
            except AssertionError:
                continue
            raise AssertionError('valid source omission passed: ' + label)
    print('stack context source contract: exact27 lanes/45 owner rows; source-only PASS')


if __name__ == '__main__':
    if len(sys.argv) not in [2, 3] or (len(sys.argv) == 3 and sys.argv[2] != '--self-test'):
        raise SystemExit('usage: test_stack_context_hooks.py ROOT [--self-test]')
    main(Path(sys.argv[1]).resolve(strict=True), len(sys.argv) == 3)
