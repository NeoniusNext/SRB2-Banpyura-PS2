"""OPT9/OPT10 network scenarios for tools/ps2/net_session.py (agent N, docs/GATES/g1/opt9-N.md; ported to Linux by X: docs/GATES/g1/opt10-X.md).

usage: python3 tools/ps2/net_specs9.py [--elf ELF] [--base build/opt10-x] [name ...]
Writes <base>/specs/<name>.json for the scenarios below (all of them without names). Run: python tools/ps2/net_session.py <base>/specs/<name>.json
(net_session.py --retries N repeats a session whose emulator died at its start).

The menu scenarios drive the console with a pad script (-padscript, src/ps2/i_joy.c): the numbers are poll counts (displayed frames). Buttons act as
the PS2 port maps them: start = open the menu / Enter on the title, cross = Enter, circle = Escape, d-pad = arrows. Screenshots: -vidshot fN.
"""
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import net_env  # noqa: E402  (Linux locations: emulator copies, PC engine, packs)

ROOT = Path(__file__).resolve().parents[2]
ap = argparse.ArgumentParser()
ap.add_argument('--elf', default='build/out/SRB2.ELF')
ap.add_argument('--base', default=net_env.BASE)
ap.add_argument('names', nargs='*')
ARGS = ap.parse_args()
ELF, BASE = ARGS.elf, ARGS.base
OUT = ROOT / BASE / 'specs'
OUT.mkdir(parents=True, exist_ok=True)
PY = Path(sys.executable).as_posix()
PC = net_env.pc_exe()  # build/pc-net: this tree with -DNETSYNC_DIAG (-netsync prints NETSYNC lines and presses ENTER on the join screens)
PCDIR = BASE + '/pc'
(ROOT / PCDIR).mkdir(parents=True, exist_ok=True)
HOME1 = (ROOT / BASE / 'pc-home1').as_posix()  # -home DIR: the engine's data folder is DIR/.srb2
HOME2 = (ROOT / BASE / 'pc-home2').as_posix()
EMU1 = net_env.EMU1
EMU2 = net_env.EMU2
H = '{HOSTIP}'
MSURL = f'http://{H}:8090/MS/0'
SPECS = {}


def press(frame, btn, hold=6, port=1):
    return [f'{frame}:{port}:+{btn}', f'{frame + hold}:{port}:-{btn}']


def pad(*items):
    """items: (frame, button) or (frame, button, hold) -> script text"""
    out = []
    for it in items:
        out += press(*it)
    return ','.join(sorted(out, key=lambda s: (int(s.split(':')[0]), s)))


def walk(port, a, b, seed=1):
    import subprocess
    return subprocess.run([sys.executable, str(ROOT / 'tools/ps2/gen_padscript.py'), str(port), str(a), str(b), '--seed', str(seed)],
                          capture_output=True, text=True).stdout.strip()


def punches(port, start, end, step=60):
    return '|'.join(f'{f}:punch {H} {port}' for f in range(start, end, step))


def mock(name, extra=None):
    return {'id': 'mock', 'kind': 'pc', 'exe': PY, 'cwd': BASE,
            'args': ['-u', (ROOT / 'tools/ps2/mock_masterserver.py').as_posix(), '--port', '8090', '--bind', '0.0.0.0', '--log',
                     (ROOT / BASE / f'run/{name}/mock.jsonl').as_posix()] + (extra or []), 'start': 0}


def pcsrv(extra=None, home=HOME1, start=0, ms=False, warp='MAP01', longto=True, **kw):
    a = ['-dedicated', '-server', '-nomusic', '-nosound', '-netsync', '-home', home, '-warp', warp]
    if longto:  # see CFG_SYNC: a slow PS2 client must not be dropped while its level loads (the timeout scenarios pass longto=False)
        a += ['+nettimeout', '2100', '+jointimeout', '2100']
    if ms:
        a += ['-room', '1', '+masterserver', MSURL, '+servername', 'PC test server']
    d = {'id': 'srv', 'kind': 'pc', 'exe': PC, 'cwd': PCDIR, 'args': a + (extra or []), 'start': start}
    d.update(kw)
    return d


def ps2(nid, emu, args, files=None, cfg='', **kw):
    d = {'id': nid, 'kind': 'ps2', 'emu': emu, 'elf': ELF, 'args': args, 'files': files or {}, 'cfg': cfg}
    d.update(kw)
    return d


def write(name, spec):
    spec.setdefault('out', BASE + '/run')
    spec.setdefault('pak', net_env.PAK)
    spec['name'] = name
    SPECS[name] = spec


# 1. the whole menu path of a client: Multiplayer > Server browser > room > list (from the mock) > join the PC server that is listed there
shots = ','.join(f'f{n}' for n in (700, 900, 1050, 1250, 1450, 1800))
write('menu-browse', {
    'timeout': 600,
    'nodes': [mock('menu-browse'), pcsrv(ms=True, start=3),
              ps2('cli', EMU1, ['-skipintro', '-netdebug', '-netsync', '-padscript', 'file:pad.txt', '-vidshot', shots],
                  files={'pad.txt': pad((250, 'start'), (330, 'down'), (400, 'cross'), (540, 'cross'), (760, 'down'), (820, 'cross'),  # list of room 1
                                        (950, 'down'), (970, 'down'), (990, 'down'), (1010, 'down'), (1100, 'cross'),                       # the server line
                                        (1300, 'cross'))},                                                                                 # "ENTER - Join"
                  cfg=f'masterserver "{MSURL}"\n', start=10, may_exit=True)],
    'until': [{'node': 'cli', 'text': 'VIDSHOT COMPLETE'}], 'grace': 2})

# 2. the REAL master server, READ ONLY (HTTP GET: versions, rooms, servers). The container reaches ds.ms.srb2.org only over HTTPS through its egress proxy and the
# PS2 speaks plain HTTP, so the PS2 asks tools/ps2/ms_relay.py (own port 8092) which forwards nothing but GET of rooms/servers/versions to the real server and
# refuses everything else (no registration can reach it). The Room menu of the server browser is opened (nothing is chosen there, so no listed server is
# contacted) and "listserv" prints the raw list in the console. Nothing is registered; no server of the real list is contacted.
def relay(name):
    return {'id': 'relay', 'kind': 'pc', 'exe': PY, 'cwd': BASE,
            'args': ['-u', (ROOT / 'tools/ps2/ms_relay.py').as_posix(), '--port', '8092', '--bind', '0.0.0.0', '--log', (ROOT / BASE / f'run/{name}/relay.jsonl').as_posix()], 'start': 0}


write('real-ms-read', {
    'timeout': 500,
    'nodes': [relay('real-ms-read'),
              ps2('cli', EMU1, ['-skipintro', '-netdebug', '-padscript', 'file:pad.txt', '-netcmd', 'file:cmd.txt', '-vidshot', 'f800,f1000'],
                  files={'pad.txt': pad((250, 'start'), (330, 'down'), (400, 'cross'), (540, 'cross')), 'cmd.txt': '900:listserv'},
                  cfg=f'masterserver "http://{H}:8092/MS/0"\nmasterserver_debug "On"\n', may_exit=True, start=2)],
    'until': [{'node': 'cli', 'text': 'VIDSHOT COMPLETE'}], 'grace': 2})

# 3. add-ons from a PC server: the client has none, the server loads -file ... (the file list reaches the joiner with the server info); the join
# screens need Cross (server info > ENTER-Join, file list > ENTER-download): a Cross every 100 polls until the level is on screen.
ADDONS = (ROOT / BASE / 'addons').as_posix()


def crosses(first, last, step=100):
    return [(f, 'cross') for f in range(first, last, step)]


def httpsrc(name, mode, start=0):
    return {'id': 'http', 'kind': 'pc', 'exe': PY, 'cwd': BASE,
            'args': ['-u', (ROOT / 'tools/ps2/http_static.py').as_posix(), '--dir', ADDONS, '--port', '8091', '--bind', '0.0.0.0', '--mode', mode, '--log',
                     (ROOT / BASE / f'run/{name}/http.jsonl').as_posix()], 'start': start}


for name, files, mode in (('addons-udp', ['NSK.pk3'], None), ('addons-http', ['NSK.pk3', 'ZT.pk3'], 'plain'), ('addons-http-chunked', ['NSK.pk3'], 'chunked'),
                          ('addons-http-404', ['NSK.pk3'], 'notfound')):
    srv_extra = ['-file'] + [f'{ADDONS}/{f}' for f in files]
    nodes = []
    if mode:
        nodes.append(httpsrc(name, mode))
        srv_extra += ['+http_source', f'http://{H}:8091']
    nodes.append(pcsrv(extra=srv_extra, start=3))
    nodes.append(ps2('cli', EMU1, ['-skipintro', '-connect', H, '-netsync', '-netdebug', '-padscript', 'file:pad.txt'],
                     files={'pad.txt': pad(*crosses(200, 3000))}, start=10))
    write(name, {'timeout': 900, 'nodes': nodes,
                 'until': [{'node': 'cli', 'text': 'NETSYNC gametic=1400'}, {'node': 'srv', 'text': 'NETSYNC gametic=1400'}], 'grace': 3})

# 4. the on-screen keyboard: Multiplayer > "Specify server address" > Triangle opens the keyboard (src/ps2/ps2_osk.c), the address is typed with the
# D-pad and Cross, Start = Enter. Cursor wraps (10 columns, 5 rows: four of characters and one of commands).
OSK_ROWS = ['1234567890', 'qwertyuiop', 'asdfghjkl.', 'zxcvbnm:/-']


def host_ip():
    import socket
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(('10.255.255.255', 1))
        return s.getsockname()[0]
    except OSError:
        return '127.0.0.1'
    finally:
        s.close()


def osk_steps(text, first, gap=9):
    """pad steps (frame, button, hold) that type text on the keyboard from its start position (0,0); returns (steps, next free frame)"""
    x = y = 0
    f = first
    out = []
    for ch in text:
        ty = next(r for r, row in enumerate(OSK_ROWS) if ch in row)
        tx = OSK_ROWS[ty].index(ch)
        dy = (ty - y) % 5
        for _ in range(min(dy, 5 - dy)):
            out.append((f, 'down' if dy <= 5 - dy else 'up', 2))
            f += gap
        dx = (tx - x) % 10
        for _ in range(min(dx, 10 - dx)):
            out.append((f, 'right' if dx <= 10 - dx else 'left', 2))
            f += gap
        out.append((f, 'cross', 2))
        f += gap
        x, y = tx, ty
    return out, f


_osk, _end = osk_steps(host_ip(), 620)


def osk_spec(shot):
    args = ['-skipintro', '-netsync', '-netdebug', '-padscript', 'file:pad.txt'] + (['-vidshot', f'f{_end - 15},f{_end + 150}'] if shot else [])
    return {'timeout': 600,
            'nodes': [pcsrv(start=0),
                      ps2('cli', EMU1, args, may_exit=True, start=8,
                          files={'pad.txt': pad((250, 'start'), (330, 'down'), (400, 'cross'), (520, 'down'), (570, 'triangle'), *_osk, (_end + 30, 'start'),
                                                *crosses(_end + 200, _end + 3000))})],
            'until': [{'node': 'cli', 'text': 'VIDSHOT COMPLETE'}] if shot else
                     [{'node': 'cli', 'text': 'NETSYNC gametic=1400'}, {'node': 'srv', 'text': 'NETSYNC gametic=1400'}], 'grace': 3}


write('osk-shot', osk_spec(True))
write('osk-connect', osk_spec(False))

# 5. life of a connection: leave and join again, a server that disappears, a client that disappears.
write('reconnect', {
    'timeout': 1200,
    'nodes': [pcsrv(start=0, longto=False),
              ps2('cli', EMU1, ['-skipintro', '-connect', H, '-netsync', '-netdebug', '-padscript', 'file:pad.txt', '-netcmd', 'file:cmd.txt'],
                  files={'pad.txt': pad(*crosses(200, 5000)), 'cmd.txt': f'1500:exitgame|1760:connect {H}'}, start=8)],
    'until': [{'node': 'cli', 'text': 'NETSYNC gametic=3000'}, {'node': 'srv', 'text': 'NETSYNC gametic=3000'}], 'grace': 3})

write('server-kill', {
    'timeout': 900,
    'nodes': [dict(pcsrv(start=0, longto=False), stop_when={'node': 'cli', 'text': 'NETSYNC gametic=700', 'delay': 0}),
              ps2('cli', EMU1, ['-skipintro', '-connect', H, '-netsync', '-netdebug', '-padscript', 'file:pad.txt'],
                  files={'pad.txt': pad(*crosses(200, 5000))}, start=8)],
    'until': [{'node': 'cli', 'text': 'PS2 net: server timeout'}], 'grace': 3})

write('client-kill', {
    'timeout': 900,
    'nodes': [ps2('srv', EMU1, ['-server', '-netsync', '-netdebug', '-padscript', 'file:pad.txt', '-netcmd', 'file:cmd.txt'], map='MAP01',
                  files={'pad.txt': walk(1, 500, 3500), 'cmd.txt': punches(5030, 120, 3600)}),
              dict(id='cli', kind='pc', exe=PC, cwd=PCDIR,
                   args=['-connect', H, '-clientport', '5030', '-nomusic', '-nosound', '-netsync', '-home', HOME2],
                   start_when={'node': 'srv', 'text': 'PS2 net: address', 'delay': 6}, stop_when={'node': 'cli', 'text': 'NETSYNC gametic=700', 'delay': 0})],
    'until': [{'node': 'srv', 'text': 'has left the game'}], 'grace': 3})

# 6. soaks: every pairing for N game tics, both sides walking and jumping (pad scripts / the PC player stands still), the state hash of both
# sides compared with tools/ps2/netsync_compare.py afterwards. extra = console commands/args of the server (gametype, map...).
# OPT10-X: nettimeout/jointimeout 2100 tics (the cvar maximum) (the default is 350 = 10 s): in the Linux container six agents share four cores and a PS2 client that loads the
# level after the join can stay silent for longer than 10 s of wall time, then the server drops it ("Connection timeout") - not what these runs measure.
# The timeout scenarios (server-kill, client-kill, reconnect) keep the defaults.
CFG_SYNC = 'resynchattempts "0"\nblamecfail "On"\nnettimeout "2100"\njointimeout "2100"\n'  # a desync is a failure, not something to repair quietly


def pair(name, srv, cli, tics, pollsrv=None, pollcli=None, srv_args=None, srv_cmds='', timeout=2400, cli_args=None, cli_extra_cfg=''):
    """srv/cli: 'ps2' or 'pc'. Gametype and map of a PS2 server go through srv_args (-warp/-gametype), of a PC server too."""
    n = pollsrv or tics * 3
    nodes = []
    if srv == 'ps2':
        nodes.append(ps2('srv', EMU1, ['-server', '-netsync', '-netdebug', '-padscript', 'file:pad.txt', '-netcmd', 'file:cmd.txt'] + (srv_args or []),
                         map='MAP01' if not srv_args or '-warp' not in srv_args else None,
                         files={'pad.txt': walk(1, 500, n), 'cmd.txt': (punches(5030, 120, 3000) + ('|' + srv_cmds if srv_cmds else '')) if cli == 'ps2' or cli == 'pc' else srv_cmds},
                         cfg=CFG_SYNC))
    else:
        sa = list(srv_args or [])
        warp = 'MAP01'
        if '-warp' in sa:  # pcsrv() puts -warp itself
            i = sa.index('-warp')
            warp = sa[i + 1]
            del sa[i:i + 2]
        nodes.append(pcsrv(extra=sa, start=0, warp=warp))
    if cli == 'ps2':
        nodes.append(ps2('cli', EMU2 if srv == 'ps2' else EMU1, ['-skipintro', '-connect', H] + (['-clientport', '5030'] if srv == 'ps2' else []) +
                         ['-netsync', '-netdebug', '-padscript', 'file:pad.txt'] + (cli_args or []),
                         files={'pad.txt': pad(*crosses(150, 600, 60)) + ',' + walk(1, 700, pollcli or n, seed=2)}, cfg=CFG_SYNC + cli_extra_cfg,
                         **({'start_when': {'node': 'srv', 'text': 'PS2 net: address', 'delay': 2}} if srv == 'ps2' else {'start': 8})))
    else:
        nodes.append(dict(id='cli', kind='pc', exe=PC, cwd=PCDIR,
                          args=['-connect', H, '-clientport', '5030', '-nomusic', '-nosound', '-netsync', '-home', HOME2] + (cli_args or []),
                          start_when={'node': 'srv', 'text': 'PS2 net: address', 'delay': 6}))
    write(name, {'timeout': timeout, 'nodes': nodes,
                 'until': [{'node': 'srv', 'text': f'NETSYNC gametic={tics}'}, {'node': 'cli', 'text': f'NETSYNC gametic={tics}'}], 'grace': 3})


SOAK = 6545  # a multiple of 35: NETSYNC lines are printed at gametic % TICRATE == 0
pair('soak-ps2srv-ps2cli', 'ps2', 'ps2', SOAK)
pair('soak-pcsrv-ps2cli', 'pc', 'ps2', SOAK)
pair('soak-ps2srv-pccli', 'ps2', 'pc', SOAK)

# 7. game modes (PS2 server + PS2 client; the same with a PC peer where it matters): the server starts the map in the mode with -warp/-gametype.
MODES = [('match', 'MAPM0', 'match'), ('ctf', 'MAPF0', 'ctf'), ('race', 'MAP01', 'race'), ('tag', 'MAPM0', 'tag'), ('coop', 'MAP01', 'coop'),
         ('teammatch', 'MAPM0', 'teammatch')]
for mname, mmap, mtype in MODES:
    pair(f'mode-{mname}-ps2srv-ps2cli', 'ps2', 'ps2', 2100, srv_args=['-warp', mmap, '-gametype', mtype])
    pair(f'mode-{mname}-pcsrv-ps2cli', 'pc', 'ps2', 2100, srv_args=['-warp', mmap, '-gametype', mtype])

# 8. add-ons served by a PS2 host: the file sits in the host: directory of the server node and is loaded with -file; a PC client and a PS2 client fetch it
# over the game connection (UDP). The client's DOWNLOAD folder is emptied first.
for cname, ckind in (('pccli', 'pc'), ('ps2cli', 'ps2')):
    srv = ps2('srv', EMU1, ['-server', '-netsync', '-netdebug', '-padscript', 'file:pad.txt', '-netcmd', 'file:cmd.txt', '-file', 'NSK.pk3', 'ZT.pk3'], map='MAP01',
              files={'pad.txt': walk(1, 500, 6000), 'cmd.txt': punches(5030, 120, 3000)}, cfg=CFG_SYNC,
              copy={'NSK.pk3': (BASE + '/addons/NSK.pk3'), 'ZT.pk3': (BASE + '/addons/ZT.pk3')})
    if ckind == 'pc':
        cli = dict(id='cli', kind='pc', exe=PC, cwd=PCDIR, wipe=[BASE + '/pc-home2/.srb2/DOWNLOAD'],
                   args=['-connect', H, '-clientport', '5030', '-nomusic', '-nosound', '-netsync', '-home', HOME2],
                   start_when={'node': 'srv', 'text': 'PS2 net: address', 'delay': 6})
    else:
        cli = ps2('cli', EMU2, ['-skipintro', '-connect', H, '-clientport', '5030', '-netsync', '-netdebug', '-padscript', 'file:pad.txt'],
                  files={'pad.txt': pad(*crosses(150, 3000, 60))}, cfg=CFG_SYNC, start_when={'node': 'srv', 'text': 'PS2 net: address', 'delay': 2})
    write(f'addons-ps2srv-{cname}', {'timeout': 1500, 'nodes': [srv, cli],
                                    'until': [{'node': 'srv', 'text': 'NETSYNC gametic=2100'}, {'node': 'cli', 'text': 'NETSYNC gametic=2100'}], 'grace': 3})

# 9. a PS2 host started from the menu (Multiplayer > Internet/LAN > Room > Start) registers on the mock master server; a PC client joins it.
HOSTPAD = [(250, 'start'), (330, 'down'), (400, 'cross'),                       # title > main menu > Multiplayer
           (520, 'down'), (560, 'down'), (600, 'down'), (680, 'cross'),         # Internet/LAN...
           (760, 'up'), (820, 'cross'),                                         # Room... (fetches versions and rooms from the master server)
           (1050, 'down'), (1110, 'cross'),                                     # Standard
           (1230, 'up'), (1290, 'cross')]                                       # Start (the cursor wraps from Room... to Start)


def host_spec(shot):
    args = ['-skipintro', '-netsync', '-netdebug', '-padscript', 'file:pad.txt', '-netcmd', 'file:cmd.txt'] + (['-vidshot', 'f700,f1000,f1200,f1350,f1700'] if shot else [])
    nodes = [mock('ps2host-menu' + ('-shot' if shot else '')),
             ps2('srv', EMU1, args, files={'pad.txt': pad(*HOSTPAD), 'cmd.txt': punches(5030, 1300, 4300)}, cfg=f'masterserver "{MSURL}"\n', may_exit=shot, start=2)]
    if not shot:
        nodes.append(dict(id='cli', kind='pc', exe=PC, cwd=PCDIR,
                          args=['-connect', H, '-clientport', '5030', '-nomusic', '-nosound', '-netsync', '-home', HOME2],
                          start_when={'node': 'srv', 'text': 'Master server registration successful', 'delay': 8}))
    return {'timeout': 900, 'nodes': nodes,
            'until': [{'node': 'srv', 'text': 'VIDSHOT COMPLETE'}] if shot else [{'node': 'srv', 'text': 'NETSYNC gametic=1400'}, {'node': 'cli', 'text': 'NETSYNC gametic=1400'}],
            'grace': 3}


write('ps2host-menu-shot', host_spec(True))
write('ps2host-menu', host_spec(False))

# 10. leaving: a PS2 host quits (console "quit") in a match map and in a co-op map, alone: where the I_Error of the shutdown comes from
for qn, qmap, qtype in (('match', 'MAPM0', 'match'), ('coop', 'MAP01', 'coop')):
    write(f'quit-{qn}', {'timeout': 400, 'nodes': [ps2('srv', EMU1, ['-server', '-netdebug', '-warp', qmap, '-gametype', qtype, '-netcmd', 'file:cmd.txt'], files={'cmd.txt': '800:quit'},
                                                      may_exit=True)],
                         'until': [{'node': 'srv', 'text': 'NETCMD frame 800'}], 'grace': 25})

# 11. a master server that does not answer: the PS2 host registers on start (blocking HTTP, no threads) - how long does the game stand still
for bn, burl in (('blackhole', 'http://10.255.255.1:8090/MS/0'), ('refused', f'http://{H}:8099/MS/0')):
    write(f'ms-{bn}', {'timeout': 400, 'nodes': [ps2('srv', EMU1, ['-server', '-netsync', '-netdebug'], map='MAP01',
                                                     cfg=f'masterserver "{burl}"\nmasterserver_room_id "1"\nmasterserver_debug "On"\n')],
                       'until': [{'node': 'srv', 'text': 'NETSYNC gametic=700'}], 'grace': 3})

for qn, qmap, qtype in (('match', 'MAPM0', 'match'), ('coop', 'MAP01', 'coop')):
    write(f'quit-{qn}-2p', {'timeout': 600, 'nodes': [
        ps2('srv', EMU1, ['-server', '-netdebug', '-netsync', '-warp', qmap, '-gametype', qtype, '-netcmd', 'file:cmd.txt', '-padscript', 'file:pad.txt'],
            files={'cmd.txt': punches(5030, 120, 1500) + '|1500:quit', 'pad.txt': walk(1, 500, 3000)}, may_exit=True),
        ps2('cli', EMU2, ['-skipintro', '-connect', H, '-clientport', '5030', '-netsync', '-netdebug', '-padscript', 'file:pad.txt'],
            files={'pad.txt': pad(*crosses(150, 600, 60)) + ',' + walk(1, 700, 3000, seed=2)}, start_when={'node': 'srv', 'text': 'PS2 net: address', 'delay': 2})],
        'until': [{'node': 'srv', 'text': 'NETCMD frame 1500'}], 'grace': 25})

# 12. the hardware renderer in a network game: the PS2 client draws with -renderer Hardware (HUD, connection screens, chat line, score table with the
# D-pad Down held); the server is a PC dedicated server. Screenshots from the client: -vidshot reads the GS frame back.
HWSHOTS = (900, 1150, 1500, 1650, 1850)


def hw_net(name, srv, cli_renderer='Hardware', mode='coop', mmap='MAP01'):
    srv_node = pcsrv(extra=['-gametype', mode], warp=mmap, start=0)
    cli = ps2('cli', EMU1, ['-skipintro', '-connect', H, '-renderer', cli_renderer, '-netsync', '-netdebug', '-padscript', 'file:pad.txt', '-netcmd', 'file:cmd.txt',
                            '-vidshot', ','.join(f'f{n}' for n in HWSHOTS)],
              files={'pad.txt': pad(*crosses(200, 1100, 60), (1750, 'down', 150)), 'cmd.txt': '1500:say Hello from the PS2 in hardware mode'},
              may_exit=True, start=8)
    write(name, {'timeout': 900, 'nodes': [srv_node, cli], 'until': [{'node': 'cli', 'text': 'VIDSHOT COMPLETE'}], 'grace': 2})


hw_net('hw-net-coop', 'pc')
hw_net('sw-net-coop', 'pc', 'Software')
hw_net('hw-net-match', 'pc', mode='match', mmap='MAPM0')

if __name__ == '__main__':
    for n, s in SPECS.items():
        if not ARGS.names or n in ARGS.names:
            (OUT / f'{n}.json').write_text(json.dumps(s, indent=1))
            print('spec', n)
