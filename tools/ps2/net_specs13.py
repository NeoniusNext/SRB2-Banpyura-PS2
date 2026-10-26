"""OPT13-IO network scenarios for tools/ps2/net_session.py (docs/GATES/g1/opt13-IO.md): the keep-alive of a client whose game thread loads (RS-09) and the
buffered write of a download (RS-08).

usage: python3 tools/ps2/net_specs13.py [--elf ELF] [--base build/opt12-net] [name ...]
Writes <base>/specs/<name>.json. Run: python3 tools/ps2/net_session.py <base>/specs/<name>.json
Builds on net_specs9.py / net_specs12.py (pcsrv, ps2, pad, crosses, walk ...).

 ka-stall      PC dedicated server with the DEFAULT time-out (nettimeout 350 tics = 10 s), PS2 client; at frame 700 the client's game thread is held for 14 s ("ps2stall 14000",
               what a level load from a bad medium looks like to the server: no packet for 14 s). With the keep-alive of the receive thread the client stays in the game.
 ka-stall-off  the same with -netnokeepalive (the A/B switch): the server drops the node ("Connection timeout"), the run cannot succeed (exit 5).
 ka-ps2srv     PS2 server (default time-out), PC client; the PS2 client role is not involved: a control of the keep-alive of the PC client while the PS2 server loads is not made here.
 dl-buf0/dl-buf1  add-on download of NSK.pk3 + ZT.pk3 with the stream buffer of RS-08 forced off / on (-dlbuf 0|1); the downloaded files are compared with the sources by md5.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import net_specs12 as T  # noqa: E402  (parses sys.argv like net_specs9: --elf/--base/names)
S = T.S

H = S.H
pad, walk, crosses, ps2, pcsrv = S.pad, S.walk, S.crosses, S.ps2, S.pcsrv
EMU1 = S.EMU1
CFG_SYNC = S.CFG_SYNC
MINE13 = {}


def mine(name, spec):
    S.write(name, spec)
    MINE13[name] = S.SPECS[name]


def stall(name, extra_cli, stall_ms=14000, tics=2100):
    srv = pcsrv(extra=['-netlat'], start=0, longto=False)  # the default time-out: 350 tics
    cli = ps2('cli', EMU1, ['-skipintro', '-connect', H, '-netsync', '-netdebug', '-netlat', '-padscript', 'file:pad.txt', '-netcmd', 'file:cmd.txt'] + extra_cli,
              files={'pad.txt': pad(*crosses(200, 600, 60)) + ',' + walk(1, 700, tics * 3, seed=2), 'cmd.txt': f'700:ps2stall {stall_ms}'},
              cfg='resynchattempts "0"\nblamecfail "On"\n', start=8)
    mine(name, {'timeout': 1200, 'nodes': [srv, cli], 'abort_on': [{'node': 'srv', 'text': 'left the game (Connection timeout)'}],
                'until': [{'node': 'cli', 'text': 'PS2STALL over'}, {'node': 'cli', 'text': 'NETSYNC gametic=', 'min': tics}, {'node': 'srv', 'text': 'NETSYNC gametic=', 'min': tics}], 'grace': 3})


stall('ka-stall', [])
stall('ka-stall-off', ['-netnokeepalive'])
stall('ka-stall-short', [], stall_ms=7000, tics=1400)  # control: 7 s is under the time-out, the node stays with or without the keep-alive

ADDONS = S.ADDONS
for _name, _buf in (('dl-buf0', '0'), ('dl-buf1', '1')):
    srv = pcsrv(extra=['-netlat', '-file'] + [f'{ADDONS}/{f}' for f in ('NSK.pk3', 'ZT.pk3')], start=3)
    cli = ps2('cli', EMU1, ['-skipintro', '-connect', H, '-netsync', '-netdebug', '-netlat', '-dlbuf', _buf, '-padscript', 'file:pad.txt'],
              files={'pad.txt': pad(*crosses(200, 4000))}, cfg=CFG_SYNC, start=10)
    mine(_name, {'timeout': 900, 'nodes': [srv, cli], 'until': [{'node': 'cli', 'text': 'NETSYNC gametic=', 'min': 1400}, {'node': 'srv', 'text': 'NETSYNC gametic=', 'min': 1400}], 'grace': 3})

if __name__ == '__main__':
    import json
    names = [n for n in T.ARGS_NAMES if not n.startswith('-') and n in MINE13]
    for n, s in MINE13.items():
        if not names or n in names:
            (S.OUT / f'{n}.json').write_text(json.dumps(s, indent=1))
            print('spec', n)
