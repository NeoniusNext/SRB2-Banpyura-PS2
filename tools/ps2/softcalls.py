"""List the engine functions that call libgcc soft-float / 64-bit helpers (R5900 has no double and no 64-bit mul/div).

usage: python3 tools/ps2/softcalls.py build/<out>/obj [name-substring ...]
Disassembles every engine object of a non-LTO build (SRB2_PS2_LTO=0) and prints "function: helper x count" lines.
"""
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

OD = '/opt/ps2dev-x/ps2dev/ee/bin/mips64r5900el-ps2-elf-objdump'
HELPERS = re.compile(r'\b(__muldf3|__adddf3|__subdf3|__divdf3|__floatsidf|__floatunsidf|__fixdfsi|__fixunsdfsi|__extendsfdf2|__truncdfsf2|'
                     r'__gtdf2|__ltdf2|__gedf2|__ledf2|__eqdf2|__nedf2|__floatdidf|__fixdfdi|__divdi3|__moddi3|__udivdi3|__umoddi3|'
                     r'__muldi3|sqrt|sqrtf|floor|floorf|ceil|sin|cos|atan2|atan|tan|pow|fabs|__ashldi3|__lshrdi3|__ashrdi3)\b')


def main():
    obj = Path(sys.argv[1])
    flt = sys.argv[2:]
    res = defaultdict(lambda: defaultdict(int))
    for o in sorted(obj.glob('*.o')):
        txt = subprocess.run([OD, '-dr', str(o)], capture_output=True, text=True).stdout
        fn = None
        for line in txt.splitlines():
            m = re.match(r'^[0-9a-f]+ <(.*)>:$', line)
            if m:
                fn = m.group(1)
                continue
            if 'R_MIPS_26' in line or 'R_MIPS_HI16' in line:
                h = HELPERS.search(line.split()[-1])
                if h and h.group(1) == line.split()[-1]:
                    res[(o.name, fn)][h.group(1)] += 1
    for (o, fn), d in sorted(res.items()):
        if flt and not any(f in o or f in (fn or '') for f in flt):
            continue
        print('%-28s %-40s %s' % (o, fn, ' '.join('%s x%d' % kv for kv in sorted(d.items()))))


if __name__ == '__main__':
    main()
