#!/usr/bin/env python3
"""Build/run the complete real-zone mipmap/spill proof on EE32 serially."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

import build as B
from next9_memory_hosttest import ROOT, TOOLS, parts


def run(command, log, timeout=240):
    result = subprocess.run(list(map(str, command)), cwd=ROOT, capture_output=True, text=True, timeout=timeout)
    log.write_text(result.stdout + result.stderr)
    assert result.returncode == 0, str(log) + ': ' + result.stdout[-1000:] + result.stderr[-4000:]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, default=ROOT / 'build/nov15-next9/baseline-src')
    parser.add_argument('--out', type=Path, default=ROOT / 'build/nov15-next9/memory/ee')
    parser.add_argument('--host-proof', type=Path, default=ROOT / 'build/nov15-next9/memory/host')
    parser.add_argument('--run', action='store_true')
    parser.add_argument('--analyze-only', action='store_true')
    args = parser.parse_args()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    host = args.host_proof.resolve()
    proof = json.loads((host / 'report.json').read_text())
    frozen = (host / 'baseline/snapshot.bin').read_bytes()
    assert hashlib.sha256(frozen).hexdigest() == proof['snapshot_sha256']
    assert hashlib.sha256((TOOLS / 'next9_memory_hosttest.c').read_bytes()).hexdigest() == proof['fixture_sha256']
    flags = [f for f in B.CFLAGS if f not in ('-flto', '-MMD', '-MP', '-D_EE', '-DPS2', '-DPS2_PROFILE', '-DHWRENDER')]
    flags += ['-U_EE', '-DPARANOIA', '-Wno-unused-function', '-Wno-unused-variable', '-Wno-sign-compare', '-Wno-clobbered']
    builds = {}
    for label, root, extra in [('baseline', args.baseline.resolve(), []), ('current', ROOT, []),
                               ('old-switch', ROOT, ['-DPS2_NOOPT_MIPMAP_LAYOUT'])]:
        work = args.out / label
        work.mkdir(exist_ok=True)
        elf = work / 'MEMORY.ELF'
        if not args.analyze_only:
            defines, hashes = [], {}
            for macro, content in parts(root).items():
                path = work / (macro.lower() + '.inc')
                path.write_text(content)
                defines.append('-D' + macro + '="' + str(path) + '"')
                hashes[macro] = hashlib.sha256(content.encode()).hexdigest()
            obj = work / 'driver.o'
            run([B.CC, *flags, '-I' + str(TOOLS), *B.INCS, *defines, *extra,
                 '-c', TOOLS / 'next9_memory_hosttest.c', '-o', obj], work / 'build.log')
            run([B.CC, *B.LDFLAGS, obj, '-o', elf, '-ldebug', '-lpatches', '-lm'], work / 'link.log')
            inputs = {'elf_sha256': hashlib.sha256(elf.read_bytes()).hexdigest(), 'source_sha256': hashes,
                      'fixture_sha256': hashlib.sha256((TOOLS / 'next9_memory_hosttest.c').read_bytes()).hexdigest(),
                      'host_oracle_sha256': hashlib.sha256(frozen).hexdigest()}
            (work / 'inputs.json').write_text(json.dumps(inputs, indent=2) + '\n')
        else:
            inputs = json.loads((work / 'inputs.json').read_text())
        assert hashlib.sha256(elf.read_bytes()).hexdigest() == inputs['elf_sha256']
        assert hashlib.sha256(frozen).hexdigest() == inputs['host_oracle_sha256']
        assert hashlib.sha256((TOOLS / 'next9_memory_hosttest.c').read_bytes()).hexdigest() == inputs['fixture_sha256']
        assert {k: hashlib.sha256(v.encode()).hexdigest() for k, v in parts(root).items()} == inputs['source_sha256']
        builds[label] = inputs
        print('Built' if not args.analyze_only else 'Analyzing', elf, flush=True)
        if args.run:
            (work / 'snapshot.bin').unlink(missing_ok=True)
            run([sys.executable, '-B', TOOLS / 'run_pcsx2.py', '--elf', elf, '--log', work / 'run.log',
                 '--args=MEMORY.ELF host:snapshot.bin', '--until', 'MEMORY9 DONE', '--timeout', '180'],
                work / 'runner.log', timeout=210)
    if not (args.run or args.analyze_only):
        return
    results = {}
    for label in builds:
        work = args.out / label
        log = (work / 'run.log').read_text()
        assert 'pcsx2 exit code 0' in (work / 'runner.log').read_text()
        actual = (work / 'snapshot.bin').read_bytes()
        assert actual == frozen, label + ' EE32 literal snapshot differs at ' + str(next((i for i, (a, b) in enumerate(zip(actual, frozen)) if a != b), min(len(actual), len(frozen))))
        tail = re.search(r'MEMORY9 DONE cases=(\d+) checks=(\d+) sizeof=(\d+) map=(\d+) align=(\d+)', log)
        assert tail, label + ' incomplete'
        results[label] = dict(zip(('cases', 'checks', 'sizeof', 'map', 'align'), map(int, tail.groups())))
        results[label]['rows'] = [dict(re.findall(r'(\w+)=([\da-f]+)', line)) for line in log.splitlines() if 'MEMORY9 case=' in line]
        assert results[label]['cases'] == proof['results']['current']['cases']
        assert results[label]['align'] == 4 and len(results[label]['rows']) == results[label]['cases']
    assert results['baseline'] == results['old-switch'], 'opt-out EE32 state/allocator rows differ'
    assert results['baseline']['sizeof'] == 88 and results['current']['sizeof'] == 76
    assert results['baseline']['map'] == 96 and results['current']['map'] == 84
    for before, after in zip(results['baseline']['rows'], results['current']['rows']):
        assert before['case'] == after['case'] and before['kind'] == after['kind']
        if before['kind'] == '0':
            for key in ('request', 'used', 'peak'):
                assert int(after[key]) <= int(before[key]), (before, after)
    report = {'results': results, 'builds': builds, 'literal_compared_bytes': len(frozen),
              'all_EE32_snapshots_equal_frozen_host_oracle': True, 'old_switch_all_rows_equal': True,
              'typed_table_request_used_peak_nonincreasing': True,
              'scope': 'Actual EE32 types, map/patch metadata constructors and lifecycle, selected driver helpers, production spill wrappers and real zone; libc/GPU/resource conversion boundaries controlled; final engine integration separate.'}
    (args.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('EE32 all', results['current']['cases'], 'cases and literal bytes equal frozen host oracle:', len(frozen), flush=True)


if __name__ == '__main__':
    main()
