#!/usr/bin/env python3
"""Separate, key-free maintenance app/manifest validator; never emits ordinary OTA parts."""
from __future__ import annotations
import argparse,hashlib,importlib.util,json,re,sys
from pathlib import Path

def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    if spec is None or spec.loader is None: raise ValueError('missing canonical validator')
    value=importlib.util.module_from_spec(spec);sys.modules[name]=value;spec.loader.exec_module(value);return value
ROOT=Path(__file__).resolve().parents[1]
FW=module('repair_firmware_artifacts',ROOT/'scripts/check-firmware-artifacts.py')
SOURCE=module('repair_source_inventory',ROOT/'scripts/check-build-artifact-inventory.py')
TARGETS=('esp32','esp32s3','esp32c3','esp32c6')
VERSION=re.compile(r'cfgrepair-[0-9a-f]{16}\Z')

def validate_app(path,target,version,*,signed=False):
    if target not in TARGETS or not VERSION.fullmatch(version): raise ValueError('invalid maintenance identity')
    data=FW.regular_bytes(path,'maintenance app')
    pin=FW.read_public_key_digest(ROOT/'scripts/ota-signing-public-key.sha256') if signed else None
    info=FW.parse_image(data,signed=signed,expect_app_desc=True,expected_public_key_digest=pin)
    if (info.chip_id,info.version,info.project)!=(FW.CHIP_IDS[target],version,'tesla-key-config-repair'):
        raise ValueError('maintenance image identity crossing')
    projected=((len(data)+0xffff)//0x10000)*0x10000+0x1000 if not signed else len(data)
    if projected>FW.APP_POLICY_LIMIT: raise ValueError('maintenance image exceeds unchanged app limit')
    return {'sha256':hashlib.sha256(data).hexdigest(),'size':len(data),'projectedSignedSize':projected}

def manifest(directory,version,source_sha):
    if directory.is_symlink() or not directory.is_dir() or directory.name!='maintenance-dist' or directory.resolve()==ROOT/'dist': raise ValueError('wrong maintenance output root')
    source=SOURCE.source_state(ROOT,source_sha)
    if version!='cfgrepair-'+source['sha256'][:16]: raise ValueError('version is not exact maintenance source')
    if {p.name for p in directory.iterdir()}-{'config-repair-manifest.json'}!=set(TARGETS): raise ValueError('maintenance target inventory drift')
    apps={}
    for t in TARGETS:
        allowed={'tesla-key-config-repair.bin','tesla-key-config-repair.elf','tesla-key-config-repair.map','sdkconfig.'+t,'stack-usage-'+t+'.json','size-'+t+'.json'}
        if (directory/t).is_symlink() or {p.name for p in (directory/t).iterdir()}!=allowed or any(p.is_symlink() or not p.is_file() for p in (directory/t).iterdir()):raise ValueError('maintenance diagnostic inventory drift')
        path=directory/t/'tesla-key-config-repair.bin'
        apps[t]={'path':t+'/tesla-key-config-repair.bin',**validate_app(path,t,version)}
    result={'schema':1,'kind':'tesla-key-config-repair-apps','role':'config-repair','project':'tesla-key-config-repair',
            'version':version,'source':source,'signed':False,'ordinaryOtaCompatible':False,'apps':apps,'diagnostics':{t:{p.name:{'sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'size':p.stat().st_size} for p in sorted((directory/t).iterdir())} for t in TARGETS}}
    existing=directory/'config-repair-manifest.json'
    if existing.exists():
        if existing.is_symlink() or existing.read_bytes()!=(json.dumps(result,sort_keys=True,separators=(',',':'))+'\n').encode():raise ValueError('maintenance manifest/body provenance drift')
    return result

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--target',choices=TARGETS);p.add_argument('--version',required=True)
    p.add_argument('--app',type=Path);p.add_argument('--signed',action='store_true');p.add_argument('--directory',type=Path);p.add_argument('--source-sha')
    a=p.parse_args()
    if a.directory:
        if a.app or a.target or a.signed or not a.source_sha:p.error('manifest requires directory/source only')
        result=manifest(a.directory,a.version,a.source_sha)
    else:
        if not a.app or not a.target:p.error('app requires target')
        result=validate_app(a.app,a.target,a.version,signed=a.signed)
    print(json.dumps(result,sort_keys=True,separators=(',',':')))
if __name__=='__main__':main()
