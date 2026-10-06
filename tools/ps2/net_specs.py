"""Writes the network test specs (for tools/ps2/net_session.py) into build/opt7-s/specs. python tools/ps2/net_specs.py [ELF]
Names: ps2srv-pccli (PS2 server, PC client), pcsrv-ps2cli (PC server, PS2 client), ps2srv-ps2cli (two PCSX2), ps2host-mock (master server).
PCSX2's Sockets mode passes inbound datagrams only from addresses the guest has sent to: a PS2 server is "punched" towards the client's fixed
port (client: -clientport 5030) before the client joins (console command "punch", src/netcode/i_tcp.c)."""
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ELF = sys.argv[1] if len(sys.argv) > 1 else 'build/opt7-s/s6.ELF'
OUT = ROOT / 'build/opt7-s/specs'
OUT.mkdir(parents=True, exist_ok=True)
PY = Path(sys.executable).as_posix()
PC = 'build/opt7-s/pc/srb2-s7pc.exe'
EMU1 = 'D:/PCSX2-net1/pcsx2-qt.exe'
EMU2 = 'D:/PCSX2-net2/pcsx2-qt.exe'


def pad(port, a, b, seed=1):
    return subprocess.run([sys.executable, str(ROOT / 'tools/ps2/gen_padscript.py'), str(port), str(a), str(b), '--seed', str(seed)],
                          capture_output=True, text=True).stdout.strip()


def punches(port, start, end, step=60):
    return '|'.join(f'{f}:punch {{HOSTIP}} {port}' for f in range(start, end, step))


def write(name, spec):
    spec.setdefault('out', 'build/opt7-s/run')
    spec.setdefault('pak', 'build/opt6-s/pak')
    spec['name'] = name
    (OUT / f'{name}.json').write_text(json.dumps(spec, indent=1))


GAMETICS = 1400  # per-side condition for "N tics"
write('ps2srv-pccli', {
    'timeout': 900,
    'nodes': [
        {'id': 'srv', 'kind': 'ps2', 'emu': EMU1, 'elf': ELF, 'map': 'MAP01',
         'args': ['-server', '-netsync', '-netdebug', '-padscript', 'file:pad.txt', '-netcmd', 'file:cmd.txt'],
         'files': {'pad.txt': pad(1, 500, 3500), 'cmd.txt': punches(5030, 120, 2400)},
         'cfg': 'resynchattempts "0"\nblamecfail "On"\n'},
        {'id': 'cli', 'kind': 'pc', 'exe': PC, 'cwd': 'build/opt7-s/pc', 'logfile': 'latest-log.txt',
         'args': ['-connect', '{HOSTIP}', '-clientport', '5030', '-nomusic', '-nosound', '-netsync', '-home', '../pc-home2'],
         'start_when': {'node': 'srv', 'text': 'PS2 net: address', 'delay': 6}}],
    'until': [{'node': 'srv', 'text': f'NETSYNC gametic={GAMETICS}'}, {'node': 'cli', 'text': f'NETSYNC gametic={GAMETICS}'}], 'grace': 3})

write('pcsrv-ps2cli', {
    'timeout': 900,
    'nodes': [
        {'id': 'srv', 'kind': 'pc', 'exe': PC, 'cwd': 'build/opt7-s/pc', 'logfile': 'latest-log.txt',
         'args': ['-dedicated', '-server', '-nomusic', '-nosound', '-netsync', '-home', '../pc-home1', '-warp', 'MAP01'], 'start': 0},
        {'id': 'cli', 'kind': 'ps2', 'emu': EMU1, 'elf': ELF,
         'args': ['-skipintro', '-connect', '{HOSTIP}', '-netsync', '-netdebug', '-padscript', 'file:pad.txt'],
         'files': {'pad.txt': pad(1, 150, 3500, seed=2)}, 'start': 5}],
    'until': [{'node': 'cli', 'text': f'NETSYNC gametic={GAMETICS}'}, {'node': 'srv', 'text': f'NETSYNC gametic={GAMETICS}'}], 'grace': 3})

write('ps2srv-ps2cli', {
    'timeout': 1200,
    'nodes': [
        {'id': 'srv', 'kind': 'ps2', 'emu': EMU1, 'elf': ELF, 'map': 'MAP01',
         'args': ['-server', '-netsync', '-netdebug', '-padscript', 'file:pad.txt', '-netcmd', 'file:cmd.txt'],
         'files': {'pad.txt': pad(1, 500, 3500), 'cmd.txt': punches(5030, 120, 3000)},
         'cfg': 'resynchattempts "0"\nblamecfail "On"\n'},
        {'id': 'cli', 'kind': 'ps2', 'emu': EMU2, 'elf': ELF,
         'args': ['-skipintro', '-connect', '{HOSTIP}', '-clientport', '5030', '-netsync', '-netdebug', '-padscript', 'file:pad.txt'],
         'files': {'pad.txt': pad(1, 150, 3500, seed=2)},
         'start_when': {'node': 'srv', 'text': 'PS2 net: address', 'delay': 2}}],
    'until': [{'node': 'srv', 'text': f'NETSYNC gametic={GAMETICS}'}, {'node': 'cli', 'text': f'NETSYNC gametic={GAMETICS}'}], 'grace': 3})

write('ps2host-mock', {
    'timeout': 420,
    'nodes': [
        {'id': 'mock', 'kind': 'pc', 'exe': PY, 'cwd': 'build/opt7-s',
         'args': ['-u', (ROOT / 'tools/ps2/mock_masterserver.py').as_posix(), '--port', '8090', '--bind', '0.0.0.0', '--log',
                  (ROOT / 'build/opt7-s/run/ps2host-mock/mock.jsonl').as_posix()], 'start': 0},
        {'id': 'ps2', 'kind': 'ps2', 'emu': EMU1, 'elf': ELF, 'map': 'MAP01', 'start': 2,
         'args': ['-server', '-netdebug', '-netcmd', 'file:cmd.txt'],
         'files': {'cmd.txt': '900:listserv'},
         'cfg': 'masterserver "http://{HOSTIP}:8090/MS/0"\nmasterserver_room_id "1"\nservername "PS2 mock test"\nmasterserver_debug "On"\n'}],
    'until': [{'node': 'ps2', 'text': 'Master server registration successful'}, {'node': 'ps2', 'text': 'NETCMD frame'}], 'grace': 10})
print('specs written to', OUT)
