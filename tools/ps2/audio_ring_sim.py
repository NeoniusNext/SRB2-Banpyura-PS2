"""Replay of an engine run (-atrace/-adump) through a model of the stock audsrv.irx ring: what does the SPU2 actually play?

usage: python3 tools/ps2/audio_ring_sim.py <run dir> [--wav heard.wav] [--json out.json] [--quiet]
The run dir holds .srb2/atrace.bin (journal of audsrv writes and ring reads, written by I_ShutdownSound with -atrace N) and
.srb2/apcm.raw (-adump F: every block the mixer rendered, in order, 22050 Hz s16 stereo).

Model (iop/sound/audsrv/src/audsrv.c, version 0.93, 22050 Hz / 16 bit / stereo): ring of 9400 bytes. play_thread advances readpos
by 940 bytes every 512 SPU2 samples (10.667 ms) once the first audsrv_play_audio set playing=1 and does NOT look at writepos, so
when the EE stops writing, readpos overtakes writepos and the IOP plays the OLD ring content again and again.
The model needs only the clock phase of the 940-byte steps: it is fitted from the journal's reads (available/queued give
readpos - writepos, and writepos is the sum of the bytes written).
Output: the "heard" stream (what the SPU2 would play), the number of bytes read from the ring that were NOT written since the
previous read of that position (stale), how much of that was not silence, and the longest stale non-silent stretch.
"""
import argparse
import json
import struct
import sys
import wave
from pathlib import Path

import numpy as np

RING = 9400
STEP = 940
STEP_US = 512 * 1e6 / 48000.0
BLOCK_BYTES = 2048
REC = struct.Struct('<IHHii')
TR_WRITE, TR_OBS, TR_MARK = 1, 2, 3


def load(run):
    run = Path(run)
    home = run / '.srb2' if (run / '.srb2').is_dir() else run
    tr = np.frombuffer((home / 'atrace.bin').read_bytes(), dtype=np.dtype([('t', '<u4'), ('k', '<u2'), ('o', '<u2'), ('a', '<i4'), ('b', '<i4')]))
    pcm = home / 'apcm.raw'
    dump = np.fromfile(pcm, dtype='<i2') if pcm.exists() else np.zeros(0, dtype='<i2')
    return tr, dump.view(np.uint8) if dump.size else np.zeros(0, dtype=np.uint8)


def fit_phase(tr):
    """Find (t0, k0): readpos at time t is 940 * ((k0 + floor((t - t0) / STEP_US)) mod 10). Returns (t0, k0, matched, total)."""
    pos_w = 0
    obs = []
    started = False
    for r in tr:
        if r['k'] == TR_WRITE:
            pos_w = (pos_w + int(r['a'])) % RING
            started = started or r['a'] > 0
        elif r['k'] == TR_OBS and started:       # before the first write playing=0 and readpos stays at 4700
            av, q = int(r['a']), int(r['b'])
            if av > 0 and av + q == RING:        # unambiguous: pointers differ
                obs.append((float(r['t']), (pos_w + av) % RING))
    if not obs:
        return None
    ts = np.array([o[0] for o in obs])
    rs = np.array([o[1] for o in obs])
    ok = (rs % STEP == 0)
    ts, rs = ts[ok], rs[ok]
    ks = rs // STEP
    best = (-1, 0.0, 0)
    for k0 in range(10):
        for t0 in np.arange(0.0, STEP_US, 25.0):
            pred = (k0 + np.floor((ts - t0) / STEP_US)).astype(np.int64) % 10
            m = int((pred == ks).sum())
            if m > best[0]:
                best = (m, float(t0), k0)
    return best[1], best[2], best[0], len(ts)


def simulate(tr, dump, t0, k0, wav=None):
    ring = np.zeros(RING, dtype=np.uint8)
    fresh = np.zeros(RING, dtype=bool)
    # data stream of writes, in time order; content from the dump (rendered block index b, byte offset o), b < 0 = silence
    writes = [(float(r['t']), int(r['a']), int(r['b']), int(r['o'])) for r in tr if r['k'] == TR_WRITE and r['a'] > 0]
    if not writes:
        return None
    t_end = float(tr['t'].max())
    t_first = writes[0][0]
    # first IOP step at or after the first write: playing=1, readpos starts at 4700 (feed*5) and moves in 940 steps
    n_first = int(np.floor((t_first - t0) / STEP_US)) + 1
    k_first = (k0 + n_first) % 10
    n_last = int(np.floor((t_end - t0) / STEP_US))
    wpos = 0
    wi = 0
    heard = []
    stale_bytes = stale_nonzero = 0
    runs = []                     # (start step, steps) of stale non-silent stretches
    cur = None
    steps = []
    unknown = 0
    onsets = []                   # [t_write, first ring byte, heard step time or None]: the first audible write after silence (an SFX onset)
    last_audible_t = -1e18
    for n in range(n_first, n_last + 1):
        t = t0 + n * STEP_US
        while wi < len(writes) and writes[wi][0] <= t:
            tw, cnt, b, off = writes[wi]
            if b >= 0:
                if tw - last_audible_t > 150000.0:       # more than 150 ms since the last audible write: a new sound
                    onsets.append([tw, wpos, None])
                last_audible_t = tw
            if b >= 0 and (b * BLOCK_BYTES + off + cnt) <= len(dump):
                data = dump[b * BLOCK_BYTES + off:b * BLOCK_BYTES + off + cnt]
            elif b >= 0:
                data = np.full(cnt, 0x55, dtype=np.uint8)      # content unknown (outside the dump): assume audible
                unknown += cnt
            else:
                data = np.zeros(cnt, dtype=np.uint8)
            idx = (wpos + np.arange(cnt)) % RING
            ring[idx] = data
            fresh[idx] = True
            wpos = (wpos + cnt) % RING
            wi += 1
        rpos = ((k_first + (n - n_first)) % 10) * STEP
        for o in onsets:
            if o[2] is None and rpos <= o[1] < rpos + STEP:
                o[2] = t
        sl = np.arange(rpos, rpos + STEP) % RING
        chunk = ring[sl].copy()
        st = ~fresh[sl]
        fresh[sl] = False
        nstale = int(st.sum())
        nz = bool(chunk[st].any()) if nstale else False
        stale_bytes += nstale
        if nstale and nz:
            stale_nonzero += nstale
        steps.append((t, nstale, nz))
        if nz:
            if cur is None:
                cur = [n, 0]
            cur[1] += 1
        elif cur is not None:
            runs.append(tuple(cur)); cur = None
        heard.append(chunk)
    if cur is not None:
        runs.append(tuple(cur))
    heard = np.concatenate(heard) if heard else np.zeros(0, dtype=np.uint8)
    if wav:
        pcm = heard[:len(heard) // 4 * 4].view('<i2')
        with wave.open(str(wav), 'wb') as w:
            w.setnchannels(2); w.setsampwidth(2); w.setframerate(22050); w.writeframes(pcm.tobytes())
    total_steps = len(steps)
    lat = sorted((o[2] - o[0]) / 1000.0 for o in onsets if o[2] is not None)
    res = {
        'onsets': len(onsets), 'onset_latency_ms_min': round(lat[0], 1) if lat else None, 'onset_latency_ms_median': round(lat[len(lat) // 2], 1) if lat else None,
        'onset_latency_ms_max': round(lat[-1], 1) if lat else None,
        'steps': total_steps, 'heard_s': round(total_steps * STEP_US / 1e6, 3),
        'stale_bytes': stale_bytes, 'stale_ms': round(stale_bytes / 88.2, 1),
        'stale_nonsilent_ms': round(stale_nonzero / 88.2, 1),
        'stale_nonsilent_runs': len(runs),
        'longest_stale_nonsilent_ms': round(max([r[1] for r in runs], default=0) * STEP_US / 1000, 1),
        'unknown_bytes': unknown,
    }
    return res


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument('run')
    ap.add_argument('--wav', default='')
    ap.add_argument('--json', default='')
    ap.add_argument('--quiet', action='store_true')
    a = ap.parse_args(argv)
    tr, dump = load(a.run)
    f = fit_phase(tr)
    nw = int((tr['k'] == TR_WRITE).sum()); no = int((tr['k'] == TR_OBS).sum())
    if not f:
        print('no usable ring reads in the journal (%d writes, %d reads)' % (nw, no))
        return 1
    t0, k0, matched, total = f
    res = simulate(tr, dump, t0, k0, a.wav or None)
    if res is None:
        print('no writes in the journal'); return 1
    res.update({'records': len(tr), 'writes': nw, 'reads': no, 'phase_matched': matched, 'phase_total': total,
                'dump_frames': int(len(dump) // 4)})
    if not a.quiet:
        print(json.dumps(res, indent=1))
    if a.json:
        Path(a.json).write_text(json.dumps(res, indent=1))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
