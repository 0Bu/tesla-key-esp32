#!/usr/bin/env python3
"""Real owned process-group/pipe tests; query deadlines use an explicit clock offset.

The Git proxy never fetches. Denied signal/observation cases are controlled error doubles,
then cleaned by the test owner; only ESRCH permits the production helper to report cleanup.
"""
from pathlib import Path
from unittest.mock import patch
import datetime,hashlib,importlib.util,json,os,shutil,signal,subprocess,sys,tempfile,time
os.umask(0o077)
ROOT=Path(__file__).resolve().parents[1]
F=ROOT/'scripts/dependency_source.py'
spec=importlib.util.spec_from_file_location('f088_query_v2',F);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
rows=[];real_mono=time.monotonic;real_killpg=os.killpg;real_popen=subprocess.Popen

def clean_error_double(child):
 # The real EOF guardian may be exiting concurrently after an intentionally denied cleanup.
 # Reap and retry transient EPERM; only actual ESRCH confirms fixture-group disappearance.
 until=real_mono()+2
 while real_mono()<until:
  child.poll()
  try:real_killpg(child.pid,signal.SIGKILL)
  except ProcessLookupError:
   child.wait(timeout=1);return
  except PermissionError:pass
  try:real_killpg(child.pid,0)
  except ProcessLookupError:
   child.wait(timeout=1);return
  except PermissionError:pass
  time.sleep(.01)
 raise AssertionError('test-error fixture cleanup could not be confirmed')

with tempfile.TemporaryDirectory(prefix='dependency-query-lifetime-') as tmp:
 root=Path(tmp).resolve();tool=root/'git';marker=root/'owned.json';trusted=root/'trusted';trusted.mkdir()
 tool.write_text('#!'+__import__('sys').executable+'\n'+'''import json,os,signal,sys,time
from pathlib import Path
mode=os.environ['F088_CASE'];marker=Path(os.environ['F088_MARKER'])
if mode=='both-pipes':
 os.write(2,b'diagnostic'*2000);os.write(1,b'OK');sys.exit(0)
if mode=='nonzero-exit':sys.exit(4)
if mode=='stdout-bound':os.write(1,b'x'*9)
if mode in ('stderr-bound','signal-error','observation-error','observation-transient','unconfirmed-cleanup'):os.write(2,b'x'*65537)
if mode in ('exited-leader','term-resistant-descendant'):
 child=os.fork()
 if child==0:
  if mode=='term-resistant-descendant':signal.signal(signal.SIGTERM,signal.SIG_IGN)
  marker.write_text(json.dumps({'group':os.getpgrp(),'descendant':os.getpid()}));time.sleep(60);os._exit(0)
 while not marker.exists():time.sleep(.005)
 os._exit(0)
marker.write_text(json.dumps({'group':os.getpgrp()}));time.sleep(60)
''');tool.chmod(0o700)
 cases=['both-pipes','stdout-bound','stderr-bound','nonzero-exit','deadline-live-leader','exited-leader','term-resistant-descendant','signal-error','observation-error','observation-transient','unconfirmed-cleanup']
 for case in cases:
  if marker.exists():marker.unlink()
  env=m.git_environment();env.update(PATH=str(root),F088_CASE=case,F088_MARKER=str(marker))
  children=[];signals=[];ticks=[0];offset=[0.0]
  def popen(*args,**kwargs):
   child=real_popen(*args,**kwargs);children.append(child);return child
  def clock():
   ticks[0]+=1
   if ticks[0]==2 and case in ('deadline-live-leader','exited-leader','term-resistant-descendant'):
    until=real_mono()+3
    while not marker.exists() and real_mono()<until:time.sleep(.005)
    assert marker.exists(),'fixture did not start'
    time.sleep(.05);offset[0]=31
   return real_mono()+offset[0]
  def killpg(pid,sig):
   assert children and pid==children[0].pid
   signals.append(int(sig))
   if case=='signal-error':raise PermissionError('fixture signal refusal')
   if case=='observation-error':
    if sig==0:raise PermissionError('fixture observation refusal')
    return None
   if case=='observation-transient' and sig==0 and signals.count(0)==1:raise PermissionError('fixture transient observation refusal')
   if case=='unconfirmed-cleanup':return None
   return real_killpg(pid,sig)
  error=None
  try:
   with patch.object(m.subprocess,'Popen',popen),patch.object(m.time,'monotonic',clock),patch.object(m.os,'killpg',killpg):
    value=m.git_query(trusted,['fixture-only'],env,maximum=8)
   assert case=='both-pipes' and value==b'OK'
  except m.SourceError as e:
   assert case!='both-pipes';error=str(e)
   expected={'signal-error':'cannot signal','observation-error':'could not be confirmed','unconfirmed-cleanup':'could not be confirmed'}.get(case)
   if expected:assert expected in error,(case,error)
   elif case=='nonzero-exit':assert 'query failed' in error
   elif 'bound' in case or case=='observation-transient':assert 'output bound' in error
   else:assert 'deadline' in error,(case,error)
  finally:
   assert len(children)==1
   child=children[0]
   assert child.stdout.closed and child.stderr.closed
   # Only the one fixture session created above; independently clean test-error doubles.
   if case in ('signal-error','observation-error','unconfirmed-cleanup') and child.poll() is None:
    clean_error_double(child)
   child.wait(timeout=2)
  if case in ('exited-leader','term-resistant-descendant'):
   assert signal.SIGKILL in signals
   # Parent cleanup or the independent EOF guardian may deliver the decisive KILL.
   assert signals[-1] == 0  # Actual helper observed ESRCH after reaping its leader.
   owned=json.loads(marker.read_text())
   try:os.kill(owned['descendant'],0)
   except ProcessLookupError:pass
   else:raise AssertionError('owned descendant still exists after accepted cleanup')
  rows.append({'case':case,'result':'PASS','error':error,'signalKinds':sorted(set(signals)),'streamsClosed':True})
def present(pid):
 try:os.kill(pid,0);return True
 except ProcessLookupError:return False

def group_absent(group,seconds=2):
 until=real_mono()+seconds
 while real_mono()<until:
  try:real_killpg(group,0)
  except ProcessLookupError:return True
  except PermissionError:pass
  time.sleep(.01)
 return False

def clean_group(group):
 try:real_killpg(group,signal.SIGKILL)
 except ProcessLookupError:return
 assert group_absent(group),'owned cancellation fixture group did not disappear'

# Real outer cancellation must not rely on the killed Python parent's finally block.
with tempfile.TemporaryDirectory(prefix='dependency-query-outer-') as temporary:
 root=Path(temporary).resolve();cmake=shutil.which('cmake');assert cmake
 for case in ('cmake-live','canonical-live','canonical-exited','sigkill-exited','sigkill-cleanup','guardian-eof-omitted','cleanup-atomic-omitted','restored'):
  work=root/case;work.mkdir();tools=work/'tools';tools.mkdir()
  record=work/'query.json';helper_record=work/'helper.json';effect=work/'delayed-effect';ready=work/'descendant-ready'
  local_source=work/'dependency_source.py';source=F.read_text()
  if case=='guardian-eof-omitted':
   needle='if select.select([alive], [], [], 0.02)[0]:'
   assert source.count(needle)==1;source=source.replace(needle,'if False:')
  if case=='cleanup-atomic-omitted':
   needle='stop_owned_group(child, atomic=True)'
   assert source.count(needle)==1;source=source.replace(needle,'stop_owned_group(child)')
  compile(source,str(local_source),'exec');local_source.write_text(source)
  fake=tools/'git';exited=case in ('canonical-exited','sigkill-exited','sigkill-cleanup','cleanup-atomic-omitted')
  fake.write_text('#!'+sys.executable+'\n'+
   'from pathlib import Path\nimport json,os,signal,time\n'+
   'child=os.fork()\n'+
   'if child==0:\n signal.signal(signal.SIGTERM,signal.SIG_IGN)\n Path('+repr(str(ready))+').write_text("TERM resistance installed")\n time.sleep(3.2)\n Path('+repr(str(effect))+').write_text("owned inert effect")\n time.sleep(20)\n os._exit(0)\n'+
   'until=time.monotonic()+1\nwhile not Path('+repr(str(ready))+').exists() and time.monotonic()<until:time.sleep(.005)\nassert Path('+repr(str(ready))+').exists(),"descendant did not become ready"\n'+
   'Path('+repr(str(record))+').write_text(json.dumps({"git":os.getpid(),"child":child,"group":os.getpgrp()}))\n'+
   ('os._exit(0)\n' if exited else 'time.sleep(20)\n'))
  fake.chmod(0o700)
  helper=work/'query.py'
  cleanup_hook=''
  if case in ('sigkill-cleanup','cleanup-atomic-omitted'):
   cleanup_hook=('import signal,time\nreal_clock=m.time.monotonic;offset=[0];ticks=[0]\n'+
    'def clock():\n ticks[0]+=1\n if ticks[0]==2:\n  until=real_clock()+3\n  while not Path('+repr(str(record))+').exists() and real_clock()<until:time.sleep(.005)\n  assert Path('+repr(str(record))+').exists()\n  offset[0]=31\n return real_clock()+offset[0]\n'+
    'real_kill=m.os.killpg\n'+
    'def kill_before_atomic(group,sig):\n if sig==signal.SIGKILL:\n  Path('+repr(str(work/'cleanup-interruption'))+').write_text("parent killed before group KILL")\n  os.kill(os.getpid(),signal.SIGKILL)\n return real_kill(group,sig)\n'+
    'm.time.monotonic=clock;m.os.killpg=kill_before_atomic\n')
  helper.write_text('from pathlib import Path\nimport importlib.util,json,os,sys\n'+
   'sys.dont_write_bytecode=True\n'+
   'Path('+repr(str(helper_record))+').write_text(json.dumps({"pid":os.getpid(),"group":os.getpgrp()}))\n'+
   's=importlib.util.spec_from_file_location("actual_query",'+repr(str(local_source))+')\n'+
   'm=importlib.util.module_from_spec(s);s.loader.exec_module(m)\n'+
   cleanup_hook+
   'env=m.git_environment();env["PATH"]='+repr(str(tools))+'\n'+
   'm.git_query(Path('+repr(str(work))+'),["fixture-only"],env,maximum=8)\n')
  env=m.git_environment();env['PYTHONDONTWRITEBYTECODE']='1'
  if case=='cmake-live':
   command_file=work/'outer.cmake'
   command_file.write_text('execute_process(COMMAND "'+sys.executable+'" "'+str(helper)+'" TIMEOUT 2 RESULT_VARIABLE result)\nfile(WRITE "'+str(work/'cmake-result')+'" "${result}")\n')
   command=[cmake,'-P',str(command_file)]
  elif case in ('sigkill-exited','sigkill-cleanup','cleanup-atomic-omitted'):command=[sys.executable,str(helper)]
  else:command=[sys.executable,str(ROOT/'tools/agent-hooks/run_with_timeout.py'),'2',sys.executable,str(helper)]
  start=real_mono();owned=None;outer=None
  try:
   outer=real_popen(command,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,start_new_session=True)
   if case=='sigkill-exited':
    until=real_mono()+3
    while not record.exists() and real_mono()<until:time.sleep(.005)
    assert record.exists(),'owned query never started'
    os.kill(outer.pid,signal.SIGKILL)
   out,err=outer.communicate(timeout=8)
   assert record.is_file() and helper_record.is_file(),'fixture did not start before outer deadline'
   owned=json.loads(record.read_text());hp=json.loads(helper_record.read_text())
   assert owned['group']!=hp['group'] and owned['group']!=owned['git']
   assert not present(hp['pid'])
   assert not effect.exists(),'outer did not return before the delayed effect'
   if case=='cmake-live':assert 'timeout' in (work/'cmake-result').read_text().lower()
   elif case in ('sigkill-exited','sigkill-cleanup','cleanup-atomic-omitted'):assert outer.returncode==-signal.SIGKILL
   else:assert outer.returncode==124
   if case in ('sigkill-cleanup','cleanup-atomic-omitted'):assert (work/'cleanup-interruption').is_file()
   if case in ('guardian-eof-omitted','cleanup-atomic-omitted'):
    until=start+4.2
    while not effect.exists() and real_mono()<until:time.sleep(.01)
    assert effect.exists() and present(owned['child']),'valid EOF omission did not defeat the cancellation oracle'
   else:
    assert group_absent(owned['group']) and not present(owned['child'])
    assert not effect.exists()
   rows.append({'case':case,'result':'PASS','outerExit':outer.returncode,'expectedUnsafeOmission':case in ('guardian-eof-omitted','cleanup-atomic-omitted'),'stderrSha256':hashlib.sha256(err).hexdigest()})
  finally:
   if owned is None and record.exists():owned=json.loads(record.read_text())
   if owned is not None:clean_group(owned['group'])
   if outer is not None:
    if outer.poll() is None:clean_group(outer.pid)
    outer.wait(timeout=2)

# A missing, duplicated, malformed or out-of-range leader status never becomes success.
with tempfile.TemporaryDirectory(prefix='dependency-query-status-') as temporary:
 root=Path(temporary).resolve();fake=root/'git';fake.write_text('#!'+sys.executable+'\nimport os\nos.write(1,b"OK")\n');fake.chmod(0o700)
 env=m.git_environment();env['PATH']=str(root)
 needle='(str(code) + "\\n").encode("ascii")'
 assert m.GIT_GUARDIAN.count(needle)==1
 for label,status in [('empty','b""'),('duplicate','b"0\\n0\\n"'),('malformed','b"x\\n"'),('out-of-range','b"-999\\n"'),('over-budget','b"0"*17')]:
  start=real_mono()
  with patch.object(m,'GIT_GUARDIAN',m.GIT_GUARDIAN.replace(needle,status)):
   try:m.git_query(root,['fixture-only'],env,maximum=8)
   except m.SourceError as error:
    assert ('output bound' if label=='over-budget' else 'status') in str(error),(label,str(error))
   else:raise AssertionError('invalid status accepted: '+label)
  assert real_mono()-start<3
  rows.append({'case':'status-'+label,'result':'PASS'})
 assert m.git_query(root,['fixture-only'],env,maximum=8)==b'OK'
 rows.append({'case':'status-restored','result':'PASS'})
 # Status EOF must fail immediately even while an exited Git leader's child holds data pipes.
 marker=root/'status-owned.json'
 fake.write_text('#!'+sys.executable+'\nfrom pathlib import Path\nimport json,os,time\nchild=os.fork()\nif child==0:\n time.sleep(20)\n os._exit(0)\nPath('+repr(str(marker))+').write_text(json.dumps({"group":os.getpgrp(),"child":child}))\nos._exit(0)\n')
 wait='import time\nuntil=time.monotonic()+2\nwhile not os.path.exists('+repr(str(marker))+') and time.monotonic()<until:time.sleep(.005)\nos.close(status)\nstatus=-1\nchild=None\nos.close(1)'
 assert m.GIT_GUARDIAN.count('os.close(1)')==1
 start=real_mono();owned=None
 try:
  with patch.object(m,'GIT_GUARDIAN',m.GIT_GUARDIAN.replace('os.close(1)',wait)):
   try:m.git_query(root,['fixture-only'],env,maximum=8)
   except m.SourceError as error:assert 'status' in str(error)
   else:raise AssertionError('empty status admitted with held data pipes')
  assert marker.is_file();owned=json.loads(marker.read_text())
  assert real_mono()-start<3 and group_absent(owned['group']) and not present(owned['child'])
  rows.append({'case':'status-eof-with-held-data-pipes','result':'PASS'})
 finally:
  if owned is None and marker.exists():owned=json.loads(marker.read_text())
  if owned is not None:clean_group(owned['group'])

result={'at':datetime.datetime.now(datetime.timezone.utc).isoformat(),'sourceSha256':hashlib.sha256(F.read_bytes()).hexdigest(),'scope':'Actual helper, real outer CMake/canonical-runner cancellation, parent SIGKILL and inert owned subprocesses only. Internal deadline clock offset; valid EOF omission defeats the effect oracle. All fixture groups are cleaned. No network/source dependency/device/signing operations.','rows':rows}
print(json.dumps({'result':'PASS','cases':len(rows),'scope':result['scope']}))
