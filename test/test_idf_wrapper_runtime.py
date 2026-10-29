#!/usr/bin/env python3
"""Execute the shipped IDF wrapper against a passive Docker CLI, without any daemon access."""
import json
import os
import pathlib
import shutil
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix='tesla-idf-wrapper-') as temporary:
        workspace = pathlib.Path(temporary).resolve()
        template = workspace / 'git-template'
        template.mkdir()
        archive, primary = workspace / 'archive', workspace / 'primary'
        linked = primary / 'linked-checkout'
        runtime = ('scripts/idf-docker.sh', 'scripts/idf-version.sh',
                   'scripts/check-idf-container-mounts.py', 'esp-idf-toolchain.txt')
        for root in (archive, primary):
            for name in runtime:
                destination = root / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(ROOT / name, destination)
        git = ['git', '-c', 'core.hooksPath=/dev/null', '-c', 'commit.gpgsign=false', '-c', 'user.name=Fixture',
               '-c', 'user.email=fixture@example.invalid']
        subprocess.run([*git, 'init', '-q', '--template=' + str(template), str(primary)], check=True, capture_output=True)
        subprocess.run([*git, '-C', str(primary), 'commit', '-q', '--allow-empty', '-m', 'fixture'],
                       check=True, capture_output=True)
        subprocess.run([*git, '-C', str(primary), 'worktree', 'add', '-q', '--detach', str(linked), 'HEAD'],
                       check=True, capture_output=True)
        for name in runtime:
            destination = linked / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / name, destination)
        binary = workspace / 'fake-bin'
        binary.mkdir()
        docker = binary / 'docker'
        docker.write_text('''#!/usr/bin/env python3
import json,os,pathlib,sys
args=sys.argv[1:]
with pathlib.Path(os.environ['FIXTURE_DOCKER_LOG']).open('a') as log:
 log.write(json.dumps(args)+'\\n')
if args[:2]==['volume','inspect']:sys.exit(1)
if args[:2]==['volume','create']:sys.exit(0)
if args and args[0]=='inspect':
 print('false');sys.exit(0)
if args and args[0]=='run' and '--entrypoint' in args:
 sys.exit(int(os.environ.get('FIXTURE_HELPER_EXIT','0')))
if args and args[0] in ('run','exec','rm'):sys.exit(0)
sys.exit(87)
''')
        docker.chmod(0o700)
        image = 'espressif/idf:' + (ROOT / 'esp-idf-toolchain.txt').read_text().strip()
        common = primary / '.git'
        layouts = (('archive', archive, None),
                   ('primary', primary, str(common) + ':/project/.git:ro'),
                   ('linked', linked, str(common) + ':' + str(common) + ':ro'))
        for layout, root, metadata_mount in layouts:
            for mode, helper_exit in (('ordinary', 0), ('fast', 0), ('daemon', 0), ('fast', 37), ('daemon', 37)):
                log = workspace / 'calls.jsonl'
                log.unlink(missing_ok=True)
                env = os.environ.copy()
                for key in list(env):
                    if key.startswith(('IDF_', 'CCACHE_', 'NINJA_')):
                        env.pop(key)
                env.update(PATH=str(binary)+os.pathsep+env['PATH'], FIXTURE_DOCKER_LOG=str(log),
                           FIXTURE_HELPER_EXIT=str(helper_exit))
                if mode == 'fast':env['IDF_FAST_BUILD'] = '1'
                args = ['daemon', 'start'] if mode == 'daemon' else ['true']
                result = subprocess.run(['bash', str(root / 'scripts/idf-docker.sh'), *args],
                                        cwd=root, env=env, text=True, capture_output=True, timeout=15)
                calls = [json.loads(line) for line in log.read_text().splitlines()]
                runs = [args for args in calls if args[0] == 'run']
                expected = helper_exit if helper_exit else 0
                assert result.returncode == expected, (layout, mode, helper_exit, result.returncode, result.stderr)
                assert runs, (layout, mode, 'no command exercised')
                for args in runs:
                    assert args[args.index('--cpus')+1] == '1.5', args
                    assert args[args.index('--memory')+1] == '1800m', args
                    assert image in args, args
                    if '--entrypoint' not in args:
                        mounts = [args[i+1] for i, value in enumerate(args[:-1]) if value == '-v']
                        assert str(root) + ':/project' in mounts, mounts
                        if metadata_mount:
                            assert metadata_mount in mounts, (layout, mounts)
                        else:
                            assert not any(value.endswith(':/project/.git:ro') for value in mounts), mounts
                        assert str(primary) + ':' + str(primary) not in mounts, mounts
                helpers = [args for args in runs if '--entrypoint' in args]
                assert len(helpers) == int(mode != 'ordinary'), (layout, mode, helpers)
                if helpers:
                    assert helpers[0][helpers[0].index('--entrypoint')+1] == '/bin/chmod'
                if helper_exit:
                    assert runs == helpers, ('helper failure must stop before build/daemon', runs)
                    assert not any(args[0] == 'exec' for args in calls)
                else:
                    assert len(runs) == (1 if mode == 'ordinary' else 2), runs
                print('PASS actual IDF wrapper:', layout, mode, 'helper exit', helper_exit)
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
