#!/usr/bin/env python3
"""Offline actual deploy-step execution, structured gates and disposable RSA fixtures."""
from pathlib import Path
import copy,hashlib,importlib.util,json,os,shutil,subprocess,sys,tempfile

ROOT=Path(__file__).resolve().parents[1]
def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    module=importlib.util.module_from_spec(spec);sys.modules[name]=module;spec.loader.exec_module(module)
    return module
policy=load('deploy_policy',ROOT/'scripts/check-workflow-policy.py')
artifact=load('deploy_artifact',ROOT/'scripts/check-firmware-artifacts.py')
workflow=ROOT/'.github/workflows/build.yml';text=workflow.read_text()
steps=policy.workflow_model(workflow,text)['jobs']['deploy']['steps']
name='Validate downloaded signed root and local Pages bytes'
index=next(i for i,s in enumerate(steps) if s.get('name')==name)
step=steps[index];body=step['run'];policy.validate_deploy_signed_candidate(steps)
rows=[];targets=[('esp32',''),('esp32s3','-s3'),('esp32c3','-c3'),('esp32c6','-c6')]

def refused(label,changed):
    try:policy.validate_deploy_signed_candidate(changed)
    except policy.PolicyError:rows.append(label)
    else:raise AssertionError('unsafe structured deploy admitted: '+label)
for label,change in [
    ('if-false',lambda s:s.update({'if':False})),
    ('continue-on-error',lambda s:s.update({'continue-on-error':True})),
    ('background',lambda s:s.update(run=s['run'].replace('done\n','done &\n'))),
    ('ignore-status',lambda s:s.update(run=s['run'].replace('--expected-public-key-digest scripts/ota-signing-public-key.sha256','--expected-public-key-digest scripts/ota-signing-public-key.sha256 || true'))),
    ('missing-target',lambda s:s.update(run=s['run'].replace(" 'esp32c6|-c6'",''))),
    ('wrong-pin',lambda s:s.update(run=s['run'].replace('scripts/ota-signing-public-key.sha256','scripts/wrong.sha256'))),
    ('wrong-version',lambda s:s.update(run=s['run'].replace('"$DISPLAY_VERSION"','"1.0.0"'))),
    ('missing-signed-flag',lambda s:s.update(run=s['run'].replace(' --signed-app',''))),
]:
    changed=copy.deepcopy(steps);change(changed[index]);refused(label,changed)
changed=copy.deepcopy(steps);changed.pop(index);refused('missing-consumer',changed)
changed=copy.deepcopy(steps);changed.insert(index,copy.deepcopy(step));refused('duplicate-consumer',changed)
for boundary in ('Download exact signed Release and Pages candidate','Deploy root site to gh-pages','Deploy dev site to gh-pages'):
    changed=copy.deepcopy(steps);consumer=changed.pop(index)
    destination=next(i for i,s in enumerate(changed) if s.get('name')==boundary)
    changed.insert(destination if boundary.startswith('Download') else destination+1,consumer)
    refused('wrong-order-'+boundary,changed)

with tempfile.TemporaryDirectory(prefix='deploy-consumer-') as temporary:
    root=Path(temporary);tools=root/'tools';tools.mkdir();scripts=root/'scripts';scripts.mkdir()
    proxy=tools/'python3'
    proxy.write_text('#!'+sys.executable+'\n'+'''from pathlib import Path
import json,os,sys
a=sys.argv[1:]
with Path(os.environ['SPY_LOG']).open('a') as out:out.write(json.dumps(a)+'\\n')
if a[0]=='scripts/check-firmware-artifacts.py':
 if os.environ.get('REAL_RSA')=='1':os.execv(sys.executable,[sys.executable,str(Path(os.environ['SOURCE_ROOT'])/a[0]),*a[1:]])
 if a[a.index('--target')+1]==os.environ.get('FAIL_TARGET'):sys.exit(37)
''');proxy.chmod(0o700)
    log=root/'calls.jsonl';marker=root/'branch-write-reached'
    env={k:v for k,v in os.environ.items() if not k.startswith(('GIT_','PYTHON','LD_','DYLD_')) and k not in ('GH_TOKEN','GITHUB_TOKEN','OTA_SIGNING_KEY','OTA_SIGNING_KEY_FILE','ENV','BASH_ENV')}
    env.update(PATH=str(tools)+os.pathsep+os.environ.get('PATH',''),DISPLAY_VERSION='1.6.3-dev.999',SOURCE_SHA='0'*40,SPY_LOG=str(log),SOURCE_ROOT=str(ROOT))
    def execute(extra,expect):
        if log.exists():log.unlink()
        if marker.exists():marker.unlink()
        result=subprocess.run(['bash','-c',body+'printf reached > branch-write-reached\n'],cwd=root,env={**env,**extra},capture_output=True,timeout=30)
        assert result.returncode==expect,(extra,result.returncode,result.stderr)
        assert marker.exists()==(expect==0)
        calls=[json.loads(line) for line in log.read_text().splitlines()]
        signatures=[a for a in calls if a[0]=='scripts/check-firmware-artifacts.py']
        for a,(target,suffix) in zip(signatures,targets):
            assert a==['scripts/check-firmware-artifacts.py','--target',target,'--version',env['DISPLAY_VERSION'],'--signed-app','--bootloader',f'_deploy-input/_fw/{target}/bootloader.bin','--app',f'_deploy-input/tesla-key-esp32{suffix}.bin','--expected-public-key-digest','scripts/ota-signing-public-key.sha256']
        return signatures
    for channel in ('release','dev'):
        assert len(execute({'CHANNEL':channel},0))==4
        rows.append(channel+'-actual-shell-four-targets')
        for i,(target,_) in enumerate(targets):
            assert len(execute({'CHANNEL':channel,'FAIL_TARGET':target},37))==i+1
            rows.append(channel+'-exit37-'+target)
    apps={}
    for target,suffix in targets:
        directory=root/'_deploy-input/_fw'/target;directory.mkdir(parents=True)
        (directory/'bootloader.bin').write_bytes(artifact.make_image(target,env['DISPLAY_VERSION'],app=False))
        app=root/'_deploy-input'/('tesla-key-esp32'+suffix+'.bin')
        data=artifact.fake_sign(artifact.make_image(target,env['DISPLAY_VERSION'],app=True));app.write_bytes(data);apps[target]=(app,data)
    offset=len(data)-artifact.SIGNATURE_SECTOR
    pin=hashlib.sha256(data[offset+artifact.RSA_N_OFFSET:offset+artifact.RSA_PUBLIC_FIELDS_END]).hexdigest()
    pinfile=scripts/'ota-signing-public-key.sha256';pinfile.write_text(pin+'\n')
    assert len(execute({'REAL_RSA':'1'},0))==4;rows.append('actual-shell-four-disposable-rsa-positive')
    for i,(target,_) in enumerate(targets):
        app,data=apps[target];bad=bytearray(data);bad[40]^=1;app.write_bytes(bad)
        assert len(execute({'REAL_RSA':'1'},1))==i+1
        app.write_bytes(data);rows.append('actual-shell-rsa-tamper-'+target)
    pinfile.write_text('0'*64+'\n');assert len(execute({'REAL_RSA':'1'},1))==1
    rows.append('actual-shell-wrong-pin');pinfile.write_text(pin+'\n')
    assert len(execute({'REAL_RSA':'1'},0))==4;rows.append('actual-shell-rsa-restored')

    # Test-only in-memory digest admission isolates semantic gates from the raw-job hash.
    # It neither refreshes the source inventory nor supplies independent job approval.
    fixture=root/'whole-policy';shutil.copytree(ROOT,fixture,ignore=shutil.ignore_patterns('__pycache__'))
    file=fixture/'.github/workflows/build.yml';original=file.read_text()
    _,jobs=policy.split_jobs(original,'build.yml');deploy=jobs['deploy']
    old_digest=policy.EXPECTED_PRIVILEGED_JOB_SHA256[('build.yml','deploy')]
    policy.EXPECTED_PRIVILEGED_JOB_SHA256[('build.yml','deploy')]=hashlib.sha256(deploy.encode()).hexdigest()
    policy.validate(fixture);rows.append('whole-policy-test-only-hash-positive')
    for label,changed in [
        ('whole-policy-if-false',deploy.replace('      - name: '+name+'\n','      - name: '+name+'\n        if: false\n')),
        ('whole-policy-wrong-pin',deploy.replace('--expected-public-key-digest scripts/ota-signing-public-key.sha256','--expected-public-key-digest scripts/wrong.sha256')),
    ]:
        assert changed!=deploy
        file.write_text(original.replace(deploy,changed,1))
        policy.EXPECTED_PRIVILEGED_JOB_SHA256[('build.yml','deploy')]=hashlib.sha256(changed.encode()).hexdigest()
        try:policy.validate(fixture)
        except policy.PolicyError as error:
            expected='mandatory four-target production-pin consumer' if label.endswith('if-false') else 'exact SHA/version artifact'
            assert expected in str(error),str(error)
        else:raise AssertionError(label+' was admitted')
        rows.append(label)
        if label=='whole-policy-if-false':
            # A valid removal of only the new actual consumer call defeats this oracle.
            owner=fixture/'scripts/check-workflow-policy.py';actual=owner.read_text()
            call='    validate_deploy_signed_candidate(deploy_steps)'
            assert actual.count(call)==1
            owner.write_text(actual.replace(call,'    pass  # owned omission control'))
            compile(owner.read_text(),str(owner),'exec')
            omitted=load('deploy_policy_consumer_omitted',owner)
            omitted.EXPECTED_PRIVILEGED_JOB_SHA256[('build.yml','deploy')]=hashlib.sha256(changed.encode()).hexdigest()
            omitted.validate(fixture);rows.append('whole-policy-valid-consumer-omission-admits-if-false')
            owner.write_text(actual)
    file.write_text(original);policy.EXPECTED_PRIVILEGED_JOB_SHA256[('build.yml','deploy')]=hashlib.sha256(deploy.encode()).hexdigest()
    policy.validate(fixture);rows.append('whole-policy-restored')
    policy.EXPECTED_PRIVILEGED_JOB_SHA256[('build.yml','deploy')]=old_digest
print(json.dumps({'result':'PASS','cases':len(rows),'rows':rows,'scope':'Actual YAML consumer and canonical RSA verifier with disposable fixture key only; initial inventory CLIs are explicit inert spies. No network, production key, Git mutation, SDK, publication or hardware.'}))
