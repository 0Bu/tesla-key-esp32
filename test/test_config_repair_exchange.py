#!/usr/bin/env python3
"""Actual exchange main with owned synthetic serial/time/filesystem doubles; never a real port."""
import contextlib,importlib.util,io,json,os,stat,sys,tempfile,types,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
HOST=ROOT/'maintenance/config-repair/exchange_once.py'
def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module
class ApplyError(OSError):pass
class CloseError(OSError):pass
class EvidenceError(OSError):pass
class OpenError(OSError):pass

class Exchange(unittest.TestCase):
    def run_case(self,case,path=HOST):
        with tempfile.TemporaryDirectory(prefix='cfg-exchange-synthetic-') as temporary:
            directory=Path(temporary);host=load('synthetic_exchange',path)
            close_fails='-close' in case;retention_fails='-result-fail' in case
            phase=case.split('-close',1)[0].split('-result-fail',1)[0].split('-diagnostic-fail',1)[0]
            point='04'+'11'*64;mac='02:00:00:00:00:01';vin='A'*17
            owner={'schema':1,'ownerConfirmedKnownIdentity':True,'expectedBoardMac':mac,
                   'knownPublicKeyHex':point,'knownVin':vin,
                   'config':{'wifi_ssid':'','wifi_pass':'','vin':vin,'mqtt_uri':'','syslog_uri':'','ota_channel':0}}
            owner_file=directory/'owner.json';owner_file.write_text(json.dumps(owner));owner_file.chmod(0o600)
            info=b'TKCR1'+bytes.fromhex(mac.replace(':',''))+bytes.fromhex(point)+b'N'*32
            if phase=='wrong-key':info=info[:11]+b'\4'+b'\x22'*64+info[76:]
            reply=b'TKRS1'+(b'\0'*5 if phase=='refused' else b'\1'*5)
            if phase=='lost-receipt':reply=b''
            state={'open':0,'write':0,'close':0,'resultAttempt':0,'now':0.,'stream':bytearray(info+reply)}
            class Port:
                def __init__(self,**kwargs):self.port=None
                def open(self):
                    state['open']+=1
                    if phase=='open':raise OpenError('synthetic open failure')
                def close(self):
                    state['close']+=1
                    if close_fails:raise CloseError('synthetic close failure')
                def read(self,size):
                    state['now']+=0.02
                    data=bytes(state['stream'][:size]);del state['stream'][:size];return data
                def write(self,data):
                    state['write']+=1
                    if phase=='throw':raise ApplyError('synthetic write uncertainty')
                    return len(data)-1 if phase=='partial' else len(data)
            old_serial=sys.modules.get('serial');sys.modules['serial']=types.SimpleNamespace(Serial=Port)
            old_argv=sys.argv
            actual_lstat=os.lstat;actual_open=os.open
            host.os=types.SimpleNamespace(**vars(os))
            host.os.lstat=lambda value:types.SimpleNamespace(st_mode=stat.S_IFCHR) if str(value)=='/dev/cu.synthetic-repair' else actual_lstat(value)
            def open_file(value,*args,**kwargs):
                if Path(value).name=='result.json':
                    state['resultAttempt']+=1
                    if retention_fails:raise EvidenceError('synthetic terminal persistence failure')
                return actual_open(value,*args,**kwargs)
            host.os.open=open_file
            host.time=types.SimpleNamespace(monotonic=lambda:state['now'])
            sys.argv=['exchange_once.py','--port','/dev/cu.synthetic-repair','--owner-file',str(owner_file),
                      '--evidence-directory',str(directory/'evidence')]
            if phase!='unarmed':sys.argv+=['--execute-once-explicit-config-write']
            class DiagnosticFailure(io.StringIO):
                def write(self,value):raise OSError('synthetic diagnostic sink failure')
            caught=None
            try:
                with contextlib.redirect_stdout(DiagnosticFailure() if '-diagnostic-fail' in case else io.StringIO()),contextlib.redirect_stderr(io.StringIO()):
                    try:host.main()
                    except BaseException as error:caught=error
            finally:
                sys.argv=old_argv
                if old_serial is None:sys.modules.pop('serial',None)
                else:sys.modules['serial']=old_serial
            self.assertEqual(state['open'],0 if phase=='unarmed' else 1)
            self.assertEqual(state['write'],0 if phase in ('unarmed','wrong-key','open') else 1)
            self.assertEqual(state['close'],0 if phase=='unarmed' else 1)
            self.assertEqual(state['resultAttempt'],0 if phase=='unarmed' else 1)
            if phase=='throw':self.assertIsInstance(caught,ApplyError) # close/retain cannot mask APPLY
            elif phase=='open':self.assertIsInstance(caught,OpenError)
            elif phase in ('partial','lost-receipt','wrong-key'):self.assertIsInstance(caught,ValueError)
            elif phase=='refused' and not close_fails:self.assertIsInstance(caught,SystemExit)
            elif close_fails or retention_fails or '-diagnostic-fail' in case:self.assertIsInstance(caught,RuntimeError)
            else:self.assertIsNone(caught)
            if phase=='unarmed':
                self.assertFalse((directory/'evidence').exists());return
            result_path=directory/'evidence/result.json'
            if retention_fails:
                self.assertFalse(result_path.exists());return # no false claim of durable evidence
            result=json.loads(result_path.read_text())
            expected='CONFIRMED' if phase=='confirmed' else 'REFUSED' if phase in ('wrong-key','refused','open') else 'AMBIGUOUS'
            self.assertEqual(result['outcome'],expected)
            self.assertEqual(result['applyWriteCalled'],phase not in ('wrong-key','open'))
            self.assertEqual(result['terminalResponseReceived'],phase in ('confirmed','refused'))
            self.assertEqual(result['closeOutcome'],'FAILED' if close_fails else 'CLOSED')
            self.assertFalse(result['automaticRetry'])
            self.assertLess(result_path.stat().st_size,512)
            self.assertNotIn('/dev/',result_path.read_text())
            self.assertNotIn('synthetic',result_path.read_text())

    def test_actual_exchange_boundaries(self):
        # Keep all seven V2 cases, then exercise attempted/partial/throwing/lost/terminal writes
        # with close failure, plus open, persistence and diagnostic failures. No serial access.
        for case in ('unarmed','confirmed','refused','wrong-key','partial','throw','lost-receipt',
                     'confirmed-close','partial-close','throw-close','lost-receipt-close',
                     'refused-close','wrong-key-close','open-close','confirmed-result-fail',
                     'throw-close-result-fail','confirmed-diagnostic-fail'):
            with self.subTest(case=case):self.run_case(case)

    def test_actual_close_containment_omission_restore(self):
        source=HOST.read_text()
        before="""        try:
            port.close()
            result['closeOutcome']='CLOSED'
        except BaseException:
            close_failed=True;result['closeOutcome']='FAILED'
"""
        self.assertEqual(source.count(before),1)
        with tempfile.TemporaryDirectory(prefix='cfg-close-omission-') as temporary:
            directory=Path(temporary)
            (directory/'encode_request.py').write_bytes((HOST.parent/'encode_request.py').read_bytes())
            altered=directory/'exchange_once.py'
            altered.write_text(source.replace(before,"        port.close()\n        result['closeOutcome']='CLOSED'\n",1))
            for case in ('confirmed-close','throw-close'):
                with self.subTest(omission=case):
                    with self.assertRaises(AssertionError):self.run_case(case,altered)
            altered.write_text(source)
            for case in ('confirmed-close','throw-close'):self.run_case(case,altered)

    def test_actual_terminal_retention_omission_restore(self):
        source=HOST.read_text()
        before="        retain('result.json',(json.dumps(result,sort_keys=True)+'\\n').encode())"
        self.assertEqual(source.count(before),1)
        with tempfile.TemporaryDirectory(prefix='cfg-record-omission-') as temporary:
            directory=Path(temporary)
            (directory/'encode_request.py').write_bytes((HOST.parent/'encode_request.py').read_bytes())
            altered=directory/'exchange_once.py'
            altered.write_text(source.replace(before,'        pass # omitted terminal retention',1))
            with self.assertRaises(AssertionError):self.run_case('confirmed-close',altered)
            altered.write_text(source);self.run_case('confirmed-close',altered)

if __name__=='__main__':unittest.main()
