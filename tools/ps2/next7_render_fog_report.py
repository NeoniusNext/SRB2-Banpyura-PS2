"""Summarize all rows of the exact fog/solid EE benchmark, including regressions."""
import argparse
import hashlib
import json
from pathlib import Path
import re


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', required=True, type=Path)
    a = p.parse_args()
    work = a.out.resolve()
    source = (work / 'run.log').read_text(errors='replace')
    rows = []
    pattern = r'FOGBENCH drawer=(\w+) n=(\d+) mode=(\d+) calls=(\d+) before=(\d+) after=(\d+)'
    for m in re.finditer(pattern, source):
        name, length, mode, calls, before, after = m.groups()
        calls, before, after = int(calls), int(before), int(after)
        rows.append({'drawer': name, 'length': int(length), 'mode': int(mode), 'calls': calls,
                     'before': before, 'after': after, 'before_per_call': before / calls,
                     'after_per_call': after / calls, 'reduction_pct': 100 * (before - after) / before})
    if len(rows) != 168 or len({(r['drawer'], r['length'], r['mode']) for r in rows}) != 168 or 'FOG DONE' not in source:
        raise SystemExit('Incomplete benchmark, expected 168 unique rows and FOG DONE')
    groups = {}
    choices = [('all', rows), ('one_pixel', [r for r in rows if r['length'] == 1]),
               ('two_or_more_pixels', [r for r in rows if r['length'] >= 2])]
    choices.extend((name, [r for r in rows if r['drawer'] == name]) for name in dict.fromkeys(r['drawer'] for r in rows))
    for name, selected in choices:
        before, after = sum(r['before'] for r in selected), sum(r['after'] for r in selected)
        groups[name] = {'rows': len(selected), 'before': before, 'after': after,
                        'reduction_pct': 100 * (before - after) / before,
                        'faster': sum(r['after'] < r['before'] for r in selected),
                        'equal': sum(r['after'] == r['before'] for r in selected),
                        'slower': sum(r['after'] > r['before'] for r in selected),
                        'minimum_pct': min(r['reduction_pct'] for r in selected),
                        'maximum_pct': max(r['reduction_pct'] for r in selected)}
    report = {'groups': groups, 'rows': rows,
              'elf_sha256': hashlib.sha256((work / 'FOG.ELF').read_bytes()).hexdigest(),
              'all_rows_check_complete_framebuffer_canaries_lighting_and_cursor_before_timing': True,
              'timing': '2048 calls, best of five paired repetitions, alternating order',
              'scope': 'selected EE kernels under PCSX2; no whole-game FPS or physical-PS2 claim'}
    (work / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(groups, indent=2))
    print('Slower rows:', json.dumps([r for r in rows if r['after'] > r['before']], indent=2))


if __name__ == '__main__':
    main()
