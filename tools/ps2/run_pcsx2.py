"""Run an ELF in PCSX2 under a machine-wide lock per emulator copy (OPT9: several identical copies run in parallel).

usage: run_pcsx2.py --elf FILE --log FILE [--args "..."] [--timeout SEC]
                    [--unlimited] [--wait-for-exit | --until "TEXT"]
 - host: = the directory of the ELF (PCSX2 HLE host filesystem).
 - Exit code 0 only if PCSX2 exited by itself (poweroff/exit) within the timeout,
   or, with --until, the text appeared in the log (PCSX2 is then killed).
 - Prints the last lines of the log that start with the marker '--marker' (default none).
OPT9 slots: the standard 32 MB profile D:/PCSX2-test has identical private copies D:/PCSX2-s1..s3 (same inis). A run asking for
D:/PCSX2-test takes the first free copy, each copy has its own lock file, so up to four agents run the emulator at once.
SRB2_PCSX2_SLOTS=1 restores the old single-emulator behaviour. Other copies (PCSX2-test128, PCSX2-net1/2) have one lock each.
A start that dies at once (e.g. "Failed to create swap chain") is retried (3 times) on the next free copy.
"""
import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

LOCKDIR = Path('C:/Users/loban/AppData/Local/Temp')
LOCK = LOCKDIR / 'pcsx2-run.lock'  # the lock of D:/PCSX2-test (slot 0); kept for older tools
NETLOCK = LOCKDIR / 'pcsx2-net.lock'  # net_session.py: the PCSX2-net1/net2 copies and the fixed host UDP ports
PCSX2 = os.environ.get('SRB2_PCSX2', 'D:/PCSX2-test/pcsx2-qt.exe')  # private copy of D:/PCSX2 with ExtraMemory=false (real 32 MB)
BASE = 'D:/PCSX2-test/pcsx2-qt.exe'
_NSLOTS = int(os.environ.get('SRB2_PCSX2_SLOTS', '4'))
SLOTS = [BASE] + [f'D:/PCSX2-s{i}/pcsx2-qt.exe' for i in range(1, _NSLOTS)]
SLOTS = [x for x in SLOTS if Path(x).exists()] or [BASE]


def _norm(exe):
    return os.path.normcase(os.path.abspath(exe))


def lock_of(exe):
    if _norm(exe) == _norm(BASE):
        return LOCK
    return LOCKDIR / ('pcsx2-run-' + Path(exe).parent.name.lower() + '.lock')


def _try_lock(lock):
    try:
        fd = os.open(str(lock), os.O_CREAT | os.O_EXCL | os.O_WRONLY)
        os.write(fd, str(os.getpid()).encode())
        os.close(fd)
        return True
    except FileExistsError:
        try:  # stale lock (owner gone or older than 15 min)
            if time.time() - lock.stat().st_mtime > 900:
                lock.unlink()
        except OSError:
            pass
        return False


def acquire_exe(exe, wait):
    """Lock a free emulator equivalent to exe; returns (exe, lockfile)."""
    pool = SLOTS if _norm(exe) == _norm(BASE) else [exe]
    end = time.time() + wait
    while True:
        for cand in pool:
            lk = lock_of(cand)
            if _try_lock(lk):
                return cand, lk
        if time.time() > end:
            raise SystemExit('could not get the PCSX2 lock')
        time.sleep(1)


def acquire(wait, lock=None):
    """Old interface: lock one specific lock file (default: slot 0)."""
    lock = lock or LOCK
    end = time.time() + wait
    while not _try_lock(lock):
        if time.time() > end:
            raise SystemExit('could not get the PCSX2 lock')
        time.sleep(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', required=True)
    ap.add_argument('--log', required=True)
    ap.add_argument('--args', default='')
    ap.add_argument('--timeout', type=float, default=120)
    ap.add_argument('--lock-wait', type=float, default=900)
    ap.add_argument('--unlimited', action='store_true', help='BROKEN in PCSX2 2.6.3 here: the run hangs with an empty log; do not use')
    ap.add_argument('--until', default='')
    ap.add_argument('--until-file', default='', help='file (e.g. engine -logfile in the ELF dir) searched for --until instead of the PCSX2 log')
    ap.add_argument('--marker', default='')
    a = ap.parse_args()
    Path(a.log).parent.mkdir(parents=True, exist_ok=True)
    code = 1
    for attempt in range(4):
        exe, lock = acquire_exe(PCSX2, a.lock_wait)
        cmd = [exe, '-portable', '-batch', '-nogui', '-fastboot', '-elf', str(Path(a.elf).resolve()),
               '-logfile', str(Path(a.log).resolve())]
        if a.unlimited:
            cmd.append('-unlimited')
        if a.args:
            cmd += ['-gameargs', a.args]
        for stale_file in (a.log, a.until_file):
            try:
                if stale_file:
                    Path(stale_file).unlink()  # --until must not match a stale log
            except OSError:
                pass
        code = 1
        early = False
        t0 = time.time()
        try:
            si = subprocess.STARTUPINFO()
            si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            si.wShowWindow = 0
            p = subprocess.Popen(cmd, startupinfo=si, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            end = time.time() + a.timeout
            seen = False
            while time.time() < end:
                if p.poll() is not None:
                    code = p.returncode
                    break
                if a.until:
                    try:
                        if a.until in Path(a.until_file or a.log).read_text(errors='replace'):
                            seen = True
                            break
                    except OSError:
                        pass
                time.sleep(0.5)
            if p.poll() is None:
                # polite close first so PCSX2 flushes its log file
                subprocess.run(['taskkill', '/PID', str(p.pid)], capture_output=True)
                try:
                    p.wait(8)
                except subprocess.TimeoutExpired:
                    pass
            if p.poll() is None:
                p.terminate()
                try:
                    p.wait(10)
                except subprocess.TimeoutExpired:
                    p.kill()
            if seen:
                code = 0
            elif time.time() >= end:
                code = 2  # a forced close is not successful completion
            elif p.poll() is not None:
                code = p.returncode
            if not seen and time.time() - t0 < 25:
                logtext = Path(a.log).read_text(errors='replace') if Path(a.log).exists() else ''
                early = 'swap chain' in logtext.lower() or not logtext.strip()
        finally:
            try:
                lock.unlink()
            except OSError:
                pass
        print(f'pcsx2 slot {exe} attempt {attempt + 1} code {code}', flush=True)
        if not early:
            break
        time.sleep(5)
    text = Path(a.log).read_text(errors='replace') if Path(a.log).exists() else ''
    if a.marker:
        print('\n'.join(l for l in text.splitlines() if a.marker in l))
    print('pcsx2 exit code', code, '(2 = timeout)')
    return code


if __name__ == '__main__':
    sys.exit(main())
