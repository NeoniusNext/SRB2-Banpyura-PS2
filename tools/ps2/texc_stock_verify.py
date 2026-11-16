#!/usr/bin/env python3
"""Compare every stock TEXC byte with its actual host-compositor dump."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess

import cook
import pak_layout_verify as packtest
from resource_stream_opt17_hosttest import function
import verify_pack

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'tools/ps2'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--pak', type=Path, required=True)
    ap.add_argument('--dump', type=Path, required=True)
    ap.add_argument('--previous-dump', type=Path)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--negative-controls', action='store_true')
    a = ap.parse_args()
    a.pak = a.pak.resolve(); a.dump = a.dump.resolve(); a.out = a.out.resolve()
    a.out.mkdir(exist_ok=True, parents=True)
    sources = [ROOT / 'src/ps2/ps2_texc.c', ROOT / 'src/r_textures.c', ROOT / 'src/w_pack.c',
               ROOT / 'src/w_pack.h', ROOT / 'src/w_wad.h', TOOLS / 'cook.py', TOOLS / 'cook_texc.sh',
               TOOLS / 'texc_stock_hosttest.c', TOOLS / 'resource_stream_opt17_hosttest.c',
               TOOLS / 'resource_stream_opt17_hosttest.py', Path(__file__).resolve()]
    report = {'source_sha256': {str(p.relative_to(ROOT)): digest(p) for p in sources},
              'pack_sha256': digest(a.pak), 'dump_sha256': digest(a.dump)}
    if a.previous_dump:
        assert packtest.literal_equal(a.dump, a.previous_dump), 'repeated exports differ'
        report['repeat_export_identical'] = True
    records = cook.read_texc_dump(a.dump)
    bykey = {}
    for number, key, width, height, pixels in records:
        if key in bykey:
            assert bykey[key] == pixels, 'duplicate key has differing pixels'
        bykey[key] = pixels
    metadata = packtest.metadata(a.pak)
    table, names = metadata['table'], metadata['names']
    assert names[0] == b'TEXCINFO' and len(table) == len(bykey)+1
    expected = a.out / 'compositor-literal.raw'
    with a.pak.open('rb') as pack, expected.open('wb') as literal:
        for i, (name, entry) in enumerate(zip(names, table)):
            original = cook.TEXC_INFO + struct.pack('<III', 1, len(bykey), 0) if i == 0 else bykey[int(name, 16)]
            decoded = verify_pack.decode(pack, entry[0], entry[1], entry[2], entry[5], metadata['block'])
            assert decoded == original, 'Python decoded compositor mismatch: ' + str(name)
            literal.write(original)
    report['compositor'] = {'records': len(records), 'unique_keys': len(bykey),
                            'pixel_bytes_all_records': sum(len(r[4]) for r in records),
                            'literal_bytes_unique_plus_info': expected.stat().st_size,
                            'literal_sha256': digest(expected), 'python_all_bytes_equal': True}
    report['readers'] = {}
    for sanitize in (False, True):
        exe = packtest.build(a.out, ROOT / 'src/w_pack.c', sanitize)
        for nohead in (False, True):
            label = ('asan' if sanitize else 'plain') + ('-nohead' if nohead else '-heads')
            raw, _table, info = packtest.run_reader(exe, a.pak, a.out, label, nohead)
            assert packtest.literal_equal(raw, expected), label + ': C decoded bytes differ'
            report['readers'][label] = info
            raw.unlink()
    sdk = Path(os.environ.get('PS2SDK', '/workspace/.onboarding/ps2dev-v2/ps2dev/ps2sdk'))
    flags = ['cc', '-std=gnu17', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
             '-Wno-unused-function', '-Wno-sign-compare', '-Wno-unused-parameter',
             '-DPS2_PROFILE', '-DNOHW', '-DNDEBUG', '-DRESOURCE_BASELINE=0',
             '-I' + str(ROOT / 'src'), '-I' + str(ROOT / 'build/host-gen'), '-I' + str(sdk / 'ports/include'),
             '-DRESOURCE_PACK_SOURCE="' + str(ROOT / 'src/w_pack.c') + '"']
    fetch = function((ROOT / 'src/ps2/ps2_texc.c').read_text(), 'boolean PS2TexC_Fetch(')
    texc_source = (ROOT / 'src/ps2/ps2_texc.c').read_text()
    item_type = re.search(r'^typedef struct \{ UINT32 pos, disk, off;.*?\} tc_item_t;', texc_source, re.M).group() + '\n'
    prefetch = a.out / 'prefetch.inc'
    prefetch.write_text(item_type + ''.join(function(texc_source, signature) for signature in
        ['static int TC_CmpItem(', 'static void TC_Mark(', 'void PS2TexC_PrefetchLevel(']))

    def fetchtest(label, text, sanitize=False, reject=False, oracle=None):
        inc = a.out / (label + '-fetch.inc'); inc.write_text(text)
        exe = a.out / label
        command = [*flags, '-DRESOURCE_FETCH_SOURCE="' + str(inc) + '"',
                   '-DRESOURCE_PREFETCH_SOURCE="' + str(prefetch) + '"',
                   str(TOOLS / 'texc_stock_hosttest.c'), '-Wl,-l:liblz4.so.1', '-lz', '-o', str(exe)]
        if sanitize:
            command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie']
        p = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
        (a.out / (label + '-build.log')).write_text(p.stdout + p.stderr)
        assert p.returncode == 0, (label, p.stderr)
        p = subprocess.run([str(exe), str(a.pak), str(oracle or a.dump)], cwd=ROOT, capture_output=True, text=True, timeout=180)
        (a.out / (label + '.log')).write_text(p.stdout + p.stderr)
        if reject:
            assert p.returncode == 1 and 'FAIL' in p.stderr, (label, p.returncode, p.stdout, p.stderr)
        else:
            assert p.returncode == 0 and 'STOCK TEXC PASS' in p.stdout, (label, p.returncode, p.stdout, p.stderr)
        report[label] = {'rc': p.returncode, 'stdout': p.stdout.strip(), 'stderr': p.stderr.strip()}
        print(label, report[label], flush=True)

    fetchtest('fetch-plain', fetch)
    fetchtest('fetch-asan', fetch, sanitize=True)
    assert report['fetch-plain']['stdout'] == report['fetch-asan']['stdout']
    report['budget_stress'] = [dict(zip(['budget', 'resident', 'peak_extra', 'temporary', 'composites'], map(int, row)))
                               for row in re.findall(r'TEXC_BUDGET budget=(\d+) resident=(\d+) peak_extra=(\d+) temporary=(\d+) composites=(\d+)',
                                                     report['fetch-plain']['stdout'])]
    if a.negative_controls:
        changed = a.out / 'negative-compositor.dump'; shutil.copyfile(a.dump, changed)
        with changed.open('r+b') as stream:
            stream.seek(28); value = stream.read(1); stream.seek(28); stream.write(bytes([value[0]^1]))
        fetchtest('negative-compositor-byte', fetch, reject=True, oracle=changed)
        changed.unlink()
        baseline = subprocess.check_output(['git', 'show', '85e35eef5:src/ps2/ps2_texc.c'], cwd=ROOT).decode()
        fetchtest('negative-fetch-allocation', function(baseline, 'boolean PS2TexC_Fetch('), reject=True)
    report['storage'] = {'file_bytes': a.pak.stat().st_size, 'name_pool_bytes': len(metadata['regions']['pool']),
                         'lumps': len(table), 'largest_decoded': max(e[2] for e in table),
                         'largest_stored': max(e[1] for e in table),
                         'largest_prefetch_single_run_rounded': (max(e[1] for e in table)+4095)&~4095,
                         'unique_stored_bytes': sum(e[1] for e in {e[0]: e for e in table}.values())}
    expected.unlink()
    (a.out / 'report.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps({'compositor': report['compositor'], 'storage': report['storage']}, indent=2))


if __name__ == '__main__':
    main()
