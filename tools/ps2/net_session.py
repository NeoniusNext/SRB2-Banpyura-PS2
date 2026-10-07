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
Exit code: 0 all conditions met, 2 timeout/condition missing, 3 an emulator died or its network did not start (--retries N starts the session again),
4 an engine log names the real master server (audit), 5 an "abort_on" line appeared (the run cannot succeed any more), 6 an emulator log outgrew the
runaway limit (SRB2_PCSX2_LOG_LIMIT_MB, default 300; not retried), 7 the engine log of a PS2 node did not grow for 300 s ("stall": SECONDS per node): the game is stuck.
"""
import argparse
import json
import re
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
STALL = 300  # seconds an engine log of a running PS2 node may stay unchanged (the engine prints ASTAT/NETSYNC lines all the time): longer = the game is stuck, exit code 7
STARTUP_HANG = 300  # seconds without an engine log and with a pcsx2.log that stays tiny = the emulator hangs in its start-up (a flake: the session is run again)
XVFB = ['xvfb-run', '-a', '-s', '-screen 0 800x600x24']
DEAD_MS = 'http://127.0.0.1:9/MS/0'  # nothing listens on port 9 (discard): the master server URL of a node that must not reach any master server


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
    # PCSX2 rewrites its PCSX2.ini when it exits; an emulator killed in that moment leaves an EMPTY file, and the next start then waits forever in the first-run
    # wizard (seen once: net1, 0 bytes, "Loading config" the last line of pcsx2.log). <copy>/usr/bin/PCSX2.ini.good (made once by hand) is the way back.
    ini = PCSX2_ROOT / name / 'usr/bin/inis/PCSX2.ini'
    good = PCSX2_ROOT / name / 'usr/bin/PCSX2.ini.good'
    try:
        if good.exists() and ini.stat().st_size < 1000:
            shutil.copy2(good, ini)
            print(f'restored the empty {ini} from {good.name}', flush=True)
    except OSError:
        pass
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
        self.log_size = -1
        self.log_changed = None

    def start(self):
        s = self.spec
        env = dict(os.environ)
        for w in s.get('wipe', []):  # directories (relative to the repo) emptied before the node starts: a PC client's DOWNLOAD folder
            shutil.rmtree(ROOT / w, ignore_errors=True)
        for dst, src in s.get('copy', {}).items():  # files copied into the node directory (the host: root of a PS2 node): add-ons for -file
            shutil.copy2(ROOT / src, self.dir / dst)
        if s['kind'] == 'ps2':
            # SAFETY (OPT10-X): the engine's default master server is the REAL one (http://ds.ms.srb2.org/MS/0). A test never registers on it or talks to it:
            # every PS2 node starts with a master server URL of its own (the mock, the read-only relay) or with a dead local port, set in reference.cfg
            # which is read before the server is started.
            cfg = s.get('cfg', '')
            if 'masterserver "' not in cfg:
                cfg = f'masterserver "{DEAD_MS}"\n' + cfg
            opt_run.stage(self.dir, (ROOT / s['elf']).resolve(), (ROOT / self.pak).resolve(), None, cfg)
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
            is_engine = 'python' not in exe.name
            if '-home' in av:  # the engine's data folder is <home>/.srb2 (the engine does not create it itself: "Can't create file .../$$$.sav")
                (Path(av[av.index('-home') + 1]) / '.srb2').mkdir(parents=True, exist_ok=True)
                if is_engine:
                    # SAFETY (OPT10-X): see the PS2 branch. config.cfg is read before "Starting Server" (a "+masterserver" argument is applied too late: the
                    # first version of the menu-browse scenario registered on the real master server that way). Spec keys: "masterserver" (URL) and "cfg" (more lines).
                    # config.cfg is the client's file, a dedicated server reads dconfig.cfg (checked in the log: "Executing .../dconfig.cfg"): both get the lines
                    for cfgname in ('config.cfg', 'dconfig.cfg'):
                        (Path(av[av.index('-home') + 1]) / '.srb2' / cfgname).write_text(f'masterserver "{s.get("masterserver", DEAD_MS)}"\n' + s.get('cfg', ''))
            if is_engine:  # no route out of the container for an engine: the proxy variables are what lets an engine reach the real master server
                for k in [k for k in env if k.lower() in ('https_proxy', 'http_proxy', 'all_proxy')]:
                    del env[k]
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
                    self.proc.wait(25)  # PCSX2 saves PCSX2.ini on its way out: a kill during that write leaves an empty file (see emu_path)
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
                    if n.spec['kind'] == 'ps2':
                        # RUNAWAY GUARD (as run_pcsx2.py): an emulator log that outgrows LOG_LIMIT (a TLB-miss storm wrote 16 GB once) ends the session, the log is truncated
                        try:
                            big = (n.dir / 'pcsx2.log').stat().st_size > run_pcsx2.LOG_LIMIT
                        except OSError:
                            big = False
                        if big:
                            for m in nodes:
                                m.stop()
                            (n.dir / 'pcsx2.log').write_text('RUNAWAY: emulator log exceeded %d MB, session stopped by net_session.py; log truncated\n' % (run_pcsx2.LOG_LIMIT >> 20))
                            result['died'] = n.id
                            result['runaway'] = True
                            print(f'[{time.strftime("%H:%M:%S")}] {n.id}: RUNAWAY emulator log, session stopped', flush=True)
                            raise StopIteration
                    if n.spec['kind'] == 'ps2' and not n.spec.get('may_exit'):
                        # STALL WATCHDOG: soak-ps2srv-pccli once stopped printing at gametic 5215 (the emulator kept running, its CPU thread in the frame limiter, the guest
                        # printed nothing for 7 minutes until the 40 minute session timeout): a node whose engine log does not grow for STALL seconds ends the session (code 7)
                        try:
                            size = (n.dir / 'boot.txt').stat().st_size
                        except OSError:
                            size = -1
                        if size != n.log_size or n.log_changed is None:
                            n.log_size, n.log_changed = size, time.time()
                        elif size >= 0 and time.time() - n.log_changed > n.spec.get('stall', STALL):
                            result['stalled'] = n.id
                            print(f'[{time.strftime("%H:%M:%S")}] {n.id}: the engine log has not grown for {n.spec.get("stall", STALL)} s (last size {size}): the game is stuck', flush=True)
                            raise StopIteration
                    if n.spec['kind'] == 'ps2' and time.time() - n.started > STARTUP_HANG and not (n.dir / 'boot.txt').exists():
                        # an emulator that hangs in its Qt start-up (pcsx2.log stops at "Loading config from ...", 0% CPU for 10 minutes was seen under load): a flake, run again
                        try:
                            small = (n.dir / 'pcsx2.log').stat().st_size < 1500
                        except OSError:
                            small = True
                        if small:
                            result['died'] = n.id
                            print(f'[{time.strftime("%H:%M:%S")}] {n.id}: the emulator hangs at its start (no engine log after {STARTUP_HANG} s)', flush=True)
                            raise StopIteration
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
            for ab in spec.get('abort_on', []):  # {"node", "text"}: a line that says this run cannot succeed any more (a dropped client): end it now, exit code 5
                if ab['text'] in next(n for n in nodes if n.id == ab['node']).text(ab.get('file', '')):
                    result['aborted'] = f"{ab['node']}: {ab['text']}"
                    print(f'[{time.strftime("%H:%M:%S")}] aborted: {result["aborted"]}', flush=True)
                    raise StopIteration
            conds = spec.get('until', [])
            if conds:
                hits = []
                for c in conds:
                    node = next(n for n in nodes if n.id == c['node'])
                    if 'min' in c:  # {"node", "text": "NETSYNC gametic=", "min": 2100}: the largest number after the text is >= min (a client that joins late never prints one exact value)
                        nums = [int(x) for x in re.findall(re.escape(c['text']) + r'(\d+)', node.text(c.get('file', '')))]
                        hit = bool(nums) and max(nums) >= c['min']
                    else:
                        hit = c['text'] in node.text(c.get('file', ''))
                    result['conditions'][f"{c['node']}:{c['text']}" + (f">={c['min']}" if 'min' in c else '')] = hit
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
    # AUDIT (OPT10-X): no engine log may name the real master server (ds.ms.srb2.org) in a request: the only master servers of a test are the mock, the read-only
    # relay (ms_relay.py) and the dead local port. A hit is a failure of the session (exit code 4) whatever else happened.
    hits = []
    for n in nodes:
        for fname in ('out.txt', 'boot.txt'):
            for line in n.text(fname).splitlines():
                if 'ds.ms.srb2.org' in line and ('connecting' in line or 'Registering' in line):
                    hits.append(f'{n.id}/{fname}: {line.strip()[:160]}')
    if hits:
        result['real_master_server_contact'] = hits[:10]
        print('MASTER SERVER AUDIT FAILED: an engine contacted the real master server:', *hits[:5], sep='\n  ', flush=True)
    (out / 'result.json').write_text(json.dumps(result, indent=1))
    print(json.dumps(result), flush=True)
    return 4 if hits else 6 if result.get('runaway') else 7 if result.get('stalled') else 3 if result.get('died') else 5 if result.get('aborted') else 0 if not result['timeout'] and all(result['conditions'].values() or [True]) else 2


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
