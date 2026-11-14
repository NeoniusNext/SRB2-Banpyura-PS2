#!/usr/bin/env python3
"""Summarize the final refill proof's target counts, object sizes and frames."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def run(*args):
    return subprocess.check_output([str(arg) for arg in args], text=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--target', required=True, type=Path)
    ap.add_argument('--native', type=Path, help='additional exact-default-step proof directory')
    args = ap.parse_args()
    work = args.target.resolve()
    ee = work / 'ee'
    log = (ee / 'run.log').read_text()
    result = re.search(r'Audio refill next6: checks=(\d+) frames=(\d+) files=(\d+) PASS', log)
    if not result:
        raise SystemExit('complete target proof did not pass')
    manifest = json.loads((work / 'source-manifest.json').read_text())
    native_report = None
    if args.native:
        native = args.native.resolve()
        native_log = (native / 'ee/native.log').read_text()
        passed = re.search(r'NATIVE6 DONE checks=(\d+) frames=(\d+) PASS', native_log)
        if not passed:
            raise SystemExit('native proof did not pass')
        other_manifest = json.loads((native / 'source-manifest.json').read_text())
        for key in ('reference', 'candidate'):
            if manifest[key] != other_manifest[key]:
                raise SystemExit('production sources changed between target proofs')
        identical = []
        for path in sorted(ee.glob('*.o')):
            if path.name.startswith('bench_audio_refill_'):
                continue
            other = native / 'ee' / path.name
            if path.read_bytes() != other.read_bytes():
                raise SystemExit('production target object differs: ' + path.name)
            identical.append(path.name)
        native_report = {'checks': int(passed[1]), 'frames': int(passed[2]),
                         'identical_complete_production_objects': identical,
                         'elf_sha256': hashlib.sha256((native / 'ee/AUDIO_REFILL_NEXT6.ELF').read_bytes()).hexdigest(),
                         'manifest': other_manifest}
        log += '\n' + native_log
    rows = []
    for line in log.splitlines():
        if 'MUSIC6 TIME' not in line:
            continue
        row = {key: float(value) if '.' in value else int(value)
               for key, value in re.findall(r'(\w+)=([\d.]+)', line)}
        row['reduction_pct'] = (row['baseline'] - row['candidate']) * 100 / row['baseline']
        rows.append(row)
    if len(rows) != 180:
        raise SystemExit('missing target timing rows: ' + str(len(rows)))
    groups = {}
    for key, selected in [('all', rows), ('native', [row for row in rows if row['step'] == 65536]),
                          ('general', [row for row in rows if row['step'] != 65536])]:
        before = sum(row['baseline'] for row in selected)
        after = sum(row['candidate'] for row in selected)
        groups[key] = {'rows': len(selected), 'baseline_count': before, 'candidate_count': after,
                       'reduction_pct': (before - after) * 100 / before if before else None,
                       'min_pct': min((row['reduction_pct'] for row in selected), default=None),
                       'max_pct': max((row['reduction_pct'] for row in selected), default=None),
                       'slower_rows': sum(row['candidate'] > row['baseline'] for row in selected)}
    objects = {}
    for kind in ('reference', 'candidate'):
        for unit in ('music', 'midi'):
            for codecs in (False, True):
                name = kind + '_ps2_' + unit + ('.codecs' if codecs else '')
                obj = ee / (name + '.o')
                values = run('mips64r5900el-ps2-elf-size', obj).splitlines()[1].split()
                functions, current = {}, None
                for line in (ee / (name + '.dis')).read_text().splitlines():
                    match = re.match(r'^[0-9a-f]+ <(.+)>:$', line)
                    if match:
                        current = match[1]
                        functions[current] = {'stack_bytes': 0, 'instructions': 0}
                    elif current and re.match(r'^\s+[0-9a-f]+:\s+[0-9a-f]{8}\s', line):
                        functions[current]['instructions'] += 1
                        frame = re.search(r'addiu\s+sp,sp,-(\d+)', line)
                        if frame:
                            functions[current]['stack_bytes'] = max(functions[current]['stack_bytes'], int(frame[1]))
                objects[name] = {'text': int(values[0]), 'data': int(values[1]), 'bss': int(values[2]),
                                 'sha256': hashlib.sha256(obj.read_bytes()).hexdigest(),
                                 'functions': {key: value for key, value in functions.items()
                                               if key in ('Decode', 'Ensure', 'Refill', 'Ref_MusicRender',
                                                          'PS2_MusicRender', 'Ref_MIDIRead', 'PS2_MIDIRead')}}
    report = {'checks': int(result[1]), 'frames': int(result[2]), 'files': int(result[3]),
              'groups': groups, 'rows': rows, 'objects': objects,
              'elf_sha256': hashlib.sha256((ee / 'AUDIO_REFILL_NEXT6.ELF').read_bytes()).hexdigest(),
              'manifest': manifest, 'native_proof': native_report}
    (work / 'final-target-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'checks': report['checks'], 'frames': report['frames'], 'groups': groups}, indent=2))


if __name__ == '__main__':
    main()
