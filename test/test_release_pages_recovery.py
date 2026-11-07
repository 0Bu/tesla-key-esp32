#!/usr/bin/env python3
"""Exercise authenticated recovery bytes and authority changes with passive GitHub/Git I/O."""
from copy import deepcopy
from contextlib import redirect_stderr, redirect_stdout
import hashlib
import io
import importlib.util
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('release_recovery', ROOT / 'scripts/recover-release-pages.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
SOURCE, MAIN, VERSION = 'a' * 40, 'b' * 40, '1.2.3'


def evidence():
    run = dict(id=123, head_sha=SOURCE, head_branch='main', path='.github/workflows/build.yml',
               status='completed', conclusion='success', event='workflow_dispatch')
    names = ['logic-test', 'logic-harness', 'build', 'independent-rebuild', 'publish']
    for target in ('esp32', 'esp32s3', 'esp32c3', 'esp32c6'):
        names += [f'build-target ({target}, primary)', f'independent-rebuild-target ({target})']
    jobs = [dict(name=name, conclusion='success', steps=[]) for name in names]
    by_name = {job['name']: job for job in jobs}
    for job, names in [('logic-test', ['Run host-side logic tests']),
                       ('logic-harness', ['Run tesla-ble integration harness', 'Prove and run Linux memory-safety instrumentation']),
                       ('publish', ['Validate build provenance', 'Bind all 16 local Pages parts before any publication',
                                    'Upload signed firmware artifacts', 'Verify production signing authority for all staged apps',
                                    'Provision OTA signing key', 'Sign and stage release artifacts', 'Remove OTA signing key'])]:
        by_name[job]['steps'] = [dict(name=name, conclusion='success') for name in names]
    return run, jobs


class Recovery(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='release-recovery-test-')
        cls.work = Path(cls.temporary.name)
        reuse = m.sibling('prepare-reused-release')
        cls.artifact, download, cls.release = reuse.write_fixture(cls.work, VERSION, SOURCE)
        cls.pin = reuse.fixture_key_digest(download)
        reuse.validate_and_stage(cls.release, download, cls.artifact, cls.artifact / '_fw', VERSION, SOURCE, cls.pin)
        (cls.artifact / 'dist/build-metadata.txt').write_text(f'head_sha={SOURCE}\ndisplay_version={VERSION}\n')
        site = cls.artifact / '_site'; site.mkdir()
        manifest = m.sibling('check-pages-manifest').make_test_site(site)
        for build, target in zip(manifest['builds'], ('esp32', 'esp32s3', 'esp32c3', 'esp32c6')):
            for part, name in zip(build['parts'], ('bootloader.bin', 'partition-table.bin', 'tesla-key-esp32.bin', 'ota_data_initial.bin')):
                data = (cls.artifact / '_fw' / target / name).read_bytes()
                (site / part['path']).write_bytes(data)
                part.update(size=len(data), sha256=hashlib.sha256(data).hexdigest())
        (site / 'manifest.json').write_text(json.dumps(manifest))
        (site / 'changelog.json').write_text(json.dumps(dict(version=VERSION, changelog='A bounded change.')))
        (site / 'index.html').write_text('<!doctype html>Original installer')
        (site / 'installer-bootstrap.mjs').write_text('export const original = true;')
        cls.archive = cls.work / 'original.zip'
        with zipfile.ZipFile(cls.archive, 'w') as bundle:
            for name, data in m.snapshot_tree(cls.artifact).items():
                bundle.writestr(name, data)
        cls.digest = 'sha256:' + hashlib.sha256(cls.archive.read_bytes()).hexdigest()
        cls.remote_artifact = dict(id=9, name=f'tesla-key-esp32-{VERSION}-{SOURCE}', expired=False, digest=cls.digest)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def clients(self, release=None, *, main=MAIN, tag=SOURCE, ancestor=True, run=None, jobs=None):
        release = release or self.release
        default_run, default_jobs = evidence()
        def api(endpoint, *, pages=False):
            values = {'git/ref/heads/main': {'object': {'sha': main}},
                      'releases/latest': release, f'releases/tags/v{VERSION}': release,
                      f'git/ref/tags/v{VERSION}': {'object': {'type': 'commit', 'sha': tag}},
                      'actions/runs/123': run or default_run,
                      'actions/runs/123/artifacts': [{'artifacts': [self.remote_artifact]}],
                      'actions/runs/123/jobs': [{'jobs': jobs or default_jobs}],
                      'pages': {'build_type': 'legacy', 'source': {'branch': 'gh-pages', 'path': '/'},
                                'html_url': 'https://0bu.github.io/tesla-key-esp32/'}}
            self.assertIn(endpoint, values)
            return deepcopy(values[endpoint])
        def command(*args, **kwargs):
            if args == ('git', 'remote', 'get-url', 'origin'): return f'https://github.com/{m.REPOSITORY}.git'
            if args[-1] == '--porcelain': return ''
            if args == ('git', 'rev-parse', 'HEAD'): return MAIN
            if args[:3] == ('git', 'merge-base', '--is-ancestor'):
                self.assertEqual(args[3:], (SOURCE, main))
                if not ancestor: raise subprocess.CalledProcessError(1, args)
                return ''
            self.fail(f'unexpected external action: {args}')
        return patch.object(m, 'api', api), patch.object(m, 'command', command)

    def verify(self, **kwargs):
        changes = kwargs.pop('changes', {})
        artifact = kwargs.pop('artifact', self.artifact)
        pin = kwargs.pop('expected_pin', self.pin)
        api, command = self.clients(**changes)
        with api, command, patch.dict(os.environ, {}, clear=True):
            return m.verify(artifact, 123, expected_pin=pin, archive_digest=self.digest, **kwargs)

    def test_published_source_after_main_advanced_and_refusals(self):
        self.assertEqual(self.verify()[1:3], (SOURCE, VERSION))
        for changes in ({'main': 'c' * 40}, {'tag': 'c' * 40}, {'ancestor': False}):
            with self.subTest(changes=changes), self.assertRaises((m.RecoveryError, subprocess.CalledProcessError)):
                self.verify(changes=changes)
        for key, value in [('immutable', False), ('draft', True), ('prerelease', True), ('target_commitish', 'c' * 40)]:
            release = deepcopy(self.release); release[key] = value
            with self.subTest(key=key), self.assertRaises((m.RecoveryError, ValueError)):
                self.verify(changes={'release': release})
        with self.assertRaises(m.RecoveryError): self.verify(expected_id=999)
        with self.assertRaises(ValueError): self.verify(expected_pin=b'\0' * 32)

    def test_full_coverage_key_removal_reuse_and_post_publication_failure(self):
        run, jobs = evidence()
        m.validate_provenance(run, [self.remote_artifact], jobs, SOURCE, VERSION)
        for conclusion in ('cancelled', 'timed_out', 'action_required', None):
            refused = dict(run, conclusion=conclusion)
            with self.subTest(run_conclusion=conclusion), self.assertRaises(m.RecoveryError):
                m.validate_provenance(refused, [self.remote_artifact], jobs, SOURCE, VERSION)
        run['conclusion'] = 'failure'
        next(job for job in jobs if job['name'] == 'publish')['conclusion'] = 'failure'
        # The independent immutable Release check still supplies publication authority.
        self.verify(changes={'jobs': jobs})
        for job in jobs:
            for step in job['steps']:
                duplicated = deepcopy(jobs)
                next(j for j in duplicated if j['name'] == job['name'])['steps'].append(dict(step, conclusion='failure'))
                with self.subTest(duplicate=step['name']), self.assertRaises(m.RecoveryError):
                    m.validate_provenance(run, [self.remote_artifact], duplicated, SOURCE, VERSION)
                for status in ('skipped', 'failure'):
                    changed = deepcopy(jobs)
                    next(s for j in changed if j['name'] == job['name'] for s in j['steps']
                         if s['name'] == step['name'])['conclusion'] = status
                    with self.subTest(step=step['name'], status=status), self.assertRaises(m.RecoveryError):
                        m.validate_provenance(run, [self.remote_artifact], changed, SOURCE, VERSION)
        for omitted in ('logic-test', 'build-target (esp32c6, primary)', 'independent-rebuild-target (esp32s3)'):
            with self.assertRaises(m.RecoveryError):
                m.validate_provenance(run, [self.remote_artifact], [j for j in jobs if j['name'] != omitted], SOURCE, VERSION)
        publish = next(j for j in jobs if j['name'] == 'publish')
        for step in publish['steps']:
            if step['name'] in ('Provision OTA signing key', 'Sign and stage release artifacts', 'Remove OTA signing key'):
                step['conclusion'] = 'skipped'
        publish['steps'].append(dict(name='Reuse and stage exact immutable Release bytes', conclusion='success'))
        m.validate_provenance(run, [self.remote_artifact], jobs, SOURCE, VERSION)
        duplicate = deepcopy(jobs)
        next(j for j in duplicate if j['name'] == 'publish')['steps'].append(dict(name='Reuse and stage exact immutable Release bytes', conclusion='success'))
        with self.assertRaises(m.RecoveryError): m.validate_provenance(run, [self.remote_artifact], duplicate, SOURCE, VERSION)
        publish['steps'][0]['conclusion'] = 'failure'
        with self.assertRaises(m.RecoveryError): m.validate_provenance(run, [self.remote_artifact], jobs, SOURCE, VERSION)

    def test_authenticated_zip_includes_installer_bytes_and_rejects_tampering(self):
        files = m.archive_files(self.archive, self.digest)
        self.assertEqual(files['_site/installer-bootstrap.mjs'], b'export const original = true;')
        tampered = self.work / 'tampered.zip'
        with zipfile.ZipFile(tampered, 'w') as archive:
            for name, data in files.items():
                archive.writestr(name, b'changed' if name == '_site/installer-bootstrap.mjs' else data)
        with self.assertRaises(m.RecoveryError): m.archive_files(tampered, self.digest)
        for name in ('../escape', '/escape', 'a/../escape'):
            with zipfile.ZipFile(tampered, 'w') as archive: archive.writestr(name, b'x')
            digest = 'sha256:' + hashlib.sha256(tampered.read_bytes()).hexdigest()
            with self.assertRaises(m.RecoveryError): m.archive_files(tampered, digest)
        info = zipfile.ZipInfo('linked'); info.external_attr = (stat.S_IFLNK | 0o777) << 16
        with zipfile.ZipFile(tampered, 'w') as archive: archive.writestr(info, b'target')
        digest = 'sha256:' + hashlib.sha256(tampered.read_bytes()).hexdigest()
        with self.assertRaises(m.RecoveryError): m.archive_files(tampered, digest)

    def test_candidate_site_changes_and_aliases_are_rejected(self):
        candidate = self.work / 'candidate'
        m.write_snapshot(m.snapshot_tree(self.artifact / '_site'), candidate)
        with self.assertRaises(m.RecoveryError): self.verify(candidate=candidate)
        m.sibling('pages_publish_control').ensure_static_site(candidate)
        self.verify(candidate=candidate)
        (candidate / 'installer-bootstrap.mjs').write_text('modified after validation')
        with self.assertRaises(m.RecoveryError): self.verify(candidate=candidate)
        (candidate / 'alias').symlink_to(candidate / 'index.html')
        with self.assertRaises(m.RecoveryError): m.snapshot_tree(candidate)
        (candidate / 'alias').unlink()
        os.link(candidate / 'index.html', candidate / 'alias')
        with self.assertRaises(m.RecoveryError): m.snapshot_tree(candidate)

    def test_foreign_authority_overrides_refuse_before_external_io(self):
        for key, value in [('GH_HOST', 'foreign.invalid'), ('TEST_REMOTE', '/foreign'),
                           ('GITHUB_SERVER_URL', 'https://foreign.invalid'), ('GITHUB_REPOSITORY', 'foreign/repo')]:
            with self.subTest(key=key), patch.dict(os.environ, {key: value}, clear=True), self.assertRaises(m.RecoveryError):
                m.validate_environment()

    def test_only_empty_marker_is_added_to_legacy_or_current_site(self):
        with tempfile.TemporaryDirectory(dir=self.work) as temporary:
            root = Path(temporary); original = root / 'artifact'; candidate = root / 'candidate'
            m.write_snapshot(m.snapshot_tree(self.artifact), original)
            payload = m.snapshot_tree(original / '_site')
            m.write_snapshot({**payload, '.nojekyll': b''}, candidate)
            self.verify(artifact=original, candidate=candidate)
            self.assertFalse((original / '_site/.nojekyll').exists())
            (original / '_site/.nojekyll').write_bytes(b'')
            self.verify(artifact=original, candidate=candidate)
            (original / '_site/.nojekyll').write_bytes(b'not empty')
            with self.assertRaises(m.RecoveryError): self.verify(artifact=original, candidate=candidate)
            (original / '_site/.nojekyll').unlink()
            (original / '_site/.nojekyll').mkdir()
            with self.assertRaises(m.RecoveryError): self.verify(artifact=original, candidate=candidate)
            (original / '_site/.nojekyll').rmdir()
            for name in ('.nojekyll', '.unexpected', 'index.html', 'installer-bootstrap.mjs',
                         'manifest.json', 'changelog.json', next(key for key in payload if key.endswith('.bin'))):
                with self.subTest(name=name):
                    path = candidate / name; before = path.read_bytes() if path.exists() else None
                    path.write_bytes(b'changed')
                    with self.assertRaises(m.RecoveryError): self.verify(artifact=original, candidate=candidate)
                    if before is None: path.unlink()
                    else: path.write_bytes(before)

    def test_actual_cli_authenticates_archive_before_validators_or_publication(self):
        api, command = self.clients()
        real_verify = m.verify
        verified = []
        def fixture_pin(*args, **kwargs):
            verified.append(True)
            kwargs['expected_pin'] = self.pin
            return real_verify(*args, **kwargs)
        tampered = self.work / 'cli-tampered.zip'
        with zipfile.ZipFile(tampered, 'w') as archive:
            for name, data in m.archive_files(self.archive, self.digest).items():
                archive.writestr(name, b'changed installer' if name == '_site/index.html' else data)
        for archive, expected in ((self.archive, 0), (tampered, 1)):
            verified.clear(); output = io.StringIO()
            with api, command, patch.dict(os.environ, {}, clear=True), patch.object(m, 'verify', fixture_pin), \
                 patch.object(sys, 'argv', ['recovery', '--run-id', '123', '--artifact-archive', str(archive)]), \
                 patch.object(m.subprocess, 'run', side_effect=AssertionError('verify-only must not mutate')), \
                 redirect_stderr(output), redirect_stdout(output):
                self.assertEqual(m.main(), expected)
            self.assertEqual(len(verified), int(expected == 0))
            if expected: self.assertIn('artifact archive differs from the authenticated GitHub digest', output.getvalue())

    def test_actual_publisher_recovery_guard_propagates_each_refusal(self):
        source = (ROOT / 'scripts/publish-pages-branch.sh').read_text()
        function = source[source.index('validate_channel_candidate() {'):source.index('# Apply the requested change')]
        script = self.work / 'publisher-guard.sh'
        script.write_text('set -euo pipefail\nmode=root-recovery\n'
            'python3() { printf "%s\\n" "$*" >> "$TRACE"; return "$REFUSE"; }\n' + function +
            '\nvalidate_channel_candidate "$1"\n')
        trace = self.work / 'publisher-guard.trace'
        for refused in (0, 37):
            trace.unlink(missing_ok=True)
            env = dict(os.environ, SOURCE_SHA=SOURCE, RELEASE_RECOVERY_ARCHIVE=str(self.archive),
                       RELEASE_RECOVERY_RUN_ID='123', RELEASE_RECOVERY_ID='7', RELEASE_RECOVERY_VERSION=VERSION,
                       TRACE=str(trace), REFUSE=str(refused))
            result = subprocess.run(['bash', str(script), str(self.artifact / '_site')], env=env, capture_output=True)
            self.assertEqual(result.returncode, refused)
            calls = trace.read_text().splitlines()
            self.assertEqual(len(calls), 1)
            self.assertIn('--artifact-archive', calls[0])
            self.assertIn('--verify-candidate', calls[0])
            self.assertIn('--release-id 7', calls[0])


class SignedPreviewEligibility(unittest.TestCase):
    def test_actual_validate_step_pagination_and_refusals(self):
        # Parse and execute the shipped run body; use the real jq on two API pages.
        model = json.loads(subprocess.check_output(['ruby', str(ROOT / 'scripts/check-yaml-syntax.rb'),
            '--json', str(ROOT / '.github/workflows/signed-pr-preview.yml')], text=True))
        steps = model['jobs']['validate']['steps']
        self.assertEqual(len(steps), 1)
        body = steps[0]['run']
        with tempfile.TemporaryDirectory(prefix='preview-eligibility-') as temporary:
            work = Path(temporary)
            gh = work / 'gh'
            gh.write_text(f'#!{sys.executable}\n' + '''import json,os,sys
a=sys.argv[1:]; mode=os.environ['FIXTURE_MODE']
if '--slurp' in a:
 if any(x in a for x in ('--jq','--template','-q','-t')): sys.exit(91)
 if mode=='api-error': sys.exit(43)
 if mode=='malformed': print('not json');sys.exit(0)
 items=[] if mode=='missing' else [{'name':'firmware-unsigned','expired':mode=='expired'}]
 print(json.dumps([{'artifacts':[{'name':'unrelated','expired':False}]},{'artifacts':items}]))
else:
 selector=a[a.index('--jq')+1]
 values={'.head.sha':('c'*40 if mode=='stale' else 'a'*40),
 '.head.repo.full_name':('foreign/repo' if mode=='fork' else '0Bu/tesla-key-esp32'),
 '.state':('closed' if mode=='closed' else 'open')}
 print(values.get(selector,'false' if mode=='unlabelled' else 'true'))
''')
            gh.chmod(0o700)
            for mode in ('eligible', 'missing', 'expired', 'stale', 'fork', 'closed', 'unlabelled', 'api-error', 'malformed'):
                output = work / 'output'; output.unlink(missing_ok=True)
                env = dict(os.environ, PATH=str(work)+os.pathsep+os.environ['PATH'], FIXTURE_MODE=mode,
                    GITHUB_REPOSITORY='0Bu/tesla-key-esp32', PR_NUMBER='450', RUN_HEAD_SHA='a'*40,
                    SOURCE_RUN_ID='123', GITHUB_OUTPUT=str(output))
                result = subprocess.run(['bash', '-c', body], env=env, text=True, capture_output=True, timeout=10)
                with self.subTest(mode=mode):
                    if mode in ('api-error', 'malformed'):
                        self.assertNotEqual(result.returncode, 0)
                        self.assertFalse(output.exists(), result.stdout)
                    else:
                        self.assertEqual(result.returncode, 0, result.stderr)
                        self.assertIn('eligible=' + str(mode=='eligible').lower(), output.read_text().splitlines())


if __name__ == '__main__':
    unittest.main()
