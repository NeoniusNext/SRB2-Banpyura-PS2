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
    # the test harness (opt_run.py) writes fpscap "35" into reference.cfg; the game's default is "Match refresh rate" (60 Hz with interpolation in Hardware): that is what a player has
    cli = ps2('cli', EMU1, cli_args, files={'pad.txt': pad(*crosses(200, 600, 60)) + ',' + walk(1, 700, walkto or tics * 3, seed=2)},
              cfg=CFG_SYNC + ('fpscap "Match refresh rate"\n' if renderer == 'Hardware' else '') + cfg, start=8)
    write_(name, timeout, [srv, cli], tics)


def write_(name, timeout, nodes, tics):
    mine(name, {'timeout': timeout, 'nodes': nodes, 'abort_on': [{'node': 'srv', 'text': 'left the game (Connection timeout)'}],
                'until': [{'node': 'cli', 'text': 'NETSYNC gametic=', 'min': tics}, {'node': 'srv', 'text': 'NETSYNC gametic=', 'min': tics}], 'grace': 3})


lat_pair('lat-sw', 'Software')
lat_pair('lat-hw', 'Hardware')
lat_pair('lat-sw-trace', 'Software', tics=900, extra_cli=['-netlattrace'])
lat_pair('lat-sw-noearly', 'Software', extra_cli=['-netnoearly'])
lat_pair('lat-hw-noearly', 'Hardware', extra_cli=['-netnoearly'])

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


# ---- impairments: tools/ps2/udp_netem.py between the PS2 client and the PC server (loss, delay, "cable pulled" windows) ----
NETEM_PORT = 5031


def netem(name, args, start=0):
    return {'id': 'netem', 'kind': 'pc', 'exe': S.PY, 'cwd': S.BASE, 'start': start,
            'args': ['-u', (ROOT / 'tools/ps2/udp_netem.py').as_posix(), '--listen', str(NETEM_PORT), '--bind', '0.0.0.0', '--server', f'{H}:5029',
                     '--log', (ROOT / S.BASE / f'run/{name}/netem.jsonl').as_posix()] + args}


def impaired(name, netem_args, tics=2100, extra_cli=None, cmds='', long_timeout=False, until=None, abort=True, timeout=1200, renderer='Software', pad_to=None):
    srv = pcsrv(extra=['-netlat'], start=0, longto=not long_timeout)
    cfg = ('' if long_timeout else CFG_SYNC)
    if long_timeout:
        cfg = 'resynchattempts "0"\nblamecfail "On"\n'  # the default nettimeout (350 tics): the client must give up by itself when the line is gone
    cli_args = ['-skipintro', '-connect', f'{H}:{NETEM_PORT}', '-netsync', '-netdebug', '-netlat', '-padscript', 'file:pad.txt'] + (['-renderer', renderer] if renderer != 'Software' else []) + (extra_cli or [])
    files = {'pad.txt': pad(*crosses(200, 600, 60)) + ',' + walk(1, 700, pad_to or tics * 3, seed=2)}
    if cmds:
        files['cmd.txt'] = cmds
        cli_args += ['-netcmd', 'file:cmd.txt']
    cli = ps2('cli', EMU1, cli_args, files=files, cfg=cfg + ('fpscap "Match refresh rate"\n' if renderer == 'Hardware' else ''), start=8)
    spec = {'timeout': timeout, 'nodes': [netem(name, netem_args), srv, cli],
            'until': until or [{'node': 'cli', 'text': 'NETSYNC gametic=', 'min': tics}, {'node': 'srv', 'text': 'NETSYNC gametic=', 'min': tics}], 'grace': 3}
    if abort:
        spec['abort_on'] = [{'node': 'srv', 'text': 'left the game (Connection timeout)'}]
    mine(name, spec)


impaired('loss-5', ['--loss', '5', '--delay', '15', '--jitter', '5'])
impaired('loss-15', ['--loss', '15', '--delay', '30', '--jitter', '10'])
impaired('loss-30', ['--loss', '30', '--delay', '40', '--jitter', '15'], tics=1400)
impaired('delay-150', ['--delay', '150', '--jitter', '20'], tics=1400)
# the cable is pulled for 4 s (shorter than the 10 s time-out): the game goes on, the state stays equal
impaired('cable-short', ['--schedule', '40:blackhole=4'], tics=2800)
# the cable is pulled for 25 s: the client gives up (server timeout -> title), no hang; then it connects again ("connect" typed at displayed frame 3000)
impaired('cable-long', ['--schedule', '40:blackhole=25'], long_timeout=True, abort=False, cmds=f'3000:connect {H}:{NETEM_PORT}', tics=0,
         until=[{'node': 'cli', 'text': 'PS2 net: server timeout'}, {'node': 'cli', 'text': 'NETSYNC gametic=', 'min': 4200}], timeout=1500)

# ---- soak: 10 minutes of game time (21000 tics) with both ends walking, the state hash compared afterwards (net_batch.py) ----
for _r in ('Software', 'Hardware'):
    lat_pair(f'soak10-{_r[:2].lower()}', _r, tics=21000, timeout=3000, walkto=21000 * 3 + 3000)

if __name__ == '__main__':
    names = [n for n in ARGS_NAMES if not n.startswith('-') and n in MINE]
    for n, s in MINE.items():
        if not names or n in names:
            (S.OUT / f'{n}.json').write_text(json.dumps(s, indent=1))
            print('spec', n)
