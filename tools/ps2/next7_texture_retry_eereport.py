"""Validate the complete EE32 recovery row/status and frozen byte-oracle proof."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
from next7_texture_retry_hosttest import snapshots


def target_charge(blob):
    fallback, width, height = struct.unpack_from('<III', blob)
    assert not fallback
    position, posts = 20 + width * height, 0
    for _ in range(width):
        count = struct.unpack_from('<I', blob, position)[0]
        posts += count
        position += 4 + 12 * count
    assert position == len(blob)
    # Actual EE32 column_t/post_t sizes and alignments, asserted in the ELF.
    columns = (width * height + 3) & ~3
    return columns + 12 * width + 12 * posts


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', required=True, type=Path)
    p.add_argument('--host-proof', required=True, type=Path)
    a = p.parse_args()
    out, host = a.out.resolve(), a.host_proof.resolve()
    inputs = json.loads((out / 'inputs.json').read_text())
    assert hashlib.sha256((out / 'RETRY.ELF').read_bytes()).hexdigest() == inputs['elf_sha256']
    proof = json.loads((host / 'report.json').read_text())
    log = (out / 'run.log').read_text()
    runner = (out / 'runner.log').read_text()
    assert 'pcsx2 exit code 0' in runner
    done = re.search(r'RETRY DONE verified_bytes=(\d+) columns=12 posts=12 checks=(\d+)', log)
    assert done
    rows = [dict(re.findall(r'(\w+)=([\da-f]+)', line))
            for line in log.splitlines() if 'RETRY case=' in line]
    expected = proof['candidate']['rows']
    assert len(rows) == len(expected) == 135
    fields = ('case', 'fail', 'afail', 'rfail', 'change', 'fallback', 'finals',
              'allocs', 'reads', 'normalreads', 'converters', 'hash')
    for i, (actual, reference) in enumerate(zip(rows, expected)):
        assert all(actual[k] == reference[k] for k in fields), (i, actual, reference)
    frozen = snapshots(host / 'baseline/snapshots.bin')
    charges = {r['case']: target_charge(b) for r, b in zip(proof['baseline']['rows'], frozen)
               if r['fail'] == '0' and r['fallback'] == '0'}
    charges['5'] = charges['0']
    assert all(int(row['bytes']) == charges[row['case']] for row in rows)
    assert int(done[1]) == proof['complete_compared_bytes']
    natural = next(r for r in rows if r['case'] == '5')
    assert natural['fail'] == '0' and natural['fallback'] == '0' and natural['finals'] == '2'
    assert natural['bytes'] == '530432'
    report = {'elf_sha256': inputs['elf_sha256'], 'source_sha256': inputs['source_sha256'],
              'scope': inputs['scope'], 'rows': len(rows),
              'complete_compared_bytes': int(done[1]), 'checks': int(done[2]),
              'column_bytes': 12, 'post_bytes': 12,
              'status_and_hash_fields_match_real_zone_host_proof': list(fields),
              'all_target_memory_charges_match_frozen_post_layout': True,
              'all_rows_bounded_at_two_attempts': all(int(r['finals']) <= 2 for r in rows),
              'natural_fragmentation_recovery': natural,
              'run_sha256': hashlib.sha256((out / 'run.log').read_bytes()).hexdigest(),
              'runner_exit_zero': True}
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
