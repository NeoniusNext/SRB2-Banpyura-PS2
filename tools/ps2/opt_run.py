"""Run one PS2 engine scenario in PCSX2 (32 MB retail or 128 MB Dev console) and collect the memory/CPU lines.

usage: python tools/ps2/opt_run.py --name NAME --elf SRB2.ELF [--ram 32|128] [--pak build/pak-a] [--out build/agent-opt-run]
                                   [--timeout 600] [--until TEXT] [--map MAPxx] [--demo DEMO_001] [--no-ref] -- engine args...
Stages <out>/<name>/ (ELF, hardlinked packs, .srb2/reference.cfg), starts the emulator ONLY through run_pcsx2.py (machine-wide
lock), waits for TEXT (default "ZQUIT DONE" if -zquit/-zquitall is among the arguments, else the end of the demo) in the engine log
boot.txt, and writes summary.json with every "ZSTAT", "PROF" and "[zmem]" line.
 --ram 32   D:/PCSX2-test (ExtraMemory=false, the standard profile)
 --ram 128  D:/PCSX2-test128 (copy of it with ExtraMemory=true: PCSX2 "Enable 128MB RAM (Dev Console)")
 --map      adds "-skipintro -warp MAPxx" (the player stands at the start: a static view of the level)
 --demo     adds "-timedemo DEMO_00n.lmp" and, with --ref, the PS2REF dump (tics.csv/frames.csv/frame-*.idx) into <run>/refout
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PCSX2 = {32: 'D:/PCSX2-test/pcsx2-qt.exe', 128: 'D:/PCSX2-test128/pcsx2-qt.exe'}


def stage(run, elf, pak, demo, cfg_extra=''):
    run.mkdir(parents=True, exist_ok=True)
    shutil.copy2(elf, run / 'SRB2.ELF')
    for p in pak.glob('*.PAK'):
        dst = run / p.name
        if dst.exists() and dst.stat().st_size == p.stat().st_size and dst.stat().st_mtime >= p.stat().st_mtime:
            continue
        try:
            dst.unlink()
        except OSError:
            pass
        try:
            os.link(p, dst)
        except OSError:
            shutil.copy2(p, dst)
    (run / '.srb2').mkdir(exist_ok=True)
    (run / '.srb2/reference.cfg').write_text('fpscap "35"\nfullscreen "Off"\nshowfps "No"\nshowping "Off"\nrollingdemos "Off"\n' + cfg_extra)
    if demo:
        shutil.copy2(ROOT / 'golden/phase0-v2' / f'{demo}.lmp', run / '.srb2' / f'{demo}.lmp')
    refout = run / 'refout'
    shutil.rmtree(refout, ignore_errors=True)
    refout.mkdir()
    return refout


def summarize(boot):
    text = boot.read_text(errors='replace') if boot.exists() else ''
    out = {'zstat': [], 'prof': [], 'zmem': [], 'errors': []}
    for line in text.splitlines():
        if line.startswith('ZSTAT '):
            out['zstat'].append(dict(kv.split('=', 1) for kv in line.split()[1:]))
        elif line.startswith('PROF '):
            out['prof'].append(line)
        elif line.startswith('[zmem]'):
            out['zmem'].append(line)
        elif 'I_Error' in line or 'OOM:' in line or 'Out of memory' in line or 'HEAP CHECK FAILED' in line:
            out['errors'].append(line)
    out['done'] = 'ZQUIT DONE' in text
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--name', required=True)
    ap.add_argument('--elf', required=True)
    ap.add_argument('--ram', type=int, choices=[32, 128], default=32)
    ap.add_argument('--pak', default=str(ROOT / 'build/pak-a'))
    ap.add_argument('--out', default=str(ROOT / 'build/agent-opt-run'))
    ap.add_argument('--timeout', type=float, default=600)
    ap.add_argument('--until', default='')
    ap.add_argument('--map', default='')
    ap.add_argument('--demo', default='')
    ap.add_argument('--no-ref', action='store_true')
    ap.add_argument('--emu', default='', help='pcsx2-qt.exe of another private copy (e.g. D:/PCSX2-net1/pcsx2-qt.exe: DEV9 Ethernet in Sockets mode for the network tests)')
    ap.add_argument('--golden', action='store_true', help='compare the PS2REF dump of --demo with golden/phase0-v2/run1/<demo> (tics.csv, frames.csv, frame-*.idx)')
    ap.add_argument('--compare-to', default='', help='run directory (under --out) whose refout must be byte-identical: tics.csv, frames.csv, frame-*.idx')
    ap.add_argument('extra', nargs='*')
    a = ap.parse_args()
    run = Path(a.out).resolve() / a.name
    refout = stage(run, Path(a.elf).resolve(), Path(a.pak).resolve(), a.demo or None)
    args = ['-logfile', 'boot.txt', '-config', 'reference.cfg', '-nolog', '-noendtxt']
    if a.demo and not a.no_ref:
        args += ['-ps2ref', 'host:/refout']
    if a.demo:
        args += ['-timedemo', a.demo + '.lmp']
    if a.map:
        args += ['-skipintro', '-warp', a.map]
    args += a.extra
    until = a.until or ('ZQUIT DONE' if any(x in ('-zquit', '-zquitall') for x in a.extra) else '')
    cmd = [sys.executable, str(ROOT / 'tools/ps2/run_pcsx2.py'), '--elf', str(run / 'SRB2.ELF'), '--log', str(run / 'pcsx2.log'),
           '--args=' + ' '.join(args), '--timeout', str(a.timeout)]
    if until:
        cmd += ['--until-file', str(run / 'boot.txt'), '--until', until]
    elif a.demo and not a.no_ref:
        cmd += ['--until-file', str(refout / 'complete.txt'), '--until', 'complete']
    env = dict(os.environ, SRB2_PCSX2=a.emu or PCSX2[a.ram])
    print(' '.join(cmd), flush=True)
    p = subprocess.run(cmd, env=env, capture_output=True, text=True)
    (run / 'run.log').write_text(p.stdout + p.stderr, encoding='utf-8')
    summary = summarize(run / 'boot.txt')
    # A fatal engine exit can still be a successful process exit in PCSX2.
    boot_text = (run / 'boot.txt').read_text(errors='replace') if (run / 'boot.txt').exists() else ''
    if summary['errors'] or (until and until not in boot_text):
        p.returncode = p.returncode or 4
    if a.demo and not a.no_ref:
        complete = refout / 'complete.txt'
        summary['ref_complete'] = complete.is_file() and 'complete' in complete.read_text(errors='replace')
        if not until and not summary['ref_complete']:
            p.returncode = p.returncode or 4
    summary.update({'name': a.name, 'ram': a.ram, 'rc': p.returncode, 'cmd': ' '.join(cmd)})
    (run / 'summary.json').write_text(json.dumps(summary, indent=1))
    if a.golden and a.demo:
        other = ROOT / 'golden/phase0-v2/run1' / a.demo
        names = sorted(p.name for p in other.glob('*') if p.name == 'tics.csv' or p.name == 'frames.csv' or p.name.startswith('frame-'))
        bad = [n for n in names if not (refout / n).exists() or (refout / n).read_bytes() != (other / n).read_bytes()]
        summary['golden'] = {'demo': a.demo, 'files': len(names), 'differ': bad}
        print(f"  golden {a.demo}: {len(names)} files, {len(bad)} differ" + (f": {bad[:6]}" if bad else ''))
        if bad or not names:
            p.returncode = p.returncode or 3
        (run / 'summary.json').write_text(json.dumps(summary, indent=1))
    if a.compare_to:
        other = Path(a.out).resolve() / a.compare_to / 'refout'
        names = sorted(p.name for p in other.glob('*') if p.name != 'complete.txt')
        bad = [n for n in names if not (refout / n).exists() or (refout / n).read_bytes() != (other / n).read_bytes()]
        extra = sorted(p.name for p in refout.glob('*') if p.name != 'complete.txt' and p.name not in names)
        summary['compare'] = {'to': a.compare_to, 'files': len(names), 'differ': bad, 'extra': extra}
        print(f"  compare to {a.compare_to}: {len(names)} files, {len(bad)} differ, {len(extra)} extra" + (f": {bad[:6]}" if bad else ''))
        if bad or extra or not names:
            p.returncode = p.returncode or 3
        (run / 'summary.json').write_text(json.dumps(summary, indent=1))
    summary['rc'] = p.returncode
    (run / 'summary.json').write_text(json.dumps(summary, indent=1))
    last = summary['zstat'][-1] if summary['zstat'] else None
    print(f"{a.name}: rc={p.returncode} done={summary['done']} errors={len(summary['errors'])}")
    if last:
        print('  ZSTAT', ' '.join(f'{k}={v}' for k, v in last.items()))
    for e in summary['errors'][:6]:
        print('  ERR', e)
    return 0 if p.returncode == 0 else p.returncode


if __name__ == '__main__':
    sys.exit(main())
