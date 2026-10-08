"""OPT11-CORE (PS2-203): keeps the type list of the static-scenery fast path in src/p_mobj.c equal to the cases of P_MobjSceneryThink's `switch (mobj->type)`.

The fast path (P_SceneryQuick) may only take a mobj whose type has NO case in that switch (the `default:` branch is the plain fuse countdown the fast path
skips because it requires fuse == 0). The list between the markers
    /* PS2-203 types begin */ ... /* PS2-203 types end */
is generated from the switch itself:
    python3 tools/ps2/core_scenery_types.py            rewrite the block in src/p_mobj.c
    python3 tools/ps2/core_scenery_types.py --check    exit 1 (and say what differs) when the block is not what the switch gives (run it after editing P_MobjSceneryThink)
MT_GHOST and MT_THOK are added by hand: P_MobjThinker fades them before it reaches the scenery thinker.
"""
import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parents[2] / 'src/p_mobj.c'
BEGIN, END = '/* PS2-203 types begin */', '/* PS2-203 types end */'
EXTRA = ['MT_GHOST', 'MT_THOK']


def switch_cases(text):
    i = text.index('static void P_MobjSceneryThink(mobj_t *mobj)')
    j = text.index('switch (mobj->type)', i)
    k = text.index('{', j)
    depth, p = 0, k
    while True:
        c = text[p]
        if c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                break
        p += 1
    body = re.sub(r'//[^\n]*', '', text[k:p])
    body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
    cases = re.findall(r'\bcase\s+(MT_[A-Z0-9_]+)\s*:', body)
    return cases


def render(cases):
    names = sorted(set(cases + EXTRA))
    out = [BEGIN]
    line = '\t'
    for n in names:
        item = 'case %s: ' % n
        if len(line) + len(item) > 150:
            out.append(line.rstrip())
            line = '\t'
        line += item
    out.append(line.rstrip())
    out.append('\t' + END)
    return '\n'.join(out)


def main():
    text = SRC.read_text()
    want = render(switch_cases(text))
    a, b = text.index(BEGIN), text.index(END) + len(END)
    have = text[a:b]
    if '--check' in sys.argv:
        if have == want:
            print('PS2-203 type list matches P_MobjSceneryThink (%d types)' % len(set(switch_cases(text) + EXTRA)))
            return 0
        sw = set(switch_cases(text) + EXTRA)
        old = set(re.findall(r'case (MT_[A-Z0-9_]+):', have))
        print('PS2-203 type list DIFFERS: only in the switch: %s; only in the list: %s' % (sorted(sw - old), sorted(old - sw)))
        return 1
    SRC.write_text(text[:a] + want + text[b:])
    print('rewrote the PS2-203 type list')
    return 0


sys.exit(main())
