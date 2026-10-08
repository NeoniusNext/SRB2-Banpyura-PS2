#!/usr/bin/env python3
"""Lag (in output frames) between the stream after a muted stretch and the unmuted reference run (PS2-317 loop wrap check).

usage: python3 tools/ps2/audio_lag.py <ref run dir> <mute run dir> <first block> [last block]
Cross-correlates the left channel of blocks first..last of the mute run with the whole reference run (the gain is the same in both runs) and
prints the best lag relative to the same position (0 = the music came back exactly where it should be, negative = early) and the
normalised correlation.  The reference may loop (a short song): the correlation peak is searched around the expected position.
"""
import sys

import numpy as np


def stream(run):
    a = np.fromfile(run + '/.srb2/apcm.raw', dtype='<i2')
    n = len(a) // 1024
    return a[:n * 1024].reshape(n * 512, 2).astype(np.float64)


def main():
    ref, mut = stream(sys.argv[1]), stream(sys.argv[2])
    b0 = int(sys.argv[3])
    b1 = int(sys.argv[4]) if len(sys.argv) > 4 else len(mut) // 512
    seg = mut[b0 * 512:b1 * 512, 0]
    R = ref[:, 0]
    n = 1 << int(np.ceil(np.log2(len(R) + len(seg))))
    c = np.fft.irfft(np.fft.rfft(R, n) * np.conj(np.fft.rfft(seg, n)), n)[:len(R) - len(seg) + 1]
    cs = np.cumsum(np.concatenate([[0], R ** 2]))
    en = cs[len(seg):len(seg) + len(c)] - cs[:len(c)]
    ncc = c / np.sqrt(en * (seg ** 2).sum() + 1e-9)
    want = b0 * 512
    lo, hi = max(0, want - 2000), min(len(ncc), want + 2000)
    k = lo + int(np.argmax(ncc[lo:hi]))
    print('best lag near the expected position: %d frames (%.2f ms), ncc %.5f' % (k - want, (k - want) * 1000 / 22050, ncc[k]))
    kg = int(np.argmax(ncc))
    print('global best: lag %d frames, ncc %.5f' % (kg - want, ncc[kg]))


if __name__ == '__main__':
    main()
