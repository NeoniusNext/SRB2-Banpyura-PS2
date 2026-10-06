"""Load every campaign map on the 32 MB profile and tabulate success, memory peak, smallest free block and load time.

usage: python tools/ps2/map_sweep.py --elf SRB2.ELF --tag NAME [--out build/opt4-s/sweep] [--maps 01,11,...] [--kinds SP,MP-special]
                                     [--frames 35] [--ram 32] [--chain] [-- extra engine args]
One emulator run per map through tools/ps2/opt_run.py (machine-wide lock), -skipintro -warp MAPxx -zck -zquit FRAMES.
Per map it reads the engine log (boot.txt) of the run and records:
  ok          the run reached "ZQUIT DONE" with no OOM/I_Error and the heap check passed
  peak        globalpeak of used zone bytes (ZSTAT gpeak), free / largest free block at the end (ZSTAT free, largest)
  load_s      EE cycles between the "level-free-before" and "precache" checkpoints (-zck) / 294.912 MHz: the level load
  first_s     EE cycles from "precache" to the first displayed frame is not measured; levelframes cycles give the frame cost
  oom         request size and tag of the allocation that failed
--chain runs all selected maps in ONE emulator session with the -zchain engine option instead (map -> map -> map, ZSTAT after each).
The map list comes from build/ps2-sw-continuation-map-inventory.json (tools/ps2/map_inventory.py).
"""
import argparse
import json
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
INV = ROOT / 'build/ps2-sw-continuation-map-inventory.json'
HZ = 294912000.0


def map_list(kinds):
    inv = json.loads(INV.read_text())
    out = []
    for m in inv:
        if m['kind'] in kinds:
            out.append((m['map'], m['kind'], m))
    # heaviest first is the useful order for a partial sweep
    out.sort(key=lambda t: -(t[2]['n_linedefs'] + t[2]['n_segs']))
    return out


def parse_boot(text):
    r = {'ok': False, 'oom': None, 'errors': [], 'zck': {}}
    for line in text.splitlines():
        if line.startswith('[zck] '):
            f = line.split()
            name = f[1]
            kv = dict(x.split('=', 1) for x in f[2:] if '=' in x)
            r['zck'].setdefault(name, []).append({k: int(v) for k, v in kv.items() if v.isdigit()})
        elif line.startswith('ZSTAT '):
            r['zstat'] = {k: int(v) for k, v in (x.split('=', 1) for x in line.split()[1:])}
        elif line.startswith('OOM: request'):
            m = re.match(r'OOM: request (\d+) B tag (\d+) \((\w+)\)', line)
            if m:
                r['oom'] = {'bytes': int(m.group(1)), 'tag': m.group(3)}
        elif 'I_Error' in line or 'HEAP CHECK FAILED' in line:
            r['errors'].append(line.strip())
    for line in text.splitlines():
        if line.startswith('ZSTAT ') and 'libcpeak=' in line:
            r['libcpeak'] = int(line.split('libcpeak=')[1].split()[0])
            r['stackused'] = int(line.split('stackused=')[1].split()[0])
            r['boot_ms'] = None
    r['done'] = 'ZQUIT DONE' in text
    r['ok'] = r['done'] and not r['errors'] and r['oom'] is None
    return r


def load_cycles(zck):
    a = zck.get('level-free-before')
    b = zck.get('precache')
    if not a or not b:
        return None
    if 't_ms' in a[-1] and 't_ms' in b[-1]:  # OPT4: the 64-bit bus-clock stamp, no 14.6 s wrap of COP0 Count
        return int((b[-1]['t_ms'] - a[-1]['t_ms']) * HZ / 1000)
    return (b[-1]['cop0'] - a[-1]['cop0']) & 0xFFFFFFFF


def chain_run(a, maps, outdir):
    """One emulator session: -warp to the first map, then -zchain through the others (a level change on every step)."""
    names = [m[0] for m in maps]
    if not names:
        print('no maps selected')
        return 2
    run = f'{a.tag}-chain'
    cmd = [sys.executable, '-B', str(ROOT / 'tools/ps2/opt_run.py'), '--name', run, '--elf', a.elf, '--ram', str(a.ram),
           '--out', str(outdir), '--pak', a.pak, '--map', 'MAP' + names[0], '--timeout', str(a.timeout * max(1, len(names)) // 3 + 600),
           '--until', 'ZQUIT DONE', '--', '-zck', '-zquit', str(a.frames), '-zchain', ','.join(names[1:])] + a.extra
    t0 = time.time()
    p = subprocess.run(cmd, capture_output=True, text=True)
    boot = outdir / run / 'boot.txt'
    text = boot.read_text(errors='replace') if boot.exists() else ''
    steps = []
    for line in text.splitlines():
        if line.startswith('ZCHAIN n='):
            steps.append({k: v for k, v in (x.split('=', 1) for x in line.split()[1:] if '=' in x)})
    r = parse_boot(text)
    final = r.get('zstat', {})
    md = ['| step | map | check | used B | peak B | free B | largest free B | evicted B | libc spare B | Mcycles |', '|---:|---|---|---:|---:|---:|---:|---:|---:|---:|']
    for i, s in enumerate(steps):
        md.append(f"| {s['n']} | MAP{names[i]} | {s['check']} | {s['used']} | {s['peak']} | {s['free']} | {s['largest']} | {s['evictedbytes']} | {s['libcfree']} | {s['mcycles']} |")
    (outdir / 'chain.md').write_text('\n'.join(md) + '\n')
    print('\n'.join(md))
    print(f'chain: {len(steps)} of {len(names) - 1} level changes reported (plus the first level); ok={r["ok"]} oom={r["oom"]} errors={r["errors"][:3]} wall={time.time() - t0:.0f}s')
    (outdir / 'chain.json').write_text(json.dumps({'maps': names, 'steps': steps, 'final': final, 'ok': r['ok'], 'oom': r['oom'], 'errors': r['errors']}, indent=1))
    return 0 if r['ok'] and len(steps) == len(names) and all(s['check'] == 'ok' for s in steps) else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', required=True)
    ap.add_argument('--tag', required=True)
    ap.add_argument('--out', default=str(ROOT / 'build/opt4-s/sweep'))
    ap.add_argument('--maps', default='')
    ap.add_argument('--kinds', default='SP,MP-special')
    ap.add_argument('--frames', type=int, default=35)
    ap.add_argument('--ram', type=int, default=32)
    ap.add_argument('--timeout', type=int, default=900)
    ap.add_argument('--pak', default=str(ROOT / 'build/pak-a'))
    ap.add_argument('--chain', action='store_true', help='all maps in one session (level change after level, -zchain)')
    ap.add_argument('extra', nargs='*')
    a = ap.parse_args()

    maps = map_list(a.kinds.split(','))
    if a.maps:
        want = a.maps.split(',')
        maps = [t for t in maps if t[0] in want]
        maps.sort(key=lambda t: want.index(t[0]))
    outdir = Path(a.out) / a.tag
    outdir.mkdir(parents=True, exist_ok=True)
    results = []
    resfile = outdir / 'results.json'
    if a.chain:
        return chain_run(a, maps, outdir)
    for name, kind, inv in maps:
        run = f'{a.tag}-MAP{name}'
        cmd = [sys.executable, '-B', str(ROOT / 'tools/ps2/opt_run.py'), '--name', run, '--elf', a.elf, '--ram', str(a.ram),
               '--out', str(outdir), '--pak', a.pak, '--map', 'MAP' + name, '--timeout', str(a.timeout), '--',
               '-zck', '-zquit', str(a.frames)] + a.extra
        t0 = time.time()
        for _try in range(20):  # PS2-76: the machine-wide emulator lock can stay busy longer than run_pcsx2's own wait (900 s); a lock timeout is not a map result
            p = subprocess.run(cmd, capture_output=True, text=True)
            rl = outdir / run / 'run.log'
            if not (rl.exists() and 'could not get the PCSX2 lock' in rl.read_text(errors='replace')):
                break
        boot = outdir / run / 'boot.txt'
        text = boot.read_text(errors='replace') if boot.exists() else ''
        r = parse_boot(text)
        r.update({'map': name, 'kind': kind, 'lines': inv['n_linedefs'], 'segs': inv['n_segs'], 'things': inv['n_things'],
                  'wall_s': round(time.time() - t0, 1), 'rc': p.returncode})
        lc = load_cycles(r['zck'])
        r['load_cycles'] = lc
        r['load_s'] = round(lc / HZ, 2) if lc is not None else None
        z = r.get('zstat', {})
        r['gpeak'] = z.get('gpeak')
        zc = r['zck'].get('level-free-before')
        r['start_ms'] = zc[-1].get('t_ms') if zc else None  # engine start -> level load begins (EE time)
        r['free'] = z.get('free')
        r['largest'] = z.get('largest')
        r['evicted'] = z.get('evictedbytes')
        r['zck_peak'] = max((v[-1].get('peak', 0) for v in r['zck'].values()), default=None)
        r.pop('zck')
        results.append(r)
        resfile.write_text(json.dumps(results, indent=1))
        print(f"MAP{name:>3} {kind:10} ok={r['ok']} gpeak={r['gpeak']} free={r['free']} largest={r['largest']} load={r['load_s']}s oom={r['oom']}", flush=True)
    md = ['| map | kind | lines | ok | peak used B | free B | largest free B | evicted B | load s | libc peak B | OOM |', '|---|---|---:|---|---:|---:|---:|---:|---:|---:|---|']
    for r in sorted(results, key=lambda r: r['map']):
        md.append(f"| MAP{r['map']} | {r['kind']} | {r['lines']} | {'yes' if r['ok'] else 'NO'} | {r['gpeak']} | {r['free']} | {r['largest']} | {r['evicted']} | {r['load_s']} | {r.get('libcpeak')} | {r['oom'] or ''} |")
    (outdir / 'results.md').write_text('\n'.join(md) + '\n')
    bad = [r['map'] for r in results if not r['ok']]
    print('failed maps:', bad if bad else 'none')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
