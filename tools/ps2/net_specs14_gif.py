"""OPT14 GIF scenarios for tools/ps2/net_session.py (docs/GATES/g1/opt14-GIF.md): a PS2 client connects to a PC dedicated server and the engine log is searched for
"Movie mode enabled" (GIF/aPNG/screenshot recording that nobody asked for).

usage: python3 tools/ps2/net_specs14_gif.py [--elf ELF] [--base build/opt14-gif] [name ...]
Writes <base>/specs/<name>.json. Run: python3 tools/ps2/net_session.py <base>/specs/<name>.json
Scenarios (each in both renderers: -sw / -hw):
  gif-conn-X    -connect HOST on the command line, Cross on the join screens (server info, file list)
  gif-osk-X     Multiplayer > "Specify server address" > on-screen keyboard > Start > the join screens (what a person does)
  gif-browse-X  Multiplayer > server browser (mock master server on the container) > room > list > the PC server > join
  gif-rec-X     like gif-conn-X, then the console commands startmovie / stopmovie (explicit recording must still work: "Movie mode enabled" appears exactly then)
The PC node is the dedicated server of build/pc-net. The master server is the MOCK of the container (tools/ps2/mock_masterserver.py); ds.ms.srb2.org is never contacted.
"""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import net_specs9 as S  # noqa: E402  (parses --elf/--base/names itself)

S.net_env.PAK = 'build/pak2'
pad, crosses, pcsrv, ps2, write, mock, MSURL, H, EMU1 = S.pad, S.crosses, S.pcsrv, S.ps2, S.write, S.mock, S.MSURL, S.H, S.EMU1
_osk, _end = S._osk, S._end
SPECS = S.SPECS
MINE = {}
RENDERERS = (('sw', 'Software'), ('hw', 'Hardware'))
UNTIL = [{'node': 'cli', 'text': 'NETSYNC gametic=', 'min': 700}, {'node': 'srv', 'text': 'NETSYNC gametic=', 'min': 700}]


def mine(name, spec):
    spec.setdefault('pak', 'build/pak2')
    write(name, spec)
    SPECS[name]['pak'] = 'build/pak2'
    MINE[name] = SPECS[name]


def pcsrv_ms(start=3):
    """PC dedicated server that registers on the MOCK master server of the container. NOT pcsrv(ms=True): that one puts masterserver_room_id into config.cfg right behind
    the masterserver line, and MasterServer_OnChange (mserv.c Set_api) sets the URL from another thread, so RegisterServer() can run with the default (REAL) address
    (seen once, 2026-10-10: a registration attempt on ds.ms.srb2.org, stopped only by the missing DNS of the container). Here the room id comes over stdin after 12 s."""
    d = pcsrv(start=start)
    d['masterserver'] = MSURL
    d['cfg'] = 'servername "PC test server"\n'
    d['stdin'] = [{'at': 12, 'text': 'masterserver_room_id 1\n'}]
    return d


def rnd(r):
    return ['-renderer', 'Hardware'] if r == 'Hardware' else []


for tag, r in RENDERERS:
    cfg = 'fpscap "Match refresh rate"\n' if r == 'Hardware' else ''
    # -connect
    mine(f'gif-conn-{tag}', {'timeout': 900, 'nodes': [pcsrv(start=0), ps2('cli', EMU1, ['-skipintro', '-connect', H, '-netsync', '-netdebug', '-padscript', 'file:pad.txt'] + rnd(r),
                                                                   files={'pad.txt': pad(*crosses(200, 2000, 60))}, cfg=S.CFG_SYNC + cfg, start=8)],
                             'until': UNTIL, 'grace': 3})
    # menu + on-screen keyboard
    mine(f'gif-osk-{tag}', {'timeout': 900, 'nodes': [pcsrv(start=0), ps2('cli', EMU1, ['-skipintro', '-netsync', '-netdebug', '-padscript', 'file:pad.txt'] + rnd(r), may_exit=True, start=8,
                                                                  cfg=S.CFG_SYNC + cfg,
                                                                  files={'pad.txt': pad((250, 'start'), (330, 'down'), (400, 'cross'), (520, 'down'), (570, 'triangle'), *_osk,
                                                                                        (_end + 30, 'start'), *crosses(_end + 200, _end + 3000, 100))})],
                            'until': UNTIL, 'grace': 3})
    # server browser on the mock master server
    shots = ','.join(f'f{n}' for n in (700, 1050, 1450))
    mine(f'gif-browse-{tag}', {'timeout': 900, 'nodes': [mock(f'gif-browse-{tag}'), pcsrv_ms(start=3),
                                                         ps2('cli', EMU1, ['-skipintro', '-netdebug', '-netsync', '-padscript', 'file:pad.txt'] + rnd(r),
                                                             files={'pad.txt': pad((250, 'start'), (330, 'down'), (400, 'cross'), (540, 'cross'), (760, 'down'), (820, 'cross'),
                                                                                   (950, 'down'), (970, 'down'), (990, 'down'), (1010, 'down'), (1100, 'cross'), *crosses(1300, 3000, 100))},
                                                             cfg=f'masterserver "{MSURL}"\n' + S.CFG_SYNC + cfg, start=10, may_exit=True)],
                               'until': UNTIL, 'grace': 3})
    # explicit start/stop of the recording on the console
    mine(f'gif-rec-{tag}', {'timeout': 900, 'nodes': [pcsrv(start=0), ps2('cli', EMU1, ['-skipintro', '-connect', H, '-netsync', '-netdebug', '-padscript', 'file:pad.txt', '-netcmd', 'file:cmd.txt'] + rnd(r),
                                                                  files={'pad.txt': pad(*crosses(200, 1000, 60)), 'cmd.txt': '1300:startmovie|1500:stopmovie'}, cfg=S.CFG_SYNC + cfg, start=8)],
                            'until': [{'node': 'cli', 'text': 'Movie mode disabled'}] + UNTIL, 'grace': 3})



def fuzz(seed, a, b, buttons, gap=(4, 14), hold=(2, 20), sticks=True):
    """random presses of `buttons` (every `gap` polls, held `hold` polls) and random stick positions between polls a and b: a person who tries every button"""
    import random
    r = random.Random(seed)
    out = []
    t = a
    while t < b:
        btn = r.choice(buttons)
        h = r.randint(*hold)
        out += [(t, f'{t}:1:+{btn}'), (t + h, f'{t + h}:1:-{btn}')]
        if sticks and r.random() < 0.3:
            ax = r.choice(('lx', 'ly', 'rx', 'ry'))
            v = r.choice((0, 64, 128, 128, 200, 255))
            out.append((t, f'{t}:1:{ax}={v}'))
        t += r.randint(*gap)
    out.append((b, f'{b}:1:lx=128'))
    out.append((b, f'{b}:1:ly=128'))
    return ','.join(s for _, s in sorted(out, key=lambda x: (x[0], x[1])))


SAFE = ['cross', 'square', 'triangle', 'l1', 'r1', 'l2', 'r2', 'select', 'l3', 'r3', 'up', 'down', 'left', 'right']
ALL = SAFE + ['circle', 'start']
for tag, r in RENDERERS:
    cfg = 'fpscap "Match refresh rate"\n' if r == 'Hardware' else ''
    for seed in (1, 2):
        # every button but Circle/Start (they abort a connection) over the connection screens and the first seconds of the game, then every button
        pad_text = fuzz(seed, 150, 1100, SAFE) + ',' + pad(*crosses(1100, 1200, 50)) + ',' + fuzz(seed + 10, 1200, 2600, ALL)
        mine(f'gif-fuzz{seed}-{tag}', {'timeout': 1200, 'nodes': [pcsrv(start=0), ps2('cli', EMU1, ['-skipintro', '-connect', H, '-netsync', '-netdebug', '-padscript', 'file:pad.txt'] + rnd(r),
                                                                          files={'pad.txt': pad_text}, cfg=S.CFG_SYNC + cfg, start=8, may_exit=True)],
                                       'until': [{'node': 'cli', 'text': 'NETSYNC gametic=', 'min': 1400}], 'grace': 3})

# every button at random from the title screen on, a master server (mock) with the PC server listed, no -connect: menus, server browser, OSK, join screens, game
for tag, r in RENDERERS:
    cfg = 'fpscap "Match refresh rate"\n' if r == 'Hardware' else ''
    for seed in (1, 2, 3):
        mine(f'gif-mash{seed}-{tag}', {'timeout': 420, 'nodes': [mock(f'gif-mash{seed}-{tag}'), pcsrv_ms(start=3),
                                                                 ps2('cli', EMU1, ['-skipintro', '-netdebug', '-netsync', '-padscript', 'file:pad.txt'] + rnd(r),
                                                                     files={'pad.txt': fuzz(seed + 100, 250, 4500, ALL, gap=(6, 26), hold=(2, 12))},
                                                                     cfg=f'masterserver "{MSURL}"\n' + S.CFG_SYNC + cfg, start=10, may_exit=True)],
                                              'until': [{'node': 'cli', 'text': 'NO SUCH TEXT'}], 'grace': 1, 'expect_timeout': True})

if __name__ == '__main__':
    names = [n for n in sys.argv[1:] if not n.startswith('-') and n in MINE]
    for n, s in MINE.items():
        if not names or n in names:
            (S.OUT / f'{n}.json').write_text(json.dumps(s, indent=1))
            print('spec', n)
