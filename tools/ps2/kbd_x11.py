"""USB keyboard stand on Linux (OPT10-X): PCSX2 copy /opt/pcsx2/usbk ([USB1] = HID keyboard, no pad bindings, no hotkeys) in a PRIVATE Xvfb; keys are
injected with xdotool into that display only (the container has no other desktop).

usage: kbd_x11.py --name NAME --elf SRB2.ELF [--script "T:key:Return;T:type:text;T:key:Escape"] [--ready TEXT] [--until TEXT] [--timeout 300]
                  [--cfg 'cvar "val"'] [--out build/runs] [--pak build/pakx] [--files a,b] [--show TEXT,TEXT] -- engine args...
Stages <out>/<name>/ like opt_run.py (ELF, packs, .srb2/reference.cfg, ps2args); starts Xvfb :N and the emulator (lock: own lock file for the usbk copy);
the script steps run T seconds after the --ready text appeared in the engine log (default: 3 s after the emulator window exists):
  key:NAME     xdotool key NAME (Return, Escape, Up, Down, BackSpace, space, a, 1, F12, ...)      type:TEXT   xdotool type (50 ms between keys)
Prints the engine log lines that contain one of --show. Exit 0 when --until appeared (or the emulator exited by itself), 2 on timeout.
"""
import argparse
import os
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from opt_run import stage  # noqa: E402
import run_pcsx2  # noqa: E402

EMU = str(run_pcsx2.PCSX2_ROOT / 'usbk/AppRun')
LOCK = run_pcsx2.LOCKDIR / 'pcsx2-run-usbk.lock'


def free_display():
    for n in range(120, 200):
        if not Path(f'/tmp/.X11-unix/X{n}').exists() and not Path(f'/tmp/.X{n}-lock').exists():
            return n
    raise SystemExit('no free X display')


def xdo(env, *args):
    return subprocess.run(['xdotool', *args], env=env, capture_output=True, text=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--name', required=True)
    ap.add_argument('--elf', required=True)
    ap.add_argument('--pak', default=str(ROOT / 'build/pakx'))
    ap.add_argument('--out', default=str(ROOT / 'build/runs'))
    ap.add_argument('--script', default='')
    ap.add_argument('--ready', default='Entering main game loop')
    ap.add_argument('--until', default='')
    ap.add_argument('--timeout', type=float, default=300)
    ap.add_argument('--cfg', default='')
    ap.add_argument('--files', default='')
    ap.add_argument('--show', default='PS2 kbd,PS2USB,KBD,I_Error,Error')
    ap.add_argument('--pc-server', action='store_true', help='also run a PC dedicated server (build/pc-net, -netsync; master server = a dead local port) on the host; takes the net lock (fixed UDP ports 5029/5030)')
    ap.add_argument('extra', nargs='*')
    a = ap.parse_args()
    run = Path(a.out).resolve() / a.name
    stage(run, Path(a.elf).resolve(), Path(a.pak).resolve(), None, a.cfg + ('\n' if a.cfg else ''))
    for f in [x for x in a.files.split(',') if x]:
        src = Path(f) if Path(f).is_absolute() else ROOT / f
        shutil.copy2(src, run / src.name)
    args = ['-logfile', 'boot.txt', '-config', 'reference.cfg', '-nolog', '-noendtxt'] + a.extra
    (run / 'ps2args').write_text('\n'.join(args[2:]) + '\n')
    boot = run / 'boot.txt'
    boot.unlink(missing_ok=True)
    steps = []
    for st in [x for x in a.script.split(';') if x]:
        t, _, cmd = st.partition(':')
        steps.append((float(t), cmd))
    steps.sort(key=lambda s: s[0])
    end = time.time() + 900
    while not run_pcsx2._try_lock(LOCK):
        if time.time() > end:
            raise SystemExit('could not get the usbk lock')
        time.sleep(1)
    xv = emu = srv = None
    code = 2
    netlock = False
    try:
        if a.pc_server:
            import net_env
            run_pcsx2.acquire(3600, run_pcsx2.NETLOCK)
            netlock = True
            home = run / 'pcsrv-home'
            (home / '.srb2').mkdir(parents=True, exist_ok=True)
            for name in ('config.cfg', 'dconfig.cfg'):  # SAFETY: never the real master server (a dedicated server reads dconfig.cfg)
                (home / '.srb2' / name).write_text('masterserver "http://127.0.0.1:9/MS/0"\nnettimeout "2100"\njointimeout "2100"\n')
            senv = {k: v for k, v in os.environ.items() if k.lower() not in ('https_proxy', 'http_proxy', 'all_proxy')}
            senv.update(SRB2WADDIR='/opt/srb2-assets', SDL_AUDIODRIVER='dummy')
            srv = subprocess.Popen([str(ROOT / net_env.pc_exe()), '-dedicated', '-server', '-nomusic', '-nosound', '-netsync', '-home', str(home), '-warp', 'MAP01'], cwd=str(home),
                                   stdout=open(run / 'pcsrv.txt', 'wb'), stderr=subprocess.STDOUT, env=senv, stdin=subprocess.DEVNULL, start_new_session=True)
            time.sleep(4)
        d = free_display()
        xv = subprocess.Popen(['Xvfb', f':{d}', '-screen', '0', '800x600x24', '-nolisten', 'tcp'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        time.sleep(1.5)
        env = dict(os.environ, DISPLAY=f':{d}', QT_QPA_PLATFORM='xcb', LC_ALL='C.UTF-8')
        cmd = [EMU, '-portable', '-batch', '-nogui', '-fastboot', '-elf', str(run / 'SRB2.ELF'), '-logfile', str(run / 'pcsx2.log'), '-gameargs', ' '.join(args[:2])]
        emu = subprocess.Popen(cmd, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        t0 = time.time()
        win = None
        ready_at = None
        done = set()
        while time.time() - t0 < a.timeout:
            if emu.poll() is not None:
                code = 0
                break
            text = boot.read_text(errors='replace') if boot.exists() else ''
            if win is None:
                r = xdo(env, 'search', '--onlyvisible', '--name', '.')
                ids = [x for x in r.stdout.split() if x.isdigit()]
                if ids:
                    win = ids[-1]
                    print('window', win, flush=True)
            if ready_at is None and win and (not a.ready or a.ready in text):
                ready_at = time.time()
                print(f'ready after {ready_at - t0:.0f}s', flush=True)
            if ready_at:
                for i, (t, c) in enumerate(steps):
                    if i not in done and time.time() - ready_at >= t:
                        done.add(i)
                        kind, _, arg = c.partition(':')
                        xdo(env, 'windowfocus', win)
                        if kind == 'key':
                            r = xdo(env, 'key', '--delay', '60', arg)
                        elif kind == 'type':
                            r = xdo(env, 'type', '--delay', '50', arg)
                        else:
                            r = None
                        print(f'[{time.time() - ready_at:6.1f}s] {c}' + (f'  (xdotool rc {r.returncode} {r.stderr.strip()})' if r else ''), flush=True)
            if a.until and a.until in text:
                code = 0
                time.sleep(1)
                break
            time.sleep(0.5)
    finally:
        for p in (emu, srv, xv):
            if p and p.poll() is None:
                try:
                    os.killpg(os.getpgid(p.pid), signal.SIGINT)
                except OSError:
                    pass
        time.sleep(2)
        if netlock:
            try:
                run_pcsx2.NETLOCK.unlink()
            except OSError:
                pass
        for p in (emu, srv, xv):
            if p:
                try:
                    os.killpg(os.getpgid(p.pid), signal.SIGKILL)
                except OSError:
                    pass
        LOCK.unlink(missing_ok=True)
    text = boot.read_text(errors='replace') if boot.exists() else ''
    keys = [k for k in a.show.split(',') if k]
    for line in text.splitlines():
        if any(k in line for k in keys):
            print(line)
    print(f'{a.name}: exit {code}')
    return code


if __name__ == '__main__':
    sys.exit(main())
