"""Multi-node network test session: PS2 engine instances in PCSX2 (DEV9 Sockets) and PC SRB2 processes, all under ONE machine-wide net lock.

usage: python3 tools/ps2/net_session.py SPEC.json [--retries N]
Linux (OPT10-X): the emulators are the AppImage copies /opt/pcsx2/net1 and /opt/pcsx2/net2 (portable, [DEV9/Eth] EthEnable=true EthApi=Sockets
EthDevice=eth0, the only interface of the container), each started headless inside its own xvfb-run; PC nodes are the Linux SRB2 binary
(SRB2WADDIR=/opt/srb2-assets, SDL dummy audio, own Xvfb unless "-dedicated"). All processes of a node live in one process group (SIGINT, then SIGKILL).
The lock is NETLOCK of tools/ps2/run_pcsx2.py (acquired once for the whole session and refreshed: the fixed host UDP ports 5029/5030 and the
two net copies are not shared). Spec (JSON):
 { "name": "ps2srv-pccli", "out": "build/opt10-x/run", "timeout": 600, "pak": "build/pak",
   "nodes": [
     {"id": "srv", "kind": "ps2", "emu": "net1", "elf": "build/out/SRB2.ELF", "args": ["-server", "-netsync"], "map": "MAP01",
      "start": 0},
     {"id": "cli", "kind": "pc", "exe": "build/pc-net/bin/.../SRB2", "args": ["-connect", "192.0.2.2"],
      "start_when": {"node": "srv", "text": "PS2 net: address", "delay": 2}, "stdin": [{"at": 30, "text": "map map01\\n"}]} ],
   "until": [{"node": "srv", "text": "NETSYNC gametic=700", "file": "boot.txt"}], "any": false, "grace": 3 }
"emu" is "net1"/"net2" (a directory under $SRB2_PCSX2_ROOT, default /opt/pcsx2) or a path to an AppRun.
"ps2" nodes get <out>/<name>/<id>/ as their host: directory (packs hardlinked, .srb2/reference.cfg, ps2args), the engine log is boot.txt there.
"pc" nodes write stdout+stderr to <out>/<name>/<id>/out.txt (CONS_Printf goes to stderr unbuffered); "stdin" lines are sent at the given seconds
after the node started (dedicated servers); "xvfb": true/false overrides the "needs a display" guess (not "-dedicated" = needs one).
"until" conditions: all (or any, with "any": true) must appear; the session then waits "grace" seconds and stops every node.
Exit code: 0 all conditions met, 2 timeout/condition missing, 3 an emulator died or its network did not start (--retries N starts the session again).
"""
import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/ps2'))
import run_pcsx2  # noqa: E402  (the machine-wide lock)
import opt_run  # noqa: E402  (stage())

PCSX2_ROOT = run_pcsx2.PCSX2_ROOT
ASSETS = os.environ.get('SRB2WADDIR', '/opt/srb2-assets')
XVFB = ['xvfb-run', '-a', '-s', '-screen 0 800x600x24']


def host_ip():
    """the address of the interface that has the default route (no packet is sent)"""
    import socket
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(('10.255.255.255', 1))
        return s.getsockname()[0]
    except OSError:
        return '127.0.0.1'
    finally:
        s.close()


HOSTIP = host_ip()


def sub(x):
    """{HOSTIP} in strings of the spec"""
    if isinstance(x, str):
        return x.replace('{HOSTIP}', HOSTIP)
    if isinstance(x, list):
        return [sub(i) for i in x]
    if isinstance(x, dict):
        return {k: sub(v) for k, v in x.items()}
    return x


def emu_path(name):
    """'net1' -> /opt/pcsx2/net1/AppRun; an existing path is used as is"""
    p = Path(name)
    if p.exists() and p.is_file():
        return str(p)
    q = PCSX2_ROOT / name / 'AppRun'
    if not q.exists():
        raise SystemExit(f'no emulator copy {name!r} ({q})')
    return str(q)


def killgroup(p, sig):
    try:
        os.killpg(os.getpgid(p.pid), sig)
    except (ProcessLookupError, PermissionError, OSError):
        pass


def refresh_lock(stop):
    while not stop.wait(30):
        try:
            os.utime(run_pcsx2.NETLOCK)
        except OSError:
            pass


class Node:
    def __init__(self, spec, rundir, pak):
        self.spec = spec
        self.id = spec['id']
        self.dir = rundir / self.id
        self.proc = None
        self.started = None
        self.stdin_done = set()
        self.dir.mkdir(parents=True, exist_ok=True)
        self.pak = pak
        self.log = None
        self.stopped = False

    def start(self):
        s = self.spec
        env = dict(os.environ)
        for w in s.get('wipe', []):  # directories (relative to the repo) emptied before the node starts: a PC client's DOWNLOAD folder
            shutil.rmtree(ROOT / w, ignore_errors=True)
        for dst, src in s.get('copy', {}).items():  # files copied into the node directory (the host: root of a PS2 node): add-ons for -file
            shutil.copy2(ROOT / src, self.dir / dst)
        if s['kind'] == 'ps2':
            opt_run.stage(self.dir, (ROOT / s['elf']).resolve(), (ROOT / self.pak).resolve(), None, s.get('cfg', ''))
            if not s.get('keep_downloads'):  # a file left by an earlier run would be found by the client and never downloaded
                shutil.rmtree(self.dir / '.srb2' / 'DOWNLOAD', ignore_errors=True)
                (self.dir / '.srb2' / '$$$.sav').unlink(missing_ok=True)
            args = ['-logfile', 'boot.txt', '-config', 'reference.cfg', '-nolog', '-noendtxt']
            if s.get('map'):
                args += ['-skipintro', '-warp', s['map']]
            args += s.get('args', [])
            (self.dir / 'boot.txt').unlink(missing_ok=True)
            for fname, content in s.get('files', {}).items():  # extra files in the engine's HOME (= the node directory): -padscript file:NAME, -netcmd file:NAME
                (self.dir / fname).write_text(content, encoding='utf-8')
            # PCSX2 -gameargs is cut at ~128 characters and passes <= 16 arguments: everything after "-logfile boot.txt" goes to <node>/ps2args (opt_run.py)
            (self.dir / 'ps2args').write_text('\n'.join(args[2:]) + '\n')
            cmd = XVFB + ['env', 'QT_QPA_PLATFORM=xcb', 'LC_ALL=C.UTF-8', emu_path(s['emu']), '-portable', '-batch', '-nogui', '-fastboot',
                          '-elf', str((self.dir / 'SRB2.ELF').resolve()), '-logfile', str((self.dir / 'pcsx2.log').resolve()), '-gameargs', ' '.join(args[:2])]
            self.proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env, start_new_session=True)
        else:
            exe = Path(s['exe'])
            exe = exe if exe.is_absolute() else ROOT / exe
            cwd = (ROOT / s['cwd']).resolve() if s.get('cwd') else self.dir
            cwd.mkdir(parents=True, exist_ok=True)
            av = s.get('args', [])
            if '-home' in av:  # the engine's data folder is <home>/.srb2 (the engine does not create it itself: "Can't create file .../$$$.sav")
                (Path(av[av.index('-home') + 1]) / '.srb2').mkdir(parents=True, exist_ok=True)
            self.log = open(self.dir / 'out.txt', 'wb')
            args = [str(exe)] + s.get('args', [])
            env.setdefault('SRB2WADDIR', ASSETS)
            env.setdefault('SDL_AUDIODRIVER', 'dummy')
            env.setdefault('LIBGL_ALWAYS_SOFTWARE', '1')
            env.update(s.get('env', {}))
            need_x = s.get('xvfb', '-dedicated' not in s.get('args', []) and 'python' not in exe.name)
            if need_x:
                args = XVFB + args
            self.proc = subprocess.Popen(args, cwd=str(cwd), stdin=subprocess.PIPE if s.get('stdin') else subprocess.DEVNULL,
                                         stdout=self.log, stderr=subprocess.STDOUT, env=env, start_new_session=True)
        self.started = time.time()
        print(f'[{time.strftime("%H:%M:%S")}] started {self.id} ({s["kind"]}) pid {self.proc.pid}', flush=True)

    def feed(self):
        for i, item in enumerate(self.spec.get('stdin', [])):
            if i not in self.stdin_done and time.time() - self.started >= item['at'] and self.proc.poll() is None:
                try:
                    self.proc.stdin.write(item['text'].encode())
                    self.proc.stdin.flush()
                except OSError:
                    pass
                self.stdin_done.add(i)
                print(f'[{time.strftime("%H:%M:%S")}] {self.id} stdin: {item["text"].strip()}', flush=True)

    def text(self, fname):
        p = self.dir / (fname or ('boot.txt' if self.spec['kind'] == 'ps2' else 'out.txt'))
        try:
            return p.read_text(errors='replace')
        except OSError:
            return ''

    def stop(self):
        if self.proc:
            if self.proc.poll() is None:
                killgroup(self.proc, signal.SIGINT)  # polite first: PCSX2 flushes its log, the engine runs I_Quit
                try:
                    self.proc.wait(8)
                except subprocess.TimeoutExpired:
                    pass
            killgroup(self.proc, signal.SIGKILL)  # leftovers of the xvfb-run tree (own session: never touches other processes)
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                pass
        if self.log:
            self.log.close()
            self.log = None


def run_session(spec):
    out = (ROOT / spec.get('out', 'build/opt10-x/run')).resolve() / spec['name']
    out.mkdir(parents=True, exist_ok=True)
    pak = spec.get('pak', 'build/pak')
    nodes = [Node(n, out, pak) for n in spec['nodes']]
    for n in nodes:  # logs of an earlier run of this scenario must not satisfy a wait condition before the node has started
        try:
            for stale in ('boot.txt', 'out.txt', 'pcsx2.log'):
                (n.dir / stale).unlink(missing_ok=True)
        except OSError as e:
            print(f'cannot remove an old log ({e}); the condition may be satisfied by it', flush=True)
    run_pcsx2.acquire(spec.get('lock_wait', 3600), run_pcsx2.NETLOCK)  # own lock (net1/net2 copies, fixed host ports)
    stop = threading.Event()
    threading.Thread(target=refresh_lock, args=(stop,), daemon=True).start()
    t0 = time.time()
    result = {'name': spec['name'], 'conditions': {}, 'timeout': False}
    try:
        pending = sorted(nodes, key=lambda n: n.spec.get('start', 0))
        (out / 'STOP').unlink(missing_ok=True)
        while time.time() - t0 < spec.get('timeout', 600):
            if (out / 'STOP').exists():  # touch <out>/<name>/STOP ends the session cleanly (nodes stopped, lock released)
                result['stopped'] = True
                break
            for n in list(pending):
                w = n.spec.get('start_when')
                if w:
                    src = next(x for x in nodes if x.id == w['node'])
                    ok = src.started and w['text'] in src.text(w.get('file', ''))
                else:
                    ok = time.time() - t0 >= n.spec.get('start', 0)
                if ok:
                    if w and w.get('delay'):
                        time.sleep(w['delay'])
                    n.start()
                    pending.remove(n)
            for n in nodes:
                sw = n.spec.get('stop_when')  # {"node": id, "text": "...", "delay": s}: stop THIS node when that text is in the other node's log (kill a server on purpose)
                if n.started and sw and not n.stopped and sw['text'] in next(x for x in nodes if x.id == sw['node']).text(sw.get('file', '')):
                    time.sleep(sw.get('delay', 0))
                    print(f'[{time.strftime("%H:%M:%S")}] stopping {n.id} on purpose', flush=True)
                    n.stop()
                    n.stopped = True
                if n.started and not n.stopped:
                    n.feed()
                    if n.spec['kind'] == 'ps2' and not n.spec.get('may_fail_net') and 'PS2 net: the network drivers did not start' in n.text(''):
                        # PCSX2's DEV9 sometimes cannot open its host adapter ("Socket: Failed to get MAC address for adapter"): an emulator start-up flake, run again
                        result['died'] = n.id
                        print(f'[{time.strftime("%H:%M:%S")}] {n.id}: the emulator network did not start (DEV9)', flush=True)
                        raise StopIteration
                    if n.spec['kind'] == 'ps2' and n.proc.poll() is not None and not n.spec.get('may_exit'):
                        # PCSX2 left before the session ended (e.g. "Failed to create swap chain" at its start): nothing more can happen, report it
                        result['died'] = n.id
                        print(f'[{time.strftime("%H:%M:%S")}] {n.id}: the emulator exited (code {n.proc.returncode}) before the session ended', flush=True)
                        raise StopIteration
            conds = spec.get('until', [])
            if conds:
                hits = []
                for c in conds:
                    node = next(n for n in nodes if n.id == c['node'])
                    hit = c['text'] in node.text(c.get('file', ''))
                    result['conditions'][f"{c['node']}:{c['text']}"] = hit
                    hits.append(hit)
                if (any(hits) if spec.get('any') else all(hits)):
                    time.sleep(spec.get('grace', 3))
                    break
            time.sleep(0.5)
        else:
            result['timeout'] = True
    except StopIteration:
        pass
    finally:
        for n in nodes:
            n.stop()
        stop.set()
        try:
            run_pcsx2.NETLOCK.unlink()
        except OSError:
            pass
    result['seconds'] = round(time.time() - t0, 1)
    (out / 'result.json').write_text(json.dumps(result, indent=1))
    print(json.dumps(result), flush=True)
    return 3 if result.get('died') else 0 if not result['timeout'] and all(result['conditions'].values() or [True]) else 2


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('spec')
    ap.add_argument('--retries', type=int, default=0, help='run the session again (up to N times) when an emulator died at its start / its network did not start')
    a = ap.parse_args()
    spec = sub(json.loads(Path(a.spec).read_text()))
    code = 3
    for attempt in range(a.retries + 1):
        code = run_session(spec)
        if code != 3:
            break
        print(f'session {spec["name"]}: emulator failure, attempt {attempt + 1} of {a.retries + 1}', flush=True)
        time.sleep(3)
    return code


if __name__ == '__main__':
    sys.exit(main())
