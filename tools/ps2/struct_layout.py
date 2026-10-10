"""OPT12-CORE: field offsets / sizes / padding holes of the engine's big structs as the EE compiler lays them out (no debugger needed).

usage: python3 tools/ps2/struct_layout.py [struct ...]     (default: the structs of the level data)
Field names come from a regex over the struct body in the header (comments and preprocessor lines dropped; a field the compiler does not know in this
configuration is dropped and the run repeated); offsets and sizes come from a real compile (nm -S of `char OFF_S_F[offsetof(S, F) + 1]` arrays).
"""
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/ps2'))
os.environ.setdefault('SRB2_PS2_OUT', str(ROOT / 'build/out-prof'))
os.environ['SRB2_PS2_HW'] = '1'
os.environ.setdefault('SRB2_PS2_NO', '')
import importlib.util
spec = importlib.util.spec_from_file_location('b', ROOT / 'tools/ps2/build.py')
b = importlib.util.module_from_spec(spec)
sys.argv = ['x']
try:
    spec.loader.exec_module(b)
except SystemExit:
    pass

STRUCTS = {  # typedef name -> (header, struct tag or typedef marker)
    'line_t': 'src/r_defs.h', 'sector_t': 'src/r_defs.h', 'side_t': 'src/r_defs.h', 'seg_t': 'src/r_defs.h', 'subsector_t': 'src/r_defs.h', 'vertex_t': 'src/r_defs.h',
    'node_t': 'src/r_defs.h', 'ffloor_t': 'src/r_defs.h', 'mobj_t': 'src/p_mobj.h', 'precipmobj_t': 'src/p_mobj.h', 'mapthing_t': 'src/doomdata.h',
    'pslope_t': 'src/r_defs.h', 'msecnode_t': 'src/r_defs.h', 'blocknode_t': 'src/p_mobj.h', 'polyobj_t': 'src/p_polyobj.h', 'player_t': 'src/d_player.h',
    'spriteframe_t': 'src/r_defs.h', 'drawseg_t': 'src/r_defs.h', 'vissprite_t': 'src/r_things.h', 'visplane_t': 'src/r_plane.h', 'thinker_t': 'src/d_think.h',
}


def fields_of(name, header):
    text = (ROOT / header).read_text(errors='replace')
    m = re.search(r'\}\s*' + re.escape(name) + r'\s*;', text)
    if not m:
        return []
    end = m.start()
    # the matching '{' going backwards
    depth, i = 0, end
    while i > 0:
        i -= 1
        if text[i] == '}':
            depth += 1
        elif text[i] == '{':
            if depth == 0:
                break
            depth -= 1
    body = text[i + 1:end]
    body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
    body = re.sub(r'//[^\n]*', '', body)
    body = re.sub(r'^\s*#.*$', '', body, flags=re.M)
    out = []
    depth = 0
    cur = ''
    for ch in body:
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
        if depth == 0 and ch == ';':
            decl = cur.strip()
            cur = ''
            if not decl or '(' in decl and '(*' not in decl or decl.startswith('typedef'):
                continue
            if depth == 0 and ('struct {' in decl or 'union' in decl or decl.endswith('}')):
                continue
            pm = re.match(r'.*?\(\s*\*\s*([A-Za-z_]\w*)\s*\)', decl)
            if pm:
                out.append(pm.group(1))
                continue
            # split "type a, *b, c[4]" : names are identifiers followed by , [ : or end, after the type
            first = decl.split(',')[0]
            nm = re.findall(r'([A-Za-z_]\w*)\s*(?:\[[^\]]*\])*\s*(?::\s*\d+)?$', first)
            names = [nm[0]] if nm else []
            for rest in decl.split(',')[1:]:
                r2 = re.findall(r'([A-Za-z_]\w*)\s*(?:\[[^\]]*\])*\s*(?::\s*\d+)?$', rest.strip())
                if r2:
                    names.append(r2[0])
            out += names
        else:
            cur += ch
    return out


def main():
    want = [a for a in sys.argv[1:] if not a.startswith('-')] or ['line_t', 'seg_t', 'side_t', 'sector_t', 'subsector_t', 'vertex_t', 'node_t', 'mobj_t', 'mapthing_t', 'ffloor_t']
    work = ROOT / 'build/tmp'
    work.mkdir(parents=True, exist_ok=True)
    cf = [f for f in b.CFLAGS if f not in ('-MMD', '-MP')]
    res = {}
    for name in want:
        header = STRUCTS[name]
        flds = fields_of(name, header)
        for attempt in range(60):
            c = work / 'layout_probe.c'
            lines = ['#include "doomdef.h"', '#include "doomstat.h"', '#include "p_local.h"', '#include "r_state.h"', '#include "p_polyobj.h"', '#include "r_things.h"', '#include "r_plane.h"', '#include "d_player.h"',
                     '#include <stddef.h>', 'char SIZE_%s[sizeof(%s) + 1];' % (name, name)]
            for f in flds:
                lines.append('char OFF_%s__%s[offsetof(%s, %s) + 1];' % (name, f, name, f))
                lines.append('char SZ_%s__%s[sizeof(((%s *)0)->%s) + 1];' % (name, f, name, f))
            c.write_text('\n'.join(lines) + '\n')
            o = work / 'layout_probe.o'
            cmd = [str(b.CC)] + cf + ['-DHWRENDER', '-fno-lto'] + b.INCS + ['-O0', '-c', str(c), '-o', str(o)]
            r = subprocess.run(cmd, env=b.ENV, capture_output=True, text=True, cwd=ROOT)
            if r.returncode == 0:
                break
            bad = set(re.findall(r"no member named '(\w+)'|has no member named .(\w+).", r.stderr))
            bad = {x for t in bad for x in t if x}
            if not bad:
                print(r.stderr[-1500:])
                raise SystemExit('compile failed for ' + name)
            flds = [f for f in flds if f not in bad]
        nm = subprocess.run([str(b.DEV / 'ee/bin/mips64r5900el-ps2-elf-nm'), '-S', str(o)], capture_output=True, text=True).stdout
        size = None
        offs, szs = {}, {}
        for l in nm.splitlines():
            p = l.split()
            if len(p) < 4:
                continue
            sz = int(p[1], 16) - 1
            if p[3] == 'SIZE_' + name:
                size = sz
            elif p[3].startswith('OFF_' + name + '__'):
                offs[p[3].split('__', 1)[1]] = sz
            elif p[3].startswith('SZ_' + name + '__'):
                szs[p[3].split('__', 1)[1]] = sz
        res[name] = (size, offs, szs, flds)
        pos = 0
        print('== %s: %d bytes' % (name, size))
        for f in sorted(offs, key=lambda k: offs[k]):
            hole = offs[f] - pos
            print('  +%-4d %-4d %-24s%s' % (offs[f], szs.get(f, 0), f, ('   <-- hole %d' % hole) if hole > 0 else ''))
            pos = offs[f] + szs.get(f, 0)
        if size > pos:
            print('  tail padding %d' % (size - pos))


if __name__ == '__main__':
    main()
