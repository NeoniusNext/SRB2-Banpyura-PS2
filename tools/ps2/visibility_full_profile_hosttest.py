#!/usr/bin/env python3
"""Validate the raw full-size profile reader against a retained native run."""
import argparse
import json
from pathlib import Path

from visibility_full_profile import ROOT, compare, inspect, sha


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('run', 'runtime', 'elf', 'out'):
        p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--pak', type=Path, default=ROOT / 'build/pak')
    p.add_argument('--map', default='MAP02')
    p.add_argument('--frames', type=int, default=350)
    a = p.parse_args()
    run, runtime, elf, out = [getattr(a, k).resolve() for k in ('run', 'runtime', 'elf', 'out')]
    out.mkdir(parents=True, exist_ok=False)
    assets = sorted(a.pak.resolve().glob('*.PAK')) + [a.pak.resolve() / 'FINEACON.DAT']
    baseline = inspect(run, runtime, elf, assets, a.map, a.frames)
    if baseline['errors']:
        raise RuntimeError('Positive native input invalid: ' + str(baseline['errors']))
    mutated = out / 'mutated-run'
    mutated.mkdir()
    for path in run.iterdir():
        (mutated / path.name).symlink_to(path, target_is_directory=path.is_dir())
    controls = []

    def reject(name, content, required):
        path = mutated / name
        path.unlink()
        path.write_text(content)
        report = inspect(mutated, runtime, elf, assets, a.map, a.frames)
        if not any(required in error for error in report['errors']):
            raise AssertionError('Mutation not rejected: ' + required)
        controls.append({'mutation': required, 'errors': report['errors']})
        path.unlink()
        path.symlink_to(run / name)

    args = (run / 'ps2args').read_text()
    reject('ps2args', args.replace('33554432', '0'), 'arguments')
    summary = json.loads((run / 'summary.json').read_text())
    summary['zstat'][0]['ram'] = '67108864'
    reject('summary.json', json.dumps(summary), 'Physical memory')
    summary = json.loads((run / 'summary.json').read_text())
    summary['ram'] = 64
    reject('summary.json', json.dumps(summary), 'official runner')
    boot = (run / 'boot.txt').read_text()
    reject('boot.txt', boot.replace(' missing=0 ', ' missing=1 '), 'content')
    reject('boot.txt', boot.replace(' skipped=0 ', ' skipped=1 '), 'content')
    reject('boot.txt', boot + '\nHWSKIP f=384 injected missing content after final profile window\n', 'outside profile windows')
    same = compare(baseline, baseline)
    if any(same['numeric_ZSTAT_deltas'].values()) or not all(
            row['equal'] for row in same['ordered_stream_comparison_not_a_quality_gate'].values()):
        raise AssertionError('Same native input does not compare equal')
    # Failure/eviction numbers are deliberately neither cleared nor treated as
    # this mode's strict correctness gate. Their original native values survive.
    if baseline['ZSTAT'] != {k: int(v) for k, v in json.loads((run / 'summary.json').read_text())['zstat'][0].items()}:
        raise AssertionError('Raw native counters changed')
    result = {'passed': True, 'tool_sha256': sha(ROOT / 'tools/ps2/visibility_full_profile.py'),
              'native_baseline': baseline, 'self_comparison': same,
              'executed_negative_controls': controls}
    (out / 'report.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'passed': True, 'negative_controls': len(controls),
                      'retained_gameplay_evictions': baseline['gameplay_zone_evictions_after_startup_flush'],
                      'retained_failed_attempts': baseline['ZSTAT']['failed']}))


if __name__ == '__main__':
    main()
