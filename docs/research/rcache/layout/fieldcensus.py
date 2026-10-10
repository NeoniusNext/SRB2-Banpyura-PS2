#!/usr/bin/env python3
"""OPT13-RCACHE: static census of which mobj_t fields the hot paths mention (a lower bound on lines touched): counts `->field` / `.field` per hot function.
usage: fieldcensus.py FIELD_LIST_FILE  (field list = one name per line, from layout.py output); prints a table per path."""
import re, sys, subprocess
ROOT = '/home/user/wt/rcache/'
PATHS = {
  'tick: P_MobjThinker+RegularThink+SceneryThink+ZMovement+CycleMobjState+SetMobjState': [
     ('src/p_mobj.c', ['P_MobjThinker','P_MobjRegularThink','P_SceneryThinker','P_ZMovement','P_XYMovement','P_CycleMobjState','P_SetMobjState','P_MobjCheckWater','P_MobjFloorZ','P_MobjCeilingZ','P_AdjustMobjFloorZ_FFloors','P_SceneryXYMovement','P_SceneryZMovement','P_SceneryQuick'])],
  'tick: CheckPosition+PIT_*': [('src/p_map.c', ['P_CheckPosition','PIT_CheckThing','PIT_DoCheckThing','P_SetThingPosition','P_UnsetThingPosition','P_CreateSecNodeList'])],
  'sw render: R_ProjectSprite': [('src/r_things.c', ['R_ProjectSprite','R_AddSprites','R_AddPrecipitationSprites'])],
  'hw render: HWR_ProjectSprite': [('src/hardware/hw_main.c', ['HWR_ProjectSprite','HWR_AddSprites','HWR_ProjectPlain'])],
}
def body(path, fn):
    s = open(ROOT + path, errors='replace').read()
    out = []
    for m in re.finditer(r'(?m)^[A-Za-z_][^\n;{}]*\b' + re.escape(fn) + r'\s*\([^;{]*\)\s*\n?\{', s):
        i = m.end(); d = 1
        while i < len(s) and d:
            c = s[i]; d += (c == '{') - (c == '}'); i += 1
        out.append(s[m.start():i])
    return '\n'.join(out)
fields = [l.split()[0] for l in open(sys.argv[1]) if l.strip()]
for name, items in PATHS.items():
    text = ''.join(body(p, f) for p, fl in items for f in fl)
    cnt = {f: len(re.findall(r'(?:->|\.)' + re.escape(f) + r'\b', text)) for f in fields}
    hot = sorted([(c, f) for f, c in cnt.items() if c], reverse=True)
    print('== %s: %d chars of code' % (name, len(text)))
    print('   ' + ' '.join('%s:%d' % (f, c) for c, f in hot))
