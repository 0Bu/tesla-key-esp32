#!/usr/bin/env python3
"""Check already-collected bootloader/app metadata. Never contact a USB device."""
import argparse
import hashlib
import json
import os
import pathlib
import re
import stat
import subprocess
import sys

FAMILIES = {'esp32': 'ESP32', 'esp32s3': 'ESP32-S3',
            'esp32c3': 'ESP32-C3', 'esp32c6': 'ESP32-C6'}
REFERENCE_KIND = 'tesla-key-esp32-reviewed-bootloaders-v1'
REFERENCES = pathlib.Path(__file__).resolve().with_name('bootloader-references.json')


def version_tuple(text):
    match = re.fullmatch(r'v([0-9]{1,5})\.([0-9]{1,5})(?:\.([0-9]{1,5}))?', text)
    if not match or len(text) > 31:
        raise ValueError('unrecognized ESP-IDF version')
    version = tuple(int(n or 0) for n in match.groups())
    if any(n > 65535 for n in version):
        raise ValueError('ESP-IDF version component is out of range')
    return version


def regular_bytes(path, limit):
    with os.fdopen(os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK), 'rb') as stream:
        metadata = os.fstat(stream.fileno())
        if not stat.S_ISREG(metadata.st_mode) or not 0 < metadata.st_size <= limit:
            raise ValueError('missing, unsafe or oversized image')
        data = stream.read(limit + 1)
        if len(data) != metadata.st_size:
            raise ValueError('image changed while reading')
        return data


def image_info(path, limit, allow_descriptorless=False):
    regular_bytes(path, limit)
    try:
        result = subprocess.run(['esptool', 'image-info', str(path.resolve())],
                                capture_output=True, text=True, timeout=10, check=True)
    except (OSError, subprocess.SubprocessError, UnicodeError) as error:
        raise ValueError('image metadata is unreadable') from error
    return parse_info(result.stdout, allow_descriptorless)


def parse_info(text, allow_descriptorless=False):
    families = re.findall(r'^Detected image type: (ESP32(?:-S3|-C3|-C6)?)\s*$', text, re.M)
    versions = re.findall(r'^ESP-IDF: (\S+)\s*$', text, re.M)
    if (len(families) == 1 and not versions and allow_descriptorless and
            not re.search(r'^ESP-IDF:', text, re.M)):
        return families[0], None
    if len(families) != 1 or len(versions) != 1:
        raise ValueError('image needs one supported chip family and one ESP-IDF version')
    return families[0], version_tuple(versions[0])


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('duplicate bootloader reference field')
        result[key] = value
    return result


def reference_catalog():
    """Repository-reviewed references, never caller-supplied version/authority overrides."""
    catalog = json.loads(regular_bytes(REFERENCES, 16384), object_pairs_hook=unique_object)
    if (not isinstance(catalog, dict) or set(catalog) != {'kind', 'references'} or
            catalog['kind'] != REFERENCE_KIND or not isinstance(catalog['references'], list) or
            len(catalog['references']) != len(FAMILIES)):
        raise ValueError('invalid reviewed bootloader catalog')
    references = {}
    fields = {'target', 'imageBytes', 'imageSha256', 'idfToolchain', 'sourceSha',
              'workflowRunId', 'artifactId', 'artifactSha256', 'artifactName', 'artifactPath',
              'localRebuildSourceSha256', 'localRebuildReceiptSha256'}
    for item in catalog['references']:
        if not isinstance(item, dict) or set(item) != fields:
            raise ValueError('invalid reviewed bootloader reference fields')
        target = item['target']
        if not isinstance(target, str) or target not in FAMILIES or target in references:
            raise ValueError('invalid or duplicate bootloader reference target')
        limit = 0x7000 if target == 'esp32' else 0x8000
        if type(item['imageBytes']) is not int or not 0 < item['imageBytes'] <= limit:
            raise ValueError('invalid bootloader reference length')
        for field in ('imageSha256', 'artifactSha256', 'localRebuildSourceSha256', 'localRebuildReceiptSha256'):
            if not isinstance(item[field], str) or not re.fullmatch('[0-9a-f]{64}', item[field]):
                raise ValueError('invalid bootloader reference digest')
        if not isinstance(item['sourceSha'], str) or not re.fullmatch('[0-9a-f]{40}', item['sourceSha']):
            raise ValueError('invalid bootloader reference source')
        for field in ('workflowRunId', 'artifactId'):
            if type(item[field]) is not int or not 0 < item[field] < 2**53:
                raise ValueError('invalid bootloader reference provenance')
        if (item['artifactName'] != 'firmware-target-' + target or
                item['artifactPath'] != f'_unsigned/{target}/bootloader/bootloader.bin' or
                not isinstance(item['idfToolchain'], str)):
            raise ValueError('invalid bootloader reference artifact binding')
        pin = re.fullmatch(r'(v[^@]+)@sha256:([0-9a-f]{64})', item['idfToolchain'])
        if not pin:
            raise ValueError('missing bootloader reference toolchain pin')
        references[target] = item, version_tuple(pin[1])
    return references


def descriptorless_bootloader(path, target):
    references = reference_catalog()
    item, version = references[target]
    limit = 0x7000 if target == 'esp32' else 0x8000
    data = regular_bytes(path, limit)
    length = item['imageBytes']
    # Accept a reference image itself or the complete project bootloader region. Never admit
    # a partial read with arbitrary trailing bytes, or discard a non-erased region suffix.
    if (len(data) not in (length, limit) or
            hashlib.sha256(data[:length]).hexdigest() != item['imageSha256'] or
            any(byte != 0xff for byte in data[length:])):
        raise ValueError('retained bootloader does not match its reviewed provenance reference')
    return FAMILIES[target], version


def require_compatible(boot, app, target):
    if target not in FAMILIES or boot[0] != FAMILIES[target] or app[0] != FAMILIES[target]:
        raise ValueError('bootloader/app chip does not match the selected target')
    if app[1] < boot[1]:
        raise ValueError('app ESP-IDF is older than the retained bootloader')


def self_test():
    reference_catalog()
    checks = 0
    for target, family in FAMILIES.items():
        boot = parse_info(f'Detected image type: {family}\nESP-IDF: v6.1.0\n')
        for version in ('v6.1', 'v6.1.0', 'v6.1.1', 'v6.2.0', 'v7.0.0'):
            require_compatible(boot, parse_info(f'Detected image type: {family}\nESP-IDF: {version}\n'), target)
            checks += 1
        for version in ('v5.5.5', 'v6.0.9'):
            try:
                require_compatible(boot, parse_info(f'Detected image type: {family}\nESP-IDF: {version}\n'), target)
            except ValueError:
                checks += 1
            else:
                raise AssertionError('older-IDF app was admitted')
        try:
            require_compatible((family, (6, 1, 1)), (family, (6, 1, 0)), target)
        except ValueError:
            checks += 1
        else:
            raise AssertionError('older patch-level app was admitted')
        wrong = 'ESP32-C3' if family != 'ESP32-C3' else 'ESP32-S3'
        for candidate in ((wrong, boot[1]),):
            for first, second in ((candidate, boot), (boot, candidate)):
                try:
                    require_compatible(first, second, target)
                except ValueError:
                    checks += 1
                else:
                    raise AssertionError('wrong chip was admitted')
    for text in ('', 'Detected image type: ESP32-C5\nESP-IDF: v6.1\n',
                 'Detected image type: ESP32\n',
                 'Detected image type: ESP32\nESP-IDF: unknown\n',
                 'Detected image type: ESP32\nESP-IDF: v6.1\nESP-IDF: v6.1\n',
                 'Detected image type: ESP32\nDetected image type: ESP32\nESP-IDF: v6.1\n',
                 'Detected image type: ESP32\nESP-IDF: v99999.1\n',
                 *[f'Detected image type: ESP32\nESP-IDF: v6.1.0-{suffix}\n'
                   for suffix in ('rc1', 'dev', 'dirty', 'unknown')]):
        try:
            parse_info(text)
        except ValueError:
            checks += 1
        else:
            raise AssertionError('ambiguous/unreadable metadata was admitted')
    print(f'bootloader compatibility self-test: PASS ({checks} controls, four targets)')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', choices=FAMILIES)
    parser.add_argument('--bootloader', type=pathlib.Path)
    parser.add_argument('--app', type=pathlib.Path)
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if not args.target or not args.bootloader or not args.app:
        parser.error('--target, --bootloader and --app are required')
    try:
        limit = 0x7000 if args.target == 'esp32' else 0x8000
        boot = image_info(args.bootloader, limit, allow_descriptorless=True)
        if boot[0] != FAMILIES[args.target]:
            raise ValueError('bootloader chip does not match the selected target')
        if boot[1] is None:
            boot = descriptorless_bootloader(args.bootloader, args.target)
        app = image_info(args.app, 0x1f0000)
        require_compatible(boot, app, args.target)
    except (ValueError, OSError, UnicodeError) as error:
        print(f'bootloader compatibility: REFUSING: {error}', file=sys.stderr)
        return 2
    print('bootloader ESP-IDF ' + '.'.join(map(str, boot[1])) + '; app ESP-IDF ' +
          '.'.join(map(str, app[1])) + ': compatible')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
