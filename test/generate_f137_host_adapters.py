#!/usr/bin/env python3
"""UNTESTED DRAFT. Generate complete actual F137 task adapters, never run a compiler.

Only whole uniquely delimited production regions are inserted. No task-loop rewrite,
catch rewrite or decision replacement is allowed. Passive platform declarations live
in the templates. Source edits for negative controls occur in owned disposable copies.
"""
import argparse
import hashlib
import json
from pathlib import Path


def region(text, begin, end):
    if text.count(begin) != 1 or text.count(end) != 1:
        raise ValueError('nonunique production extraction delimiters: '+begin)
    body = begin + text.split(begin, 1)[1].split(end, 1)[0]
    if not body.rstrip().endswith('}') and not begin.startswith('static const int'):
        raise ValueError('incomplete actual function: '+begin)
    return body


def generate(source, output, kind):
    source=Path(source); output=Path(output)
    filename='mqtt_ha.cpp' if kind=='mqtt' else 'net.cpp'
    text=(source/'main'/filename).read_text()
    if kind=='mqtt':
        bodies=[region(text,'static void publisher_task(void*) {',
                       '\n\n// ─── Public API')]
    else:
        bodies=[region(text,'static const int kWdPeriodS',
                       'static GatewaySample gateway_reachable() {'),
                region(text,'static GatewaySample gateway_reachable() {',
                       '\n\n// Force the ACTIVE transport'),
                region(text,'static void net_recover(tk::GatewayIdentity sampled) {',
                       '\n\nstatic void net_watchdog_task'),
                region(text,'static void net_watchdog_task(void*) {',
                       '\n\nbool net_watchdog_start()')]
    template=Path(__file__).with_name('f137_'+kind+'_host_template.cpp').read_text()
    if template.count('// ACTUAL_COMPLETE_BODIES')!=1: raise ValueError('template marker')
    result=template.replace('// ACTUAL_COMPLETE_BODIES','\n\n'.join(bodies))
    with output.open('x') as stream: stream.write(result)
    metadata={'scope':'UNTESTED_GENERATION_ONLY_ACTUAL_COMPLETE_BODIES',
              'sourceFile':str(source/'main'/filename),'sourceSha256':hashlib.sha256(text.encode()).hexdigest(),
              'kind':kind,'wholeRegions':[{'sha256':hashlib.sha256(b.encode()).hexdigest(),'bytes':len(b.encode())}for b in bodies],
              'generatedSha256':hashlib.sha256(result.encode()).hexdigest(),
              'templateSha256':hashlib.sha256(template.encode()).hexdigest()}
    with output.with_suffix('.source.json').open('x') as stream:json.dump(metadata,stream,indent=2);stream.write('\n')
    print(json.dumps(metadata))


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('source');parser.add_argument('output');parser.add_argument('--kind',choices=['mqtt','net'],required=True)
    a=parser.parse_args();generate(a.source,a.output,a.kind)
