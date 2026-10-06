"""Inventory of libgcc soft-float / 64-bit helper calls per function in the EE objects.

usage: SRB2_PS2_OUT=<dir> python tools/ps2/math_inventory.py [--files substr,...] [--all]
Scans <out>/obj/*.o with objdump -dr and prints, per object and function, the called helpers
(__adddf3, __muldf3, __divdi3, sin, cos ...). Default: only the render/fixed-point files.
"""
import argparse
import collections
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

DEV = Path('D:/ps2dev')
OBJDUMP = DEV / 'ee/bin/mips64r5900el-ps2-elf-objdump.exe'
OUT = Path(os.environ.get('SRB2_PS2_OUT', Path(__file__).resolve().parents[2] / 'build/ps2'))
ENV = dict(os.environ, PATH=';'.join(str(p) for p in [DEV/'ee/bin', DEV/'bin', 'C:/Windows/System32', 'C:/Windows']))
HELPERS = re.compile(r'^(__.*(df|di|ti|sf)[a-z0-9]*|__(un)?ord[sd]f2|__(lt|le|gt|ge|eq|ne)[sd]f2|__(float|fix|extend|trunc)[a-z]*|'
                     r'sin|cos|tan|atan|atan2|sqrt|pow|floor|ceil|fabs|round|exp|log|sinf|cosf|sqrtf|floorf|ceilf|roundf|fmod|ldexp|frexp|modf|lround|lrint)$')
DEFAULT = ['r_draw', 'r_plane', 'r_segs', 'r_main', 'r_things', 'r_bsp', 'r_splats', 'r_portal', 'v_video', 'm_fixed', 'screen', 'd_main', 'hu_stuff', 'st_stuff', 'r_fps', 'p_slopes']


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--files', default='')
    ap.add_argument('--all', action='store_true')
    ap.add_argument('--objects', type=Path, default=OUT / 'obj', help='actual object directory to inspect')
    ap.add_argument('--out', type=Path, help='save disassembly, hashes and JSON inventory here')
    a = ap.parse_args()
    keys = [k for k in a.files.split(',') if k] or DEFAULT
    total = collections.Counter()
    report = {'objects_dir': str(a.objects.resolve()), 'objects': {}}
    if a.out:
        a.out.mkdir(parents=True, exist_ok=True)
    inspected = 0
    for o in sorted(a.objects.glob('*.o')):
        if not a.all and not any(k in o.name for k in keys):
            continue
        p = subprocess.run([str(OBJDUMP), '-dr', str(o)], env=ENV, capture_output=True, text=True, check=True)
        txt = p.stdout
        inspected += 1
        if a.out:
            (a.out / (o.name + '.disasm')).write_text(txt, encoding='utf-8')
        func, per = None, collections.defaultdict(collections.Counter)
        for line in txt.splitlines():
            m = re.match(r'^[0-9a-f]+ <(.+)>:$', line)
            if m:
                func = m.group(1)
                continue
            m = re.search(r'R_MIPS_(?:26|CALL16|GOT_CALL|PC16)\s+(\S+)$', line)
            if m and func:
                sym = m.group(1).split('+')[0]
                if HELPERS.match(sym):
                    per[func][sym] += 1
        report['objects'][o.name] = {'sha256': hashlib.sha256(o.read_bytes()).hexdigest(),
                                     'mtime_ns': o.stat().st_mtime_ns, 'functions': dict(per)}
        if per:
            print(f'== {o.name}')
            for f, c in sorted(per.items()):
                print('  %-40s %s' % (f, ' '.join(f'{k}x{v}' for k, v in sorted(c.items()))))
                total.update(c)
    if not inspected:
        raise SystemExit(f'no matching EE objects: {a.objects}')
    print('OBJECTS', inspected, 'TOTAL', dict(total))
    report['total'] = dict(total)
    report['inspected'] = inspected
    flags = a.objects / 'flags.txt'
    if flags.exists():
        report['flags'] = flags.read_text()
    if a.out:
        (a.out / 'inventory.json').write_text(json.dumps(report, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
