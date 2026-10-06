"""Report of the statistical PC sampler (build.py --sample, engine run with -ps2sample).

usage: python tools/ps2/sample_report.py --elf SRB2.ELF --log boot.txt [--frames N] [--top 40] [--lines 40] [--period-cycles 16000]
Reads the "SM <pc> <count>" lines (profile windows 1..9), resolves functions with nm and source lines with addr2line
(the --sample build has -g1). Prints: per-function self samples with cycles per frame, the hottest source lines and the
hottest blocks. PCSX2 delivers the interrupt at the end of the running basic block, so a sample is the first PC of the
following block: read lines as "the block that starts here", functions as the ranking they are.
"""
import argparse
import re
import subprocess
from collections import defaultdict
from pathlib import Path

import os
IS_WIN = os.name == 'nt'
BIN = Path('D:/ps2dev/ee/bin' if IS_WIN else os.environ.get('PS2DEV', '/opt/ps2dev-x/ps2dev') + '/ee/bin')
ENVP = {'PATH': str(BIN) + (';C:/Windows/System32' if IS_WIN else ':/usr/bin:/bin')}


def run(tool, *args, inp=None):
    return subprocess.run([str(BIN / ('mips64r5900el-ps2-elf-' + tool + ('.exe' if IS_WIN else ''))), *args], capture_output=True, text=True, env=ENVP, input=inp).stdout


def symbols(elf):
    syms = []
    for l in run('nm', '-n', '-S', '--defined-only', str(elf)).splitlines():
        p = l.split()
        if len(p) >= 4 and p[2] in 'tTwW':
            syms.append((int(p[0], 16), int(p[1], 16), p[3]))
    return syms


def lookup(syms, addr):
    lo, hi, best = 0, len(syms) - 1, None
    while lo <= hi:
        mid = (lo + hi) // 2
        if syms[mid][0] <= addr:
            best = syms[mid]
            lo = mid + 1
        else:
            hi = mid - 1
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', required=True)
    ap.add_argument('--log', required=True)
    ap.add_argument('--frames', type=int, default=945)
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--lines', type=int, default=40)
    ap.add_argument('--period-cycles', type=float, default=16000, help='EE cycles per sample when the log has no PROF lines (2 x the T1 compare value)')
    ap.add_argument('--match', default='', help='only functions whose name contains this')
    a = ap.parse_args()
    pcs = {}
    total = 0
    cyc = frames = 0
    for line in Path(a.log).read_text(errors='replace').splitlines():
        line = re.sub(r'^(?:\[[^\]\r\n]*\]\s*)?', '', line)
        if line.startswith('SM '):
            _, pc, n = line.split()
            pcs[int(pc, 16)] = pcs.get(int(pc, 16), 0) + int(n)
        elif line.startswith('PROF win='):
            kv = dict(x.split('=', 1) for x in line.split()[1:])
            if int(kv['win']) >= 1:
                cyc += int(kv['total'])
                frames += int(kv['frames'])
        elif line.startswith('SMTOTAL'):
            total = int(line.split()[1])
    s = sum(pcs.values())
    if frames and s:
        a.frames = frames
        a.period_cycles = cyc / s  # whole measured time (including the sampler's own interrupt cost) spread over the samples
    per = a.period_cycles / a.frames
    print('samples %d (reported %d), cycles/frame covered %.2f M' % (s, total, s * per / 1e6))
    syms = symbols(a.elf)
    fn = defaultdict(int)
    for pc, n in pcs.items():
        sy = lookup(syms, pc)
        fn[sy[2] if sy and pc < sy[0] + max(sy[1], 4) else '?'] += n
    print('\n%-40s %8s %10s %6s' % ('function (self)', 'samples', 'kcyc/frame', '%'))
    shown = 0
    for k, n in sorted(fn.items(), key=lambda x: -x[1]):
        if a.match and a.match not in k:
            continue
        print('%-40s %8d %10.1f %6.2f' % (k, n, n * per / 1e3, 100.0 * n / s))
        shown += 1
        if shown >= a.top:
            break
    addrs = sorted(pcs)
    out = run('addr2line', '-e', str(a.elf), '-f', '-a', inp=''.join('%x' % x + chr(10) for x in addrs)).splitlines()
    lines = defaultdict(int)
    where = {}
    i = 0
    while i + 2 < len(out) + 1 and i < len(out):
        addr = int(out[i], 16)
        func, loc = out[i + 1], out[i + 2]
        loc = loc.replace(chr(92), '/')
        loc = re.sub(r'^.*/(src/)', r'\1', loc)
        lines[loc] += pcs[addr]
        where[addr] = (func, loc)
        i += 3
    # --group view: samples by source file category (path based; libgcc soft-float cannot be attributed to its caller)
    cats = defaultdict(int)
    for addr, (func, loc) in where.items():
        f = loc.split(':')[0]
        b = f.rsplit('/', 1)[-1]
        if 'libgcc' in f or 'fp-bit' in f or 'libgcc2' in f:
            c = 'libgcc (soft double / 64-bit helpers)'
        elif 'newlib' in f:
            c = 'newlib (memcpy/memset/libc)'
        elif 'vorbis' in f or 'ogg' in f or 'mpg123' in f:
            c = 'audio decoders (vorbis/mp3)'
        elif f.startswith('src/ps2/ps2_a') or f.startswith('src/ps2/i_sound') or f.startswith('src/s_sound') or f.startswith('src/ps2/ps2_music') or f.startswith('src/ps2/ps2_midi'):
            c = 'audio engine (mixer, music, s_sound)'
        elif b.startswith(('r_draw', 'r_plane', 'r_splats')):
            c = 'render: drawers/planes'
        elif b.startswith(('r_segs', 'r_bsp')):
            c = 'render: walls/bsp'
        elif b.startswith(('r_things', 'r_portal')):
            c = 'render: sprites/masked'
        elif b.startswith(('r_main', 'r_fps', 'r_data', 'r_textures', 'r_picformats', 'r_patch')):
            c = 'render: main/data/textures'
        elif b.startswith(('p_', 'g_game', 'g_demo')) or f.startswith('src/p_'):
            c = 'tick: p_*/g_*'
        elif b.startswith(('v_video', 'st_', 'hu_', 'm_menu', 'console', 'screen')):
            c = 'hud/video'
        elif f.startswith('src/ps2/'):
            c = 'ps2 platform'
        else:
            c = 'other:' + (f if len(f) < 40 else b)
        cats[c] += pcs[addr]
    print('\n%-44s %8s %10s %6s' % ('category (by source file)', 'samples', 'kcyc/frame', '%'))
    for k, n in sorted(cats.items(), key=lambda x: -x[1])[:25]:
        print('%-44s %8d %10.1f %6.2f' % (k, n, n * per / 1e3, 100.0 * n / s))
    print('\n%-50s %8s %10s %6s' % ('source line (blocks starting there)', 'samples', 'kcyc/frame', '%'))
    shown = 0
    for k, n in sorted(lines.items(), key=lambda x: -x[1]):
        if a.match and not any(a.match in where[p][0] for p in where if where[p][1] == k):
            continue
        print('%-50s %8d %10.1f %6.2f' % (k, n, n * per / 1e3, 100.0 * n / s))
        shown += 1
        if shown >= a.lines:
            break


if __name__ == '__main__':
    main()
