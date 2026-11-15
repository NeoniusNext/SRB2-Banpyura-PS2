#!/usr/bin/env python3
"""Build/run the same complete real-zone sprite proof on EE32, serially."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

import build as B
from next8_memory_hosttest import ROOT, TOOLS, parts


def run(command, log, timeout=180):
    result = subprocess.run(list(map(str, command)), cwd=ROOT, capture_output=True, text=True, timeout=timeout)
    log.write_text(result.stdout + result.stderr)
    assert result.returncode == 0, str(log) + ': ' + result.stdout[-1000:] + result.stderr[-4000:]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, default=ROOT / 'build/nov15-next8/baseline-src')
    parser.add_argument('--out', type=Path, default=ROOT / 'build/nov15-next8/memory/ee')
    parser.add_argument('--host-proof', type=Path, default=ROOT / 'build/nov15-next8/memory/host')
    parser.add_argument('--run', action='store_true')
    parser.add_argument('--analyze-only', action='store_true')
    args = parser.parse_args()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    host = args.host_proof.resolve()
    proof = json.loads((host / 'report.json').read_text())
    frozen = (host / 'baseline/snapshot.bin').read_bytes()
    manifest = (host / 'stock.bin').read_bytes()
    assert hashlib.sha256(manifest).hexdigest() == proof['stock_manifest_sha256']
    flags = [flag for flag in B.CFLAGS if flag not in ('-flto', '-MMD', '-MP', '-D_EE', '-DPS2', '-DPS2_PROFILE')]
    builds = {}
    for label, root, extra in [('baseline', args.baseline.resolve(), []), ('current', ROOT, []),
                               ('old-switch', ROOT, ['-DPS2_NOOPT_SPRFRAME_LAYOUT'])]:
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
            (work / 'stock.bin').write_bytes(manifest)
            obj = work / 'driver.o'
            run([B.CC, *flags, '-U_EE', '-DPARANOIA', '-Wno-unused-function', '-Wno-unused-variable',
                 '-Wno-sign-compare', '-Wno-clobbered', '-I' + str(TOOLS), *B.INCS, *defines, *extra,
                 '-c', TOOLS / 'next8_memory_hosttest.c', '-o', obj], work / 'build.log')
            run([B.CC, *B.LDFLAGS, obj, '-o', elf, '-ldebug', '-lpatches', '-lm'], work / 'link.log')
            inputs = {'elf_sha256': hashlib.sha256(elf.read_bytes()).hexdigest(), 'source_sha256': hashes,
                      'fixture_sha256': hashlib.sha256((TOOLS / 'next8_memory_hosttest.c').read_bytes()).hexdigest(),
                      'stock_manifest_sha256': hashlib.sha256(manifest).hexdigest(),
                      'host_oracle_sha256': hashlib.sha256(frozen).hexdigest()}
            (work / 'inputs.json').write_text(json.dumps(inputs, indent=2) + '\n')
        else:
            inputs = json.loads((work / 'inputs.json').read_text())
        assert hashlib.sha256(elf.read_bytes()).hexdigest() == inputs['elf_sha256']
        assert hashlib.sha256((work / 'stock.bin').read_bytes()).hexdigest() == inputs['stock_manifest_sha256']
        assert hashlib.sha256(frozen).hexdigest() == inputs['host_oracle_sha256']
        assert hashlib.sha256((TOOLS / 'next8_memory_hosttest.c').read_bytes()).hexdigest() == inputs['fixture_sha256']
        assert {macro: hashlib.sha256(content.encode()).hexdigest() for macro, content in parts(root).items()} == inputs['source_sha256']
        builds[label] = inputs
        print('Built' if not args.analyze_only else 'Analyzing', elf, flush=True)
        if args.run:
            (work / 'snapshot.bin').unlink(missing_ok=True)
            run([sys.executable, '-B', TOOLS / 'run_pcsx2.py', '--elf', elf, '--log', work / 'run.log',
                 '--args=MEMORY.ELF host:stock.bin host:snapshot.bin', '--until', 'MEMORY8 DONE', '--timeout', '60'],
                work / 'runner.log', timeout=90)
    if not (args.run or args.analyze_only):
        return
    results = {}
    for label in builds:
        work = args.out / label
        log = (work / 'run.log').read_text()
        assert 'pcsx2 exit code 0' in (work / 'runner.log').read_text()
        assert (work / 'snapshot.bin').read_bytes() == frozen, label + ' EE32 literal snapshot differs'
        tail = re.search(r'MEMORY8 DONE cases=(\d+) checks=(\d+) stockframes=(\d+) stockroots=(\d+) sizeof=(\d+) align=(\d+)', log)
        assert tail, label + ' incomplete'
        results[label] = dict(zip(['cases', 'checks', 'stockframes', 'stockroots', 'sizeof', 'align'], map(int, tail.groups())))
        results[label]['rows'] = [dict(re.findall(r'(\w+)=([\da-f]+)', line)) for line in log.splitlines() if 'MEMORY8 case=' in line]
        assert results[label]['cases'] == 69 and results[label]['align'] == 4
        assert len(results[label]['rows']) == 69
        assert results[label]['stockframes'] == proof['results']['current']['stockframes']
        assert results[label]['stockroots'] == proof['results']['current']['stockroots']
    assert results['baseline']['sizeof'] == results['old-switch']['sizeof'] == 108
    assert results['current']['sizeof'] == 104
    assert results['baseline'] == results['old-switch']
    for before, after in zip(results['baseline']['rows'], results['current']['rows']):
        assert before.keys() == after.keys()
        for key in before:
            if key in ('request', 'used', 'peak'):
                assert int(after[key]) <= int(before[key]), (before, after)
            else:
                assert before[key] == after[key], (before, after)
    stock_before, stock_after = results['baseline']['rows'][-1], results['current']['rows'][-1]
    report = {'results': results, 'builds': builds, 'literal_compared_bytes': len(frozen),
              'all_EE32_snapshots_equal_frozen_host_oracle': True, 'old_switch_all_rows_equal': True,
              'stock_replay_arena_saving': int(stock_before['used']) - int(stock_after['used']),
              'stock_replay_peak_saving': int(stock_before['peak']) - int(stock_after['peak']),
              'no_measured_request_used_or_peak_increase': True,
              'scope': 'Actual EE32 sprite metadata, loader, growth statements, rotation-cache accessors and real zone; boundary resource metadata/rotation kernel stubs; full-engine integration is separate.'}
    (args.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('EE32 literal bytes equal:', len(frozen), 'stock replay arena saving:', report['stock_replay_arena_saving'], flush=True)


if __name__ == '__main__':
    main()
