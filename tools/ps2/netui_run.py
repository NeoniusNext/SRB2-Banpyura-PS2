"""NETUI (OPT11, PS2-330..349): one PCSX2 run with the DEV9 Ethernet copy of the emulator and screen grabs of the emulator window.

usage: python3 tools/ps2/netui_run.py --name N --elf SRB2.ELF [--emu netui1] [--ini DEV9/Eth.EthEnable=false ...]
           [--grab 'TEXT@DELAY=file'] [--periodic SEC] [--timeout S] [--until TEXT] [--cfg 'cvar "value"'] [--pak build/pak] [--out build/runs]
           [--home-file SRC=DST] -- engine args

* The run directory <out>/<name>/ is staged like opt_run.py (ELF, hard-linked packs, .srb2/reference.cfg) and is the host: of the engine; the engine log is boot.txt.
* The emulator is /opt/pcsx2/<emu>/AppRun (a private copy of slot0 with [DEV9/Eth] EthEnable=true EthApi=Sockets, see docs/GATES/g1/opt11-NETUI.md).
  --ini SECTION.KEY=VALUE rewrites a key of that copy's PCSX2.ini for the run (EthEnable=false: no Ethernet device; InterceptDHCP=false: the emulator answers no DHCP
  request) and puts the saved .good file back at the end. The copy is private to this tool: the shared slots 0..3 are never touched.
* The emulator runs inside its own Xvfb (display :70..:79). --grab 'TEXT@DELAY=name' takes a picture of the whole X screen (the emulator window; `import -window root`)
  DELAY seconds after TEXT first appears in boot.txt; --periodic SEC takes one every SEC seconds (to show a frozen or a moving picture).
* A lock file per emulator copy (run_pcsx2.py) keeps two runs of one copy apart.
Exit: 0 the --until text appeared (or the process ended by itself), 2 timeout, 3 --stall.
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
import opt_run  # noqa: E402  (stage())
import run_pcsx2  # noqa: E402  (locks)


def killgroup(p, sig):
    try:
        os.killpg(os.getpgid(p.pid), sig)
    except (ProcessLookupError, PermissionError, OSError):
        pass


def set_ini(path, edits):
    """edits: ['Section.Key=Value']; the section is created when missing"""
    text = path.read_text()
    lines = text.split('\n')
    for e in edits:
        sk, _, val = e.partition('=')
        sec, _, key = sk.rpartition('.')
        head = '[%s]' % sec
        try:
            i = lines.index(head)
        except ValueError:
            lines += ['', head, '%s = %s' % (key, val)]
            continue
        j = i + 1
        done = False
        while j < len(lines) and not lines[j].startswith('['):
            if lines[j].split('=')[0].strip() == key:
                lines[j] = '%s = %s' % (key, val)
                done = True
            j += 1
        if not done:
            lines.insert(i + 1, '%s = %s' % (key, val))
    path.write_text('\n'.join(lines))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--name', required=True)
    ap.add_argument('--elf', required=True)
    ap.add_argument('--emu', default='netui1')
    ap.add_argument('--ini', action='append', default=[])
    ap.add_argument('--grab', action='append', default=[])
    ap.add_argument('--periodic', type=float, default=0)
    ap.add_argument('--timeout', type=float, default=180)
    ap.add_argument('--until', default='')
    ap.add_argument('--stall', default='', help='TEXT=SECONDS: stop (exit 3) when the number of times TEXT is in boot.txt has not grown for SECONDS after it first appeared')
    ap.add_argument('--cfg', action='append', default=[])
    ap.add_argument('--pak', default=str(ROOT / 'build/pak'))
    ap.add_argument('--out', default=str(ROOT / 'build/runs'))
    ap.add_argument('--home-file', action='append', default=[])
    ap.add_argument('extra', nargs='*')
    a = ap.parse_args()

    run = Path(a.out).resolve() / a.name
    shutil.rmtree(run / 'grabs', ignore_errors=True)
    opt_run.stage(run, Path(a.elf).resolve(), Path(a.pak).resolve(), None, ''.join(l + '\n' for l in a.cfg))
    for hf in a.home_file:
        src, _, dst = hf.partition('=')
        (run / '.srb2' / dst).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, run / '.srb2' / dst)
    (run / 'grabs').mkdir(exist_ok=True)
    for f in ('boot.txt', 'pcsx2.log', 'reference.cfg'):  # reference.cfg in the run directory is the config that the engine SAVED at the end of the last run (a cvar set by a command, e.g. menuhints 0, would come back): every run starts from the staged .srb2/reference.cfg
        (run / f).unlink(missing_ok=True)
    args = ['-logfile', 'boot.txt', '-config', 'reference.cfg', '-nolog', '-noendtxt'] + a.extra
    (run / 'ps2args').write_text('\n'.join(args[2:]) + '\n')

    emu_dir = run_pcsx2.PCSX2_ROOT / a.emu
    emu = emu_dir / 'AppRun'
    ini = emu_dir / 'usr/bin/inis/PCSX2.ini'
    good = emu_dir / 'usr/bin/PCSX2.ini.good'
    if not emu.exists():
        raise SystemExit('no emulator copy %s' % emu)
    lock = run_pcsx2.LOCKDIR / ('pcsx2-run-%s.lock' % a.emu)
    t0 = time.time()
    while not run_pcsx2._try_lock(lock):
        if time.time() - t0 > 900:
            raise SystemExit('could not get the lock of %s' % a.emu)
        time.sleep(1)
    xp = None
    p = None
    code = 2
    try:
        if good.exists():
            shutil.copy2(good, ini)  # every run starts from the known-good file
        if a.ini:
            set_ini(ini, a.ini)
        n = 70
        while Path('/tmp/.X11-unix/X%d' % n).exists() and n < 80:
            n += 1
        disp = ':%d' % n
        xp = subprocess.Popen(['Xvfb', disp, '-screen', '0', '800x600x24', '-nolisten', 'tcp'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        for _ in range(50):
            if Path('/tmp/.X11-unix/X%d' % n).exists():
                break
            time.sleep(0.1)
        env = dict(os.environ, DISPLAY=disp, QT_QPA_PLATFORM='xcb', LC_ALL='C.UTF-8')
        cmd = [str(emu), '-portable', '-batch', '-nogui', '-fastboot', '-elf', str(run / 'SRB2.ELF'), '-logfile', str(run / 'pcsx2.log'), '-gameargs', ' '.join(args[:2])]
        p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env, start_new_session=True)
        print('[%s] emulator pid %d on %s' % (time.strftime('%H:%M:%S'), p.pid, disp), flush=True)
        grabs = []
        for g in a.grab:
            left, _, name = g.rpartition('=')
            text, _, delay = left.rpartition('@')
            grabs.append({'text': text, 'delay': float(delay or 0), 'name': name, 'seen': None, 'done': False})
        start = time.time()
        last_periodic = start
        stall_text, _, stall_secs = a.stall.rpartition('=')
        stall_count, stall_time = 0, start
        seq = 0
        while time.time() - start < a.timeout:
            if p.poll() is not None:
                code = 0
                break
            try:
                boot = (run / 'boot.txt').read_text(errors='replace')
            except OSError:
                boot = ''
            now = time.time()
            for g in grabs:
                if g['done']:
                    continue
                if g['seen'] is None and g['text'] in boot:
                    g['seen'] = now
                if g['seen'] is not None and now - g['seen'] >= g['delay']:
                    out = run / 'grabs' / (g['name'] if g['name'].endswith('.png') else g['name'] + '.png')
                    subprocess.run(['import', '-display', disp, '-window', 'root', str(out)], check=False, timeout=30)
                    print('[%s] grab %s' % (time.strftime('%H:%M:%S'), out.name), flush=True)
                    g['done'] = True
            if a.periodic and now - last_periodic >= a.periodic:
                last_periodic = now
                out = run / 'grabs' / ('p%04d.png' % seq)
                seq += 1
                subprocess.run(['import', '-display', disp, '-window', 'root', str(out)], check=False, timeout=30)
            if a.until and a.until in boot and all(g['done'] for g in grabs):
                code = 0
                break
            if stall_text:
                n_now = boot.count(stall_text)
                if n_now != stall_count:
                    stall_count, stall_time = n_now, now
                elif stall_count and now - stall_time > float(stall_secs):
                    code = 3
                    break
            time.sleep(0.25)
    finally:
        if p is not None and p.poll() is None:
            killgroup(p, signal.SIGINT)
            try:
                p.wait(25)
            except subprocess.TimeoutExpired:
                pass
        if p is not None:
            killgroup(p, signal.SIGKILL)
        if xp is not None:
            killgroup(xp, signal.SIGTERM)
            try:
                xp.wait(5)
            except subprocess.TimeoutExpired:
                killgroup(xp, signal.SIGKILL)
        try:
            if good.exists():
                shutil.copy2(good, ini)
        except OSError:
            pass
        try:
            lock.unlink()
        except OSError:
            pass
    print('%s: exit %d, %.0f s' % (a.name, code, time.time() - t0))
    return code


if __name__ == '__main__':
    sys.exit(main())
