"""A PC dedicated server started WITHOUT an explicit master server room must not register anywhere (OPT10-X, after the menu-browse incident).

usage: python3 tools/ps2/dedicated_noreg_test.py [--seconds 25]
Starts the network-test PC engine (build/pc-net) as `-dedicated -server -warp MAP01` exactly like the soak scenarios do (no -room, no masterserver argument), with
the safety environment of net_session.py (no proxy variables, masterserver = the dead local port 127.0.0.1:9 in config.cfg), waits, stops it and checks the log:
no "Registering this server", no "HMS: connecting" line. The reason is in the source (src/netcode/d_clisrv.c SV_StartSinglePlayerServer/D_ClientServerInit: RegisterServer only
if masterserver_room_id > 0; its default is -1, src/netcode/mserv.c); this test shows it for the real binary. Nothing leaves the container.
"""
import argparse
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/ps2'))
import net_env  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--seconds', type=float, default=25)
    a = ap.parse_args()
    home = ROOT / 'build/opt10-x/noreg-home'
    (home / '.srb2').mkdir(parents=True, exist_ok=True)
    for name in ('config.cfg', 'dconfig.cfg'):  # the guard of net_session.py (not an explicit room); a dedicated server reads dconfig.cfg
        (home / '.srb2' / name).write_text('masterserver "http://127.0.0.1:9/MS/0"\n')
    env = {k: v for k, v in os.environ.items() if k.lower() not in ('https_proxy', 'http_proxy', 'all_proxy')}
    env.update(SRB2WADDIR='/opt/srb2-assets', SDL_AUDIODRIVER='dummy')
    log = home / 'out.txt'
    with open(log, 'wb') as f:
        p = subprocess.Popen([str(ROOT / net_env.pc_exe()), '-dedicated', '-server', '-nomusic', '-nosound', '-home', str(home), '-warp', 'MAP01'], cwd=str(home), stdout=f,
                             stderr=subprocess.STDOUT, env=env, stdin=subprocess.DEVNULL, start_new_session=True)
        time.sleep(a.seconds)
        os.killpg(os.getpgid(p.pid), signal.SIGINT)
        time.sleep(2)
        try:
            os.killpg(os.getpgid(p.pid), signal.SIGKILL)
        except OSError:
            pass
    text = log.read_text(errors='replace')
    bad = [l for l in text.splitlines() if 'Registering this server' in l or 'HMS:' in l or 'master server' in l.lower()]
    started = 'Starting Server' in text and 'Entering main game loop' in text
    print(f'server started: {started}; master-server lines: {len(bad)}')
    for l in bad[:5]:
        print('  ', l)
    ok = started and not bad
    print('OK' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
