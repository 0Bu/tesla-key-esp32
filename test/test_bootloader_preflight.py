#!/usr/bin/env python3
"""Exercise shared offline compatibility and its USB consumer guards; no device access."""
import json
import hashlib
import os
import pathlib
import re
import runpy
import shlex
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
SKILLS = ('.agents/skills/flash-esp32/SKILL.md', '.agents/skills/ship/SKILL.md',
          '.agents/skills/usb-recovery/SKILL.md')
CALL = 'python3 scripts/check-bootloader-compat.py --target "$TARGET" --bootloader '


def consumer_guards(sources):
    guards = []
    for name, expected in zip(SKILLS[:2], (2, 1)):
        count = 0
        for block in re.findall(r'```bash\n(.*?)\n```', sources[name], re.S):
            writes = re.findall(r'write[-_]flash 0x20000 "(\$[A-Z_]+)"', block)
            if not writes:
                continue
            calls = re.findall(r'^python3 scripts/check-bootloader-compat.py --target "\$TARGET" '
                               r'--bootloader "\$\{BOOT_IMAGE:\?[^"\n]+\}" --app "(\$[A-Z_]+)"$', block, re.M)
            if writes != calls or len(writes) != 1 or not block.startswith('set -euo pipefail\n'):
                raise AssertionError('app-only write lacks a checked shared guard: ' + name)
            line = next(line for line in block.splitlines() if line.startswith(CALL))
            if block.index(line) > block.index('write-flash' if 'write-flash' in block else 'write_flash'):
                raise AssertionError('compatibility guard follows the write: ' + name)
            guards.append(line)
            count += 1
        if count != expected or '#retained-bootloader-compatibility)' not in sources[name]:
            raise AssertionError('missing app-only compatibility consumer: ' + name)
    owner = "\n".join(line for block in re.findall(r'```bash\n(.*?)\n```', sources[SKILLS[2]], re.S)
                      for line in block.splitlines() if not line.lstrip().startswith("#"))
    call = CALL + '"$BOOT_IMAGE" --app "$APP"'
    if owner.count(call) != 1 or owner.index(call) > owner.index('if ! esptool --chip "$TARGET" -p "$PORT" write_flash'):
        raise AssertionError('recovery must share the offline checker before any app write')
    return guards


def reference_cases(work, binary, helper):
    """Actual copied CLI with explicitly synthetic catalog bytes, never a physical reference."""
    copied = work / 'reference-checker' / 'check-bootloader-compat.py'
    copied.parent.mkdir()
    copied.write_bytes((ROOT / 'scripts/check-bootloader-compat.py').read_bytes())
    catalog_path = copied.with_name('bootloader-references.json')
    original = json.loads((ROOT / 'scripts/bootloader-references.json').read_text())
    fixture = {target: ('reference-' + target).encode() for target in helper['FAMILIES']}
    for item in original['references']:
        data = fixture[item['target']]
        item['imageBytes'] = len(data)
        item['imageSha256'] = hashlib.sha256(data).hexdigest()
    cases = 0
    for target, family in helper['FAMILIES'].items():
        limit = 0x7000 if target == 'esp32' else 0x8000
        for scenario in ('exact', 'full-ff', 'truncated', 'partial-padding', 'dirty-tail',
                         'wrong-bytes', 'missing-catalog', 'wrong-digest', 'wrong-target',
                         'unknown-pin', 'duplicate-record', 'duplicate-field', 'older-app'):
            allowed = scenario in ('exact', 'full-ff')
            catalog = json.loads(json.dumps(original))
            item = next(x for x in catalog['references'] if x['target'] == target)
            data = fixture[target]
            if scenario == 'full-ff':data += b'\xff' * (limit - len(data))
            if scenario == 'truncated':data = data[:-1]
            if scenario == 'partial-padding':data += b'\xff'
            if scenario == 'dirty-tail':data += b'\xff' * (limit - len(data) - 1) + b'\x00'
            if scenario == 'wrong-bytes':data = b'X' + data[1:]
            if scenario == 'wrong-digest':item['imageSha256'] = '0' * 64
            if scenario == 'wrong-target':item['target'] = 'esp32c5'
            if scenario == 'unknown-pin':item['idfToolchain'] = 'v6.1-unknown@sha256:' + '0' * 64
            if scenario == 'duplicate-record':catalog['references'][0] = dict(catalog['references'][1])
            catalog_path.write_text(json.dumps(catalog))
            if scenario == 'duplicate-field':
                catalog_path.write_text(catalog_path.read_text().replace('{', '{"kind":"duplicate",', 1))
            if scenario == 'missing-catalog':catalog_path.unlink()
            boot = work / 'boot.bin';boot.write_bytes(data)
            app = work / 'app.bin';app.write_bytes(b'app fixture')
            marker = work / 'reference-writes';marker.unlink(missing_ok=True)
            env = os.environ.copy()
            env.update(PATH=str(binary)+os.pathsep+env['PATH'], FIXTURE_WRITES=str(marker),
                       FIXTURE_BOOT_FAMILY=family, FIXTURE_APP_FAMILY=family,
                       FIXTURE_BOOT_IDF='', FIXTURE_APP_IDF='v6.0' if scenario == 'older-app' else 'v6.1',
                       FIXTURE_UNREADABLE='')
            command = shlex.join([sys.executable, str(copied), '--target', target,
                                  '--bootloader', str(boot), '--app', str(app)])
            command += '\nesptool write-flash fixture-only'
            result = subprocess.run(['bash', '-euo', 'pipefail', '-c', command], cwd=ROOT,
                                    env=env, capture_output=True, text=True, timeout=15)
            assert result.returncode == (0 if allowed else 2), (target, scenario, result.stderr)
            assert marker.exists() == allowed, (target, scenario, 'reference write admission')
            cases += 1
    return cases


def main():
    source = {name: (ROOT / name).read_text() for name in SKILLS}
    guards = consumer_guards(source)
    for name in SKILLS:
        changed = dict(source)
        changed[name] = changed[name].replace(CALL, '# omitted: ' + CALL, 1)
        try:
            consumer_guards(changed)
        except AssertionError:
            pass
        else:
            raise AssertionError('consumer omission canary passed: ' + name)
    helper = runpy.run_path(str(ROOT / 'scripts/check-bootloader-compat.py'))
    helper['self_test']()
    cases = 0
    with tempfile.TemporaryDirectory(prefix='tesla-boot-compat-') as temporary:
        work = pathlib.Path(temporary)
        binary = work / 'bin'
        binary.mkdir()
        fake = binary / 'esptool'
        fake.write_text('''#!/usr/bin/env python3
import os,pathlib,sys
args=sys.argv[1:]
if args and args[0]=='image-info':
 kind='BOOT' if pathlib.Path(args[1]).name=='boot.bin' else 'APP'
 if os.environ.get('FIXTURE_UNREADABLE')==kind:sys.exit(37)
 print('Detected image type: '+os.environ['FIXTURE_'+kind+'_FAMILY'])
 version=os.environ['FIXTURE_'+kind+'_IDF']
 if version:print('ESP-IDF: '+version)
 sys.exit(0)
if 'write-flash' in args:
 pathlib.Path(os.environ['FIXTURE_WRITES']).write_text('WRITE')
 sys.exit(0)
sys.exit(87)
''')
        fake.chmod(0o700)
        boot = work / 'boot.bin'
        app = work / 'app.bin'
        boot.write_bytes(b'fixture')
        app.write_bytes(b'fixture')
        for target, family in helper['FAMILIES'].items():
            for scenario, allowed in (('same', True), ('newer', True), ('older', False),
                                       ('older-patch', False), ('unknown', False),
                                       ('unreadable', False), ('wrong-boot', False), ('wrong-app', False),
                                       ('app-rc', False), ('app-unknown', False),
                                       ('boot-rc', False), ('app-dirty', False)):
                marker = work / 'writes'
                marker.unlink(missing_ok=True)
                env = os.environ.copy()
                env.update(PATH=str(binary)+os.pathsep+env['PATH'], TARGET=target, PORT='/fixture-only',
                           APP=str(app), SIGNED_APP=str(app), BOOT_IMAGE=str(boot),
                           FIXTURE_WRITES=str(marker), FIXTURE_BOOT_FAMILY=family,
                           FIXTURE_APP_FAMILY=family, FIXTURE_BOOT_IDF='v6.1',
                           FIXTURE_APP_IDF='v6.1', FIXTURE_UNREADABLE='')
                if scenario == 'newer':env['FIXTURE_APP_IDF'] = 'v6.2'
                if scenario == 'older':env['FIXTURE_APP_IDF'] = 'v5.5.5'
                if scenario == 'older-patch':env['FIXTURE_BOOT_IDF'] = 'v6.1.1'
                if scenario == 'unknown':env['FIXTURE_BOOT_IDF'] = ''
                if scenario == 'unreadable':env['FIXTURE_UNREADABLE'] = 'BOOT'
                other = 'ESP32-C3' if family != 'ESP32-C3' else 'ESP32-S3'
                if scenario == 'wrong-boot':env['FIXTURE_BOOT_FAMILY'] = other
                if scenario == 'wrong-app':env['FIXTURE_APP_FAMILY'] = other
                if scenario == 'app-rc':env['FIXTURE_APP_IDF'] = 'v6.1.0-rc1'
                if scenario == 'app-unknown':env['FIXTURE_APP_IDF'] = 'v6.1.0-unknown'
                if scenario == 'boot-rc':env['FIXTURE_BOOT_IDF'] = 'v6.1.0-rc1'
                if scenario == 'app-dirty':env['FIXTURE_APP_IDF'] = 'v6.2.0-dirty'
                # Run the exact local-flash guard line followed by a passive stub write.
                command = guards[0] + '\nesptool --chip "$TARGET" -p "$PORT" write-flash 0x20000 "$SIGNED_APP"'
                result = subprocess.run(['bash', '-euo', 'pipefail', '-c', command], cwd=ROOT,
                                        env=env, capture_output=True, text=True, timeout=15)
                assert (result.returncode == 0) == allowed, (target, scenario, result.stderr)
                assert marker.exists() == allowed, (target, scenario, 'write admission')
                cases += 1
        references = reference_cases(work, binary, helper)
    print(f'bootloader preflight: PASS ({cases} metadata and {references} synthetic-reference actual CLI/write-admission cases; three consumer omission controls)')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
