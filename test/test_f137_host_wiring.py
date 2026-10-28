#!/usr/bin/env python3
"""UNRUN DRAFT: mandatory F137 actual host consumers; passive flow, no compiler."""
from functools import lru_cache
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
import shutil
import time
import importlib.util

FILES=(
 'test/test_f137_host.py','test/generate_f137_host_adapters.py',
 'test/f137_process.py','test/f137_process_controls.py','test/f137_driver_controls.py','test/f137_task_lsan.hpp',
 'test/f137_mqtt_host_template.cpp','test/f137_net_host_template.cpp',
 'test/f137_stubs/esp_task_wdt.h','test/f137_stubs/ping/ping_sock.h',
 'test/f137-callback-probe-v1.py','test/f137-callback-probe-template-v1.cpp',
 'test/test_f137_host_wiring.py',
)
CALL='python3 test/test_f137_host.py --source . --controls --execute-controlled-host-only'
SAN='python3 test/test_f137_host.py --source . --sanitize --execute-controlled-host-only'


def require(value,message):
 if not value:raise ValueError('F137 host wiring: '+message)


@lru_cache(maxsize=8)
def fast_flow(source, process_source):
 directory=tempfile.mkdtemp(prefix='f137-fast-flow-')
 quiet=True
 try:
  root=Path(directory);(root/'scripts').mkdir();tools=root/'bin';tools.mkdir();trace=root/'trace.jsonl'
  script=root/'scripts/run-fast-tests.sh';script.write_text(source+'\npython3 __f137_after__\n')
  process_module=root/'f137_process.py';process_module.write_text(process_source)
  stub='''#!PYTHON
import json,os,sys
from pathlib import Path
args=sys.argv[1:]
def note(item):
 with open(os.environ['F137_FLOW_TRACE'],'a')as out:out.write(json.dumps(item)+'\\n')
if Path(sys.argv[0]).name=='python3':
 if args==['test/test_f137_host.py','--source','.','--controls','--execute-controlled-host-only']:
  note(['target'])
  if os.environ['F137_FLOW_FAIL']=='1':raise SystemExit(37)
 elif args==['__f137_after__']:note(['after'])
elif '-o'in args:
 p=Path(args[args.index('-o')+1]);p.parent.mkdir(parents=True,exist_ok=True);p.write_text('#!/bin/sh\\nexit 0\\n');p.chmod(0o700)
'''.replace('#!PYTHON','#!'+sys.executable)
  for name in ('python3','compiler'):
   q=tools/name;q.write_text(stub);q.chmod(0o700)
  env={k:v for k,v in os.environ.items()if k not in ('ENV','BASH_ENV','OTA_SIGNING_KEY','OTA_SIGNING_KEY_FILE','GH_TOKEN','GITHUB_TOKEN')and not k.startswith(('BASH_FUNC_','PYTHON','LD_','DYLD_'))}
  env.update(PATH=str(tools)+os.pathsep+os.defpath,CXX=str(tools/'compiler'),CC=str(tools/'compiler'),F137_FLOW_TRACE=str(trace))
  for fail in (False,True):
   trace.unlink(missing_ok=True)
   spec=importlib.util.spec_from_file_location('f137_wiring_owned_process',process_module)
   owner=importlib.util.module_from_spec(spec);spec.loader.exec_module(owner)
   try:
    receipt=owner.run_owned(['/bin/bash',str(script),'--boundary'],cwd=root,env={**env,'F137_FLOW_FAIL':str(int(fail))},output=root,label='fast-flow-'+str(int(fail)),deadline=time.monotonic()+10,stage_seconds=10)
   except BaseException:
    quiet=False
    raise
   quiet=quiet and receipt['processesQuiescent']
   require(quiet and not receipt['error'] and not receipt['timeout'],'owned passive fast-flow cleanup/deadline')
   calls=[json.loads(line)for line in trace.read_text().splitlines()]if trace.exists()else[]
   require(receipt['exit']==(37 if fail else 0)and calls==([['target']]if fail else[['target'],['after']]),'actual fast target must run exactly once before sentinel and propagate37')
 finally:
  if quiet:shutil.rmtree(directory)
  # Unknown cleanup deliberately leaves owned private scratch instead of racing a descendant.


def validate(root):
 root=Path(root);mock=(root/'scripts/run-mock-tests.sh').read_text();fast=(root/'scripts/run-fast-tests.sh').read_text();san=(root/'scripts/run-sanitizer-tests.sh').read_text();cmake=(root/'test/CMakeLists.txt').read_text()
 for name in FILES:
  require((root/name).is_file(), 'required inventory '+name)
  require('        '+name+' \\\n'in mock,'required strict inventory '+name)
 require('run_gate "MQTT/WDT and network iteration bodies" 120 '+CALL in mock,'mandatory mock invocation')
 require('  '+CALL+'\n'in fast,'mandatory fast invocation')
 require(SAN in san,'mandatory sanitizer invocation')
 code=re.sub(r'#\[(=*)\[.*?\]\1\]',lambda m:'\n'*m.group().count('\n'),cmake,flags=re.S);code=re.sub(r'(?m)#.*$','',code)
 needle='add_test(NAME f137_task_host_contract'
 require(code.count(needle)==1,'active CTest admission')
 at=code.index(needle);stack=[]
 for token in re.finditer(r'\b(if|endif|foreach|endforeach|function|endfunction|macro|endmacro|while|endwhile|return)\s*\(([^)]*)\)',code[:at]):
  command,args=token.groups()
  if command.startswith('end'):
   require(bool(stack),'unbalanced CMake prefix');stack.pop()
  elif command=='return':raise ValueError('F137 host wiring: earlier CMake return')
  else:stack.append((command,args.strip()))
 require(stack==[('if','Python3_Interpreter_FOUND')],'CTest hidden behind additional scope')
 require('COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_f137_host.py\n                --source ${CMAKE_CURRENT_SOURCE_DIR}/.. --controls --execute-controlled-host-only)'in code,'exact CTest argv')
 require('set_tests_properties(f137_task_host_contract PROPERTIES TIMEOUT 120)'in code,'CTest120s deadline')
 fast_flow(fast,(root/'test/f137_process.py').read_text())


def main():
 p=argparse.ArgumentParser();p.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[1]);a=p.parse_args();validate(a.root);print('PASS F137 mandatory inventory/CTest and actual passive fast flow')


if __name__=='__main__':main()
