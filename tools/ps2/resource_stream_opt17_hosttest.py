#!/usr/bin/env python3
"""Actual SRP2/TEXC allocation, bounded CRC and failure/lifecycle proof."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'tools/ps2'


def function(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end] + '\n'


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--baseline', type=Path, default=ROOT / 'build/nov17-resource/baseline')
    ap.add_argument('--out', type=Path, default=ROOT / 'build/nov17-resource/host')
    ap.add_argument('--negative-controls', action='store_true')
    a = ap.parse_args()
    a.out = a.out.resolve()
    a.out.mkdir(parents=True, exist_ok=True)
    for name in ['src/w_pack.c', 'src/ps2/ps2_texc.c']:
        target = a.baseline / name
        if not target.exists():
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(subprocess.check_output(['git', 'show', '85e35eef5:' + name], cwd=ROOT))
    sdk = Path(os.environ.get('PS2SDK', '/workspace/.onboarding/ps2dev-v2/ps2dev/ps2sdk'))
    report = {'sources': {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in [ROOT / 'src/w_pack.c', ROOT / 'src/w_pack.h', ROOT / 'src/w_wad.h', ROOT / 'src/ps2/ps2_texc.c',
                                    TOOLS / 'resource_stream_opt17_hosttest.c', Path(__file__).resolve()]}}
    report['baseline_revision'] = '85e35eef5'
    report['baseline_sources'] = {name: hashlib.sha256((a.baseline / name).read_bytes()).hexdigest()
                                  for name in ['src/w_pack.c', 'src/ps2/ps2_texc.c']}
    flags = ['cc', '-std=gnu17', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
             '-Wno-unused-function', '-Wno-sign-compare', '-Wno-unused-parameter',
             '-DPS2_PROFILE', '-DNOHW', '-DNDEBUG', '-I' + str(ROOT / 'src'),
             '-I' + str(ROOT / 'build/host-gen'), '-I' + str(sdk / 'ports/include')]

    def test(label, pack, fetch, baseline=False, sanitize=False, reject=False):
        work = a.out / label
        work.mkdir(exist_ok=True)
        fetchpath = work / 'fetch.inc'
        fetchpath.write_text(fetch)
        exe = work / 'test'
        command = [*flags, '-DRESOURCE_PACK_SOURCE="' + str(pack.resolve()) + '"',
                   '-DRESOURCE_FETCH_SOURCE="' + str(fetchpath) + '"',
                   '-DRESOURCE_BASELINE=' + str(int(baseline)),
                   str(TOOLS / 'resource_stream_opt17_hosttest.c'), '-Wl,-l:liblz4.so.1', '-lz', '-o', str(exe)]
        if sanitize:
            command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie']
        p = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
        (work / 'build.log').write_text(p.stdout + p.stderr)
        assert p.returncode == 0, (label, p.stderr)
        p = subprocess.run([str(exe)], cwd=ROOT, capture_output=True, text=True, timeout=120)
        (work / 'test.log').write_text(p.stdout + p.stderr)
        if reject:
            assert p.returncode == 1 and 'FAIL' in p.stderr, (label, p.returncode, p.stdout, p.stderr)
        else:
            assert p.returncode == 0 and 'PASS checks=' in p.stdout, (label, p.returncode, p.stdout, p.stderr)
        report[label] = {'rc': p.returncode, 'stdout': p.stdout.strip(), 'stderr': p.stderr.strip()}
        print(label, report[label], flush=True)

    pack = ROOT / 'src/w_pack.c'
    fetch = function((ROOT / 'src/ps2/ps2_texc.c').read_text(), 'boolean PS2TexC_Fetch(')
    oldfetch = function((a.baseline / 'src/ps2/ps2_texc.c').read_text(), 'boolean PS2TexC_Fetch(')
    test('baseline', a.baseline / 'src/w_pack.c', oldfetch, baseline=True)
    test('current', pack, fetch)
    test('sanitize', pack, fetch, sanitize=True)
    assert report['current']['stdout'] == report['sanitize']['stdout']
    io = r'cmds=\d+ hits=\d+ misses=\d+ bulk=\d+ bytes=\d+'
    assert re.search(io, report['baseline']['stdout']).group() == re.search(io, report['current']['stdout']).group()
    if a.negative_controls:
        packtext = pack.read_text()
        variants = {
            'reset-chunk-crc': ('crc = Crc32(crc, buf, chunk);', 'crc = Crc32(0, buf, chunk);'),
            'unbounded-verify': ('const size_t need = min((size_t)l->size, (size_t)(4 * WPACK_BLOCK));',
                                 'const size_t need = l->size;'),
            'prefetch-cap-after-realloc': ('if (cap > 65536)\n\t\t\treturn;\n\t\tgrown = realloc(pf.items, cap * sizeof *grown);\n\t\tif (!grown)',
                                           'grown = realloc(pf.items, cap * sizeof *grown);\n\t\tif (!grown || cap > 65536)'),
            'retain-prefetch-index': ('free(pf.items);\n\tpf.items = NULL;\n\tpf.cap = 0;', ''),
        }
        for label, (before, after) in variants.items():
            assert before in packtext, label
            variant = a.out / (label + '.c')
            variant.write_text(packtext.replace(before, after, 1))
            test('negative-' + label, variant, fetch, reject=True)
        test('negative-fetch-allocation', pack, oldfetch, reject=True)
        bad = fetch.replace('if ((size_t)l->size != bytes)', 'if (0)')
        assert bad != fetch
        test('negative-fetch-size-guard', pack, bad, reject=True)
        bad = fetch.replace('ok = WPack_DecodeMem(l->compression, TC.arena + TC.toff[texnum] - 1, l->disksize, dest, l->size);', 'ok = true;')
        assert bad != fetch
        test('negative-resident-decode', pack, bad, reject=True)
    (a.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
