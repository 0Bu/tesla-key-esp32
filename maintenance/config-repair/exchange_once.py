#!/usr/bin/env python3
"""Explicit single physical maintenance exchange; never flashes, resets or automatically retries."""
from __future__ import annotations
import argparse,importlib.util,json,os,stat,sys,time
from pathlib import Path

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port',required=True);p.add_argument('--owner-file',required=True,type=Path)
    p.add_argument('--evidence-directory',required=True,type=Path)
    p.add_argument('--execute-once-explicit-config-write',action='store_true')
    a=p.parse_args()
    if not a.execute_once_explicit_config_write:
        print('Plan only: no serial open, reset, flash or configuration write. Exact-board cfg-write authorization is required.');return
    if not a.port.startswith(('/dev/cu.','/dev/tty')):raise ValueError('explicit local serial port required')
    node=os.lstat(a.port)
    if not stat.S_ISCHR(node.st_mode):raise ValueError('port must be an actual non-symlink character device')
    s=importlib.util.spec_from_file_location('repair_offline_encoding',Path(__file__).with_name('encode_request.py'))
    m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
    owner=json.loads(m.private_read(a.owner_file,4096).decode('utf-8'),object_pairs_hook=m.unique_object)
    os.umask(0o077);a.evidence_directory.mkdir(mode=0o700)
    def retain(name,data):
        fd=os.open(a.evidence_directory/name,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
        try:
            with os.fdopen(fd,'wb',closefd=False) as f:f.write(data);f.flush();os.fsync(fd)
        finally:os.close(fd)
    # Import only after the explicit execution decision. Opening/closing a physical USB serial
    # endpoint can affect board reset lines despite requested DTR/RTS=false; the runbook must
    # include this in its independently reviewed physical operation. No software reset is sent.
    import serial
    port=serial.Serial(port=None,baudrate=115200,timeout=0.1,write_timeout=5,exclusive=True)
    port.dtr=False;port.rts=False;port.port=a.port
    sent=False;terminal_response=False;close_failed=False;retention_failed=False;diagnostic_failed=False
    result={'outcome':'REFUSED','applyWriteCalled':False,'automaticRetry':False,'closeOutcome':'NOT_ATTEMPTED','terminalResponseReceived':False}
    try:
        port.open()
        deadline=time.monotonic()+15;captured=bytearray();start=-1
        while time.monotonic()<deadline and len(captured)<65536:
            part=port.read(1)
            if part:captured.extend(part)
            if start<0 and captured.endswith(b'TKCR1'):start=len(captured)-5
            if start>=0 and len(captured)-start==108:break
        retain('console-before.bin',captured)
        if start<0 or len(captured)-start!=108:raise ValueError('no complete bounded INFO')
        frame=m.encode(owner,bytes(captured[start:]))
        retain('request.bin',frame)
        # One write invocation only. Partial, throwing or lost response means uncertain delivery;
        # never resend or invoke write again. Firmware's one-boot gate remains authoritative.
        result['applyWriteCalled']=True;sent=True
        count=port.write(frame)
        if count!=len(frame):raise ValueError('partial APPLY delivery')
        end=time.monotonic()+40;reply=bytearray()
        while len(reply)<10 and time.monotonic()<end:
            part=port.read(10-len(reply))
            if part:reply.extend(part)
        retain('receipt.bin',reply)
        if len(reply)!=10 or reply[:5]!=b'TKRS1' or any(x not in (0,1) for x in reply[6:]):raise ValueError('no exact terminal receipt')
        # Outcome enum0=Refused,1=Confirmed,2=Ambiguous from the actual shared core.
        if reply[5]==1 and reply[6:]==b'\1\1\1\1':result['outcome']='CONFIRMED'
        elif reply[5]==0 and reply[6:]==b'\0\0\0\0':result['outcome']='REFUSED'
        else:result['outcome']='AMBIGUOUS'
        terminal_response=True;result['terminalResponseReceived']=True
    except BaseException:
        if sent and not terminal_response:result['outcome']='AMBIGUOUS'
        raise
    finally:
        # Close and terminal retention are independent. Preserve an in-flight APPLY/read error;
        # cleanup or diagnostic failure must never replace it or authorize a replay.
        try:
            port.close()
            result['closeOutcome']='CLOSED'
        except BaseException:
            close_failed=True;result['closeOutcome']='FAILED'
        try:
            retain('result.json',(json.dumps(result,sort_keys=True)+'\n').encode())
        except BaseException:
            retention_failed=True
        # No values, port, identity, exception text or private evidence path are printed.
        try:
            if retention_failed:
                print('Terminal evidence write failed. Preserve existing evidence; no resend or retry.',file=sys.stderr)
            else:
                print('Physical exchange stopped: '+result['outcome']+'. No automatic retry or restoration.')
        except BaseException:
            diagnostic_failed=True
    if close_failed or retention_failed or diagnostic_failed:
        raise RuntimeError('Maintenance finalization failed; no automatic retry or restoration.')
    if result['outcome']!='CONFIRMED':raise SystemExit(1)
if __name__=='__main__':
    try:main()
    except (Exception,KeyboardInterrupt):
        print('Maintenance stopped. Preserve private evidence; do not resend or reset to retry.',file=sys.stderr);raise SystemExit(1)
