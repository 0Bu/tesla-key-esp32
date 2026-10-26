#!/usr/bin/env python3
"""Unsigned four-target maintenance build; run through scripts/idf-docker.sh only."""
from __future__ import annotations
import argparse,hashlib,importlib.util,json,os,re,shutil,subprocess,sys,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
TARGETS=('esp32','esp32s3','esp32c3','esp32c6')
PROJECT='tesla-key-config-repair'

def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    if spec is None or spec.loader is None:raise ValueError('missing build contract')
    m=importlib.util.module_from_spec(spec);sys.modules[name]=m;spec.loader.exec_module(m);return m

def run(args,root,env):subprocess.run([str(x) for x in args],cwd=root,env=env,check=True,timeout=1800)
def copy_exact(root,out,manifest):
    for name,row in manifest['files'].items():
        source=root/name
        if source.is_symlink() or not source.is_file() or hashlib.sha256(source.read_bytes()).hexdigest()!=row['sha256']:
            raise ValueError('source changed during isolated copy: '+name)
        dest=out/name;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(source,dest)
        dest.chmod(0o755 if row['executable'] else 0o644)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source-sha',required=True);a=p.parse_args()
    if not re.fullmatch('[0-9a-f]{40}',a.source_sha):p.error('full exact source base required')
    forbidden=('SDKCONFIG_DEFAULTS','EXTRA_COMPONENT_DIRS','CC','CXX','CFLAGS','CXXFLAGS','CPPFLAGS','LDFLAGS',
               'OTA_SIGNING_KEY','OTA_SIGNING_KEY_FILE','ENV','BASH_ENV')
    for name in os.environ:
        if name in forbidden or name.startswith(('IDF_COMPONENT_','TESLA_BLE_','CCACHE_','BASH_FUNC_')):
            raise ValueError('build injection or real-key environment refused: '+name)
    if not os.environ.get('IDF_PATH'):raise ValueError('use the pinned scripts/idf-docker.sh wrapper')
    env=dict(os.environ,IDF_CCACHE_ENABLE='0',EXTRA_CFLAGS='-fstack-usage',EXTRA_CXXFLAGS='-fstack-usage')
    env.pop('IDF_TARGET',None)
    run(['python3','scripts/check-config-repair-contract.py'],ROOT,env)
    run(['python3','scripts/check-dependency-contract.py','--root','.', '--idf-path',env['IDF_PATH']],ROOT,env)
    run(['python3','scripts/check-partition-contract.py','--csv','partitions.csv'],ROOT,env)
    inventory=load('repair_build_source',ROOT/'scripts/check-build-artifact-inventory.py')
    before=inventory.source_state(ROOT,a.source_sha);version='cfgrepair-'+before['sha256'][:16]
    output=ROOT/'maintenance-dist';output.mkdir(mode=0o700) # no deletion, reuse or partial-target option
    files={}
    paths=subprocess.check_output(['git','-c','core.fsmonitor=false','ls-files','--cached','--others','--exclude-standard','-z'],cwd=ROOT).split(b'\0')
    for raw in paths:
        if not raw or os.fsdecode(raw)=='version.txt':continue
        name=os.fsdecode(raw);path=ROOT/name
        if path.is_file():files[name]={'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'executable':bool(path.stat().st_mode&0o111)}
    manifest={'files':files}
    # Two complete exact-body source copies per target: no inherited objects, build directories,
    # managed workspace or old app. Nothing receives a private key. Copies/outputs stay ignored.
    (ROOT/'.tmp').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='config-repair-',dir=ROOT/'.tmp') as scratch:
        for target in TARGETS:
            retained=output/target;retained.mkdir()
            binaries=[]
            for phase in ('primary','repro'):
                copy=Path(scratch)/(target+'-'+phase);copy.mkdir();copy_exact(ROOT,copy,manifest)
                defaults=';'.join(str(copy/n) for n in ('sdkconfig.defaults','sdkconfig.defaults.'+target,
                    'maintenance/config-repair/sdkconfig.defaults','maintenance/config-repair/sdkconfig.defaults.'+target))
                base=['idf.py','-B','build','-D','TESLA_BUILD_ROLE=config-repair','-D','PROJECT_VER='+version,
                      '-D','SDKCONFIG_DEFAULTS='+defaults]
                run(base+['set-target',target],copy,env)
                run(['python3','scripts/check-config-repair-contract.py','--target',target,'--sdkconfig','sdkconfig'],copy,env)
                run(base+['build'],copy,env)
                run(['python3','scripts/check-build-semantics.py','--role','config-repair','--target',target,
                     '--sdkconfig','sdkconfig','--compile-commands','build/compile_commands.json','--source-root',copy],copy,env)
                run(['python3','scripts/check-partition-contract.py','--csv','partitions.csv','--binary','build/partition_table/partition-table.bin'],copy,env)
                run(['python3','scripts/check-otadata-contract.py','build/ota_data_initial.bin'],copy,env)
                run(['python3','scripts/check-config-repair-artifacts.py','--target',target,'--version',version,'--app','build/'+PROJECT+'.bin'],copy,env)
                for lock in ('dependencies.lock.'+target,):
                    if (copy/lock).read_bytes()!=(ROOT/lock).read_bytes():raise ValueError('lock changed')
                app=(copy/'build'/(PROJECT+'.bin')).read_bytes();elf=(copy/'build'/(PROJECT+'.elf')).read_bytes()
                binaries.append((app,elf))
                if phase=='primary':
                    for src,name in [('build/'+PROJECT+'.bin',PROJECT+'.bin'),('build/'+PROJECT+'.elf',PROJECT+'.elf'),
                                     ('build/'+PROJECT+'.map',PROJECT+'.map'),('sdkconfig','sdkconfig.'+target)]:
                        shutil.copyfile(copy/src,retained/name)
                    run(['python3','scripts/check-stack-usage.py','--target',target,'--stack-root','build/esp-idf/main',
                         '--write-observed',retained/('stack-usage-'+target+'.json')],copy,env)
                    with (retained/('size-'+target+'.json')).open('xb') as out:
                        subprocess.run(['python','-m','esp_idf_size','--format','json2','build/'+PROJECT+'.map'],cwd=copy,env=env,check=True,stdout=out,timeout=60)
            if binaries[0]!=binaries[1]:raise ValueError('independent maintenance app/ELF reproduction mismatch: '+target)
        for target in TARGETS:
            retained=output/target
            projected=((len((retained/(PROJECT+'.bin')).read_bytes())+0xffff)//0x10000)*0x10000+0x1000
            # Ordinary size caps are unchanged. Stack admission has a separate actual role/TU
            # inventory; its UNREVIEWED baseline intentionally refuses until all four enabled
            # observations have been independently reviewed. Never populate the normal baseline.
            run(['python3','scripts/report-firmware-size.py','--idf-size',retained/('size-'+target+'.json'),
                 '--unsigned-app',retained/(PROJECT+'.bin'),'--projected-signed-size',projected,
                 '--policy-limit',0x1e8000,'--target',target,'--budget-baseline','scripts/firmware-size-baseline.json','--enforce-budget'],ROOT,env)
            run(['python3','scripts/check-config-repair-stack.py','--role','config-repair','--target',target,'--observed-json',retained/('stack-usage-'+target+'.json'),
                 '--baseline','maintenance/config-repair/firmware-stack-baseline.json'],ROOT,env)
    if inventory.source_state(ROOT,a.source_sha)!=before:raise ValueError('source changed during maintenance build')
    validator=load('repair_artifact_manifest',ROOT/'scripts/check-config-repair-artifacts.py')
    document=validator.manifest(output,version,a.source_sha)
    (output/'config-repair-manifest.json').open('x').write(json.dumps(document,sort_keys=True,separators=(',',':'))+'\n')
    print('Unsigned maintenance-only four-target source/repro/resource contract completed; no signing or physical qualification.')
if __name__=='__main__':main()
