"""Validate and summarize the complete, checked next6 NPOT EE timing stream."""
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
    log = (work / 'run.log').read_text()
    rows = []
    for m in re.finditer(r'NPOTBENCH (\S+) dim=(\d+)x(\d+) n=(\d+) dense=(\d+) calls=(\d+) base=(\d+) cand=(\d+) unit=(\S+)', log):
        rows.append({'drawer': m[1], 'width': int(m[2]), 'height': int(m[3]),
                     'length': int(m[4]), 'dense': int(m[5]), 'calls': int(m[6]),
                     'base': int(m[7]), 'candidate': int(m[8]), 'unit': m[9]})
    if len(rows) != 252 or len({(r['drawer'], r['width'], r['height'], r['length'], r['dense']) for r in rows}) != 252:
        raise SystemExit('Expected all 252 unique benchmark rows')
    if 'NPOT DONE' not in log or 'NPOT EE FAIL' in log or 'NPOT FAIL' in log:
        raise SystemExit('Benchmark did not finish cleanly')
    if any(r['unit'] != 'cycles_x10' or r['calls'] != 1024 for r in rows):
        raise SystemExit('Unexpected timing units or call count')
    cache_rows = []
    for m in re.finditer(r'NPOTCACHE (\S+) cycle=63x47,257x17,319x241 n=(\d+) calls=(\d+) base=(\d+) cand=(\d+) unit=(\S+)', log):
        cache_rows.append({'drawer': m[1], 'length': int(m[2]), 'calls': int(m[3]),
                           'base': int(m[4]), 'candidate': int(m[5]), 'unit': m[6]})
    if len(cache_rows) != 21 or len({(r['drawer'], r['length']) for r in cache_rows}) != 21:
        raise SystemExit('Expected all 21 unique cache-thrash rows')
    if any(r['unit'] != 'cycles_x10' or r['calls'] != 768 for r in cache_rows):
        raise SystemExit('Unexpected cache-thrash units or call count')

    def summarize(items):
        base = sum(r['base'] for r in items)
        cand = sum(r['candidate'] for r in items)
        percentages = [(r['base'] - r['candidate']) * 100 / r['base'] for r in items]
        return {'rows': len(items), 'base_sum': base, 'candidate_sum': cand,
                'reduction_percent': (base - cand) * 100 / base,
                'faster': sum(r['candidate'] < r['base'] for r in items),
                'equal': sum(r['candidate'] == r['base'] for r in items),
                'slower': sum(r['candidate'] > r['base'] for r in items),
                'minimum_reduction_percent': min(percentages),
                'maximum_reduction_percent': max(percentages)}

    report = {'elf_sha256': hashlib.sha256((work / 'NPOT.ELF').read_bytes()).hexdigest(),
              'overall': summarize(rows),
              'drawers': {name: summarize([r for r in rows if r['drawer'] == name])
                          for name in dict.fromkeys(r['drawer'] for r in rows)},
              'cache_thrash': summarize(cache_rows),
              'rows': rows, 'cache_rows': cache_rows}
    (work / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: v for k, v in report.items() if k not in ('rows', 'cache_rows')}, indent=2))


if __name__ == '__main__':
    main()
