#!/usr/bin/env python3
"""Actual preview helper/cleanup producer controls with injected HTTP and fake time."""
import base64
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
from urllib.parse import parse_qs, urlsplit

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('preview_served', ROOT / 'scripts/check-pr-preview-served.py')
M = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = M
spec.loader.exec_module(M)
META = {'url':'https://api.github.com/repos/owner/project/pages','build_type':'legacy',
        'source':{'branch':'gh-pages','path':'/'},'html_url':'https://owner.github.io/project/'}
BASE = M.preview_base(META, 'owner/project', 12)
EXPECTED = {name: b'fixture-' + name.encode() for name in ('manifest.json', *M.PARTS)}


class Rig:
    def __init__(self, edit=None, latency=0):
        self.time = 0
        self.calls = []
        self.edit, self.latency = edit, latency
    def sleep(self, value):self.time += value
    def fetch(self, url, maximum, timeout):
        self.calls.append((url, maximum, timeout))
        self.time += self.latency
        name = urlsplit(url).path.split('/')[-1]
        result = (200, EXPECTED.get(name, b'fixture-other-path'))
        return self.edit(url, result, self) if self.edit else result
    def verify(self, expected=EXPECTED, absent=None, **kwargs):
        return M.verify(BASE, expected, absent, fetcher=self.fetch, clock=lambda:self.time,
                        sleeper=self.sleep, **kwargs)


class ServedControls(unittest.TestCase):
    def test_all_parts_and_final_manifest(self):
        rig = Rig();result = rig.verify()
        self.assertEqual(result['result'], 'published-accepted')
        self.assertEqual(len(rig.calls), 18)
        self.assertEqual(len({parse_qs(urlsplit(u).query)['accept'][0] for u,_,_ in rig.calls}), 1)
        self.assertTrue(all(urlsplit(u).path.startswith('/project/PR/12/') for u,_,_ in rig.calls))
    def test_wrong_part(self):
        rig = Rig(lambda u,r,s:(r[0],r[1]+b'x') if u.split('?')[0].endswith('tesla-key-esp32-c6.bin') else r)
        with self.assertRaises(M.AcceptanceError):rig.verify(attempts=1)
    def test_mixed_end_manifest(self):
        rig = Rig(lambda u,r,s:(200,b'mixed') if len(s.calls)==18 else r)
        with self.assertRaises(M.AcceptanceError):rig.verify(attempts=1)
    def test_http_status_required(self):
        for status in (301,302,304,404,410,500,503):
            with self.subTest(status=status),self.assertRaises(M.AcceptanceError):
                Rig(lambda u,r,s:(status,r[1])).verify(attempts=1)
    def test_retry_is_complete_and_fresh(self):
        rig = Rig(lambda u,r,s:(200,b'stale') if len(s.calls)==8 else r)
        self.assertEqual(rig.verify(attempts=2)['attempts'],2)
        self.assertEqual(len(rig.calls),26)
        tokens=[parse_qs(urlsplit(u).query)['accept'][0] for u,_,_ in rig.calls]
        self.assertNotEqual(tokens[0],tokens[8])
        self.assertTrue(rig.calls[8][0].split('?')[0].endswith('/manifest.json'))
    def test_overall_deadline_and_sleep_bound(self):
        rig=Rig(lambda u,r,s:(503,b''),latency=3)
        with self.assertRaises(M.AcceptanceError):rig.verify(attempts=10,budget=7,interval=5)
        self.assertLessEqual(rig.time,7)
        self.assertEqual(len(rig.calls),1)
    def test_fetch_overrun_is_not_success(self):
        with self.assertRaises(M.AcceptanceError):Rig(latency=2).verify(attempts=1,budget=1)
    def test_final_response_cannot_exceed_deadline(self):
        def late(url,result,rig):
            if len(rig.calls)==18:rig.time+=2
            return result
        with self.assertRaises(M.AcceptanceError):Rig(late).verify(attempts=1,budget=1)
    def test_absent_all_canonical_and_inventory_paths(self):
        paths=M.removal_paths({'pr':12,'result':'removed','paths':['assets/old.js']},12)
        rig=Rig(lambda u,r,s:(410,b'gone'))
        result=rig.verify(expected=None,absent=paths)
        self.assertIn('assets/old.js',result['checkedPaths'])
        self.assertEqual(len(result['checkedPaths']),19)
        self.assertEqual(len(result['checkedUrls']),38)
        self.assertEqual(sum(not urlsplit(u).query for u in result['checkedUrls']),19)
    def test_fresh_404_does_not_hide_stale_canonical_url(self):
        for canonical_status,fresh_status in ((200,404),(404,200)):
            rig=Rig(lambda u,r,s:(fresh_status if urlsplit(u).query else canonical_status,b'stale'))
            with self.assertRaises(M.AcceptanceError):
                rig.verify(expected=None,absent=['manifest.json'],attempts=1)
    def test_still_served_part_prevents_absence(self):
        rig=Rig(lambda u,r,s:(200,b'stale') if 'tesla-key-esp32-c3.bin?' in u else (404,b'gone'))
        with self.assertRaises(M.AcceptanceError):
            rig.verify(expected=None,absent=M.removal_paths({'pr':12,'result':'already-absent','paths':[]},12),attempts=1)
    def test_network_failure_never_proves_absence(self):
        def fail(*_):raise OSError('fixture DNS/TLS failure')
        with self.assertRaises(M.AcceptanceError):M.verify(BASE,None,['manifest.json'],fetcher=fail,attempts=1)
    def test_kept_has_no_absence_claim(self):
        self.assertIsNone(M.removal_paths({'pr':12,'result':'kept-eligible','paths':[]},12))
    def test_malformed_cleanup_decisions(self):
        for value in ({},{'pr':12,'result':'removed'}, {'pr':13,'result':'removed','paths':[]},
                      {'pr':12,'result':'kept-eligible','paths':['index.html']},
                      {'pr':12,'result':'removed','paths':['../outside']},
                      {'pr':12,'result':'removed','paths':['x']*257}):
            with self.subTest(value=value),self.assertRaises(M.AcceptanceError):M.removal_paths(value,12)
    def test_authority_and_path_refusals(self):
        for field,value in (('url','https://api.github.com/repos/other/project/pages'),
                            ('html_url','https://user@owner.github.io/project/'),
                            ('html_url','https://owner.github.io/project/../other/'),
                            ('html_url','https://owner.github.io/project/?stale=1')):
            bad=copy.deepcopy(META);bad[field]=value
            with self.subTest(value=value),self.assertRaises((M.AcceptanceError,ValueError)):M.preview_base(bad,'owner/project',12)
        for name in ('../x','/x','x//y','x?redirect=1','x%2fy','x\\y'):
            with self.assertRaises(M.AcceptanceError):M.url_for(BASE,name,'fixture')
    def test_duplicate_manifest_fields(self):
        with self.assertRaises(M.AcceptanceError):M.decode(b'{"a":1,"a":2}')
    def test_oversized_absence_response(self):
        rig=Rig(lambda u,r,s:(404,b'x'*(M.MAX_MANIFEST+1)))
        with self.assertRaises(M.AcceptanceError):rig.verify(expected=None,absent=['manifest.json'],attempts=1)


class HttpsTransport(unittest.TestCase):
    def run_fetch(self,*,status=200,body=b'data',effective=None,code=0,timeout=False):
        url=M.url_for(BASE,'manifest.json','test')
        def child(command,**kwargs):
            self.command,self.options=command,kwargs
            Path(command[command.index('--output')+1]).write_bytes(body)
            if timeout:raise subprocess.TimeoutExpired(command,1)
            return subprocess.CompletedProcess(command,code,f'{status}\n{effective or url}'.encode(),b'')
        with mock.patch.object(M.subprocess,'run',side_effect=child):return M.fetch_https(url,4,1)
    def test_actual_transport_command_and_bounds(self):
        self.assertEqual(self.run_fetch(),(200,b'data'))
        self.assertEqual(self.command[:2],['/usr/bin/curl','-q'])
        self.assertNotIn('--location',self.command);self.assertNotIn('-L',self.command)
        self.assertEqual(self.command[self.command.index('--proxy')+1],'')
        self.assertEqual(self.command[self.command.index('--max-filesize')+1],'4')
        self.assertEqual(self.command[self.command.index('--proto')+1],'=https')
        self.assertEqual(self.options['timeout'],1.25)
        self.assertNotIn('GH_TOKEN',self.options['env'])
        self.assertNotIn('OTA_SIGNING_KEY',self.options['env'])
    def test_transfer_error_redirect_and_oversize(self):
        for kwargs in ({'code':28},{'timeout':True},{'effective':'https://other.example/part'}, {'body':b'12345'}):
            with self.subTest(kwargs=kwargs),self.assertRaises(M.AcceptanceError):self.run_fetch(**kwargs)


class LocalSignedSite(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary=tempfile.TemporaryDirectory(prefix='preview-site-fixture-')
        cls.site=Path(cls.temporary.name)
        cls.manifest=M.load_module('check-pages-manifest.py').make_test_site(cls.site)
        cls.firmware=M.load_module('check-firmware-artifacts.py')
        cls.manifest['version']='1.2.3-PR-12'
        for target,suffix in M.TARGETS:
            boot=cls.site/f'bootloader-{target}.bin'
            boot.write_bytes(cls.firmware.make_image(target,'unused',app=False))
            app=cls.site/f'tesla-key-esp32{suffix}.bin'
            app.write_bytes(cls.firmware.fake_sign(cls.firmware.make_image(target,'1.2.3-PR-12',app=True)))
        for build in cls.manifest['builds']:
            for part in build['parts']:
                raw=(cls.site/part['path']).read_bytes()
                part.update(size=len(raw),sha256=hashlib.sha256(raw).hexdigest())
        (cls.site/'manifest.json').write_text(json.dumps(cls.manifest))
        cls.pin=cls.site/'fixture-pin.sha256'
        info=cls.firmware.parse_image((cls.site/'tesla-key-esp32.bin').read_bytes(),signed=True,expect_app_desc=True)
        cls.pin.write_text(info.public_key_digest+'\n')
    @classmethod
    def tearDownClass(cls):cls.temporary.cleanup()
    def test_actual_four_target_signed_site(self):
        self.assertEqual(len(M.prepare_site(self.site,'a'*40,'1.2.3-PR-12',12,self.pin)),17)
    def test_source_version_and_pr_refusals(self):
        for source,version,pr in (('b'*40,'1.2.3-PR-12',12),('a'*40,'1.2.4-PR-12',12),('a'*40,'1.2.3-PR-12',13)):
            with self.assertRaises((M.AcceptanceError,RuntimeError,ValueError)):M.prepare_site(self.site,source,version,pr,self.pin)
    def test_wrong_public_pin(self):
        wrong=self.site/'wrong.sha256';wrong.write_text('0'*64+'\n')
        with self.assertRaises(ValueError):M.prepare_site(self.site,'a'*40,'1.2.3-PR-12',12,wrong)
    def test_local_manifest_version_must_match_expected(self):
        file=self.site/'manifest.json';raw=file.read_bytes()
        bad=copy.deepcopy(self.manifest);bad['version']='1.2.4-PR-12'
        try:
            file.write_text(json.dumps(bad))
            with self.assertRaises(RuntimeError):M.prepare_site(self.site,'a'*40,'1.2.3-PR-12',12,self.pin)
        finally:file.write_bytes(raw)


class CleanupProducer(unittest.TestCase):
    def run_producer(self,state,*,bad_api=False,bad_inventory=False):
        with tempfile.TemporaryDirectory(prefix='preview-cleanup-fixture-') as temporary:
            root=Path(temporary);scripts=root/'scripts';scripts.mkdir()
            shutil.copy2(ROOT/'scripts/reconcile-pr-previews.sh',scripts/'reconcile-pr-previews.sh')
            (scripts/'publish-pages-branch.sh').write_text('#!/bin/bash\nprintf "removed" > "$FIXTURE_REMOVE"\n')
            (scripts/'publish-pages-branch.sh').chmod(0o700)
            binpath=root/'bin';binpath.mkdir()
            manifest=base64.b64encode(json.dumps({'layoutVersion':2,'sourceSha':'a'*40}).encode()).decode()
            tree={'truncated':False,'tree':[] if state=='absent' else [
                {'type':'blob','path':'PR/12/manifest.json','sha':'b'*40},
                {'type':'blob','path':'PR/12/'+('../unsafe' if bad_inventory else 'asset.js'),'sha':'c'*40}]}
            pr={'state':'open' if state=='eligible' else 'closed','head':{'sha':'a'*40,'repo':{'full_name':'owner/project'}},'labels':[{'name':'signed-preview'}]}
            replies={f'repos/owner/project/git/trees/gh-pages?recursive=1':tree,
                     'repos/owner/project/pulls/12':pr,
                     'repos/owner/project/git/blobs/'+'b'*40:{'encoding':'base64','content':manifest}}
            gh=binpath/'gh'
            gh.write_text('#!'+sys.executable+'\nimport json,sys\nr='+repr(replies)+'\n'
                          +'sys.exit(3)\n' if bad_api else '#!'+sys.executable+'\nimport json,sys\nr='+repr(replies)+'\nprint(json.dumps(r[sys.argv[2]]))\n')
            gh.chmod(0o700)
            env={k:v for k,v in os.environ.items() if k in ('PATH','LANG','TMPDIR')}
            env.update(PATH=str(binpath)+os.pathsep+env['PATH'],GH_TOKEN='fixture-only',GITHUB_TOKEN='fixture-only',
                       GITHUB_REPOSITORY='owner/project',FIXTURE_REMOVE=str(root/'removed'))
            run=subprocess.run(['bash',str(scripts/'reconcile-pr-previews.sh'),'remove-if-stale','12'],capture_output=True,env=env,timeout=10)
            return run,(root/'removed').exists()
    def test_actual_absent_decision(self):
        run,removed=self.run_producer('absent');self.assertEqual(run.returncode,0,run.stderr)
        self.assertEqual(M.removal_paths(M.decode(run.stdout),12),sorted(('manifest.json','index.html',*M.PARTS)))
        self.assertFalse(removed)
    def test_actual_kept_decision(self):
        run,removed=self.run_producer('eligible');self.assertEqual(run.returncode,0,run.stderr)
        self.assertIsNone(M.removal_paths(M.decode(run.stdout),12));self.assertFalse(removed)
    def test_actual_removed_inventory(self):
        run,removed=self.run_producer('closed');self.assertEqual(run.returncode,0,run.stderr)
        self.assertEqual(M.decode(run.stdout)['result'],'removed');self.assertTrue(removed)
        self.assertIn('asset.js',M.decode(run.stdout)['paths'])
    def test_api_failure_and_unsafe_inventory_stop_before_deletion(self):
        for kwargs in ({'bad_api':True},{'bad_inventory':True}):
            run,removed=self.run_producer('closed',**kwargs)
            self.assertNotEqual(run.returncode,0);self.assertFalse(removed);self.assertFalse(run.stdout)


if __name__=='__main__':unittest.main()
