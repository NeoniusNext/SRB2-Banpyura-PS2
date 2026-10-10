#!/usr/bin/env python3
"""OPT13-RCACHE: turn a callgrind (cache-sim) profile of the host PS2_PROFILE build into a ranking "emulator cost" vs "modelled console cost".
usage: cgmodel.py CALLGRIND_OUT [--frames N] [--emu-mcyc-per-frame X] [--miss 40] [--ptrfix 0.75] [--csv out.csv]
Self (exclusive) costs per function parsed straight from the callgrind file (events Ir Dr Dw I1mr D1mr D1mw ILmr DLmr DLmw). Classification by the file and name of the function.
Harness (ps2ref hooks, Z_TagsUsage, SDL/libc) and level loading are excluded. Calibration: the emulator total of the classified functions is scaled to
--emu-mcyc-per-frame x frames (k = EE cycles per host instruction).
Console model per function: cyc_hw = k*Ir + miss*(D1mr + D1mw)*ptrfix + miss*I1mr
(ptrfix: x86-64 structures are 1.3..1.6x the EE ones; the frame buffer and tables are not affected; the number is a knob, not a measurement)."""
import argparse, re, sys
from collections import defaultdict

EV = ['Ir', 'Dr', 'Dw', 'I1mr', 'D1mr', 'D1mw', 'ILmr', 'DLmr', 'DLmw']

STAGES = [
 ('harness', r'ps2ref|Z_TagsUsage|^/usr/|\.S$|^\?\?\?|sdl/|mixer_sound|host_lz4|host_shim|string/|malloc/|stdlib|sysdeps|libSDL|libgallium|ld-linux|libc\.so|i_system\.c.*SDL|I_FinishUpdate|I_UpdateNoBlit|Impl_|SDL_'),
 ('load/init', r'p_setup\.c|p_udmf|w_wad\.c|w_pack|r_data\.c|r_textures|r_picformats|r_patch|deh_|dehacked|lua_|blua|filesrch|m_menu|f_finale|r_skins|r_sky\.c|P_SpawnSpecials|p_polyobj|P_SpawnSlope|P_SpawnMapThing|G_DoLoadLevel|R_Init|R_Set|HU_Load|V_Init|z_zone\.c|ps2_mem|W_|Z_|P_LoadLevel|PS2Ref|PS2Pack'),
 ('tick:thinkers', r'p_mobj\.c|p_enemy\.c|p_user\.c|p_tick\.c|p_inter\.c|p_pspr|p_telept'),
 ('tick:collision/sight', r'p_map\.c|p_maputl\.c|p_sight\.c|p_slopes\.c|R_PointInSubsector|R_IsPointInSector|R_PointToAngle|R_PointToDist|tables\.c|m_fixed'),
 ('tick:specials/interp', r'p_spec\.c|p_floor|p_ceilng|p_lights|p_fof|r_fps\.c|p_saveg'),
 ('tick:other', r'g_game\.c|g_demo|d_net|netcode|command\.c|m_random|d_main'),
 ('render:bsp/setup', r'r_bsp\.c|r_main\.c|r_portal'),
 ('render:walls', r'r_segs\.c'),
 ('render:planes', r'r_plane\.c'),
 ('render:drawers(col/span)', r'r_draw'),
 ('render:sprites/masked', r'r_things\.c|r_splats'),
 ('render:hud/video', r'v_video|st_stuff|hu_stuff|screen\.c|i_video|console|am_map'),
]

def stage(file, fn):
    key = file + ':' + fn
    if 'src/' not in file and 'tools/' not in file:
        return 'harness'       # libc, ld.so, SDL, mesa: not engine code
    if re.search(r'm_misc\.c:M_|f_wipe|g_game\.c:G_Load|d_main\.c:D_|command\.c:COM_|m_menu|R_Init|P_Init|S_Init|I_Init', key):
        return 'load/init'
    for name, pat in STAGES:
        if re.search(pat, key):
            return name
    return 'other'

def parse(path):
    names, files = {}, {}
    cost = defaultdict(lambda: [0] * len(EV))
    fnfile = {}
    cur = None
    curfile = None
    skip_next = False
    def name_of(tbl, s):
        m = re.match(r'\((\d+)\)\s*(.*)', s)
        if not m: return s
        i, n = m.group(1), m.group(2)
        if n: tbl[i] = n
        return tbl.get(i, '?' + i)
    with open(path, errors='replace') as f:
        for line in f:
            if not line or line[0] in '#':
                continue
            if line.startswith('fl='):
                curfile = name_of(files, line[3:].strip()); continue
            if line.startswith(('fi=', 'fe=')):
                name_of(files, line[3:].strip()); continue
            if line.startswith('fn='):
                cur = name_of(names, line[3:].strip())
                fnfile.setdefault(cur, curfile)
                continue
            if line.startswith(('cfn=', 'cfi=', 'cfl=', 'cob=', 'ob=')):
                if line.startswith('cfn='): name_of(names, line[4:].strip())
                if line.startswith(('cfi=', 'cfl=')): name_of(files, line[4:].strip())
                continue
            if line.startswith('calls='):
                skip_next = True; continue
            if line[0].isdigit() or line[0] in '+-*':
                if skip_next:
                    skip_next = False; continue
                p = line.split()
                if cur is None: continue
                vals = p[1:]
                c = cost[cur]
                for i, v in enumerate(vals[:len(EV)]):
                    try: c[i] += int(v)
                    except ValueError: pass
    return cost, fnfile

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cg'); ap.add_argument('--frames', type=float, default=1049)
    ap.add_argument('--tics', type=float, default=1050)
    ap.add_argument('--emu-mcyc-per-frame', type=float, default=8.28)
    ap.add_argument('--emu-tick-mcyc', type=float, default=0.0, help='emulator tick cost per frame (M cycles); with it the tick and render stages get their own calibration k')
    ap.add_argument('--miss', type=float, default=40); ap.add_argument('--ptrfix', type=float, default=0.75)
    ap.add_argument('--imiss', type=float, default=-1, help='I-cache miss penalty (default = --miss)')
    ap.add_argument('--top', type=int, default=45); ap.add_argument('--csv')
    a = ap.parse_args()
    cost, fnfile = parse(a.cg)
    rows = []
    for fn, c in cost.items():
        d = dict(zip(EV, c)); d['fn'] = fn; d['file'] = fnfile.get(fn) or '?'
        d['stage'] = stage(d['file'], fn)
        rows.append(d)
    tot = {k: sum(r[k] for r in rows) for k in EV}
    model = [r for r in rows if r['stage'] not in ('harness', 'load/init')]
    if a.imiss < 0: a.imiss = a.miss
    istick = lambda r: r['stage'].startswith('tick')
    mir = sum(r['Ir'] for r in model)
    emu_total = a.emu_mcyc_per_frame * 1e6 * a.frames
    k = emu_total / mir if mir else 0
    ktick = kren = k
    if a.emu_tick_mcyc > 0:
        irt = sum(r['Ir'] for r in model if istick(r)); irr = mir - irt
        ktick = a.emu_tick_mcyc * 1e6 * a.frames / irt
        kren = (emu_total - a.emu_tick_mcyc * 1e6 * a.frames) / irr
        print('separate calibration: tick k=%.3f (Ir %.0fM), render k=%.3f (Ir %.0fM)' % (ktick, irt / 1e6, kren, irr / 1e6))
    kof = lambda r: ktick if istick(r) else kren
    # structure-heavy stages carry the x86-64 pointer-size inflation; frame buffer / table traffic does not
    pf = lambda r: 1.0 if r['stage'] in ('render:drawers(col/span)', 'render:hud/video') else a.ptrfix
    print('callgrind totals: Ir %.1fM Dr %.1fM Dw %.1fM D1mr %.2fM D1mw %.2fM I1mr %.2fM' % tuple(tot[x] / 1e6 for x in ('Ir', 'Dr', 'Dw', 'D1mr', 'D1mw', 'I1mr')))
    print('modelled set (no harness, no load): Ir %.1fM = %.0f%% of all; k = %.3f EE cycles per host instruction (emulator %.2f Mcyc/frame x %d frames)' % (mir / 1e6, 100.0 * mir / tot['Ir'], k, a.emu_mcyc_per_frame, a.frames))
    print('model: hw = k*Ir + %.0f*(D1mr+D1mw)*%.2f(structure stages; 1.0 for drawers/hud) + %.0f*I1mr\n' % (a.miss, a.ptrfix, a.imiss))
    def hw(r): return kof(r) * r['Ir'] + a.miss * (r['D1mr'] + r['D1mw']) * pf(r) + a.imiss * r['I1mr']
    stages = defaultdict(lambda: defaultdict(float))
    for r in model:
        s = stages[r['stage']]
        s['emu'] += kof(r) * r['Ir']; s['dmiss'] += a.miss * (r['D1mr'] + r['D1mw']) * pf(r); s['imiss'] += a.imiss * r['I1mr']
        s['Ir'] += r['Ir']; s['D1m'] += r['D1mr'] + r['D1mw']; s['I1m'] += r['I1mr']
    print('%-26s %9s %9s %9s %9s %7s %8s %8s' % ('stage', 'emu M/frm', 'D-miss', 'I-miss', 'hw M/frm', 'hw/emu', 'D1m/kIr', 'I1m/kIr'))
    for n, s in sorted(stages.items(), key=lambda kv: -(kv[1]['emu'] + kv[1]['dmiss'] + kv[1]['imiss'])):
        e, d, i = s['emu'] / a.frames / 1e6, s['dmiss'] / a.frames / 1e6, s['imiss'] / a.frames / 1e6
        print('%-26s %9.3f %9.3f %9.3f %9.3f %6.2fx %8.2f %8.2f' % (n, e, d, i, e + d + i, (e + d + i) / e if e else 0, 1000.0 * s['D1m'] / max(s['Ir'], 1), 1000.0 * s['I1m'] / max(s['Ir'], 1)))
    te, td, ti = [sum(s[x] for s in stages.values()) / a.frames / 1e6 for x in ('emu', 'dmiss', 'imiss')]
    print('%-26s %9.3f %9.3f %9.3f %9.3f %6.2fx' % ('TOTAL', te, td, ti, te + td + ti, (te + td + ti) / te))
    print('\nrank by emulator cost vs rank by modelled console cost (functions):')
    byemu = sorted(model, key=lambda r: -r['Ir']); byhw = sorted(model, key=lambda r: -hw(r))
    remu = {r['fn']: i + 1 for i, r in enumerate(byemu)}
    print('%-4s %-34s %-24s %8s %8s %8s %6s %6s  %s' % ('hw#', 'function', 'stage', 'emuK/frm', 'hwK/frm', 'hw/emu', 'emu#', 'D1m/fr', 'file'))
    for i, r in enumerate(byhw[:a.top]):
        e = kof(r) * r['Ir'] / a.frames / 1e3; h = hw(r) / a.frames / 1e3
        print('%-4d %-34s %-24s %8.1f %8.1f %7.2fx %6d %6.0f  %s' % (i + 1, r['fn'][:34], r['stage'], e, h, h / e if e else 0, remu[r['fn']], (r['D1mr'] + r['D1mw']) / a.frames, r['file'].split('/')[-1] if r['file'] else '?'))
    if a.csv:
        with open(a.csv, 'w') as f:
            f.write('fn,file,stage,Ir,Dr,Dw,I1mr,D1mr,D1mw,hw_cycles\n')
            for r in sorted(model, key=lambda r: -hw(r)):
                f.write('%s,%s,%s,%d,%d,%d,%d,%d,%d,%.0f\n' % (r['fn'].replace(',', ';'), r['file'], r['stage'], r['Ir'], r['Dr'], r['Dw'], r['I1mr'], r['D1mr'], r['D1mw'], hw(r)))

if __name__ == '__main__':
    main()
