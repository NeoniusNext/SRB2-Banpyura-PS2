"""OPT11-STAB: scripted tours of the non-level screens in both renderers (title, menus, options, intro, intermission, ending, credits, NiGHTS, split screen,
renderer toggling), run through opt_run.py with a pad script; the end of the run is -zquitall and the engine log is checked for errors.

usage: stab_ui.py --elf SRB2.ELF --tag NAME --scenario menus|intro|ending|credits|nights|split|toggle|special|title-long [--renderer Hardware|Software] [--timeout 900]
Each scenario writes build/runs/<tag>-<scenario>-<renderer>/ (boot.txt, vidshot-*.ppm) and prints one result line: ok = ZQUIT DONE and no I_Error/OOM/HEAP CHECK.
The pictures (-vidshot) are for looking at: convert x.ppm -scale 200% x.png.
"""
import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/ps2'))
import padseq  # noqa: E402

PAK = '/home/user/SRB2-Banpyura-PS2/build/pak'


def menus_items():
    """start opens the main menu; every entry is entered with cross, looked at, left with circle; Options is walked through its submenus."""
    items = ['120:start']
    t = 180
    for i in range(6):
        items += [f'{t}:cross', f'{t + 60}:circle', f'{t + 100}:down']
        t += 140
    # Options: back to the top entries, enter Options, walk the list
    items += [f'{t}:up*8/20', f'{t + 200}:cross']
    t += 260
    for i in range(8):
        items += [f'{t}:cross', f'{t + 50}:circle', f'{t + 80}:down']
        t += 110
    items += [f'{t}:circle*3/30']
    return items, t + 150


SCEN = {
    # name: (engine args, pad items or callable, frames, vidshots)
    'menus': (['-skipintro'], menus_items, None, 't60,f300,f520,f800,f1000,f1200,f1500,f1800,f2100'),
    'intro': ([], lambda: (['600:start*3/40'], 2600), None, 'f200,f500,f900,f1400,f2000'),
    'title-long': (['-skipintro'], lambda: ([], 3000), None, 't100,t800,t1600,t2400'),
    # the screens of a finished game, started from the console (ps2_finale N, src/ps2/ps2_hwfb.c)
    'ending': (['-skipintro', '-warp', 'MAP01', '-netcmd', '60:ps2_finale 1'], lambda: ([], 1400), None, 'f150,f400,f700,f1000,f1300'),
    'credits': (['-skipintro', '-warp', 'MAP01', '-netcmd', '60:ps2_finale 2'], lambda: ([], 1400), None, 'f150,f400,f700,f1000,f1300'),
    'evaluation': (['-skipintro', '-warp', 'MAP01', '-netcmd', '60:ps2_finale 3'], lambda: ([], 900), None, 'f150,f400,f800'),
    'continue': (['-skipintro', '-warp', 'MAP01', '-netcmd', '60:ps2_finale 4'], lambda: ([], 900), None, 'f150,f400,f800'),
    'gameend': (['-skipintro', '-warp', 'MAP01', '-netcmd', '60:ps2_finale 5'], lambda: ([], 900), None, 'f150,f400,f800'),
    'intro-cmd': (['-skipintro', '-warp', 'MAP01', '-netcmd', '60:ps2_finale 6'], lambda: ([], 1800), None, 'f150,f500,f900,f1400'),
    # special stage / NiGHTS / bonus: the player runs and jumps (scripted pad), a few hundred frames of real play, then the intermission
    'special': (['-skipintro', '-warp', 'MAP50', '-netcmd', '1500:exitlevel'], lambda: (['10:cross*1', '30:up*40/30~25'], 2300), None, 'l100,l600,l1200,i30'),
    'nights': (['-skipintro', '-warp', 'MAP70', '-netcmd', '1500:exitlevel'], lambda: (['10:cross*1', '30:right*40/30~25'], 2300), None, 'l100,l600,l1200,i30'),
    # the renderer switch in both directions, 25 times: in a level, on the title screen, with the menu open (the engine's -hwtoggle N changes the `renderer` cvar every N frames;
    # the game must start in Software: -renderer would make the cvar read-only)
    'toggle-level': (['-skipintro', '-warp', 'MAP01', '-hwtoggle', '70'], lambda: ([], 1900), None, 'l50,f400,f900,f1500'),
    'toggle-title': (['-skipintro', '-hwtoggle', '55'], lambda: ([], 1500), None, 't40,f400,f900'),
    'toggle-menu': (['-skipintro', '-hwtoggle', '65'], lambda: (['100:start', '150:cross', '300:circle', '340:down', '380:cross', '520:circle'], 1700), None, 'f300,f800,f1300'),
}


def run(a, name, renderer):
    args, items, frames, shots = SCEN[name]
    run_name = f'{a.tag}-{name}-{renderer[:2].lower()}'
    d = ROOT / 'build/runs' / run_name
    d.mkdir(parents=True, exist_ok=True)
    pad, nframes = items()
    (d / 'pad.txt').write_text(padseq.script(pad) if pad else '')
    extra = list(args)
    if renderer == 'Hardware' and not name.startswith('toggle'):
        extra += ['-renderer', 'Hardware']
    extra += ['-zquitall', str(nframes), '-vidshot', shots, '-zck']
    if pad:
        extra += ['-padscript', 'file:pad.txt']
    cmd = [sys.executable, '-B', str(ROOT / 'tools/ps2/opt_run.py'), '--name', run_name, '--elf', a.elf, '--pak', PAK, '--out', str(ROOT / 'build/runs'),
           '--timeout', str(a.timeout), '--until', 'ZQUIT DONE', '--'] + extra
    # the pad script file must exist next to the ELF before the run: opt_run stages the directory, a file placed in it stays
    p = subprocess.run(cmd, capture_output=True, text=True)
    text = (d / 'boot.txt').read_text(errors='replace') if (d / 'boot.txt').exists() else ''
    bad = [l.strip()[:160] for l in text.splitlines() if 'I_Error' in l or 'OOM:' in l or 'HEAP CHECK FAILED' in l or 'WATCHDOG' in l]
    ok = 'ZQUIT DONE' in text and not bad
    shotsn = len(list(d.glob('vidshot-*.ppm'))) + len(list((d / '.srb2').glob('vidshot-*.ppm')))
    print(f'{run_name}: {"ok" if ok else "FAIL"} frames={nframes} shots={shotsn} {bad[:2]}', flush=True)
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', required=True)
    ap.add_argument('--tag', required=True)
    ap.add_argument('--scenario', default='menus')
    ap.add_argument('--renderer', default='both')
    ap.add_argument('--timeout', type=int, default=1200)
    a = ap.parse_args()
    ok = True
    for name in a.scenario.split(','):
        for r in (['Software', 'Hardware'] if a.renderer == 'both' and not name.startswith('toggle') else ['Software'] if name.startswith('toggle') else [a.renderer]):
            ok &= run(a, name, r)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
