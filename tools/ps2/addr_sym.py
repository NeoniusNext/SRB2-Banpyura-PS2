"""Resolve code addresses of a PS2 ELF to the function that contains them (OPT10-X, for the "caller %p" of the PS2 net diagnostics).

usage: python3 tools/ps2/addr_sym.py build/out/nm.txt 0x220298 0x3cab9c ...      (nm.txt = mips64r5900el-ps2-elf-nm -n SRB2.ELF)
"""
import bisect
import sys

syms = []
for line in open(sys.argv[1]):
    p = line.split()
    if len(p) == 3 and p[1] in 'tTwW':
        syms.append((int(p[0], 16), p[2]))
syms.sort()
keys = [s[0] for s in syms]
for a in sys.argv[2:]:
    v = int(a, 16)
    i = bisect.bisect_right(keys, v) - 1
    print(f'{a}: {syms[i][1]} + {v - syms[i][0]:#x}' if i >= 0 else f'{a}: ?')
