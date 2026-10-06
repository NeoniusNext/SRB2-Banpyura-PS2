"""Compare a saved pre-edit mixer to production, including exact PCM/voice state.

python -B tools/ps2/audio_mix_bench.py --baseline path/to/audio_before.c --out build/mix --ee
The EE run uses the existing PCSX2 wrapper and shared lock. Host runs always execute.
"""
import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path
import math_common as C
import build as B


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--baseline', required=True, type=Path)
    p.add_argument('--out', required=True, type=Path)
    p.add_argument('--ee', action='store_true')
    a = p.parse_args()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    baseline = out / 'baseline.c'
    baseline.write_bytes(a.baseline.read_bytes())
    names = re.findall(r'\b(PS2_\w+)\s*\(', (C.ROOT / 'src/ps2/ps2_audio.h').read_text())
    rename = out / 'rename.h'
    rename.write_text(''.join('#define ' + n + ' ' + n.replace('PS2_', 'PS2_Baseline_', 1) + '\n' for n in names))
    inc = '/I' + str(C.ROOT / 'src/ps2')
    fixture = C.ROOT / 'tools/ps2/audio_mix_bench.c'
    candidate = C.ROOT / 'src/ps2/ps2_audio.c'
    exe = C.msvc_build(out / 'host', 'audio_mix', [
        (baseline, 'baseline.obj', [inc, '/FI' + str(rename)]),
        (candidate, 'candidate.obj', [inc]), (fixture, 'bench.obj', [inc])], cl_extra=['/W4', '/WX'])
    rc, text = C.run(exe, log=out / 'host/run.log')
    print(text, end='')
    if rc or 'AM DONE checks=2048 failures=0' not in text:
        return 1
    if not a.ee:
        return 0
    ee = out / 'ee'
    ee.mkdir(exist_ok=True)
    objects, commands = [], []
    flags = [f for f in B.CFLAGS if f not in ('-MMD', '-MP')] + ['-Werror']
    for source, name, extra in [(baseline, 'baseline', ['-include', str(rename)]),
                                (candidate, 'candidate', []), (fixture, 'bench', [])]:
        obj = ee / (name + '.o')
        cmd = [str(B.CC), *flags, *B.INCS, *extra, '-c', str(source), '-o', str(obj)]
        commands.append(cmd)
        r = subprocess.run(cmd, env=B.ENV, capture_output=True, text=True)
        (ee / (name + '.log')).write_text(r.stdout + r.stderr)
        if r.returncode or r.stdout.strip() or r.stderr.strip():
            print(r.stdout + r.stderr)
            return 1
        objects.append(str(obj))
    elf = ee / 'AUDIO_MIX.ELF'
    cmd = [str(B.CC), *B.LDFLAGS, *objects, '-o', str(elf), '-ldebug', '-lpatches', '-lm']
    commands.append(cmd)
    r = subprocess.run(cmd, env=B.ENV, capture_output=True, text=True)
    (ee / 'commands.json').write_text(json.dumps(commands, indent=2))
    (ee / 'link.log').write_text(r.stdout + r.stderr)
    if r.returncode or r.stdout.strip() or r.stderr.strip():
        print(r.stdout + r.stderr)
        return 1
    log = ee / 'run.log'
    rc = subprocess.run([sys.executable, str(C.ROOT / 'tools/ps2/run_pcsx2.py'), '--elf', str(elf),
                         '--log', str(log), '--until', 'AM DONE', '--timeout', '180']).returncode
    text = log.read_text(errors='replace') if log.exists() else ''
    lines = [line for line in text.splitlines() if re.search(r'\bAM (?:bits=|params |DONE)', line)]
    print('\n'.join(lines))
    passed = rc == 0 and 'AM DONE checks=2048 failures=0' in text
    (ee / 'report.json').write_text(json.dumps({'passed': passed, 'lines': lines,
        'elf_sha256': hashlib.sha256(elf.read_bytes()).hexdigest(),
        'baseline_sha256': hashlib.sha256(baseline.read_bytes()).hexdigest(),
        'candidate_sha256': hashlib.sha256(candidate.read_bytes()).hexdigest()}, indent=2))
    return 0 if passed else 1


if __name__ == '__main__':
    sys.exit(main())
