#!/usr/bin/env python3
"""PS2-317 check: music muted (digmusicvolume 0) and unmuted again, against a run that never muted (scenes m1_ref / m1_mute of audio_scen.py).

usage: python3 tools/ps2/audio_mutecheck.py <ref run dir> <mute run dir>
Both runs stand still in MAP01 (music only), -adump writes every rendered block (512 frames) to .srb2/apcm.raw.  Block k of one run is song position
k*512 frames of the other (the music starts at block 0 in both).  Prints the blocks in which the muted run is silent / equal to the reference /
different, as ranges: before the mute equal, during it silent, after it equal again except for the short stretch of silent blocks that were already
decoded when the volume came back (<= PS2E_MUTE_AHEAD blocks) and equal afterwards (the decoder was resynchronised by a seek).
"""
import sys
from pathlib import Path

import numpy as np


def blocks(run):
    p = Path(run) / '.srb2' / 'apcm.raw'
    a = np.fromfile(p, dtype='<i2')
    n = len(a) // 1024
    return a[:n * 1024].reshape(n, 1024)


def ranges(flags, name):
    out = []
    start = None
    for i, f in enumerate(list(flags) + [False]):
        if f and start is None:
            start = i
        if not f and start is not None:
            out.append((start, i - 1))
            start = None
    return '%s: %s' % (name, out if out else 'none')


def main():
    ref, mut = blocks(sys.argv[1]), blocks(sys.argv[2])
    n = min(len(ref), len(mut))
    equal = np.array([np.array_equal(ref[i], mut[i]) for i in range(n)])
    silent = np.array([not mut[i].any() for i in range(n)])
    other = ~equal & ~silent
    print('blocks: ref %d, mute %d, compared %d' % (len(ref), len(mut), n))
    print(ranges(equal & ~silent, 'equal to the reference'))
    print(ranges(silent & ~equal, 'silent (reference not)'))
    print(ranges(other, 'different and not silent'))
    # after the last silent block: how many blocks until equal again, and are all later blocks equal
    idx = np.nonzero(silent & ~equal)[0]
    if len(idx):
        last = int(idx[-1])
        tail = equal[last + 1:]
        print('after the last silent block (%d): %d blocks, %d equal to the reference (%.2f %%)' % (last, len(tail), int(tail.sum()), 100.0 * tail.sum() / max(1, len(tail))))
    return 0


if __name__ == '__main__':
    sys.exit(main())
