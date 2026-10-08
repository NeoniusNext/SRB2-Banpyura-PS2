"""Run audio scenarios in PCSX2 and judge them with the IOP ring model (tools/ps2/audio_ring_sim.py).

usage: python3 tools/ps2/audio_scen.py --elf build/X.ELF --tag before [--out build/runs] [--timeout 700] SCEN [SCEN ...]
Each scenario is a demo (timed, as fast as the emulated EE runs it) with the audio diagnostics -adump/-atrace and a normal exit at
level frame --frames (-aquit). The run writes <out>/<tag>_<scen>/{boot.txt,.srb2/atrace.bin,.srb2/apcm.raw}; afterwards the model replays
the journal and prints one row: the ASTAT counters and the time the SPU2 would have spent on stale (replayed) non-silent audio.
Scenarios: d1_mus d1_off d1_stop d1_vol0 d1_track d2_off d4_off_hw d4_mus_hw d1_nomusic  (see SCEN below).
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PAK = '/home/user/SRB2-Banpyura-PS2/build/pak'
OFF = ['digimusic "Off"', 'midimusic "Off"']
SCEN = {
    # name: (demo, cfg lines, extra engine args)
    'd1_mus': ('DEMO_001', [], []),
    'd1_off': ('DEMO_001', OFF, []),
    'd1_nomusic': ('DEMO_001', [], ['-nomusic']),
    'd1_stop': ('DEMO_001', [], ['-acmd', '300', 'tunes -none']),
    'd1_vol0': ('DEMO_001', [], ['-acmd', '300', 'digmusicvolume 0']),
    'd1_mute': ('DEMO_001', [], ['-acmd', '300', 'digmusicvolume 0', '-acmd', '700', 'digmusicvolume 31']),
    'd1_track': ('DEMO_001', [], ['-acmd', '300', 'tunes 5', '-acmd', '600', 'tunes 8']),
    'd2_off': ('DEMO_002', OFF, []),
    'd2_mus': ('DEMO_002', [], []),
    'd3_off': ('DEMO_003', OFF, []),
    'd4_off_hw': ('DEMO_004', OFF, ['-renderer', 'Hardware']),
    'd4_mus_hw': ('DEMO_004', [], ['-renderer', 'Hardware']),
    'd1_off_hw': ('DEMO_001', OFF, ['-renderer', 'Hardware']),
    # no demo: the player stands still in MAP01 (a pause, then a track change; no SFX of its own)
    'm1_ref': ('MAP:MAP01', [], []),
    'm1_mute': ('MAP:MAP01', [], ['-acmd', '200', 'digmusicvolume 0', '-acmd', '500', 'digmusicvolume 31']),
    'm1_pause': ('MAP:MAP01', [], ['-acmd', '300', 'pause', '-acmd', '600', 'pause']),
    'm1_none': ('MAP:MAP01', [], ['-acmd', '300', 'tunes -none']),
}


def run(elf, tag, name, out, timeout, frames, dump, notrace=False):
    demo, cfg, extra = SCEN[name]
    run_name = '%s_%s' % (tag, name)
    cmd = [sys.executable, str(ROOT / 'tools/ps2/opt_run.py'), '--name', run_name, '--elf', str(elf), '--pak', PAK, '--out', str(out),
           *(['--map', demo[4:]] if demo.startswith('MAP:') else ['--demo', demo]), '--no-ref', '--timeout', str(timeout), '--until', 'ASTAT final threads' if notrace else 'ASTAT trace']
    for c in cfg:
        cmd += ['--cfg', c]
    cmd += ['--'] + ([] if notrace else ['-adump', str(dump), '-atrace', '60000']) + ['-aquit', str(frames)] + extra
    subprocess.run(cmd, capture_output=True, text=True)
    return Path(out) / run_name


def astat(run_dir):
    text = (run_dir / 'boot.txt').read_text(errors='replace') if (run_dir / 'boot.txt').exists() else ''
    last = None
    for line in text.splitlines():
        if line.startswith('ASTAT final mode='):
            last = dict(x.split('=', 1) for x in line.split()[2:] if '=' in x)
    return last, text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', required=True)
    ap.add_argument('--tag', required=True)
    ap.add_argument('--out', default=str(ROOT / 'build/runs'))
    ap.add_argument('--timeout', type=float, default=700)
    ap.add_argument('--frames', type=int, default=1000)
    ap.add_argument('--dump', type=int, default=700000)
    ap.add_argument('--no-run', action='store_true', help='only analyse existing run dirs')
    ap.add_argument('--notrace', action='store_true', help='no -adump/-atrace (the journal itself costs two RPCs per wake-up): ASTAT counters only, no ring model')
    ap.add_argument('scen', nargs='+')
    a = ap.parse_args()
    rows = []
    for name in a.scen:
        d = Path(a.out) / ('%s_%s' % (a.tag, name)) if a.no_run else run(a.elf, a.tag, name, a.out, a.timeout, a.frames, a.dump, a.notrace)
        st, text = astat(d)
        sim = {}
        r = subprocess.run([sys.executable, str(ROOT / 'tools/ps2/audio_ring_sim.py'), str(d), '--wav', str(d / 'heard.wav'),
                            '--json', str(d / 'sim.json'), '--quiet'], capture_output=True, text=True)
        if (d / 'sim.json').exists():
            sim = json.loads((d / 'sim.json').read_text())
        music = len(re.findall(r'music playback', text))
        row = {'scen': name, 'tag': a.tag, 'music_starts': music}
        if st:
            row.update({k: st.get(k) for k in ('cap', 'underruns', 'gaptotal_ms', 'blocks', 'flushblk', 'sent', 'minq_ms', 'mstarve', 'hmis', 'cmdfull', 'maingap_ms')})
        row.update({k: sim.get(k) for k in ('heard_s', 'stale_ms', 'stale_nonsilent_ms', 'stale_nonsilent_runs', 'longest_stale_nonsilent_ms', 'phase_matched', 'phase_total')})
        if not sim:
            row['sim_error'] = (r.stdout + r.stderr).strip()[-200:]
        rows.append(row)
        print(json.dumps(row), flush=True)
        with open(ROOT / 'build/audio_scen.jsonl', 'a') as f:
            f.write(json.dumps(row) + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
