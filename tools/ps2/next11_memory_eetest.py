#!/usr/bin/env python3
"""Build/run the unchanged complete slope/pool proof on EE32 serially."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

import build as B
from next11_memory_hosttest import ROOT, TOOLS, parts, rows, compare_rows
from next11_memory_replay import validate_candidate


def run(command, log, timeout=300):
    result = subprocess.run(list(map(str, command)), cwd=ROOT, capture_output=True, text=True, timeout=timeout)
    log.write_text(result.stdout + result.stderr)
    assert result.returncode == 0, str(log) + ': ' + result.stdout[-1000:] + result.stderr[-4000:]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, default=ROOT / 'build/nov15-next11/baseline-src')
    parser.add_argument('--candidate', type=Path, default=ROOT / 'build/nov15-next11/memory/byte-pool/candidate-src')
    parser.add_argument('--out', type=Path)
    parser.add_argument('--host-proof', type=Path)
    parser.add_argument('--run', action='store_true')
    parser.add_argument('--analyze-only', action='store_true')
    args = parser.parse_args()
    stage = validate_candidate(args.candidate)
    replay = ROOT / ('build/nov15-next11/memory/replay-' + stage)
    args.out = (args.out or replay / 'ee').resolve()
    args.host_proof = args.host_proof or replay / 'host'
    if not args.out.is_relative_to(ROOT / 'build') or args.out == ROOT / 'build':
        parser.error('--out must be an isolated child of build/')
    if not args.analyze_only and any((args.out / name / 'inputs.json').exists() for name in ('baseline','current','old-switch','pool-old')):
        parser.error('Compiled witness inputs exist; use --analyze-only or choose a fresh --out')
    args.out.mkdir(parents=True, exist_ok=True)
    host = args.host_proof.resolve()
    proof = json.loads((host / 'report.json').read_text())
    frozen = (host / 'baseline/snapshot.bin').read_bytes()
    fixture = TOOLS / 'next11_memory_hosttest.c'
    assert hashlib.sha256(frozen).hexdigest() == proof['snapshot_sha256']
    assert hashlib.sha256(fixture.read_bytes()).hexdigest() == proof['fixture_sha256']
    flags = [f for f in B.CFLAGS if f not in ('-flto', '-MMD', '-MP', '-D_EE', '-DPS2', '-DPS2_PROFILE', '-DHWRENDER')]
    flags += ['-U_EE', '-DPARANOIA', '-fwrapv', '-Werror', '-Wno-unused-function', '-Wno-unused-variable', '-Wno-sign-compare', '-Wno-clobbered']
    builds = {}
    for label, root, extra in [('baseline', args.baseline.resolve(), []), ('current', args.candidate.resolve(), []),
                               ('old-switch', args.candidate.resolve(), ['-DPS2_NOOPT_SLOPE_LAYOUT']),
                               ('pool-old', args.candidate.resolve(), ['-DPS2_NOOPT_SLOPE_POOL_BYTES'])]:
        work = args.out / label
        work.mkdir(exist_ok=True)
        elf = work / 'MEMORY.ELF'
        if not args.analyze_only:
            defines, hashes = [], {}
            for macro, content in parts(root).items():
                path = work / (macro.lower() + '.inc')
                path.write_text(content)
                defines.append('-D' + macro + '=\"' + str(path) + '\"')
                hashes[macro] = hashlib.sha256(content.encode()).hexdigest()
            obj = work / 'driver.o'
            run([B.CC, *flags, '-I' + str(TOOLS), *B.INCS, *defines, *extra, '-c', fixture, '-o', obj], work / 'build.log')
            run([B.CC, *B.LDFLAGS, obj, '-o', elf, '-ldebug', '-lpatches', '-lm'], work / 'link.log')
            inputs = {'elf_sha256': hashlib.sha256(elf.read_bytes()).hexdigest(), 'source_sha256': hashes,
                      'fixture_sha256': hashlib.sha256(fixture.read_bytes()).hexdigest(),
                      'host_oracle_sha256': hashlib.sha256(frozen).hexdigest()}
            (work / 'inputs.json').write_text(json.dumps(inputs, indent=2) + '\n')
        else:
            inputs = json.loads((work / 'inputs.json').read_text())
        assert not (work / 'build.log').read_text().strip(), label + ' fixture compile diagnostics'
        link_diagnostics = [line for line in (work / 'link.log').read_text().splitlines()
                            if 'linking abicalls files with non-abicalls files' not in line]
        assert not any(line.strip() for line in link_diagnostics), label + ' unexpected fixture link diagnostics'
        assert hashlib.sha256(elf.read_bytes()).hexdigest() == inputs['elf_sha256']
        assert hashlib.sha256(frozen).hexdigest() == inputs['host_oracle_sha256']
        assert hashlib.sha256(fixture.read_bytes()).hexdigest() == inputs['fixture_sha256']
        assert {k: hashlib.sha256(v.encode()).hexdigest() for k, v in parts(root).items()} == inputs['source_sha256']
        builds[label] = inputs
        print('Built' if not args.analyze_only else 'Analyzing', elf, flush=True)
        if args.run:
            from next11_runtime import run_verified
            (work / 'snapshot.bin').unlink(missing_ok=True)
            command = list(map(str, [sys.executable, '-B', TOOLS / 'run_pcsx2.py', '--elf', elf, '--log', work / 'run.log',
                                    '--args=MEMORY.ELF host:snapshot.bin', '--until', 'MEMORY11 DONE', '--timeout', '300']))
            frozen_inputs = [elf, work / 'driver.o', work / 'inputs.json', fixture, Path(__file__).resolve(),
                             host / 'report.json', host / 'baseline/snapshot.bin', root / 'src/r_defs.h',
                             root / 'src/z_zone.c', root / 'src/ps2/ps2_mem.c', *work.glob('*.inc')]
            result = run_verified(command, work / 'runtime-provenance', frozen_inputs)
            (work / 'runner.log').write_text(result.stdout + result.stderr)
            assert result.returncode == 0, str(work / 'runner.log') + ': ' + result.stderr[-4000:]
    if not (args.run or args.analyze_only):
        return
    results = {}
    for label in builds:
        work = args.out / label
        log = (work / 'run.log').read_text()
        assert 'pcsx2 exit code 0' in (work / 'runner.log').read_text()
        actual = (work / 'snapshot.bin').read_bytes()
        assert actual == frozen, label + ' EE32 snapshot differs at ' + str(next((i for i, (a, b) in enumerate(zip(actual, frozen)) if a != b), min(len(actual), len(frozen))))
        tail = re.search(r'MEMORY11 DONE cases=(\d+) checks=(\d+) sizeof=(\d+) align=(\d+) stride=(\d+) slots=(\d+)', log)
        assert tail, label + ' incomplete'
        results[label] = dict(zip(('cases', 'checks', 'sizeof', 'align', 'stride', 'slots'), map(int, tail.groups())))
        results[label]['rows'] = rows(log)
        assert results[label]['cases'] == proof['results']['current']['cases']
        assert results[label]['align'] == 8 and len(results[label]['rows']) == results[label]['cases']
    assert results['baseline'] == results['old-switch'], 'opt-out EE32 state/allocator rows differ'
    assert results['baseline']['sizeof'] == 120 and results['current']['sizeof'] == 112
    assert results['baseline']['stride'] == 128 and results['current']['stride'] == 112
    assert results['baseline']['slots'] == results['old-switch']['slots'] == results['pool-old']['slots'] == 64
    assert results['current']['slots'] == (73 if stage == 'byte-pool' else 64)
    compare_rows(results['baseline']['rows'], results['current']['rows'])
    compare_rows(results['baseline']['rows'], results['pool-old']['rows'])
    report = {'candidate_stage': stage, 'results': results, 'builds': builds, 'literal_compared_bytes': len(frozen),
              'all_EE32_snapshots_equal_frozen_host_oracle': True, 'old_switch_all_rows_equal': True,
              'pure_array_pool_request_used_peak_nonincreasing': True, 'scope': proof['scope']}
    (args.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('EE32 all', results['current']['cases'], 'cases and literal bytes equal frozen host oracle:', len(frozen), flush=True)


if __name__ == '__main__':
    main()
