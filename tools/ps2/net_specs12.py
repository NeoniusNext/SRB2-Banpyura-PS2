"""OPT12 NET scenarios for tools/ps2/net_session.py (docs/GATES/g1/opt12-NET.md).

usage: python3 tools/ps2/net_specs12.py [--elf ELF] [--base build/opt12-net] [name ...]
Writes <base>/specs/<name>.json. Run: python3 tools/ps2/net_session.py <base>/specs/<name>.json   (or tools/ps2/net_batch.py NAME)
Builds on the helpers of net_specs9.py (pcsrv, ps2, pad, crosses, walk ...). Every latency scenario runs both ends with -netlat (src/netcode/netlat.c) and
-netdebug; tools/ps2/net_lat.py reads the logs (and the host clock stamps <log>.ts that net_session.py writes).
"""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
_argv = sys.argv
ARGS_NAMES = [a for a in _argv[1:]]  # parsed again by net_specs9 (--elf/--base/names)
import net_specs9 as S  # noqa: E402  (parses sys.argv itself; the names given here are scenario names of this file too)

ROOT = S.ROOT
H = S.H
write = S.write
pcsrv = S.pcsrv
ps2 = S.ps2
pad = S.pad
walk = S.walk
crosses = S.crosses
punches = S.punches
EMU1, EMU2 = S.EMU1, S.EMU2
PC, PCDIR, HOME2 = S.PC, S.PCDIR, S.HOME2
CFG_SYNC = S.CFG_SYNC
SPECS = S.SPECS
MINE = {}


def mine(name, spec):
    write(name, spec)
    MINE[name] = SPECS[name]


def lat_pair(name, renderer='Software', tics=2100, extra_cli=None, extra_srv=None, srv_kind='pc', mapname='MAP01', timeout=900, cfg='', walkto=None):
    """PC dedicated server <-> PS2 client; both log latency (-netlat). The PS2 player walks and jumps (pad script)."""
    cli_args = ['-skipintro', '-connect', H, '-netsync', '-netdebug', '-netlat', '-padscript', 'file:pad.txt'] + (['-renderer', renderer] if renderer != 'Software' else []) + (extra_cli or [])
    srv = pcsrv(extra=['-netlat'] + (extra_srv or []), warp=mapname, start=0)
    cli = ps2('cli', EMU1, cli_args, files={'pad.txt': pad(*crosses(200, 600, 60)) + ',' + walk(1, 700, walkto or tics * 3, seed=2)}, cfg=CFG_SYNC + cfg, start=8)
    write_(name, timeout, [srv, cli], tics)


def write_(name, timeout, nodes, tics):
    mine(name, {'timeout': timeout, 'nodes': nodes, 'abort_on': [{'node': 'srv', 'text': 'left the game (Connection timeout)'}],
                'until': [{'node': 'cli', 'text': 'NETSYNC gametic=', 'min': tics}, {'node': 'srv', 'text': 'NETSYNC gametic=', 'min': tics}], 'grace': 3})


lat_pair('lat-sw', 'Software')
lat_pair('lat-hw', 'Hardware')

# Menu path in the hardware renderer: Multiplayer > "Specify server address" > on-screen keyboard (what a person does)
_osk, _end = S._osk, S._end
mine('hw-osk-connect', {'timeout': 700,
                        'nodes': [pcsrv(extra=['-netlat'], start=0),
                                  ps2('cli', EMU1, ['-skipintro', '-netsync', '-netdebug', '-netlat', '-renderer', 'Hardware', '-padscript', 'file:pad.txt'], may_exit=True, start=8,
                                      files={'pad.txt': pad((250, 'start'), (330, 'down'), (400, 'cross'), (520, 'down'), (570, 'triangle'), *_osk, (_end + 30, 'start'),
                                                            *crosses(_end + 200, _end + 3000))})],
                        'until': [{'node': 'cli', 'text': 'NETSYNC gametic=', 'min': 1400}, {'node': 'srv', 'text': 'NETSYNC gametic=', 'min': 1400}], 'grace': 3})

# The command line variant: "connect" typed at the title screen (-netcmd) in the hardware renderer
mine('hw-netcmd-connect', {'timeout': 700,
                           'nodes': [pcsrv(extra=['-netlat'], start=0),
                                     ps2('cli', EMU1, ['-skipintro', '-netsync', '-netdebug', '-netlat', '-renderer', 'Hardware', '-padscript', 'file:pad.txt', '-netcmd', 'file:cmd.txt'],
                                         may_exit=True, start=8, files={'cmd.txt': f'150:connect {H}', 'pad.txt': pad(*crosses(300, 3000, 80))})],
                           'until': [{'node': 'cli', 'text': 'NETSYNC gametic=', 'min': 1400}, {'node': 'srv', 'text': 'NETSYNC gametic=', 'min': 1400}], 'grace': 3})

# The network bring-up alone (no server): "PS2 net: address" must appear in a few seconds; used in a loop to measure how often the bring-up hangs (OPT12 NET-1)
for _r in ('Software', 'Hardware'):
    mine(f'netup-{_r[:2].lower()}', {'timeout': 120, 'nodes': [ps2('cli', EMU1, ['-skipintro', '-connect', H, '-netdebug', '-netlat', '-netthreads', '-netwd', '-renderer', _r], may_exit=True, start=0,
                                                              cfg=CFG_SYNC)],
                                   'until': [{'node': 'cli', 'text': 'PS2 net: address'}], 'grace': 3})

if __name__ == '__main__':
    names = [n for n in ARGS_NAMES if not n.startswith('-') and n in MINE]
    for n, s in MINE.items():
        if not names or n in names:
            (S.OUT / f'{n}.json').write_text(json.dumps(s, indent=1))
            print('spec', n)
