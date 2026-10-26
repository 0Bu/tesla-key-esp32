#!/usr/bin/env python3
"""Separate enabled-maintenance TU/large-frame admission; ordinary stack policy is unchanged."""
from __future__ import annotations
import argparse,importlib.util,json,re,sys,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    if spec is None or spec.loader is None:raise ValueError('missing canonical stack/source contract')
    module=importlib.util.module_from_spec(spec);sys.modules[name]=module;spec.loader.exec_module(module);return module
STACK=load('repair_canonical_stack',ROOT/'scripts/check-stack-usage.py')
SOURCE=load('repair_stack_source',ROOT/'scripts/config_repair_contract.py')
CANONICAL_FIELDS={'schemaVersion','baselineKind','toolchain','absoluteMaxBytes','reviewThresholdBytes','targets'}
ROLE_FIELDS={'role','project','qualification'}
def unique_object(pairs):
    result={}
    for key,value in pairs:
        if key in result:raise ValueError('duplicate maintenance stack baseline field')
        result[key]=value
    return result

def expected_units(root:Path)->set[str]:
    text=SOURCE.component_text(root,(root/'main/CMakeLists.txt').read_text(),'config-repair')
    blocks=re.findall(r'idf_component_register\s*\(\s*SRCS\s*(.*?)\n\s*(?:INCLUDE_DIRS|PRIV_INCLUDE_DIRS|REQUIRES|PRIV_REQUIRES|EMBED_FILES)\b',text,re.S)
    if len(blocks)!=1:raise ValueError('maintenance requires one closed actual source inventory')
    names=[]
    for line in blocks[0].splitlines():
        line=line.split('#',1)[0].strip()
        if not line:continue
        found=re.fullmatch(r'"([^"\n]+\.(?:c|cpp|cc|cxx))"',line)
        if found is None:raise ValueError('nonliteral maintenance source inventory')
        path=Path(found[1])
        if path.is_absolute() or '..' in path.parts or not (root/'main'/path).is_file() or (root/'main'/path).is_symlink():
            raise ValueError('maintenance owner escapes or is missing')
        names.append('CMakeFiles/__idf_main.dir/'+path.as_posix()+'.su')
    if not names or len(names)!=len(set(names)):raise ValueError('empty/duplicate maintenance owner inventory')
    return set(names)

def reviewed_baseline(path:Path,target:str,units:set[str]):
    if path.is_symlink() or not path.is_file():raise ValueError('maintenance baseline missing or symlinked')
    document=json.loads(path.read_text(),object_pairs_hook=unique_object)
    if not isinstance(document,dict) or set(document)!=CANONICAL_FIELDS|ROLE_FIELDS:
        raise ValueError('maintenance stack baseline fields differ from its explicit role schema')
    if (document['role'],document['project'])!=('config-repair','tesla-key-config-repair'):
        raise ValueError('maintenance stack baseline role/project crossing')
    if document['qualification']!='reviewed':
        raise ValueError('maintenance stack observations are UNREVIEWED; no enabled frame maxima admitted')
    targets=document['targets']
    if not isinstance(targets,dict) or set(targets)!=set(SOURCE.TARGETS):raise ValueError('maintenance stack requires exactly all four targets')
    for name,entry in targets.items():
        if not isinstance(entry,dict) or set(entry)!={'files'} or not isinstance(entry['files'],dict) or set(entry['files'])!=units:
            raise ValueError('maintenance reviewed TU inventory differs from its actual enabled role: '+name)
    canonical={key:document[key] for key in CANONICAL_FIELDS}
    # Reuse the unchanged numeric/frame validator. The temporary canonical view is not a
    # qualification record or an accepted substitute for the role-tagged reviewed owner file.
    with tempfile.TemporaryDirectory(prefix='maintenance-stack-view-') as temporary:
        canonical_path=Path(temporary)/'canonical.json';canonical_path.write_text(json.dumps(canonical))
        admitted={name:STACK.load_baseline(canonical_path,name) for name in SOURCE.TARGETS}
    return admitted[target]

def validate(root:Path,target:str,observed:Path,baseline:Path,role:str):
    if role!='config-repair' or target not in SOURCE.TARGETS:raise ValueError('wrong maintenance stack role/target')
    units=expected_units(root)
    frames=STACK.frames_from_document(observed,target)
    if set(frames)!=units:raise ValueError('maintenance observed TU inventory differs from its actual enabled role')
    failures=STACK.compare(frames) # absolute maximum applies even before independent review exists
    if failures:raise ValueError('; '.join(failures))
    admitted=reviewed_baseline(baseline,target,units)
    failures=STACK.compare(frames,admitted)
    if failures:raise ValueError('; '.join(failures))
    return len(frames)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--role',required=True,choices=('config-repair',));p.add_argument('--target',required=True,choices=SOURCE.TARGETS)
    p.add_argument('--observed-json',required=True,type=Path);p.add_argument('--baseline',required=True,type=Path)
    p.add_argument('--source-root',type=Path,default=ROOT);a=p.parse_args()
    try:count=validate(a.source_root,a.target,a.observed_json,a.baseline,a.role)
    except (OSError,UnicodeError,ValueError) as error:
        print('maintenance stack admission failed: '+str(error),file=sys.stderr);return 1
    print('maintenance stack admission: PASS ('+a.target+', '+str(count)+' enabled TUs)');return 0
if __name__=='__main__':raise SystemExit(main())
