"""PS2-LOAD-30 (OPT12-LOAD, "Lua 1:1"): run the same Lua script on the PC reference build and on the PS2 engine in PCSX2, compare the "LQ " lines.

usage: lua_equiv.py --elf SRB2.ELF [--pak build/pak2] [--out build/lua-equiv] [--pc EXE] [--warp 1|""] [--until "LQ DONE"] [--demo DEMO_001] SCRIPT.lua [SCRIPT.lua ...]
Each script prints its results with print("LQ ...") (CONS_Printf: stdout of the PC game, the engine log of the PS2 one) and ends with a line that contains --until
(default: "LQ DONE", the scripts use "LQ DONE_<name>"). All the scripts of one call are loaded together (-file a.lua -file b.lua ...), PC and PS2 alike; the lines of the
two runs are compared as they came (same order). A script that needs a level is run with --warp (default none: the title screen); --demo plays a golden demo (-playdemo,
the PC build has the same demos) so that hooks run on both sides on the same game.
Exit code 0: every line equal. Differences are printed (first 40) and written to <out>/<name>/diff.txt.
"""
import argparse
import difflib
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import opt_run  # noqa: E402

DEFAULT_PC = 'build/pc-golden/bin/lsdlsrb2_claude/srb2-ps2-optimization-gk1x6k'


def lq(text):
    out = []
    for l in text.splitlines():
        l = l.rstrip('\r\n')
        i = l.find('LQ ')
        if i >= 0 and (i == 0 or not l[i - 1].isalnum()):
            out.append(re.sub(r'(?:[\w.~-]*[:/\\])+(?=[\w.-]+\.lua)', '', l[i:]))   # the path of the script differs (PC: absolute, PS2: host:)
    return out


def alerts(text):
    """WARNING / ERROR lines the scripts cause (the engine's own messages about them), paths cut to the file name; the lines of the base game alone are not in the set"""
    out = []
    in_tb = False   # the lines of a "stack traceback:" that follows an alert are part of it
    for l in text.splitlines():
        l = l.rstrip('\r\n')
        if in_tb and re.match(r'^(\s+\S|stack traceback:)', l) and 'LQ ' not in l:
            out.append('LA ' + re.sub(r'(?:[\w.~-]*[:/\\])+(?=[\w.-]+\.lua)', '', l))
            continue
        in_tb = False
        m = re.match(r'^(WARNING|ERROR|NOTICE): ?(.*)$', l)
        if not m and re.search(r'allocated\.$|Ran out of', l):
            m = True   # freeslot() messages
        if not m or 'Demo' in l or 'config' in l or 'Couldn' in l:
            continue
        out.append('LA ' + re.sub(r'(?:[\w.~-]*[:/\\])+(?=[\w.-]+\.lua)', '', l))
        in_tb = True
    return out


def run_pc(name, scripts, a, out):
    o = out / 'pc'
    shutil.rmtree(o, ignore_errors=True)
    (o / 'home' / '.srb2').mkdir(parents=True)
    exe = Path(a.pc) if Path(a.pc).is_absolute() else ROOT / a.pc
    args = [str(exe), '-home', str(o / 'home'), '-skipintro', '-nosound']
    if a.warp:
        args += ['-warp', a.warp]
    if a.demo:
        shutil.copy2(ROOT / 'golden/phase0-v2' / f'{a.demo}.lmp', o / 'home' / '.srb2' / f'{a.demo}.lmp')
        args += ['-timedemo' if a.timedemo else '-playdemo', a.demo + '.lmp']
    for s in a.extra:
        shutil.copy2(s, o / 'home' / '.srb2' / s.name)
        shutil.copy2(s, o / s.name)
    for s in scripts:
        args += ['-file', str(s)]
    args += a.pc_args
    env = dict(os.environ, SRB2WADDIR=os.environ.get('SRB2WADDIR', '/opt/srb2-assets'), SDL_AUDIODRIVER='dummy', LIBGL_ALWAYS_SOFTWARE='1')
    outf = open(o / 'pc.out', 'wb')
    p = subprocess.Popen(['xvfb-run', '-a', '-s', '-screen 0 800x600x24'] + args, cwd=str(o), stdout=outf, stderr=subprocess.STDOUT, env=env, start_new_session=True)
    t0 = time.time()
    state = 'timeout'
    while time.time() - t0 < a.pc_timeout:
        if p.poll() is not None:
            state = 'exit %d' % p.returncode
            break
        if a.until in (o / 'pc.out').read_text(errors='replace'):
            state = 'until'
            time.sleep(1.0)
            break
        time.sleep(0.5)
    try:
        os.killpg(os.getpgid(p.pid), signal.SIGINT)
        time.sleep(1.5)
        os.killpg(os.getpgid(p.pid), signal.SIGKILL)
    except OSError:
        pass
    outf.close()
    text = (o / 'pc.out').read_text(errors='replace')
    return state, text


def run_ps2(name, scripts, a, out):
    run = out / 'ps2'
    extra = ['-skipintro']
    if a.warp:
        extra += ['-warp', a.warp]
    for s in scripts:
        extra += ['-file', s.name]
    extra += a.ps2_args
    cmd = [sys.executable, str(ROOT / 'tools/ps2/ftest_run.py'), '--name', 'ps2', '--out', str(out), '--elf', a.elf, '--pak', a.pak, '--files', ','.join(str(s) for s in list(scripts) + [e.resolve() for e in a.extra]),
           '--until', a.until, '--timeout', str(a.ps2_timeout), '--show', 'LQ '] + (['--demo', a.demo] if a.demo else []) + ['--'] + extra
    if a.demo:
        cmd += ['-timedemo' if a.timedemo else '-playdemo', a.demo + '.lmp']
    p = subprocess.run(cmd, capture_output=True, text=True)
    (out / 'ps2-run.log').write_text(p.stdout + p.stderr)
    boot = run / 'boot.txt'
    return ('exit %d' % p.returncode), (boot.read_text(errors='replace') if boot.exists() else '')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('scripts', nargs='+', type=Path)
    ap.add_argument('--elf', required=True)
    ap.add_argument('--pak', default=str(ROOT / 'build/pak2'))
    ap.add_argument('--out', default=str(ROOT / 'build/lua-equiv'))
    ap.add_argument('--name', default='')
    ap.add_argument('--pc', default=DEFAULT_PC)
    ap.add_argument('--warp', default='')
    ap.add_argument('--demo', default='')
    ap.add_argument('--timedemo', action='store_true', help='-timedemo (as fast as possible) instead of -playdemo')
    ap.add_argument('--until', default='LQ DONE')
    ap.add_argument('--pc-timeout', type=float, default=120)
    ap.add_argument('--ps2-timeout', type=float, default=900)
    ap.add_argument('--pc-args', default='', help='extra PC parameters, one quoted string')
    ap.add_argument('--ps2-args', default='', help='extra PS2 parameters, one quoted string')
    ap.add_argument('--extra', type=Path, action='append', default=[], help='file the scripts load by name (addfile, addfilelocal): copied next to the PC game and to the PS2 data directory, not loaded with -file')
    ap.add_argument('--no-pc', action='store_true', help='reuse <out>/<name>/pc.out')
    a = ap.parse_args()
    a.pc_args = shlex.split(a.pc_args)
    a.ps2_args = shlex.split(a.ps2_args)
    scripts = [s.resolve() for s in a.scripts]
    name = a.name or scripts[0].stem
    out = Path(a.out).resolve() / name
    out.mkdir(parents=True, exist_ok=True)
    if a.no_pc and (out / 'pc' / 'pc.out').exists():
        pcstate, pctext = 'reused', (out / 'pc' / 'pc.out').read_text(errors='replace')
    else:
        pcstate, pctext = run_pc(name, scripts, a, out)
    if not any(a.until in l for l in lq(pctext)):
        print(f'{name}: the PC run did not reach "{a.until}" ({pcstate}); fix the script first. Last lines:')
        for l in [x for x in pctext.splitlines() if re.search(r'Lua|lua|rror|WARNING', x)][-8:]:
            print('  PC  ', l[:200])
        return 2
    ps2state, ps2text = run_ps2(name, scripts, a, out)
    pc, ps2 = lq(pctext) + alerts(pctext), lq(ps2text) + alerts(ps2text)
    (out / 'pc.lq').write_text('\n'.join(pc) + '\n')
    (out / 'ps2.lq').write_text('\n'.join(ps2) + '\n')
    diff = list(difflib.unified_diff(pc, ps2, 'pc', 'ps2', lineterm='', n=0))
    (out / 'diff.txt').write_text('\n'.join(diff) + '\n')
    errs_pc = [l for l in pctext.splitlines() if re.search(r'Lua error|lua error|error:|stack traceback', l)][:5]
    errs_ps2 = [l for l in ps2text.splitlines() if re.search(r'Lua error|lua error|error:|stack traceback|I_Error', l)][:5]
    print(f'{name}: pc {pcstate}, {len(pc)} LQ lines; ps2 {ps2state}, {len(ps2)} LQ lines; {len([d for d in diff if d[:1] in "+-" and d[:3] not in ("+++", "---")])} differing lines')
    for l in errs_pc:
        print('  PC  ', l[:200])
    for l in errs_ps2:
        print('  PS2 ', l[:200])
    for d in diff[:40]:
        print('  ' + d[:220])
    ok = pc == ps2 and len(pc) > 0 and any(a.until in l for l in pc)
    print('RESULT', 'SAME' if ok else 'DIFFERENT')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
