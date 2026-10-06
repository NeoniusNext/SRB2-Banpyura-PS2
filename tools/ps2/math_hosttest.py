"""Host test: PS2 FixedMul/FixedDiv/FixedDiv2/FixedInt/FixedSqrt/FixedHypot == original implementation, bit for bit.

python tools/ps2/math_hosttest.py [--out DIR] [--pairs N] [--exhaustive-sqrt] [--negative-controls]
Candidate: working-tree src/m_fixed.h + src/m_fixed.c compiled with PS2_PROFILE (the code the EE build uses).
Reference: `git archive HEAD` copies of m_fixed.h/m_fixed.c compiled WITHOUT PS2_PROFILE (the 64-bit originals),
all exported names renamed ref_*. Negative controls compile deliberately broken copies of the working-tree header
and must be detected.
"""
import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import math_common as C  # noqa: E402

ROOT = C.ROOT


def check_guards(work):
    source = work / 'guards.c'
    work.mkdir(parents=True, exist_ok=True)
    source.write_text(
        '#include <stdlib.h>\n#include "m_fixed.h"\n#include "r_slopeq.h"\n'
        '#ifdef PS2_OPT_MATH\n#define M 1\n#else\n#define M 0\n#endif\n'
        '#ifdef PS2_OPT_SLOPE\n#define S 1\n#else\n#define S 0\n#endif\n'
        '#ifdef PS2_OPT_SEGS\n#define G 1\n#else\n#define G 0\n#endif\n'
        '#if M != EXPECT_M || S != EXPECT_S || G != EXPECT_G\n#error Incorrect math defaults/guards\n#endif\n'
        'int main(void) { return 0; }\n')
    modes = [
        ('pc', [], (0, 0, 0)),
        ('profile', ['PS2_PROFILE'], (1, 0, 0)),
        ('ps2', ['PS2'], (1, 0, 0)),
        ('noopt', ['PS2_PROFILE', 'PS2_NOOPT'], (0, 0, 0)),
        ('noopt-math', ['PS2_PROFILE', 'PS2_NOOPT_MATH'], (0, 0, 0)),
        ('experimental-without-fixed', ['PS2_PROFILE', 'PS2_NOOPT_MATH', 'PS2_OPT_SLOPE', 'PS2_OPT_SEGS'], (0, 1, 1)),
        ('noopt-overrides-explicit', ['PS2_PROFILE', 'PS2_NOOPT', 'PS2_OPT_MATH', 'PS2_OPT_SLOPE', 'PS2_OPT_SEGS'], (0, 0, 0)),
        ('group-overrides-explicit', ['PS2_PROFILE', 'PS2_NOOPT_SLOPE', 'PS2_NOOPT_SEGS', 'PS2_OPT_SLOPE', 'PS2_OPT_SEGS'], (1, 0, 0)),
        ('pc-rejects-explicit', ['PS2_OPT_MATH', 'PS2_OPT_SLOPE', 'PS2_OPT_SEGS'], (0, 0, 0)),
    ]
    for name, defines, (m, s, g) in modes:
        flags = ['/I' + str(ROOT / 'src'), *('/D' + d for d in defines),
                 f'/DEXPECT_M={m}', f'/DEXPECT_S={s}', f'/DEXPECT_G={g}']
        exe = C.msvc_build(work / name, 'guards', [(source, 'guards.obj', flags)])
        rc, _ = C.run(exe)
        if rc:
            raise RuntimeError(f'guards failed: {name}')
    text = f'math guard matrix: {len(modes)} modes PASS (PC, PS2, PS2_PROFILE, opt-in, noopt overrides)\n'
    (work / 'test.log').write_text(text)
    print(text.strip())


def ref_rename_header(ref_src, out):
    names = set(re.findall(r'\b(FV[234]_\w+|FM_\w+|FixedSqrt|FixedHypot)\s*\(', (ref_src / 'm_fixed.h').read_text()))
    out.write_text(''.join(f'#define {n} ref_{n}\n' for n in sorted(names)))


def build(work, header_dir=None, tag='cand'):
    work.mkdir(parents=True, exist_ok=True)
    ref = C.head_snapshot(work.parent / 'head')
    ref_rename_header(ref, work / 'ref_rename.h')
    (work / 'ref_wrap.c').write_text(
        '#include "m_fixed.h"\n'
        'fixed_t ref_FixedMul(fixed_t a, fixed_t b) { return FixedMul(a, b); }\n'
        'fixed_t ref_FixedDiv(fixed_t a, fixed_t b) { return FixedDiv(a, b); }\n'
        'fixed_t ref_FixedDiv2(fixed_t a, fixed_t b) { return FixedDiv2(a, b); }\n'
        'fixed_t ref_FixedInt(fixed_t a) { return FixedInt(a); }\n')
    (work / 'stubs.c').write_text(
        '#include <string.h>\n'
        'void *M_Memcpy(void *a, const void *b, size_t n) { return memcpy(a, b, n); }\n')
    inc_cand = ['/I' + str(ROOT / 'src')]
    if header_dir:
        inc_cand = ['/I' + str(header_dir)] + inc_cand
    cand = ['/DPS2_PROFILE', *inc_cand, '/I' + str(ROOT / 'src/ps2')]
    refc = ['/I' + str(ref), '/FI' + str(work / 'ref_rename.h')]
    units = [
        (ROOT / 'tools/ps2/math_hosttest.c', 'main.obj', cand),
        ((Path(header_dir) / 'm_fixed.c') if header_dir else (ROOT / 'src/m_fixed.c'), 'cand_fixed.obj', cand),
        (ref / 'm_fixed.c', 'ref_fixed.obj', refc),
        (work / 'ref_wrap.c', 'ref_wrap.obj', ['/I' + str(ref)]),
        (work / 'stubs.c', 'stubs.obj', []),
    ]
    return C.msvc_build(work, 'math_hosttest', units)


def mutate(text, old, new):
    old_n = old.replace('\n', '\r\n') if '\r\n' in text else old
    new_n = new.replace('\n', '\r\n') if '\r\n' in text else new
    if text.count(old_n) != 1:
        raise RuntimeError(f'mutation anchor not unique/missing: {old!r}')
    return text.replace(old_n, new_n)


CONTROLS = {
    'divlu-no-correction-1': ('m_fixed.c', '\t\tq1--;\n', ''),
    'divlu-no-correction-2': ('m_fixed.c', '\t\tq0--;\n', ''),
    'div2-no-sign': ('m_fixed.h', '\tconst UINT32 q = PS2_FixedDivMag(ua, ub);\n\treturn (fixed_t)(((a ^ b) < 0) ? 0u - q : q);\n#else',
                     '\tconst UINT32 q = PS2_FixedDivMag(ua, ub);\n\treturn (fixed_t)q;\n#else'),
    'mul-wrong-shift': ('m_fixed.h', '((UINT32)((UINT64)p >> 32) << (32 - FRACBITS))', '((UINT32)((UINT64)p >> 32) << 15)'),
    'sqrt-no-fixup-up': ('m_fixed.h', '\twhile ((UINT64)(r + 1) * (r + 1) <= n)\n\t\tr++;\n', ''),
    'div-saturate-sign': ('m_fixed.h', 'return (a^b) < 0 ? INT32_MIN : INT32_MAX;\n\t{ // here', 'return INT32_MAX;\n\t{ // here'),
    'divmag-skip-reduce': ('m_fixed.c', '\t\thi %= ud;', '\t\thi = 0;'),
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', type=Path, default=ROOT / 'build/agent-math/math-hosttest')
    ap.add_argument('--pairs', type=int, default=100000000)
    ap.add_argument('--sqrt', type=int, default=100000000)
    ap.add_argument('--exhaustive-sqrt', action='store_true')
    ap.add_argument('--negative-controls', action='store_true')
    ap.add_argument('--guards-only', action='store_true')
    a = ap.parse_args()
    a.out = a.out.resolve()
    a.out.mkdir(parents=True, exist_ok=True)
    check_guards(a.out / 'guards')
    if a.guards_only:
        return 0
    exe = build(a.out / 'cand')
    args = [f'--pairs={a.pairs}', f'--sqrt={a.sqrt}'] + (['--exhaustive-sqrt'] if a.exhaustive_sqrt else [])
    rc, text = C.run(exe, args, log=a.out / 'test.log')
    print(text)
    if rc:
        print('candidate FAILED')
        return 1
    if a.negative_controls:
        for name, (fname, old, new) in CONTROLS.items():
            d = a.out / ('neg-' + name)
            (d / 'hdr').mkdir(parents=True, exist_ok=True)
            for f in ('m_fixed.h', 'm_fixed.c'):
                text = (ROOT / 'src' / f).read_text()
                (d / 'hdr' / f).write_text(mutate(text, old, new) if f == fname else text)
            exe = build(d, d / 'hdr')
            rc, text = C.run(exe, ['--pairs=2000000', '--sqrt=2000000'], log=d / 'test.log')
            fails = re.search(r'failures=(\d+)', text)
            print(f'negative control {name}: exit={rc} {fails.group(0) if fails else text[-200:]}')
            if rc == 0:
                print('NEGATIVE CONTROL NOT DETECTED:', name)
                return 1
        print('all negative controls detected')
    return 0


if __name__ == '__main__':
    sys.exit(main())
