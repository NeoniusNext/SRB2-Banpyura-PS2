"""Build the PS2 port (EE GCC, isolated PATH, incremental, parallel).

usage: build.py [--syntax] [--debug] [--keep-going] [--jobs N] [--target ELFNAME] [files...]
  (release = -DNDEBUG is the default, --debug / SRB2_PS2_RELEASE=0 gives the diagnostic ZDEBUG/RANGECHECK/PARANOIA build)
  --syntax   run -fsyntax-only on the source list (no objects, no link)
  files...   restrict to these source files (paths relative to repo root)
Source list: tools/ps2/sources.txt (one path per line, '#' comments).
Output: build/ps2/obj/*.o, build/ps2/SRB2.ELF, build/ps2/build.log
"""
import argparse
import concurrent.futures as cf
import os
import re
import subprocess
import sys
import time
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEV = Path('D:/ps2dev')
SDK = DEV / 'ps2sdk'
OUT = Path(os.environ.get('SRB2_PS2_OUT', ROOT / 'build/ps2'))  # per-agent builds: set SRB2_PS2_OUT
OBJ = OUT / 'obj'
GEN = OUT / 'gen'
CC = DEV / 'ee/bin/mips64r5900el-ps2-elf-gcc.exe'

ENV = dict(os.environ, PS2DEV=str(DEV), PS2SDK=str(SDK))
ENV['PATH'] = ';'.join(str(p) for p in [DEV/'ee/bin', DEV/'iop/bin', DEV/'bin',
                                         Path('C:/Windows/System32'), Path('C:/Windows')])

DEFS = ['-D_EE', '-DPS2', '-DPS2_PROFILE', '-DNOHW', '-DNOMD5',  # no HAVE_PNG / HAVE_ZLIB: PS2-20 (cooked packs only)
        '-DPS2_AUDIO_VORBIS', '-DPS2_AUDIO_MP3',
        '-DNOMUMBLE', '-DNO_IPV6', '-DNOUPNP', '-DCMAKECONFIG', '-D_LARGEFILE64_SOURCE',
        '-DNOEXECINFO', '-DUNIXCOMMON']
WARN = ['-Wall', '-Wextra', '-Wno-trigraphs', '-Wno-unused-parameter', '-fwrapv']
CFLAGS = ['-G0', '-O2', '-std=gnu23', '-ffunction-sections', '-fdata-sections', '-MMD', '-MP'] + DEFS + WARN
INCS = ['-I' + str(ROOT/'src'), '-I' + str(ROOT/'src/ps2'), '-I' + str(GEN),
        '-I' + str(SDK/'ee/include'), '-I' + str(SDK/'common/include'),
        '-I' + str(DEV/'gsKit/include'), '-I' + str(SDK/'ports/include')]
LDFLAGS = ['-T' + str(SDK/'ee/startup/linkfile'), '-L' + str(SDK/'ee/lib'), '-L' + str(DEV/'gsKit/lib'),
           '-L' + str(SDK/'ports/lib'), '-Wl,-zmax-page-size=128', '-Wl,--defsym,_stack_size=0x80000',
           '-Wl,--gc-sections', '-Wl,--wrap=W_LumpLength']
LIBS = ['-lps2_drivers', '-llz4', '-lgskit', '-ldmakit', '-laudsrv', '-lpad', '-lpoweroff', '-lfileXio', '-lcdvd',
        '-ldebug', '-lpatches', '-lvorbisfile', '-lvorbis', '-logg', '-lmpg123', '-lm']


def gen_config():
    """Equivalent of cmake/Comptime.cmake for the CMake-less PS2 build."""
    GEN.mkdir(parents=True, exist_ok=True)
    text = (ROOT/'src/config.h.in').read_text()
    def git(*a):
        return subprocess.run(['git', *a], cwd=ROOT, capture_output=True, text=True).stdout.strip()
    rep = {'${SRB2_COMP_REVISION}': git('rev-parse', '--short', 'HEAD'),
           '${SRB2_COMP_BRANCH}': git('rev-parse', '--abbrev-ref', 'HEAD'),
           '${SRB2_COMP_NOTE}': git('log', '-1', '--format=%s').replace('"', "'"),
           '${CMAKE_BUILD_TYPE}': 'Release'}
    for k, v in rep.items():
        text = text.replace(k, v)
    text = text.replace('#cmakedefine SRB2_COMP_UNCOMMITTED', '/* clean */')
    text = text.replace('#cmakedefine01 SRB2_COMP_OPTIMIZED', '#define SRB2_COMP_OPTIMIZED 1')
    p = GEN/'config.h'
    if not p.exists() or p.read_text() != text:
        p.write_text(text)


# PS2-16 measurement switch: SRB2_PS2_NOOPT=1 (or "all") builds the profile with the original (pre-optimisation) code paths,
# SRB2_PS2_NOOPT=math,slope,segs,draw,gs disables only those groups (see src/m_fixed.h).
for _g in [g for g in os.environ.get('SRB2_PS2_NOOPT', '').lower().split(',') if g]:
    CFLAGS = CFLAGS + ['-DPS2_NOOPT' if _g in ('1', 'all') else '-DPS2_NOOPT_' + _g.upper()]

# PS2-40 release by default: NDEBUG like the PC release build (without it doomdef.h turns on ZDEBUG, PARANOIA, RANGECHECK and
# PACKETDROP: 32-byte zone headers + red zones, bounds checks in every column drawer; the ZDEBUG numbers are 1.3-2x worse).
# A diagnostic build is asked for explicitly: --debug or SRB2_PS2_RELEASE=0 (--zdebug adds only the zone tracking).
RELEASE = os.environ.get('SRB2_PS2_RELEASE', '1') != '0'
if RELEASE:
    CFLAGS = CFLAGS + ['-DNDEBUG']

# Optimisation level: -O2 for everything, per-unit overrides from tools/ps2/opt_units.txt ("path flags..." per line, measured
# choices only) and, for experiments, SRB2_PS2_OFLAGS (replaces -O2 everywhere, e.g. "-O3") and SRB2_PS2_UNIT_FLAGS
# ("path=flags;path=flags", wins over the file). The stamp below covers all of it, a change rebuilds everything.
if os.environ.get('SRB2_PS2_OFLAGS'):
    CFLAGS = [f for f in CFLAGS if f != '-O2'] + os.environ['SRB2_PS2_OFLAGS'].split()


# PS2-92: -G8 by default. Globals up to 8 bytes live in .sdata/.sbss and are addressed relative to $gp (one instruction instead
# of lui+lw/sw): -0.8 M cycles per frame on DEMO_001. SRB2_PS2_GSIZE=n changes the threshold, 0 gives the old -G0 build. -G needs
# -mno-abicalls; the link gets neither (the SDK libraries are LTO objects built with abicalls and keep their gp-free code).
# Units that must not use $gp are listed in tools/ps2/opt_units.txt (interrupt handlers, extern blobs without a size).
_GSIZE = os.environ.get('SRB2_PS2_GSIZE', '8')
if _GSIZE != '0':
    CFLAGS = ['-G' + _GSIZE if f == '-G0' else f for f in CFLAGS] + ['-mno-abicalls']


# PS2-93: link-time optimisation of the engine (cross-unit inlining), -12% cycles on DEMO_001 at -G0. SRB2_PS2_LTO=0 turns it off.
# The engine objects are first merged into one relocatable object (gcc -r -flinker-output=nolto-rel: the LTO step sees only the
# engine, whose options are -G8 -mno-abicalls), which the normal link then combines with the SDK libraries (LTO objects built
# with abicalls, they stay out of this step).
LTO = os.environ.get('SRB2_PS2_LTO', '1') != '0'
if LTO:
    CFLAGS = CFLAGS + ['-flto']


def load_unit_flags():
    flags = {}
    f = ROOT / 'tools/ps2/opt_units.txt'
    lines = f.read_text().splitlines() if f.exists() else []
    for l in lines + [x.replace('=', ' ', 1) for x in os.environ.get('SRB2_PS2_UNIT_FLAGS', '').split(';') if x]:
        l = l.split('#')[0].split()
        if len(l) >= 2:
            flags[l[0]] = l[1:]
    return flags


UNIT_FLAGS = load_unit_flags()

EXTRA_SOURCES = []

# Optional GS hardware renderer (docs/HW_INTEGRATION.md): SRB2_PS2_HW=1 drops -DNOHW, defines HWRENDER and adds
# tools/ps2/sources_hw.txt. SRB2_PS2_HWD=null forces the logging stub driver (ps2_hwd_null.c) instead of ps2_hwd.c.
# Without the variable the software-only build is unchanged.
HW_BUILD = os.environ.get('SRB2_PS2_HW') == '1'
if HW_BUILD:
    CFLAGS = [f for f in CFLAGS if f != '-DNOHW'] + ['-DHWRENDER', '-Werror']


def hw_sources():
    """Hardware engine validation requires the real GS driver, never an implicit null fallback."""
    out = []
    if os.environ.get('SRB2_PS2_HWD') == 'null':
        raise SystemExit('HW engine build requires ps2_hwd.c; logging null driver is not a visual renderer')
    for l in (ROOT/'tools/ps2/sources_hw.txt').read_text().splitlines():
        l = l.split('#')[0].strip()
        if not l:
            continue
        alts = [a.strip() for a in l.split('||')]
        src = alts[0]
        if not (ROOT/src).is_file():
            raise SystemExit(f'missing required HW source: {src}')
        out.append(src)
    return out


def sources(selected):
    lines = [l.split('#')[0].strip() for l in (ROOT/'tools/ps2/sources.txt').read_text().splitlines()]
    srcs = [l for l in lines + EXTRA_SOURCES + (hw_sources() if HW_BUILD else []) if l and (ROOT/l).exists()]  # files owned by other workers may not exist yet
    if selected:
        srcs = [s for s in srcs if s in selected]
    return srcs


def obj_for(src):
    return OBJ / (src.replace('/', '__').replace('.c', '.o'))


def stale(src, obj):
    if not obj.exists():
        return True
    t = obj.stat().st_mtime
    if (ROOT/src).stat().st_mtime > t:
        return True
    dep = obj.with_suffix('.d')
    if not dep.exists():
        return True
    txt = dep.read_text(errors='replace').replace('\\\n', ' ')
    for f in re.split(r'(?<!\\)\s+', txt.split(':', 1)[1].strip()):
        f = f.replace('\\ ', ' ')
        if f and os.path.exists(f) and os.path.getmtime(f) > t:
            return True
    return False


FPROF_SKIP = {'src/ps2/ps2_prof.c', 'src/ps2/ps2_mem.c', 'src/z_zone.c', 'src/ps2ref.c'}  # the profiler and the zone are not instrumented
FPROF_FLAGS = ['-finstrument-functions', '-finstrument-functions-exclude-file-list=m_fixed.h,tables.h,doomtype.h']


def compile_one(src, syntax):
    obj = obj_for(src)
    cmd = [str(CC)] + CFLAGS + UNIT_FLAGS.get(src, []) + INCS
    if '-DPS2_FPROF' in CFLAGS and src not in FPROF_SKIP:
        cmd += FPROF_FLAGS
    if syntax:
        cmd += ['-fsyntax-only', '-MF', str(obj.with_suffix('.syntax.d')), str(ROOT/src)]
    else:
        cmd += ['-c', str(ROOT/src), '-o', str(obj)]
    p = subprocess.run(cmd, env=ENV, capture_output=True, text=True, cwd=ROOT)
    return src, p.returncode, p.stdout + p.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--syntax', action='store_true')
    ap.add_argument('--keep-going', action='store_true')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--target', default='SRB2.ELF')
    ap.add_argument('--list-undefined', action='store_true')
    ap.add_argument('--ps2ref', action='store_true', help='build the observational PS2REF hooks (tic log/frame dump); use SRB2_PS2_OUT=build/ps2-ref')
    ap.add_argument('--prof', action='store_true', help='per-phase profiler (-ps2prof at run time) WITHOUT the PS2REF hooks: the production code path (PS2REF costs ~0.7-1.4 M cycles per tick in G_Ticker)')
    ap.add_argument('--subprof', action='store_true', help='-DPS2_SUBPROF: the PS2SUB_* probes inside engine functions (src/ps2/ps2_prof.h); implies --prof')
    ap.add_argument('--memprof', action='store_true', help='memcpy/memset caller statistics (MC lines, tools/ps2/sample_report.py --elf resolves callers)')
    ap.add_argument('--sample', action='store_true', help='-DPS2_SAMPLE: statistical PC sampler (run with -ps2sample, report with tools/ps2/sample_report.py); implies --prof')
    ap.add_argument('--fprof', action='store_true', help='function-level profile (with --ps2ref): -finstrument-functions + FP lines, see tools/ps2/fprof_report.py')
    ap.add_argument('--debug', action='store_true', help='diagnostic build: no NDEBUG (ZDEBUG, RANGECHECK, PARANOIA); same as SRB2_PS2_RELEASE=0')
    ap.add_argument('--zdebug', action='store_true', help='define ZDEBUG (zone owner tracking); use SRB2_PS2_OUT=build/ps2-zdebug')
    ap.add_argument('files', nargs='*')
    a = ap.parse_args()
    if a.debug and '-DNDEBUG' in CFLAGS:
        CFLAGS.remove('-DNDEBUG')
    if a.zdebug:
        CFLAGS.append('-DZDEBUG')
    if a.fprof:
        CFLAGS.append('-DPS2_FPROF')
    if a.subprof:
        CFLAGS.append('-DPS2_SUBPROF')
    if a.memprof:
        CFLAGS.append('-DPS2_MEMPROF')
        for fn in ('memcpy', 'memset', 'memmove'):
            LDFLAGS.append('-Wl,--wrap=' + fn)
    if a.sample:
        CFLAGS.append('-DPS2_SAMPLE')
        CFLAGS.append('-g1')  # line tables for tools/ps2/sample_report.py (no code change)
    if a.ps2ref or a.prof or a.subprof or a.sample or a.memprof:
        if a.ps2ref:
            CFLAGS.append('-DPS2REF')
            EXTRA_SOURCES.append('src/ps2ref.c')
        # per-phase profiler (-ps2prof at run time): linker wrappers around the engine's phase functions, no engine code
        EXTRA_SOURCES.append('src/ps2/ps2_prof.c')
        for fn in ('G_Ticker', 'P_Ticker', 'R_RenderPlayerView', 'R_RenderBSPNode', 'R_DrawPlanes', 'R_DrawMasked', 'ST_Drawer',
                   'HU_Drawer', 'M_Drawer', 'CON_Drawer', 'I_UpdateSound', 'S_UpdateSounds', 'I_FinishUpdate', 'I_Sleep', 'I_SleepDuration'):
            LDFLAGS.append('-Wl,--wrap=' + fn)
    OBJ.mkdir(parents=True, exist_ok=True)
    gen_config()
    flags = ' '.join(CFLAGS + INCS) + ''.join('|%s %s' % (k, ' '.join(v)) for k, v in sorted(UNIT_FLAGS.items()))
    stamp = OBJ / 'flags.txt'
    if not stamp.exists() or stamp.read_text() != flags:
        for o in OBJ.glob('*.o'):
            o.unlink()
        stamp.write_text(flags)
    srcs = sources(set(a.files))
    if HW_BUILD and not a.files and 'src/ps2/hw/ps2_hwd.c' not in srcs:
        raise SystemExit('real GS hardware driver missing from full engine source list')
    todo = srcs if a.syntax else [s for s in srcs if stale(s, obj_for(s))]
    log = []
    failed = []
    t0 = time.time()
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        for src, rc, out in ex.map(lambda s: compile_one(s, a.syntax), todo):
            if out.strip():
                log.append(f'=== {src} (rc={rc})\n{out}')
            if rc or (HW_BUILD and out.strip()):
                failed.append(src)
    (OUT/'build.log').write_text('\n'.join(log), encoding='utf-8')
    print(f'compiled {len(todo)}/{len(srcs)} files in {time.time()-t0:.1f}s, failed {len(failed)}, log: {OUT/"build.log"}')
    for f in failed:
        print('  FAIL', f)
    if failed or a.syntax:
        return 1 if failed else 0
    elf = OUT / a.target
    objs = [str(obj_for(s)) for s in srcs]
    if LTO:
        merged = OBJ / 'engine_lto.o'
        rcmd = [str(CC), '-r', '-nostdlib', '-flto=%d' % a.jobs, '-flinker-output=nolto-rel', '-O2', '-ffunction-sections', '-fdata-sections'] + [f for f in CFLAGS if f.startswith('-G') or f == '-mno-abicalls'] + objs + ['-o', str(merged)]
        t1 = time.time()
        pr = subprocess.run(rcmd, env=ENV, capture_output=True, text=True, cwd=ROOT)
        (OUT/'lto.log').write_text(pr.stdout + pr.stderr, encoding='utf-8')
        if pr.returncode:
            print((pr.stdout + pr.stderr)[-6000:])
            return 1
        print(f'lto step {time.time()-t1:.1f}s')
        objs = [str(merged)]
    cmd = [str(CC)] + LDFLAGS + (['-Wl,--warn-unresolved-symbols'] if a.list_undefined else []) + objs + ['-o', str(elf), '-Wl,-Map=' + str(OUT/'SRB2.map')] + LIBS
    p = subprocess.run(cmd, env=ENV, capture_output=True, text=True, cwd=ROOT)
    (OUT/'link.log').write_text(p.stdout + p.stderr, encoding='utf-8')
    # PS2-92: -mno-abicalls objects against the abicalls SDK libraries make ld say so for every object (harmless: no PIC code)
    link_out = '\n'.join(l for l in (p.stdout + p.stderr).splitlines() if 'linking abicalls files with non-abicalls files' not in l).strip()
    if p.returncode or (HW_BUILD and not a.list_undefined and link_out):
        print(link_out[-6000:] if link_out else (p.stdout + p.stderr)[-6000:])
        return 1
    print('linked', elf, elf.stat().st_size, 'bytes')
    inputs = {ROOT / s for s in srcs}
    inputs.update([Path(__file__), ROOT/'tools/ps2/sources.txt', GEN/'config.h'])
    if HW_BUILD:
        inputs.add(ROOT/'tools/ps2/sources_hw.txt')
    for src in srcs:
        dep = obj_for(src).with_suffix('.d')
        if dep.is_file():
            txt = dep.read_text(errors='replace').replace('\\\n', ' ')
            for f in re.split(r'(?<!\\)\s+', txt.split(':', 1)[1].strip()):
                path = Path(f.replace('\\ ', ' '))
                if path.is_file():
                    inputs.add(path.resolve())
    compiler = subprocess.run([str(CC), '--version'], env=ENV, capture_output=True, text=True)
    size = subprocess.run([str(CC.with_name('mips64r5900el-ps2-elf-size.exe')), str(elf)], env=ENV, capture_output=True, text=True)
    (OUT/'size.log').write_text(size.stdout + size.stderr, encoding='utf-8')
    report = {
        'command': [sys.executable, *sys.argv], 'sources': srcs, 'compiled': len(todo),
        'hardware': HW_BUILD, 'driver': 'ps2_hwd' if HW_BUILD else 'software',
        'flags': CFLAGS + INCS, 'link_command': cmd, 'compiler': compiler.stdout,
        'elf_bytes': elf.stat().st_size, 'elf_sha256': hashlib.sha256(elf.read_bytes()).hexdigest(),
        'diagnostic_build': a.list_undefined, 'compile_diagnostics': len(log),
        'link_diagnostics': bool(link_out),
        'input_sha256': {str(f): hashlib.sha256(f.read_bytes()).hexdigest() for f in sorted(inputs)},
    }
    (OUT/'build-report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('driver', report['driver'], 'sha256', report['elf_sha256'])
    return 0


if __name__ == '__main__':
    sys.exit(main())
