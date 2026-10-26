#!/usr/bin/env python3
"""Fail-closed default-off maintenance entry, source ownership, role and config contract."""
from __future__ import annotations
import argparse,importlib.util,re,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]

def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    if spec is None or spec.loader is None:raise ValueError('missing maintenance contract input')
    m=importlib.util.module_from_spec(spec);sys.modules[name]=m;spec.loader.exec_module(m);return m
C=load('repair_closed_source_contract',ROOT/'scripts/config_repair_contract.py')
CFG=load('repair_defaults_reader',ROOT/'scripts/check-sdkconfig-defaults.py')

def require(value,message):
    if not value:raise ValueError(message)

def source_contract(root):
    text=(root/'main/CMakeLists.txt').read_text()
    require(text.startswith(C.REQUIREMENTS_PREFIX),'missing closed maintenance dependency prelude')
    normal=C.normal_component_text(root,text)
    require(C.SOURCES_BLOCK in text and all(('\"'+name+'\"') not in normal for name in C.MAINTENANCE_SOURCES),'maintenance source graph is not enabled-only')
    for filename in ('config_repair.cpp','config_repair_console.cpp'):
        require(text.count('"'+filename+'"')==1,'maintenance TU inventory')
        body=(root/'main'/filename).read_text()
        require(body.startswith('#include "sdkconfig.h"\n#if defined(CONFIG_TESLA_CONFIG_REPAIR_USB) && CONFIG_TESLA_CONFIG_REPAIR_USB\n'), 'default-off TU boundary')
        require(body.rstrip().endswith('#endif'),'unterminated default-off TU')
    main=(root/'main/main.cpp').read_text()
    entry='''  try {
#if defined(CONFIG_TESLA_CONFIG_REPAIR_USB) && CONFIG_TESLA_CONFIG_REPAIR_USB
    tk::repair::console_entry(); // terminal; no normal startup or writer coexists
#else
    // Capture console output into the in-memory diagnostic ring (GET /diag).'''
    require(main.count(entry)==1 and main.count('tk::repair::console_entry();')==1,'maintenance is not the unique first app_main entry')
    require(main.count('#endif // CONFIG_TESLA_CONFIG_REPAIR_USB')==1,'missing ordinary startup branch')
    require(main.index(entry)<main.index('diag_log_init()'),'normal writer starts before maintenance')
    config=(root/'main/Kconfig.projbuild').read_text()
    require(re.search(r'config TESLA_CONFIG_REPAIR_USB\s+bool [^\n]+\s+default n\s',config),'maintenance must default off')
    console=(root/'main/config_repair_console.cpp').read_text()
    for item in ('CONFIG_ESP_CONSOLE_NONE','CONFIG_LOG_MAXIMUM_LEVEL','uart_ll_get_txfifo_len','uart_ll_write_txfifo','uart_ll_is_tx_idle','usb_serial_jtag_wait_tx_done(0)','usb_serial_jtag_read_bytes(dst+done,chunk,0)','uart_read_bytes(UART_NUM_0,dst+done,chunk,0)'):
        require(item in console,'missing raw bounded console contract: '+item)
    secondary_guard = '#if (defined(CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG) && CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG) || \\\n    (defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED) && CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED)\n#error "Maintenance raw USB owner excludes SDK primary and secondary USB stdio"\n#endif\n'
    native_guard = '#if !defined(CONFIG_ESP_CONSOLE_SECONDARY_NONE) || !CONFIG_ESP_CONSOLE_SECONDARY_NONE\n#error "Maintenance native USB requires no secondary SDK console"\n#endif\n'
    uncommented = re.sub(r'/\*.*?\*/|//[^\n]*', '', console, flags=re.S)
    require(uncommented.count(secondary_guard)==1 and uncommented.count(native_guard)==1,'missing fail-closed secondary-console compile guards')
    overlay=CFG.parse_config(root/'maintenance/config-repair/sdkconfig.defaults')
    require(overlay.get('CONFIG_ESP_CONSOLE_SECONDARY_NONE')=='y','maintenance secondary console must default to NONE')
    require(console.count('if(expired(until))return false;')==3,'raw I/O absolute deadline guards')
    code=re.sub(r'//[^\n]*','',console)
    require(not re.search(r'\b(?:uart_tx_chars|uart_write_bytes|fread|fwrite|printf|esp_restart)\s*\(',code),'blocking/shared/restarting maintenance transport')
    for target in C.TARGETS:
        require((root/f'maintenance/config-repair/sdkconfig.defaults.{target}').is_file(),'missing target route')
    build=(root/'scripts/build-config-repair.py').read_text()
    require("TARGETS=('esp32','esp32s3','esp32c3','esp32c6')" in build,'maintenance four-target set')
    for item in ('check-build-semantics.py','check-partition-contract.py','check-otadata-contract.py','report-firmware-size.py','--enforce-budget','check-stack-usage.py','check-config-repair-stack.py','maintenance/config-repair/firmware-stack-baseline.json','independent maintenance app/ELF reproduction mismatch'):
        require(item in build,'maintenance gate missing: '+item)
    require('sign_data' not in build and 'sign-data' not in build,'maintenance unsigned builder cannot sign')

def generated(target,path,application=False):
    actual=CFG.parse_config(path)
    require(actual.get('CONFIG_IDF_TARGET_'+target.upper())=='y','wrong effective target')
    if application:
        require(actual.get('CONFIG_TESLA_CONFIG_REPAIR_USB')!='y','maintenance config cannot enter application')
        return
    expected,_=CFG.expected_values(ROOT/'sdkconfig.defaults',target)
    # Only disclosed maintenance console/log selections differ from the unchanged base defaults.
    overrides={'CONFIG_ESP_CONSOLE_UART_DEFAULT','CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG',
               'CONFIG_ESP_CONSOLE_NONE','CONFIG_ESP_CONSOLE_SECONDARY_NONE',
               'CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG','CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED',
               'CONFIG_LOG_DEFAULT_LEVEL_INFO','CONFIG_LOG_DEFAULT_LEVEL',
               'CONFIG_LOG_MAXIMUM_LEVEL','CONFIG_LOG_DEFAULT_LEVEL_NONE','CONFIG_LOG_MAXIMUM_EQUALS_DEFAULT'}
    for key,value in expected.items():
        if key not in overrides:require(actual.get(key)==value,'maintenance changed normal fixed option: '+key)
    mandatory={'CONFIG_TESLA_CONFIG_REPAIR_USB':'y','CONFIG_ESP_CONSOLE_NONE':'y',
               'CONFIG_LOG_DEFAULT_LEVEL':'0','CONFIG_LOG_MAXIMUM_LEVEL':'0'}
    for key,value in mandatory.items():require(actual.get(key)==value,'missing maintenance console selection: '+key)
    require(actual.get('CONFIG_ESP_CONSOLE_UART_DEFAULT')!='y' and actual.get('CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG')!='y','SDK stdio competes with raw owner')
    require(actual.get('CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG')!='y' and
            actual.get('CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED')!='y','SDK secondary stdio competes with raw owner')
    if target!='esp32':
        require(actual.get('CONFIG_ESP_CONSOLE_SECONDARY_NONE')=='y','native maintenance secondary console is not NONE')
        require(actual.get('CONFIG_USJ_ENABLE_USB_SERIAL_JTAG')=='y','native USB module disabled')

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--target',choices=C.TARGETS);p.add_argument('--sdkconfig',type=Path);p.add_argument('--application-config',action='store_true')
    a=p.parse_args();source_contract(ROOT)
    if a.sdkconfig:
        if not a.target:p.error('effective config requires target')
        generated(a.target,a.sdkconfig,a.application_config)
    elif a.target or a.application_config:p.error('effective config requires sdkconfig')
    print('configuration-only maintenance source/role contract: PASS')
if __name__=='__main__':main()
