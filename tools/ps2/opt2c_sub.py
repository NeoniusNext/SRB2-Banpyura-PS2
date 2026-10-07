"""Decode the "SUB win=..." probe lines (build.py --subprof, src/ps2_sub.h) of an engine log into cycles per frame.

usage: python tools/ps2/opt2c_sub.py boot.txt [boot2.txt ...]
Sums the windows after the first (window 0 is the level load) and divides by the number of frames (PROF lines give them).
Ids are the PS2SUB_B/E numbers in the sources; keep the table below in sync when probes are added.
"""
import re
import sys
from pathlib import Path

NAMES = {
    0: 'R_StoreWallRange (total)', 1: '  swr: head (drawseg, dist)', 2: '  swr: scale at both ends', 3: '  swr: slope intersections',
    4: '  swr: P_GetZAt / ffloor pos', 5: '  swr: texture slides', 6: '  swr: sides, heights, marks, tex setup', 9: '  swr: rw_offset, light',
    10: '  swr: steps, light list, ffloors', 11: '  swr: back ffloors', 12: '  swr: R_CheckPlane', 13: '  swr: R_RenderSegLoop',
    14: '  swr: tail (release, clip copy)', 15: 'R_Subsector (total)', 16: '  sub: R_FakeFlat', 17: '  sub: sector z, light lists',
    18: '  sub: floor/ceiling R_FindPlane', 19: '  sub: ffloors, polyobject planes', 20: '  sub: R_AddSprites', 21: '  sub: R_AddLine loop',
    22: 'R_ProjectSprite', 23: 'R_ClipVisSprite', 24: 'R_SortVisSprites', 25: 'R_CreateDrawNodes', 26: 'R_DrawSprite',
    27: 'R_RenderMaskedSegRange', 28: 'R_RenderThickSideRange', 29: 'R_SetupFrame', 30: 'clear planes/segs/sprites/portals',
    31: 'R_ClipSprites', 33: 'TryRunTics', 34: 'D_Display (all phases)',
    40: '[count] R_DrawColumn_8 pixels', 41: '[count] R_DrawColumn_8 calls', 42: '[count] 2sMultiPatchColumn pixels', 43: '[count] 2sMultiPatchColumn calls',
    44: '[count] TranslucentColumn pixels', 45: '[count] TranslucentColumn calls', 46: '[count] DrawSpan pixels', 47: '[count] DrawSpan calls',
    48: '[count] TiltedSpan pixels', 49: '[count] TiltedSpan calls', 50: '[count] TranslucentSpan pixels', 51: '[count] TranslucentSpan calls',
    54: '[count] ClipVisSprite columns', 55: '[count] ClipVisSprite calls', 61: '[count] clip scan items', 62: '[count] clip scan x-overlap', 63: '[count] clip scan scale-pass',
    90: '[count] CreateDrawNodes rovers', 91: '[count] CreateDrawNodes node visits', 92: '[count]   plane nodes', 93: '[count]   thickseg nodes', 94: '[count]   seg nodes', 95: '[count]   sprite nodes', 96: '[count]   x-overlap passes (non-sprite)',
    58: '[count] SegLoop wall pixels (colfunc)', 60: 'SegLoop colfunc() calls (cycles incl. probe)', 35: '  seg col: marking', 36: '  seg col: ffloor block', 37: '  seg col: tex offset+light', 38: '  seg col: tiers+draw', 39: '  seg col: tail', 80: 'segloop setup cat0', 81: 'segloop setup cat1', 82: 'segloop setup cat2', 83: 'segloop setup cat3', 84: 'segloop loop cat0', 85: 'segloop loop cat1', 86: 'segloop loop cat2', 87: 'segloop loop cat3', 64: 'segloop colfunc cat0 plain/untex', 65: 'segloop colfunc cat1 plain/tex', 66: 'segloop colfunc cat2 lights', 67: 'segloop colfunc cat3 ffloors', 70: 'SegLoop cat0 plain/untextured', 71: '[count] cols cat0', 72: 'SegLoop cat1 plain/textured', 73: '[count] cols cat1', 74: 'SegLoop cat2 lights only', 75: '[count] cols cat2', 76: 'SegLoop cat3 ffloors', 77: '[count] cols cat3', 52: '[count] SegLoop columns with ffloors', 53: '[count] SegLoop columns with light list', 59: '[count] SegLoop non-plain columns', 56: '[count] SegLoop columns', 57: '[count] SegLoop textured columns',
}


def main():
    for path in sys.argv[1:]:
        cyc = [0] * 128
        cnt = [0] * 128
        frames = 0
        win = -1
        for line in Path(path).read_text(errors='replace').splitlines():
            line = re.sub(r'^(?:\[[^\]\r\n]*\]\s*)?', '', line)
            if line.startswith('PROF win='):
                kv = dict(p.split('=', 1) for p in line.split()[1:])
                win = int(kv['win'])
                if win >= 1:
                    frames += int(kv['frames'])
            elif line.startswith('SUB win='):
                kv = dict(p.split('=', 1) for p in line.split()[1:])
                if int(kv['win']) < 1:
                    continue
                for k, v in kv.items():
                    if k.startswith('s'):
                        c, n = v.split('/')
                        cyc[int(k[1:])] += int(c)
                        cnt[int(k[1:])] += int(n)
        print('%s: %d frames' % (path, frames))
        for i in range(128):
            if cnt[i]:
                print('%-40s %10.0f cyc/frame %8.1f calls/frame %8.0f cyc/call' % (NAMES.get(i, 's%d' % i), cyc[i] / frames, cnt[i] / frames, cyc[i] / cnt[i]))


if __name__ == '__main__':
    main()
