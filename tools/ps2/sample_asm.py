"""Annotated disassembly of one function with the statistical PC samples (build.py --sample, engine run with -ps2sample).

usage: python tools/ps2/sample_asm.py --elf SRB2.ELF --log boot.txt --func R_RenderSegLoop [--source] [--min 1]
Prints objdump of the function, each instruction with the number of samples whose PC (the first PC of the block that
followed the interrupted one) is exactly that address, scaled to kcycles/frame (as sample_report.py does), plus the
instruction count of each basic-block run between sampled addresses. Use it to read where blocks start, not to
time single instructions (see sample_report.py).
"""
import argparse
import re
import subprocess
from pathlib import Path

BIN = Path('D:/ps2dev/ee/bin')
ENVP = {'PATH': str(BIN) + ';C:/Windows/System32'}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', required=True)
    ap.add_argument('--log', required=True)
    ap.add_argument('--func', required=True)
    ap.add_argument('--source', action='store_true')
    ap.add_argument('--min', type=float, default=0.0, help='only print samples >= this kcyc/frame in the summary')
    a = ap.parse_args()
    pcs = {}
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
    s = sum(pcs.values())
    per = cyc / s / frames if s and frames else 16000 / 945
    args = ['-d', '-C', '--disassemble=' + a.func, str(a.elf)]
    if a.source:
        args.insert(1, '-l')
    out = subprocess.run([str(BIN / 'mips64r5900el-ps2-elf-objdump.exe'), *args], capture_output=True, text=True, env=ENVP).stdout
    tot = 0.0
    for l in out.splitlines():
        m = re.match(r'^\s*([0-9a-f]+):\s', l)
        if m:
            addr = int(m.group(1), 16)
            n = pcs.get(addr, 0)
            if n:
                k = n * per / 1e3
                tot += k
                print('%7.1f %s' % (k, l))
                continue
        print('        ' + l)
    print('function samples total: %.1f kcyc/frame' % tot)


if __name__ == '__main__':
    main()
