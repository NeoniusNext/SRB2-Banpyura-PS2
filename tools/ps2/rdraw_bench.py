"""Drawer equivalence (host, MSVC) and cycle bench (EE, PCSX2) for src/r_draw8.c + src/r_draw8_npo2.c (tools/ps2/rdraw_bench.c).

usage: python tools/ps2/rdraw_bench.py --out build/opt-r/rdraw [--host] [--ee] [--variants vanilla,base,cand] [--experimental]
                                         [--negative-control] [--base-src DIR]
Variants (each = the drawer sources from a different place, same stub environment tools/ps2/rdraw_stub.h):
  vanilla  `git show HEAD:src/...` built with PS2_NOOPT: the original drawers (HEAD drawers equal vanilla SRB2 2.2.15)
  base     --base-src DIR (default build/opt-r/base-src: copies of r_draw.c/r_draw8.c/r_draw8_npo2.c taken before the work)
  cand     the working tree
--host : every variant is built with MSVC and run; prints per-drawer/scenario hashes and fails if cand differs from vanilla.
--ee   : every variant is built with the EE GCC (engine flags, PS2_PROFILE), run in PCSX2 through run_pcsx2.py (holds the lock),
         hashes compared across variants and COP0 cycles per call printed side by side.
--negative-control : host only, cand with a deliberately broken pixel (a dest index off by one) must make the comparison fail.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import math_common as C  # noqa: E402

ROOT = C.ROOT
TOOLS = ROOT / 'tools/ps2'


def read_text(path):
    return Path(path).read_text().replace('\r\n', '\n')


def lighting(text):
    start = text.index('static INT32 tiltlighting')
    end = text.index('// ==========================================================================', start)
    return text[start:end]


def sources(variant, out, base_src):
    """Returns (include dirs in order, extra defs); writes lighting.inc into out/variant."""
    work = out / variant
    work.mkdir(parents=True, exist_ok=True)
    if variant == 'vanilla':
        head = C.head_snapshot(out / 'head')
        text = (head / 'r_draw.c').read_text().replace('\r\n', '\n')
        (work / 'lighting.inc').write_text(lighting(text))
        return [head], ['PS2_NOOPT']
    if variant == 'base':
        text = read_text(base_src / 'r_draw.c')
        (work / 'lighting.inc').write_text(lighting(text))
        return [base_src, ROOT / 'src'], []
    text = read_text(ROOT / 'src/r_draw.c')
    (work / 'lighting.inc').write_text(lighting(text))
    return [ROOT / 'src'], []


def host_build(variant, out, base_src, experimental=False, broken=False, stress=False):
    incs, defs = sources(variant, out, base_src)
    work = out / ('host-' + variant + ('-broken' if broken else ''))
    work.mkdir(parents=True, exist_ok=True)
    (work / 'lighting.inc').write_text((out / variant / 'lighting.inc').read_text())
    flags = ['/DPS2_PROFILE'] + ['/D' + d for d in defs]
    if experimental and variant == 'cand':
        flags.append('/DPS2_OPT_SLOPE')
    if broken:
        flags.append('/DRDRAW_BROKEN')
    if stress:
        flags.append('/DRDRAW_STRESS')
    flags += ['/I' + str(work), '/I' + str(TOOLS)] + ['/I' + str(i) for i in incs]
    return C.msvc_build(work, 'rdraw_bench', [(TOOLS / 'rdraw_bench.c', 'main.obj', flags)])


def parse(text):
    rows = {}
    for m in re.finditer(r'^(?:\[[^\]\r\n]*\]\s*)?RB (\S+) (\S+) calls=(\d+) cyc=(\d+) hash=([0-9a-f]+)', text, re.M):
        rows[(m.group(1), m.group(2))] = (int(m.group(4)), m.group(5))
    return rows


def run_host(out, variants, base_src, experimental, negative, stress=False):
    results = {}
    for v in variants:
        exe = host_build(v, out, base_src, experimental, stress=stress)
        rc, text = C.run(exe, [], log=out / f'host-{v}' / 'run.log')
        if rc:
            raise SystemExit(f'host {v} exited {rc}\n{text[-2000:]}')
        results[v] = parse(text)
        if not results[v]:
            raise SystemExit(f'host {v}: no drawer results')
        print(f'host {v}: {len(results[v])} drawer/scenario rows')
    ok = True
    ref = results['vanilla']
    for v in variants:
        if v == 'vanilla':
            continue
        bad = [(k, ref.get(k), results[v].get(k)) for k in ref if ref[k][1] != results[v].get(k, (0, ''))[1]]
        print(f'host {v} vs vanilla: {len(ref) - len(bad)}/{len(ref)} rows identical')
        for k, a, b in bad[:30]:
            print('   DIFF', k, a, b)
        ok &= not bad
    if negative:
        exe = host_build('cand', out, base_src, False, broken=True, stress=stress)
        rc, text = C.run(exe, [], log=out / 'host-cand-broken' / 'run.log')
        bad = [k for k, v in parse(text).items() if ref[k][1] != v[1]]
        print(f'negative control: {len(bad)} rows differ (must be > 0)')
        ok &= len(bad) > 0
    print('HOST RESULT', 'PASS' if ok else 'FAIL')
    return ok


def ee_build(variant, out, base_src):
    import build as B
    incs, defs = sources(variant, out, base_src)
    work = out / ('ee-' + variant)
    work.mkdir(parents=True, exist_ok=True)
    (work / 'lighting.inc').write_text((out / variant / 'lighting.inc').read_text())
    flags = [f for f in B.CFLAGS if not f.startswith('-MMD') and f != '-MP']
    if variant == 'vanilla':
        flags += ['-DPS2_NOOPT']
    inc_first = ['-I' + str(work), '-I' + str(TOOLS)] + ['-I' + str(i) for i in incs]
    obj = work / 'rdraw_bench.o'
    cmd = [str(B.CC)] + flags + inc_first + B.INCS + ['-c', str(TOOLS / 'rdraw_bench.c'), '-o', str(obj)]
    p = subprocess.run(cmd, env=B.ENV, capture_output=True, text=True, cwd=ROOT)
    (work / 'build.log').write_text(' '.join(cmd) + '\n' + p.stdout + p.stderr)
    if p.returncode or (p.stdout + p.stderr).strip():
        print(p.stdout + p.stderr)
        raise SystemExit(f'EE compile failed/warned: {variant}')
    elf = work / 'RDRAW.ELF'
    ld = [str(B.CC)] + B.LDFLAGS + [str(obj), '-o', str(elf), '-ldebug', '-lpatches', '-lm']
    p = subprocess.run(ld, env=B.ENV, capture_output=True, text=True, cwd=ROOT)
    if p.returncode or (p.stdout + p.stderr).strip():
        print(p.stdout + p.stderr)
        raise SystemExit(f'EE link failed: {variant}')
    return elf


def run_ee(out, variants, base_src, timeout, no_run=False):
    results = {}
    complete = True
    for v in variants:
        elf = ee_build(v, out, base_src)
        if no_run:
            print('built', elf)
            continue
        log = out / f'ee-{v}' / 'run.log'
        rc = subprocess.run([sys.executable, str(TOOLS / 'run_pcsx2.py'), '--elf', str(elf), '--log', str(log),
                             '--until', 'RB DONE', '--marker', 'RB ', '--timeout', str(timeout)]).returncode
        text = log.read_text(errors='replace') if log.exists() else ''
        results[v] = parse(text)
        print(f'ee {v}: rc={rc} rows={len(results[v])}')
        if rc or 'RB DONE' not in text or not results[v]:
            print('EE run incomplete', v)
            complete = False
    if no_run:
        return True
    ref = results[variants[0]]
    names = sorted(ref, key=lambda k: (k[0], k[1]))
    print(f'{"drawer":42s} {"scenario":20s} ' + ' '.join(f'{v:>9s}' for v in variants) + '  hash')
    ok = complete and bool(ref)
    ok &= all(set(results[v]) == set(ref) for v in variants)
    for k in names:
        cells = [results[v].get(k, (0, ''))[0] / 10.0 for v in variants]
        same = all(results[v].get(k, (0, ''))[1] == ref[k][1] for v in variants)
        ok &= same
        print(f'{k[0]:42s} {k[1]:20s} ' + ' '.join(f'{c:9.1f}' for c in cells) + ('  =' if same else '  DIFF'))
    print('EE HASHES', 'IDENTICAL' if ok else 'DIFFER')
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--host', action='store_true')
    ap.add_argument('--ee', action='store_true')
    ap.add_argument('--variants', default='vanilla,base,cand')
    ap.add_argument('--experimental', action='store_true')
    ap.add_argument('--negative-control', action='store_true')
    ap.add_argument('--stress', action='store_true', help='host: 1024 calls/row, span boundaries and signed/multi-wrap NPO2 cases')
    ap.add_argument('--base-src', type=Path, default=ROOT / 'build/opt-r/base-src')
    ap.add_argument('--timeout', type=float, default=400)
    ap.add_argument('--no-run', action='store_true', help='EE: build only')
    a = ap.parse_args()
    out = a.out.resolve()
    variants = a.variants.split(',')
    ok = True
    if a.host:
        ok &= run_host(out, variants, a.base_src.resolve(), a.experimental, a.negative_control, a.stress)
    if a.ee:
        ok &= run_ee(out, [v for v in variants], a.base_src.resolve(), a.timeout, a.no_run)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
