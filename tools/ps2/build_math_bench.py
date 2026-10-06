"""Build the EE math micro-benchmark ELF (tools/ps2/math_bench.c) and optionally run it in PCSX2.

usage: SRB2_PS2_OUT=<dir> python tools/ps2/build_math_bench.py [--run] [--timeout S]
Candidate units: working-tree src/m_fixed.c with the engine flags (PS2_PROFILE).
Reference units: `git archive HEAD` m_fixed.c/m_fixed.h compiled without PS2_PROFILE, exported names ref_*.
Output: <out>/math-bench/MATH_BENCH.ELF, <out>/math-bench/run.log
"""
import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build as B  # noqa: E402
import math_common as C  # noqa: E402

OUT = B.OUT / 'math-bench'
LIBS = ['-ldebug', '-lpatches', '-lm']
COMMANDS = []


def ee_cc(src, obj, extra=(), drop=(), incs_first=()):
    flags = [f for f in B.CFLAGS if f not in drop and not f.startswith('-MMD') and f != '-MP']
    cmd = [str(B.CC)] + flags + [*incs_first] + B.INCS + [*extra, '-c', str(src), '-o', str(obj)]
    COMMANDS.append(cmd)
    p = subprocess.run(cmd, env=B.ENV, capture_output=True, text=True, cwd=B.ROOT)
    (OUT / 'commands.json').write_text(json.dumps(COMMANDS, indent=2))
    with (OUT / 'build.log').open('a', encoding='utf-8') as log:
        log.write(f'{src}: exit={p.returncode}\n{p.stdout}{p.stderr}')
    out = (p.stdout + p.stderr).strip()
    if p.returncode or out:
        print(f'=== {src} rc={p.returncode}\n{out}')
        raise SystemExit(1)
    return obj


def build(extra_units=()):
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / 'build.log').write_text('')
    B.gen_config()
    ref = C.head_snapshot(OUT / 'head')
    names = set(re.findall(r'\b(FV[234]_\w+|FM_\w+|FixedSqrt|FixedHypot)\s*\(', (ref / 'm_fixed.h').read_text()))
    (OUT / 'ref_rename.h').write_text(''.join(f'#define {n} ref_{n}\n' for n in sorted(names)))
    (OUT / 'ref_wrap.c').write_text(
        '#include "m_fixed.h"\n'
        'fixed_t ref_FixedMul(fixed_t a, fixed_t b) { return FixedMul(a, b); }\n'
        'fixed_t ref_FixedDiv(fixed_t a, fixed_t b) { return FixedDiv(a, b); }\n'
        'fixed_t ref_FixedDiv2(fixed_t a, fixed_t b) { return FixedDiv2(a, b); }\n')
    (OUT / 'stubs.c').write_text('#include <string.h>\nvoid *M_Memcpy(void *a, const void *b, unsigned n) { return memcpy(a, b, n); }\n')
    objs = []
    objs.append(ee_cc(B.ROOT / 'tools/ps2/math_bench.c', OUT / 'math_bench.o'))
    objs.append(ee_cc(B.ROOT / 'src/m_fixed.c', OUT / 'm_fixed.o'))
    refinc = ['-I' + str(ref)]
    objs.append(ee_cc(ref / 'm_fixed.c', OUT / 'ref_m_fixed.o', extra=['-include', str(OUT / 'ref_rename.h')], drop=['-DPS2_PROFILE'], incs_first=refinc))
    objs.append(ee_cc(OUT / 'ref_wrap.c', OUT / 'ref_wrap.o', drop=['-DPS2_PROFILE'], incs_first=refinc))
    objs.append(ee_cc(OUT / 'stubs.c', OUT / 'stubs.o'))
    for src, obj, extra in extra_units:
        objs.append(ee_cc(src, obj, extra=extra))
    elf = OUT / 'MATH_BENCH.ELF'
    cmd = [str(B.CC)] + B.LDFLAGS + [str(o) for o in objs] + ['-o', str(elf), '-Wl,-Map=' + str(OUT / 'bench.map')] + LIBS
    COMMANDS.append(cmd)
    (OUT / 'commands.json').write_text(json.dumps(COMMANDS, indent=2))
    p = subprocess.run(cmd, env=B.ENV, capture_output=True, text=True, cwd=B.ROOT)
    if p.returncode or (p.stdout + p.stderr).strip():
        print(p.stdout + p.stderr)
        raise SystemExit(1)
    print('built', elf, elf.stat().st_size)
    return elf


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--run', action='store_true')
    ap.add_argument('--timeout', type=float, default=240)
    a = ap.parse_args()
    elf = build()
    if a.run:
        log = OUT / 'run.log'
        rc = subprocess.run([sys.executable, str(B.ROOT / 'tools/ps2/run_pcsx2.py'), '--elf', str(elf), '--log', str(log),
                              '--until', 'MB DONE', '--marker', 'MB ', '--timeout', str(a.timeout)]).returncode
        text = log.read_text(errors='replace') if log.exists() else ''
        counts = re.search(r'MB selftest pairs=(\d+) checks=(\d+) failures=(\d+)', text)
        passed = rc == 0 and counts is not None and counts.group(3) == '0' and 'MB FAIL' not in text and 'MB SELFTEST FAILED' not in text
        report = {'wrapper_exit': rc, 'passed': passed,
                  'elf_sha256': hashlib.sha256(elf.read_bytes()).hexdigest(),
                  'selftest': dict(zip(('pairs', 'checks', 'failures'), map(int, counts.groups()))) if counts else None,
                  'lines': [line for line in text.splitlines() if 'MB ' in line]}
        (OUT / 'report.json').write_text(json.dumps(report, indent=2))
        for line in report['lines']:
            print(line)
        return 0 if passed else 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
