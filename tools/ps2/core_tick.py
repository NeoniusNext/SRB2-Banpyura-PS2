"""OPT11-CORE: the frame split of an LTO profile run (build.py --prof, engine run with -ps2prof): tic logic (TryRunTics) per tic, display (D_Display) per
frame, sound/Lua after the display, the rest of the frame (idle, vsync wait, I_SleepDuration), lost tics and the longest frames.

usage: python3 tools/ps2/core_tick.py build/runs/<name>/boot.txt [more boot.txt ...]
Reads the "TICK win=..." lines (src/ps2/ps2_prof.c, PS2_PROF_DIRECT; window 0 is the level load and is left out) and the "PROF win=..." totals.
All figures are M cycles of the EE COP0 counter (294.912 M per second); averages over windows 1..9 (105 frames each).
  tick/tic     cycles in TryRunTics per tic run (G_Ticker, P_Ticker, Lua hooks, net; the audio threads that preempt it are in it)
  disp/frame   cycles in D_Display per displayed frame (view, HUD, I_FinishUpdate incl. the GS submit / vsync wait)
  other/frame  total - tick - disp - snd, per frame (main loop, sleep, audio threads outside the two calls)
  lost         tics the clock asked for minus tics run (only meaningful in real time, -playdemo; -timedemo runs one tic per frame)
"""
import re
import sys

CLOCK = 294.912e6


def one(path):
    tick = {}
    prof = {}
    pat = re.compile(r'TICK win=(\d+) frames=(\d+) tics=(\d+) real=(\d+) tickcyc=(\d+) dispcyc=(\d+) sndcyc=(\d+) maxframe=(\d+)(.*)')
    for line in open(path, errors='replace'):
        m = pat.search(line)
        if m:
            v = [int(x) for x in m.groups()[1:8]]
            kv = dict(re.findall(r'(\w+)=(\d+)', m[9]))
            tick[int(m[1])] = (v, {k: int(x) for k, x in kv.items()})
        m = re.search(r'PROF win=(\d+) frames=(\d+) tics=(\d+) total=(\d+)', line)
        if m:
            prof[int(m[1])] = int(m[4])
    rows = [(w, v) for w, v in sorted(tick.items()) if w >= 1 and w in prof]
    if not rows:
        print(path, ': no TICK lines (needs a --prof build with the OPT11-CORE counters)')
        return
    fr = sum(v[0][0] for _, v in rows)
    tics = sum(v[0][1] for _, v in rows)
    real = sum(v[0][2] for _, v in rows)
    tk = sum(v[0][3] for _, v in rows)
    dp = sum(v[0][4] for _, v in rows)
    sn = sum(v[0][5] for _, v in rows)
    tot = sum(prof[w] for w, _ in rows)
    mx = max(v[0][6] for _, v in rows)
    ex = lambda k: sum(v[1].get(k, 0) for _, v in rows)
    out = ('%-40s win %d frames %d tics %d | tick/tic %.2f M | disp/frame %.2f | snd %.3f | other/frame %.2f | total/frame %.2f M (%.1f FPS) | tics asked %d run %d lost %d'
           % (path.replace('build/runs/', '').replace('/boot.txt', ''), len(rows), fr, tics, tk / max(tics, 1) / 1e6, dp / fr / 1e6, sn / fr / 1e6, (tot - tk - dp - sn) / fr / 1e6,
              tot / fr / 1e6, CLOCK / (tot / fr), real, tics, real - tics))
    out += ' | longest frame %.1f ms' % (mx / CLOCK * 1000)
    if 'p1' in rows[0][1][1]:
        n = ex('p0') + ex('p1') + ex('p2') + ex('p3')
        s1, s2 = ex('isum') * 64.0, ex('isq') * 4096.0
        mean = s1 / max(n, 1)
        sd = max(s2 / max(n, 1) - mean * mean, 0) ** 0.5
        out += ' | frame intervals in 1/60 s: <0.5: %d, ~1: %d, ~2: %d, >2.5: %d (mean %.2f ms, sd %.2f ms), over 5 tics %d' % (ex('p0'), ex('p1'), ex('p2'), ex('p3'),
                                                                                                       mean / CLOCK * 1000, sd / CLOCK * 1000, ex('over5tics'))
    print(out)


for p in sys.argv[1:]:
    one(p)
