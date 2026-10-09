#!/usr/bin/env python3
"""OPT13-RCACHE: console estimate for the HARDWARE-renderer frame by analogy (the HW front end cannot be built on the host: its PS2 paths are `#ifdef PS2`, EE-only).
usage: hwmodel.py SAMPLER_REPORT [CG_CSV_of_same_demo_software_run ...] [--miss 40] [--top 40]
Every function of the sampler's function table is put in a class; a class has a multiplier hw/emu taken from the MEASURED software stages of the same demo
(cgmodel.py; the nearest analogue, listed below), tick functions use their own measured numbers from the host profile when the function exists there.
Classes (analogue -> multiplier at the nominal miss=40, range in brackets):
  traverse : walk of BSP/subsectors/segs/sectors (pointer chasing, big code)     <- R_RenderBSPNode 2.7, R_AddLine 2.3, R_StoreWallRange 2.4, R_FindPlane 2.3     use 2.4 [1.9-2.8]
  sprite   : mobj -> sprite projection, shadows, hidden tests                       <- R_AddSprites 2.1, R_ClipSprites 1.55                                        use 2.0 [1.6-2.3]
  build    : polygon building / batching / block writes (float math, streams)       <- no direct analogue: streaming write 0.4 miss/64 B + code                    use 1.5 [1.3-1.9]
  driver   : EE side of the GS driver (plan, VU1 chunks, ring writes, uploads)      <- no direct analogue: streaming + big code                                    use 1.4 [1.2-1.8]
  decode   : audio / LZ4 / pictures (tables, streams)                                                                                                            use 1.3 [1.1-1.5]
  wait     : interrupt toggles, semaphores, timers, sleeps (waiting, not work)                                                                                   use 1.0
"""
import argparse, csv, re, sys
ap = argparse.ArgumentParser()
ap.add_argument('rep'); ap.add_argument('csv', nargs='*'); ap.add_argument('--top', type=int, default=40)
ap.add_argument('--scale', type=float, default=1.0, help='scales the extra part of every multiplier (0.6 = optimistic, 1.4 = pessimistic)')
a = ap.parse_args()
MULT = {'traverse': 2.4, 'sprite': 2.0, 'build': 1.5, 'driver': 1.4, 'decode': 1.3, 'wait': 1.0}
RULES = [
 ('wait', r'^(EIntr|DIntr|WaitSema|GetTimerSystemTime|ChangeThreadPriority|PS2_SleepUs|iSignalSema|PollSema|SignalSema|ReferThreadStatus|vblank|flip|gs_wait|ring_wait|poll_dma|wd_|DelayThread|SleepThread|__?cyc|\?)'),
 ('tick', r'^(P_|A_|PIT_|PTR_|T_|R_PointToAngle|R_PointInSubsector|R_PointToDist|R_IsPointInSector|R_InterpolateMobj|R_RemoveMobjInterpolator|UpdateLevelInterpolator|R_ApplyLevel|PS2_FixedDivMag|FixedDiv|FixedMul|R_PointOnSide|Lua|LUA_|M_Random|G_|D_|TryRunTics|NetUpdate)'),
 ('traverse', r'^(HWR_Subsector|HWR_AddLine|HWR_AddLineSeen|HWR_RenderBSPNode|HWR_ProcessSeg|HWR_FrPlaneCulled|HWR_FrProcessSeg|HWR_FrLeaf|gld_clipper|HWR_Seg|HWR_Frustum|HWR_CheckBBox|HWR_StoreWallRange|HWR_ClipSolid|HWR_PS2_SideTexWord|HWR_PS2_NoCull|HWR_RenderWall|HWR_SplitWall|HWR_AddTransparent|HWR_Prep|HWR_FoFs|HWR_Get|CompareDrawNodes|HWR_CreateDrawNodes|HWR_RenderDrawNodes|HWR_DrawNode)'),
 ('sprite', r'^(HWR_ProjectPlain|HWR_ProjectSprite|HWR_DrawSprite|HWR_DrawDropShadow|HWR_AddSprites|HWR_FX_|HWR_SortVisSprites|HWR_PS2_Sprite|HWR_DrawModel|HWR_DrawSprites|PS2HWD_Spr|hw_DrawModel|spr_|HWR_Shadow|HWR_ParaHidden|HWR_FillSprite|sprite_)'),
 ('build', r'^(HWR_ProcessPolygon|HWR_PB|HWR_RenderPlane|HWR_Lighting|HWR_RenderBatches|HWR_GC|gc_|poly_plane|HWR_PlaneFlat|HWR_Plane|hw_DrawPolygon|HWR_Render|HWR_Draw|HWR_Set|HWR_ProcessSegC|HWR_Add|HWR_Calc|HWR_Light|HWR_Wall|HWR_Sort|hw_Render|hw_Set|vu_key|vu_chunk|emit_)'),
 ('decode', r'^(mapping0|ps2_book|dec_level|Picture_|fill_ap88|LZ4|lz4|mdct|floor1|res0|vorbis|mpg|decode|RotatedPatch|R_GetColumn|W_|Patch_|memcpy|memset|_memcpy|ps2_|oggpack|_01|vc_|RQ_)'),
]
def klass(n):
    for k, pat in RULES:
        if re.match(pat, n):
            return k
    return 'driver'
norm = lambda n: re.sub(r"(\.(lto_priv|part|constprop|isra|cold)\.\d+)+$", '', n)
rows = []; infn = False; total = 0.0
cats = {}; incat = False
for l in open(a.rep, errors='replace'):
    if l.startswith('samples'):
        m = re.search(r'covered ([\d.]+) M', l); total = float(m.group(1)) * 1000 if m else 0
    if l.startswith('function (self)'): infn = True; continue
    if infn and not l.strip(): infn = False
    if infn:
        m = re.match(r'^(\S.*?)\s+(\d+)\s+([\d.]+)\s+([\d.]+)\s*$', l)
        if m: rows.append([norm(m.group(1).strip()), float(m.group(3))])
    if l.startswith('category'): incat = True; continue
    if incat and not l.strip(): incat = False
    if incat:
        m = re.match(r'^(\S.*?)\s+(\d+)\s+([\d.]+)\s+([\d.]+)\s*$', l)
        if m: cats[m.group(1).strip()] = float(m.group(3))
cg = {}
for f in a.csv:
    for r in csv.DictReader(open(f)):
        n = norm(r['fn']).replace("'2", '')
        d = cg.setdefault(n, [0.0, 0.0, 0.0, r['stage']])
        d[0] += int(r['D1mr']) + int(r['D1mw']); d[1] += int(r['I1mr']); d[2] += int(r['Ir'])
FR = 1049.0
out = []; cls_tot = {}
named = sum(r[1] for r in rows)
for n, k in rows:
    c = klass(n)
    if c == 'tick':
        d = cg.get(n)
        if d:
            pf = 1.0
            hw = k + (40 * d[0] * 0.75 + 40 * d[1]) / FR / 1e3 * a.scale; src = 'host-measured'
        else:
            hw = k * (1 + (2.2 - 1) * a.scale); src = 'tick class 2.2'
    else:
        m = 1 + (MULT[c] - 1) * a.scale; hw = k * m; src = 'class %.2f' % MULT[c]
    out.append((n, c, k, hw, src))
    t = cls_tot.setdefault(c, [0.0, 0.0]); t[0] += k; t[1] += hw
rest = max(total - named, 0.0)
cls_tot.setdefault('rest(unlisted)', [0.0, 0.0]); cls_tot['rest(unlisted)'][0] += rest; cls_tot['rest(unlisted)'][1] += rest * (1 + 0.4 * a.scale)
te = sum(v[0] for v in cls_tot.values()); th = sum(v[1] for v in cls_tot.values())
print('sampler: %.0f kcyc/frame covered (functions listed: %.0f); model hw: %.0f kcyc/frame = x%.2f (scale %.1f)' % (total, named, th, th / te, a.scale))
print('%-16s %9s %9s %7s' % ('class', 'emu K', 'hw K', 'x'))
for c, v in sorted(cls_tot.items(), key=lambda kv: -kv[1][1]):
    print('%-16s %9.0f %9.0f %6.2fx' % (c, v[0], v[1], v[1] / v[0] if v[0] else 0))
er = {r[0]: i + 1 for i, r in enumerate(sorted(out, key=lambda r: -r[2]))}
print('\n%-4s %-30s %-9s %8s %8s %6s %5s  %s' % ('hw#', 'function', 'class', 'emu K', 'hw K', 'x', 'emu#', 'basis'))
for i, r in enumerate(sorted(out, key=lambda r: -r[3])[:a.top]):
    print('%-4d %-30s %-9s %8.1f %8.1f %5.2fx %5d  %s' % (i + 1, r[0][:30], r[1], r[2], r[3], r[3] / r[2] if r[2] else 0, er[r[0]], r[4]))
