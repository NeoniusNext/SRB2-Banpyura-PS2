"""Pixel test of the actual 14 tilted drawers, independent of the full host build.

Extracts the functions (not hand-written models) from HEAD and the working tree.
Default profile must match exactly. --experimental tests explicit PS2_OPT_SLOPE
and returns nonzero on any differing pixel. Synthetic vectors isolate span math;
this is not a level replay or an A2 slope-frame tolerance measurement.
"""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path

import math_common as C


def functions(text):
    result = []
    for m in re.finditer(r'void (R_DrawTilted\w+)\(void\)\s*\{', text):
        if 'SolidColor' in m.group(1) or 'Fog' in m.group(1):
            continue
        start, pos, depth = m.start(), m.end(), 1
        while depth:
            depth += (text[pos] == '{') - (text[pos] == '}')
            pos += 1
        result.append((m.group(1), text[start:pos]))
    return result


def build(out, reference=False, experimental=False, broken=False):
    out.mkdir(parents=True, exist_ok=True)
    texts = {}
    for name in ('r_draw.c', 'r_draw8.c', 'r_draw8_npo2.c'):
        if reference:
            texts[name] = subprocess.run(['git', 'show', 'HEAD:src/' + name], cwd=C.ROOT,
                                         capture_output=True, check=True).stdout.decode().replace('\r\n', '\n')
        else:
            texts[name] = (C.ROOT / 'src' / name).read_text()
    drawer = texts['r_draw.c']
    lighting = drawer[drawer.index('static INT32 tiltlighting'):drawer.index('// ==========================================================================', drawer.index('static INT32 tiltlighting'))]
    bodies = functions(texts['r_draw8.c']) + functions(texts['r_draw8_npo2.c'])
    if len(bodies) != 14:
        raise ValueError(f'expected 14 drawers, got {len(bodies)}')
    if broken:
        name, body = bodies[0]
        if 'u += stepu;' not in body:
            raise ValueError('negative control anchor missing')
        bodies[0] = (name, body.replace('u += stepu;', 'u += stepu + (1u << 26);', 1))
    (out / 'drawers.inc').write_text(lighting + '\n' + '\n'.join(body for _, body in bodies)
                                    + '\nstatic void (*drawers[])(void) = {\n'
                                    + ',\n'.join(name for name, _ in bodies) + '\n};\n'
                                    + 'static const int is_sprite[] = {' + ','.join(str(int('FloorSprite' in name)) for name, _ in bodies) + '};\n')
    flags = ['/DPS2_PROFILE', '/I' + str(C.ROOT / 'src'), '/I' + str(out)]
    if reference:
        flags += ['/DPS2_NOOPT']
    if experimental:
        flags += ['/DPS2_OPT_SLOPE']
    exe = C.msvc_build(out, 'math_draw_hosttest', [(C.ROOT / 'tools/ps2/math_draw_hosttest.c', 'main.obj', flags)])
    return exe


def compare(ref, cand):
    a, b = ref.read_bytes(), cand.read_bytes()
    if not a or len(a) != len(b) or len(a) % 320:
        raise ValueError('invalid or unequal pixel dump sizes')
    pixels, spans = 0, 0
    for pos in range(0, len(a), 320):
        diffs = sum(x != y for x, y in zip(a[pos:pos + 320], b[pos:pos + 320]))
        pixels += diffs
        spans += diffs != 0
    return {'bytes_compared': len(a), 'spans': len(a) // 320,
            'differing_spans': spans, 'differing_pixels': pixels,
            'reference_sha256': hashlib.sha256(a).hexdigest(),
            'candidate_sha256': hashlib.sha256(b).hexdigest()}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--cases', type=int, default=10000, help='cases per drawer')
    ap.add_argument('--experimental', action='store_true')
    ap.add_argument('--negative-controls', action='store_true')
    args = ap.parse_args()
    if args.cases <= 0:
        ap.error('--cases must be positive')
    out = args.out.resolve()
    paths = {}
    for tag in ('ref', 'cand'):
        exe = build(out / tag, reference=tag == 'ref', experimental=args.experimental and tag == 'cand')
        paths[tag] = out / tag / 'pixels.bin'
        rc, text = C.run(exe, [str(paths[tag]), str(args.cases)], log=out / tag / 'test.log')
        print(tag, text.strip())
        if rc:
            raise RuntimeError(f'{tag} exited {rc}')
    report = compare(paths['ref'], paths['cand'])
    report['experimental'] = args.experimental
    report['extracted_sha256'] = {tag: hashlib.sha256((out / tag / 'drawers.inc').read_bytes()).hexdigest() for tag in ('ref', 'cand')}
    report['passed'] = report['differing_pixels'] == 0
    if args.negative_controls:
        exe = build(out / 'negative', broken=True)
        rc, _ = C.run(exe, [str(out / 'negative/pixels.bin'), str(args.cases)], log=out / 'negative/test.log')
        control = compare(paths['ref'], out / 'negative/pixels.bin')
        report['negative_control'] = control
        report['passed'] &= rc == 0 and control['differing_pixels'] > 0
        print('negative control:', control)
    (out / 'report.json').write_text(json.dumps(report, indent=2))
    print('PIXEL RESULT', report)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
