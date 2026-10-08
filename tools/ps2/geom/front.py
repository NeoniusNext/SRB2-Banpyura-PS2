#!/usr/bin/env python3
"""usage: front.py ELF RUN : sums the self cycles (sampler, K cycles a frame) of the functions of the front of the engine (BSP walk -> polygon handed to the batch) from the sampler report"""
import re, subprocess, sys, os
from pathlib import Path
ROOT = Path(__file__).resolve().parents[3]
elf, run = sys.argv[1], sys.argv[2]
out = subprocess.run([sys.executable, str(ROOT / 'tools/ps2/sample_report.py'), '--elf', str(ROOT / elf), '--log', str(ROOT / 'build/runs' / run / 'boot.txt'), '--top', '600', '--lines', '0'], capture_output=True, text=True, env=dict(os.environ)).stdout
groups = {
 'walk': ['HWR_RenderBSPNode', 'HWR_Subsector', 'HWR_AddLine', 'R_PointToAngle64', 'HWR_CheckBBox', 'gld_clipper', 'HWR_FakeFlat', 'R_FakeFlat', 'R_PointOnSide', 'P_GetSector', 'R_CheckSectorLightLists'],
 'wall': ['HWR_ProcessSeg', 'HWR_SplitWall', 'HWR_CalcWallLight', 'HWR_CalcSegLight', 'HWR_RenderMidtexture', 'HWR_ProjectWall', 'HWR_DrawSkyWall'],
 'plane': ['HWR_RenderPlane', 'HWR_Lighting', 'HWR_GetLevelFlat', 'HWR_PlaneFlat'],
 'cache': ['gc_', 'HWR_GC', 'HWR_PS2_SideTexWord', 'HWR_PS2_TexTransparent'],
 'poly': ['HWR_ProcessPolygon', 'HWR_PBAdd', 'HWR_PBFast', 'HWR_PBPut', 'HWR_PBSlow'],
}
tot = {k: 0.0 for k in groups}
rows = []
for line in out.splitlines():
    m = re.match(r'^(\S+)\s+(\d+)\s+([\d.]+)\s+([\d.]+)$', line)
    if not m:
        continue
    name, k = m.group(1), float(m.group(3))
    for g, pats in groups.items():
        if any(name.startswith(p) for p in pats):
            tot[g] += k
            rows.append((g, name, k))
            break
for g in groups:
    print(f'{g:6s} {tot[g]:8.1f}')
print('front  %8.1f' % sum(tot.values()))
if len(sys.argv) > 3:
    for r in sorted(rows, key=lambda r: -r[2]):
        print('  %-6s %-40s %7.1f' % r)
