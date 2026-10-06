"""Renderer zone profile build (diagnostic): instrumented COPIES of the renderer sources, the repository sources stay untouched.

usage: SRB2_PS2_RELEASE=1 python tools/ps2/rzone_build.py --out build/opt-r/rz [--run DEMO_001] [--timeout S]
Generates <out>/rzsrc/*.c (r_bsp, r_segs, r_plane, r_things, r_draw + r_draw8 + r_draw8_npo2, r_main) where selected functions
are renamed X_rz and wrapped by X, which enters/leaves an exclusive COP0 cycle zone (src/ps2/ps2_rzone.h). Builds the --ps2ref
ELF (phase profiler included) with those copies in place of the originals and prints "RZ ..." lines every 105 frames into boot.txt.
--run DEMO_00n runs it through tools/ps2/run_ps2_ref.py (PCSX2 lock held there) and prints the zone table (cycles per frame).
Zones are exclusive: a nested zone (a column drawer called from a wall loop) is not counted in its caller.
"""
import argparse
import re
import subprocess
import sys
import os
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build as B  # noqa: E402

ROOT = B.ROOT

ZONES = {
    'src/r_bsp.c': {'R_AddLine': 'ADDLINE', 'R_CheckBBox': 'CHECKBBOX', 'R_Subsector': 'SUBSECTOR',
                    'R_ClipSolidWallSegment': 'CLIPSOLID', 'R_ClipPassWallSegment': 'CLIPPASS'},
    'src/r_segs.c': {'R_StoreWallRange': 'STOREWALL', 'R_RenderSegLoop': 'SEGLOOP', 'R_RenderMaskedSegRange': 'MASKEDSEG',
                     'R_RenderThickSideRange': 'THICK'},
    'src/r_plane.c': {'R_FindPlane': 'FINDPLANE', 'R_CheckPlane': 'CHECKPLANE', 'R_MakeSpans': 'MAKESPANS',
                      'R_MapPlane': 'MAPPLANE', 'R_MapTiltedPlane': 'MAPPLANE', 'R_DrawSinglePlane': 'DRAWPLANES',
                      'R_DrawSkyPlane': 'SKY', 'R_SetSlopePlane': 'SLOPESETUP', 'R_SetScaledSlopePlane': 'SLOPESETUP',
                      'R_ClearPlanes': 'USER1', 'R_ClearFFloorClips': 'USER1'},
    'src/r_things.c': {'R_ProjectSprite': 'PROJECT', 'R_AddSprites': 'PROJECT', 'R_ClipVisSprite': 'CLIPSPR', 'R_ClipSprites': 'CLIPSPR',
                       'R_SortVisSprites': 'SORT', 'R_DrawMaskedColumn': 'MASKEDCOL', 'R_DrawFlippedMaskedColumn': 'MASKEDCOL',
                       'R_DrawVisSprite': 'VISSPRITE', 'R_CreateDrawNodes': 'DRAWNODES'},
    'src/r_main.c': {},
    'src/r_draw.c': {},
    'src/r_draw8.c': 'DRAWERS',
    'src/r_draw8_npo2.c': 'DRAWERS',
}


def find_matching(text, pos, open_ch, close_ch):
    depth = 0
    i = pos
    while i < len(text):
        c = text[i]
        if c == open_ch:
            depth += 1
        elif c == close_ch:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    raise ValueError('unbalanced')


def split_params(params):
    params = params.strip()
    if params in ('', 'void'):
        return []
    out, depth, cur = [], 0, ''
    for c in params:
        if c in '([':
            depth += 1
        elif c in ')]':
            depth -= 1
        if c == ',' and depth == 0:
            out.append(cur)
            cur = ''
        else:
            cur += c
    out.append(cur)
    return [p.strip() for p in out]


def param_name(p):
    p = re.sub(r'\[[^\]]*\]', '', p)
    m = re.search(r'(\w+)\s*(?:\)\s*\([^)]*\))?\s*$', p)
    return m.group(1)


def wrap(text, name, zone):
    """Rename the definition of `name` to name_rz and append a zone wrapper. Returns (text, count)."""
    pat = re.compile(r'^((?:static\s+|inline\s+)*[A-Za-z_][\w\s\*]*?[\s\*])' + re.escape(name) + r'\s*\(', re.M)
    done = 0
    pos = 0
    while True:
        m = pat.search(text, pos)
        if not m:
            break
        open_paren = m.end() - 1
        close_paren = find_matching(text, open_paren, '(', ')')
        after = text[close_paren + 1:close_paren + 80].lstrip()
        if not after.startswith('{') and not after.startswith('\n{'):
            # declaration or call-like line; skip
            nxt = text[close_paren + 1:].lstrip()
            if not nxt.startswith('{'):
                pos = m.end()
                continue
        prefix = m.group(1)
        if prefix.strip().startswith(('return', 'else')):
            pos = m.end()
            continue
        params = text[open_paren + 1:close_paren]
        body_open = text.index('{', close_paren)
        body_close = find_matching(text, body_open, '{', '}')
        ret = re.sub(r'\b(static|inline)\b', '', prefix).strip()
        is_void = ret == 'void'
        names = [param_name(p) for p in split_params(params)]
        proto = f'{prefix.strip()} {name}({params.strip() or "void"});\n'
        wrapper = (f'\n{prefix.strip()} {name}({params.strip() or "void"})\n{{\n\tRZ_ENTER(RZ_{zone});\n'
                   + (f'\t{name}_rz({", ".join(names)});\n\tRZ_LEAVE();\n' if is_void else
                      f'\t{ret} rz_ret = {name}_rz({", ".join(names)});\n\tRZ_LEAVE();\n\treturn rz_ret;\n')
                   + '}\n')
        # rebuild: prototype + definition with renamed identifier + wrapper
        defn_head = text[m.start():m.end()]
        defn_head = defn_head[:defn_head.rindex(name)] + name + '_rz('
        text = text[:m.start()] + proto + defn_head + text[m.end():body_close + 1] + wrapper + text[body_close + 1:]
        done += 1
        pos = m.start() + len(proto) + len(defn_head)
        # continue after the wrapper (avoid re-matching the wrapper itself)
        pos = text.index(wrapper, pos) + len(wrapper)
    return text, done


def instrument(rel, spec, outdir):
    text = (ROOT / rel).read_text().replace('\r\n', '\n')
    if spec == 'DRAWERS':
        spec = {}
        for m in re.finditer(r'^void (R_Draw\w+)\s*\(void\)', text, re.M):
            n = m.group(1)
            spec[n] = 'TILTSPAN' if ('Tilted' in n) else ('WALLCOL' if 'Column' in n else 'SPAN')
    hits = {}
    for name, zone in spec.items():
        text, n = wrap(text, name, zone)
        hits[name] = n
        if n == 0:
            print(f'  WARNING: {rel}: {name} not found')
    head = '#include "ps2/ps2_rzone.h"\n'
    if rel.endswith('r_main.c'):
        text = head + text + RMAIN_TAIL
    elif not rel.endswith(('r_draw8.c', 'r_draw8_npo2.c')):
        text = head + text
    path = outdir / Path(rel).name
    path.write_text(text)
    return path


RMAIN_TAIL = r'''
unsigned long long rz_acc[RZ_COUNT];
unsigned rz_calls[RZ_COUNT];
unsigned char rz_stack[16];
int rz_sp;
unsigned rz_last;
void RZ_Report(void)
{
	static int frames;
	int i;
	if (!rz_last)
		rz_last = rz_now();
	if (++frames < 105)
		return;
	frames = 0;
	CONS_Printf("RZ");
	for (i = 0; i < RZ_COUNT; i++)
	{
		CONS_Printf(" %d=%llu/%u", i, rz_acc[i], rz_calls[i]);
		rz_acc[i] = 0;
		rz_calls[i] = 0;
	}
	CONS_Printf("\n");
	rz_sp = 0;
	rz_last = rz_now();
}
'''


def build(out):
    B.OUT = out
    B.OBJ = out / 'obj'
    B.GEN = out / 'gen'
    B.OBJ.mkdir(parents=True, exist_ok=True)
    B.gen_config()
    src_out = out / 'rzsrc'
    src_out.mkdir(parents=True, exist_ok=True)
    B.CFLAGS.append('-DPS2REF')
    B.CFLAGS.append('-DPS2_RZONE')
    B.EXTRA_SOURCES.append('src/ps2ref.c')
    B.EXTRA_SOURCES.append('src/ps2/ps2_prof.c')
    ldflags = list(B.LDFLAGS)
    for fn in ('G_Ticker', 'P_Ticker', 'R_RenderPlayerView', 'R_RenderBSPNode', 'R_DrawPlanes', 'R_DrawMasked', 'ST_Drawer',
               'HU_Drawer', 'M_Drawer', 'CON_Drawer', 'I_UpdateSound', 'S_UpdateSounds', 'I_FinishUpdate', 'I_Sleep', 'I_SleepDuration'):
        ldflags.append('-Wl,--wrap=' + fn)
    # the renderer sources replaced by instrumented copies
    replaced = {}
    for rel, spec in ZONES.items():
        replaced[rel] = instrument(rel, spec, src_out)
    # R_RenderPlayerView must call RZ_Report: append the call through a wrapper-free route (ps2_rzone report from the profiler wrap)
    main = src_out / 'r_main.c'
    t = main.read_text()
    t = t.replace('\tfree(masks);\n}', '\tfree(masks);\n\tRZ_Report();\n}', 1)
    t = t.replace('#include "ps2/ps2_rzone.h"\n', '#include "ps2/ps2_rzone.h"\nvoid RZ_Report(void);\n', 1)
    main.write_text(t)
    srcs = B.sources(set())
    flags = B.CFLAGS + B.INCS
    cmds, objs = [], []
    import concurrent.futures as cf

    def compile_one(s):
        obj = B.OBJ / s.replace('/', '__').replace('.c', '.o')
        use = replaced.get(s)
        src = str(use) if use else str(ROOT / s)
        if not use and not B.stale(s, obj):
            return s, 0, '', obj
        cmd = [str(B.CC)] + flags + (['-I' + str(src_out)] if use else []) + ['-c', src, '-o', str(obj)]
        p = subprocess.run(cmd, env=B.ENV, capture_output=True, text=True, cwd=ROOT)
        return s, p.returncode, p.stdout + p.stderr, obj

    failed = []
    log = []
    with cf.ThreadPoolExecutor(os.cpu_count() or 4) as ex:
        for s, rc, text, obj in ex.map(compile_one, srcs):
            objs.append(obj)
            if text.strip():
                log.append(f'=== {s} (rc={rc})\n{text}')
            if rc:
                failed.append(s)
    (out / 'build.log').write_text('\n'.join(log))
    if failed:
        print('FAILED', failed)
        raise SystemExit(1)
    elf = out / 'SRB2.ELF'
    cmd = [str(B.CC)] + ldflags + [str(o) for o in objs] + ['-o', str(elf), '-Wl,-Map=' + str(out / 'SRB2.map')] + B.LIBS
    p = subprocess.run(cmd, env=B.ENV, capture_output=True, text=True, cwd=ROOT)
    (out / 'link.log').write_text(p.stdout + p.stderr)
    if p.returncode:
        print(p.stdout + p.stderr)
        raise SystemExit(1)
    print('linked', elf, elf.stat().st_size)
    return elf


ZNAMES = ['NONE', 'ADDLINE', 'CHECKBBOX', 'SUBSECTOR', 'CLIPSOLID', 'CLIPPASS', 'STOREWALL', 'SEGLOOP', 'WALLCOL', 'FINDPLANE',
          'CHECKPLANE', 'DRAWPLANES', 'MAKESPANS', 'MAPPLANE', 'SPAN', 'TILTSPAN', 'SLOPESETUP', 'SKY', 'PROJECT', 'CLIPSPR', 'SORT',
          'MASKEDCOL', 'SPRCOL', 'MASKEDSEG', 'VISSPRITE', 'DRAWNODES', 'SPLAT', 'THICK', 'USER1', 'USER2', 'USER3', 'USER4']


def report(boot, skip=1):
    wins = [l for l in boot.read_text(errors='replace').splitlines() if l.startswith('RZ ')]
    if len(wins) <= skip:
        print('no RZ windows')
        return
    tot = {}
    calls = {}
    for l in wins[skip:]:
        for m in re.finditer(r'(\d+)=(\d+)/(\d+)', l):
            i = int(m.group(1))
            tot[i] = tot.get(i, 0) + int(m.group(2))
            calls[i] = calls.get(i, 0) + int(m.group(3))
    frames = 105 * len(wins[skip:])
    print(f'{len(wins) - skip} windows, {frames} frames; cycles per frame (exclusive):')
    s = 0
    for i in sorted(tot, key=lambda k: -tot[k]):
        if i == 0:
            continue
        s += tot[i]
        print(f'  {ZNAMES[i]:12s} {tot[i] / frames:12.0f}  calls/frame {calls[i] / frames:8.1f}  cyc/call {tot[i] / max(1, calls[i]):9.1f}')
    print(f'  {"sum":12s} {s / frames:12.0f}')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--run', default='')
    ap.add_argument('--timeout', type=float, default=900)
    ap.add_argument('--report-only', action='store_true')
    a = ap.parse_args()
    out = a.out.resolve()
    if a.report_only:
        report(out / 'run' / 'boot.txt')
        return 0
    elf = build(out)
    if a.run:
        run = out / 'run'
        run.mkdir(parents=True, exist_ok=True)
        import shutil
        shutil.copy2(elf, run / 'SRB2.ELF')
        env = dict(os.environ, SRB2_PS2_RUN=str(run))
        subprocess.run([sys.executable, str(ROOT / 'tools/ps2/run_ps2_ref.py'), '--mode', 'demo', '--demo', a.run, '--no-build',
                        '--timeout', str(a.timeout), '--', '-ps2prof'], env=env)
        report(run / 'boot.txt')
    return 0


if __name__ == '__main__':
    sys.exit(main())
