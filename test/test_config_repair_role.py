#!/usr/bin/env python3
"""Synthetic identity-crossing and actual-source omission controls; no SDK or device."""
from pathlib import Path
import copy,hashlib,importlib.util,json,os,re,struct,subprocess,sys,tempfile,unittest,shutil
ROOT=Path(__file__).resolve().parents[1]
def load(name,path):
    s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);sys.modules[name]=m;s.loader.exec_module(m);return m
C=load('repair_source_controls',ROOT/'scripts/check-config-repair-contract.py')
A=load('repair_artifact_controls',ROOT/'scripts/check-config-repair-artifacts.py')
S=load('repair_stack_controls',ROOT/'scripts/check-config-repair-stack.py')
class Role(unittest.TestCase):
 def test_closed_source_omissions(self):
    C.source_contract(ROOT)
    cases=[('main/main.cpp','tk::repair::console_entry();','/* omitted */'),
      ('main/Kconfig.projbuild','default n','default y'),
      ('main/CMakeLists.txt',C.C.SOURCES_BLOCK,'# omitted enabled-only source block\n'),
      ('scripts/build-config-repair.py','check-config-repair-stack.py','check-ordinary-stack.py'),
      ('scripts/build-config-repair.py','maintenance/config-repair/firmware-stack-baseline.json','scripts/firmware-stack-baseline.json'),
      ('main/config_repair_console.cpp','usb_serial_jtag_wait_tx_done(0)','usb_serial_jtag_wait_tx_done(portMAX_DELAY)'),
      ('main/config_repair_console.cpp','uart_ll_write_txfifo','uart_write_bytes'),
      ('maintenance/config-repair/sdkconfig.defaults','CONFIG_ESP_CONSOLE_SECONDARY_NONE=y','CONFIG_ESP_CONSOLE_SECONDARY_NONE=n'),
      ('main/config_repair_console.cpp','#if (defined(CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG)','#if (false && defined(CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG)'),
      ('main/config_repair_console.cpp','if(expired(until))return false;','/* omitted deadline */'),
      ('scripts/build-config-repair.py','--enforce-budget','--disabled-budget'),
      ('scripts/build-config-repair.py',"'esp32','esp32s3','esp32c3','esp32c6'","'esp32','esp32s3','esp32c6'")]
    with tempfile.TemporaryDirectory() as d:
      dest=Path(d)
      for n in ('main','scripts','maintenance'):shutil.copytree(ROOT/n,dest/n)
      shutil.copyfile(ROOT/'CMakeLists.txt',dest/'CMakeLists.txt')
      for name,old,new in cases:
        p=dest/name;original=p.read_text();self.assertIn(old,original)
        p.write_text(original.replace(old,new,1))
        with self.assertRaises(ValueError):C.source_contract(dest)
        p.write_text(original);C.source_contract(dest)
 def test_actual_wrapper_disables_cache_for_maintenance(self):
    with tempfile.TemporaryDirectory(prefix='cfg-role-wrapper-') as temporary:
      directory=Path(temporary);scripts=directory/'scripts';scripts.mkdir()
      for name in ('idf-docker.sh','idf-version.sh','check-idf-container-mounts.py'):
        shutil.copy2(ROOT/'scripts'/name,scripts/name)
      shutil.copy2(ROOT/'esp-idf-toolchain.txt',directory/'esp-idf-toolchain.txt')
      tools=directory/'tools';tools.mkdir();trace=directory/'docker.json'
      docker=tools/'docker'
      docker.write_text('#!'+sys.executable+'\nimport json,os,sys\nfrom pathlib import Path\nif sys.argv[1:2]==["run"]: Path(os.environ["REPAIR_DOCKER_TRACE"]).write_text(json.dumps(sys.argv[1:])); raise SystemExit(0)\nraise SystemExit(1)\n')
      docker.chmod(0o700)
      env={'PATH':str(tools)+os.pathsep+os.environ.get('PATH',os.defpath),'LANG':'C','LC_ALL':'C',
           'REPAIR_DOCKER_TRACE':str(trace),'PYTHONDONTWRITEBYTECODE':'1'}
      for script in ('scripts/build-config-repair.py','./scripts/build-config-repair.py','ordinary-command'):
        result=subprocess.run(['/bin/bash',str(scripts/'idf-docker.sh'),'python3',script,'--source-sha','1'*40],cwd=directory,env=env,capture_output=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stderr.decode(errors='replace'))
        args=json.loads(trace.read_text());self.assertIn('--cpus',args);self.assertEqual(args[args.index('--cpus')+1],'1.5')
        self.assertIn('--memory',args);self.assertEqual(args[args.index('--memory')+1],'1800m')
        cache=[arg for arg in args if arg.startswith('CCACHE_') or arg.startswith('IDF_CCACHE_ENABLE=')]
        self.assertEqual(bool(cache),script=='ordinary-command')
        self.assertEqual(args[-4:],['python3',script,'--source-sha','1'*40])

 def test_real_image_role_crossings(self):
    version='cfgrepair-'+'1'*16
    with tempfile.TemporaryDirectory() as d:
      for t in A.TARGETS:
        path=Path(d)/'app.bin'
        raw=bytearray(A.FW.make_image(t,version,app=True))
        raw[32+48:32+80]=b'tesla-key-config-repair'.ljust(32,b'\0')
        length=struct.unpack_from('<I',raw,28)[0];check=A.FW.CHECKSUM_SEED
        for byte in raw[32:32+length]:check^=byte
        raw[-33]=check;raw[-32:]=hashlib.sha256(raw[:-32]).digest();path.write_bytes(raw)
        A.validate_app(path,t,version)
        with self.assertRaises(A.FW.ArtifactError):A.FW.validate_app(t,version,path,signed_app=False)
        for other in A.TARGETS:
          if other!=t:
            with self.assertRaises(ValueError):A.validate_app(path,other,version)
        with self.assertRaises(ValueError):A.validate_app(path,t,'1.2.3')
        path.write_bytes(A.FW.make_image(t,version,app=True))
        with self.assertRaises(ValueError):A.validate_app(path,t,version)
 def test_generated_secondary_console_exclusion(self):
    with tempfile.TemporaryDirectory(prefix='cfg-secondary-') as temporary:
      cfg=Path(temporary)/'sdkconfig'
      for target in C.C.TARGETS:
        values,_=C.CFG.expected_values(ROOT/'sdkconfig.defaults',target)
        values=dict(values)
        values.update(CONFIG_TESLA_CONFIG_REPAIR_USB='y',CONFIG_ESP_CONSOLE_NONE='y',
                      CONFIG_ESP_CONSOLE_UART_DEFAULT='n',CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG='n',
                      CONFIG_LOG_DEFAULT_LEVEL_INFO='n',CONFIG_LOG_DEFAULT_LEVEL_NONE='y',
                      CONFIG_LOG_DEFAULT_LEVEL='0',CONFIG_LOG_MAXIMUM_LEVEL='0',
                      CONFIG_LOG_MAXIMUM_EQUALS_DEFAULT='y',CONFIG_ESP_CONSOLE_SECONDARY_NONE='y',
                      CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG='n',CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED='n')
        values['CONFIG_IDF_TARGET_'+target.upper()]='y'
        if target!='esp32':values['CONFIG_USJ_ENABLE_USB_SERIAL_JTAG']='y'
        def write(data):cfg.write_text(''.join(k+'='+v+'\n' for k,v in sorted(data.items())))
        write(values);C.generated(target,cfg)
        for key in ('CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG','CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED'):
          wrong={**values,key:'y'};write(wrong)
          with self.assertRaisesRegex(ValueError,'SDK secondary stdio'):C.generated(target,cfg)
        if target!='esp32':
          write({**values,'CONFIG_ESP_CONSOLE_SECONDARY_NONE':'n'})
          with self.assertRaisesRegex(ValueError,'secondary console is not NONE'):C.generated(target,cfg)
        write(values);C.generated(target,cfg)

 def test_effective_role_crossings(self):
    with tempfile.TemporaryDirectory() as d:
      d=Path(d);cfg=d/'sdkconfig';cache=d/'CMakeCache.txt'
      for role,project,on in [('application','tesla-key-esp32',False),('config-repair','tesla-key-config-repair',True)]:
        cfg.write_text('CONFIG_TESLA_CONFIG_REPAIR_USB=y\n' if on else '# CONFIG_TESLA_CONFIG_REPAIR_USB is not set\n')
        cache.write_text('TESLA_BUILD_ROLE:STRING='+role+'\nCMAKE_PROJECT_NAME:STATIC='+project+'\n')
        self.assertEqual(C.C.configured_project(role,cfg,cache),project)
        for wrong in ['application','config-repair','unknown']:
          if wrong!=role:
            with self.assertRaises(ValueError):C.C.configured_project(wrong,cfg,cache)
        cache.write_text('TESLA_BUILD_ROLE:STRING='+role+'\nCMAKE_PROJECT_NAME:STATIC=foreign\n')
        with self.assertRaises(ValueError):C.C.configured_project(role,cfg,cache)

 def test_actual_cmake_normal_and_enabled_graph(self):
    cmake=shutil.which('cmake');self.assertIsNotNone(cmake,'actual CMake interpreter required')
    text=(ROOT/'main/CMakeLists.txt').read_text()
    normal=C.C.normal_component_text(ROOT,text)
    block=re.search(r'(?s)idf_component_register\(\s*SRCS\s*(.*?)\n\s*INCLUDE_DIRS',normal)
    self.assertIsNotNone(block)
    expected=re.findall(r'"([^"\n]+\.(?:c|cpp|cc|cxx))"',block[1])
    self.assertFalse(set(C.C.MAINTENANCE_SOURCES)&set(expected))
    # Execute the actual full component CMake with owned command doubles. No SDK, generator,
    # compiler or target is run; only the role-dependent source registrations are observed.
    prefix='cmake_minimum_required(VERSION 3.16)\nfunction(idf_component_register)\n    cmake_parse_arguments(OWNER "" "" "SRCS;INCLUDE_DIRS;EMBED_FILES;REQUIRES" ${ARGN})\n    set(COMPONENT_LIB owned_fake_main PARENT_SCOPE)\n    file(WRITE "${TRACE}" "base=${OWNER_SRCS}\\n")\nendfunction()\nfunction(target_sources owner visibility)\n    if(NOT owner STREQUAL "owned_fake_main" OR NOT visibility STREQUAL "PRIVATE")\n        message(FATAL_ERROR "unowned source registration")\n    endif()\n    file(APPEND "${TRACE}" "extra=${ARGN}\\n")\nendfunction()\nfunction(target_compile_options)\nendfunction()\nfunction(add_custom_command)\nendfunction()\nfunction(add_custom_target)\nendfunction()\nfunction(add_dependencies)\nendfunction()\n'
    with tempfile.TemporaryDirectory(prefix='cfg-role-cmake-') as temporary:
      directory=Path(temporary);script=directory/'owned.cmake';trace=directory/'trace'
      script.write_text(prefix+text)
      for target in C.C.TARGETS:
       for enabled in (False,True):
        env={k:v for k,v in os.environ.items() if k not in ('ENV','BASH_ENV','OTA_SIGNING_KEY','OTA_SIGNING_KEY_FILE') and not k.startswith(('CMAKE_','BASH_FUNC_','LD_','DYLD_'))}
        target_flags=['-DCONFIG_IDF_TARGET_'+name.upper()+'='+('ON' if target==name else 'OFF') for name in C.C.TARGETS]
        result=subprocess.run([cmake,'-DTRACE='+str(trace),'-DCONFIG_TESLA_CONFIG_REPAIR_USB='+('ON' if enabled else 'OFF'),*target_flags,'-P',str(script)],cwd=directory,env=env,capture_output=True,timeout=10)
        self.assertEqual(result.returncode,0)
        lines=trace.read_text().splitlines()
        self.assertEqual(lines[0],'base='+';'.join(expected))
        self.assertEqual(lines[1:],['extra='+';'.join(C.C.MAINTENANCE_SOURCES)] if enabled else [])

 def test_separate_actual_stack_role_admission(self):
    units=S.expected_units(ROOT)
    for target in C.C.TARGETS:
      with self.subTest(target=target),tempfile.TemporaryDirectory(prefix='cfg-role-stack-') as temporary:
        directory=Path(temporary);observed=directory/'observed.json';baseline=directory/'reviewed.json'
        # Synthetic public sidecars all contain real records; an empty reviewed-large-frame map
        # is not an empty observed TU. These invented fixtures are never shipped maxima.
        frames={name:{'synthetic :: fixture()':S.STACK.Frame(16,'synthetic','fixture()','static')} for name in units}
        large='CMakeFiles/__idf_main.dir/config_repair.cpp.su'
        frames[large]['synthetic :: maintenance_large()']=S.STACK.Frame(320,'synthetic','maintenance_large()','static')
        document=S.STACK.baseline_document({name:frames for name in C.C.TARGETS})
        document.update(role='config-repair',project='tesla-key-config-repair',qualification='reviewed')
        observed.write_text(json.dumps(S.STACK.observed_document(target,frames)))
        baseline.write_text(json.dumps(document))
        self.assertEqual(S.validate(ROOT,target,observed,baseline,'config-repair'),len(units))
        with self.assertRaises(ValueError):S.validate(ROOT,target,observed,baseline,'application')
        with self.assertRaises(ValueError):S.STACK.load_baseline(baseline,target)
        with self.assertRaises(ValueError):S.reviewed_baseline(ROOT/'scripts/firmware-stack-baseline.json',target,units)
        pending=ROOT/'maintenance/config-repair/firmware-stack-baseline.json'
        pending_doc=json.loads(pending.read_text())
        if pending_doc['qualification']=='unreviewed':
          self.assertEqual(pending_doc['targets'],{name:None for name in C.C.TARGETS})
          with self.assertRaises(ValueError):S.validate(ROOT,target,observed,pending,'config-repair')
        for label,altered in (
          ('missing-owner',{n:r for n,r in frames.items() if n!=large}),
          ('unexpected-owner',{**frames,'CMakeFiles/__idf_main.dir/foreign.cpp.su':frames[large]}),
          ('new-large-frame',{**frames,large:{**frames[large],'synthetic :: unexpected_large()':S.STACK.Frame(256,'synthetic','unexpected_large()','static')}}),
          ('absolute-cap',{**frames,large:{**frames[large],'synthetic :: too_large()':S.STACK.Frame(4097,'synthetic','too_large()','static')}}),
          ('empty-observed-owner',{**frames,large:{}}),
        ):
          with self.subTest(control=label):
            observed.write_text(json.dumps(S.STACK.observed_document(target,altered)))
            with self.assertRaises(ValueError):S.validate(ROOT,target,observed,baseline,'config-repair')
        observed.write_text(json.dumps(S.STACK.observed_document(target,frames)))
        for label,field,value in (('wrong-role','role','application'),('wrong-project','project','tesla-key-esp32'),('raised-cap','absoluteMaxBytes',4097),('changed-threshold','reviewThresholdBytes',255),('wrong-toolchain','toolchain','ESP-IDF v6.0')):
          with self.subTest(control=label):
            altered=copy.deepcopy(document);altered[field]=value;baseline.write_text(json.dumps(altered))
            with self.assertRaises(ValueError):S.validate(ROOT,target,observed,baseline,'config-repair')
        altered=copy.deepcopy(document);del altered['targets'][C.C.TARGETS[-1]];baseline.write_text(json.dumps(altered))
        with self.assertRaises(ValueError):S.validate(ROOT,target,observed,baseline,'config-repair')
        baseline.write_text(json.dumps(document))
        self.assertEqual(S.validate(ROOT,target,observed,baseline,'config-repair'),len(units)) # effective restore

if __name__=='__main__':unittest.main()
