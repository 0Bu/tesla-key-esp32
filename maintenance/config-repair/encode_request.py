#!/usr/bin/env python3
"""Encode a private one-boot request offline; never opens USB, a device or the network."""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import stat
import struct
import sys
import zlib

FIELDS={'wifi_ssid','wifi_pass','vin','mqtt_uri','syslog_uri','ota_channel'}


def private_read(path: Path, cap: int) -> bytes:
    # Do not follow links or accept shared group/world-readable credential/request material.
    descriptor=os.open(path,os.O_RDONLY|os.O_NOFOLLOW)
    try:
        info=os.fstat(descriptor)
        if not stat.S_ISREG(info.st_mode) or info.st_uid!=os.getuid() or info.st_mode&0o077 or info.st_size>cap:
            raise ValueError('input must be a bounded owner-only regular file')
        raw=b''
        while len(raw)<=cap:
            part=os.read(descriptor,min(1024,cap+1-len(raw)))
            if not part:break
            raw+=part
        if len(raw)>cap:raise ValueError('input exceeds its bound')
        return raw
    finally:os.close(descriptor)


def unique_object(pairs):
    out={}
    for key,value in pairs:
        if key in out:raise ValueError('duplicate JSON field')
        out[key]=value
    return out


def text(value,limit):
    if type(value) is not str:raise ValueError('configuration requires explicit text values')
    raw=value.encode('utf-8','strict')
    if len(raw)>limit or any(c<32 or c==127 for c in raw):raise ValueError('invalid configuration text')
    return raw


def vin(value):
    raw=text(value,17)
    if len(raw)!=17 or any(chr(c) not in 'ABCDEFGHJKLMNPRSTUVWXYZ0123456789' for c in raw):
        raise ValueError('invalid owner-confirmed VIN')
    return raw


def blob(config):
    if type(config) is not dict or set(config)!=FIELDS:raise ValueError('complete exact configuration required')
    ssid=text(config['wifi_ssid'],32);password=text(config['wifi_pass'],64)
    if not ssid and password:raise ValueError('unconfigured WiFi must have empty password')
    if password and not (8<=len(password)<=63 or len(password)==64 and
                        all(chr(c) in '0123456789abcdefABCDEF' for c in password)):
        raise ValueError('invalid WiFi password form')
    if type(config['ota_channel']) is not int or config['ota_channel'] not in (0,1):
        raise ValueError('explicit release/dev channel required')
    def field(raw):return struct.pack('<H',len(raw))+raw
    # Mirror of existing TKC1/v1 codec: no new format, trial backups or legacy authority.
    raw=b'TKC1\x01'+field(ssid)+field(password)+field(b'')+field(b'')
    raw+=bytes([4|(8 if config['ota_channel'] else 0)])
    raw+=field(vin(config['vin']))+field(text(config['mqtt_uri'],64))+field(text(config['syslog_uri'],64))
    raw+=struct.pack('<I',zlib.crc32(raw)&0xffffffff)
    if len(raw)>512:raise ValueError('replacement exceeds existing codec buffer')
    return raw


def encode(owner,info):
    expected={'schema','ownerConfirmedKnownIdentity','expectedBoardMac','knownPublicKeyHex','knownVin','config'}
    if type(owner) is not dict or set(owner)!=expected or type(owner['schema']) is not int or owner['schema']!=1 or \
       owner['ownerConfirmedKnownIdentity'] is not True:
        raise ValueError('explicit independently known identity authority required')
    if len(info)!=108 or info[:5]!=b'TKCR1':raise ValueError('invalid maintenance INFO frame')
    mac=owner['expectedBoardMac'];point=owner['knownPublicKeyHex']
    if type(mac) is not str or len(mac)!=17 or any(mac[i]!=':' for i in (2,5,8,11,14)) or \
       type(point) is not str or len(point)!=130:
        raise ValueError('full independent board/key authority required')
    board=bytes.fromhex(mac.replace(':',''));public=bytes.fromhex(point)
    if len(board)!=6 or len(public)!=65 or public[0]!=4 or board!=info[5:11] or public!=info[11:76]:
        raise ValueError('maintenance board/key differs from independent authority')
    known=vin(owner['knownVin'])
    config=owner['config']
    replacement=blob(config)
    if known!=vin(config['vin']):raise ValueError('replacement VIN differs from confirmed existing VIN')
    payload=b'\x01'+board+public+info[76:108]+known+struct.pack('<H',len(replacement))+replacement
    size=7+len(payload)+4
    frame=b'TKAP1'+struct.pack('<H',size)+payload
    return frame+struct.pack('<I',zlib.crc32(frame)&0xffffffff)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--owner-file',type=Path,required=True)
    parser.add_argument('--info-file',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    owner=json.loads(private_read(args.owner_file,4096).decode('utf-8','strict'),object_pairs_hook=unique_object)
    frame=encode(owner,private_read(args.info_file,108))
    # Exclusive creation; never overwrite or expose credentials in a shared file/console.
    descriptor=os.open(args.output,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
    try:
        view=memoryview(frame)
        while view:
            written=os.write(descriptor,view)
            if written<=0:raise OSError('private request write failed')
            view=view[written:]
        os.fsync(descriptor)
    finally:os.close(descriptor)
    print('Private request encoded. No USB/device operation performed.')


if __name__=='__main__':
    try:main()
    except (OSError,ValueError,UnicodeError,KeyError,TypeError):
        print('Request refused; check protected inputs and complete known-identity configuration.',file=sys.stderr)
        raise SystemExit(1)
