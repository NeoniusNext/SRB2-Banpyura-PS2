"""OPT14 LUA network scenarios for tools/ps2/net_session.py (docs/GATES/g1/opt14-LUA.md): a PS2 client joins a PC dedicated server that runs Lua mods.

usage: python3 tools/ps2/net_specs14_lua.py [--elf ELF] [--base build/opt14-net] [name ...]
Writes <base>/specs/<name>.json; run: python3 tools/ps2/net_session.py <base>/specs/<name>.json ; then python3 tools/ps2/lua_net.py <base>/run/<name> compares the LQ lines of the nodes.

 lua-join-dl   the PC server has run lm_net.lua for ~40 s (state built up: tables, marks, Lua fields of mobjs), the PS2 client has not got the file: it downloads it, joins the running game
               (the server sends the savegame: NetVars, Lua fields of mobjs) and plays on. The server then changes a net cvar, runs a Lua command and chats.
 lua-join-pre  the same with the file loaded on the client too (-file), no download.
 lua-sw / lua-hw: the PS2 renderer (the default scenarios use Software).
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
ROOT = S.ROOT
MINE14 = {}
TESTS = ROOT / 'tools/ps2/luatests'


def mine(name, spec):
    spec['pak'] = 'build/pak2'   # the packs of the merged tree (SRP2 v2, TEXC, MODELS)
    S.write(name, spec)
    MINE14[name] = S.SPECS[name]


def scenario(name, mods, renderer='Software', predl=False, tics=4000, joinwait=40, server_cmds=False, warp='MAP01'):
    srv_stdin = []
    if server_cmds:   # seconds after the server started; the client joins at about joinwait + 25
        base = joinwait + 45
        srv_stdin = [{'at': base, 'text': 'lm_scale 17\n'}, {'at': base + 5, 'text': 'lm_cmd one two three\n'}, {'at': base + 10, 'text': 'say !lm hello from the server\n'},
                     {'at': base + 15, 'text': 'lm_scale 5\n'}]
    srv_extra = ['-netlat'] + sum([['-file', (TESTS / m).as_posix()] for m in mods], [])
    srv = pcsrv(extra=srv_extra, start=0, warp=warp)
    srv['stdin'] = srv_stdin
    # a dedicated server does not advance the game while nobody is in it: a PC client joins first and keeps the game (and the mod) running, the PS2 joins later
    pccli = {'id': 'pc', 'kind': 'pc', 'exe': S.PC, 'cwd': S.PCDIR, 'args': ['-connect', H, '-nosound', '-nomusic', '-skipintro', '-netsync', '-home', S.HOME2, '+nettimeout', '2100', '+jointimeout', '2100'] + sum([['-file', (TESTS / m).as_posix()] for m in mods], []),
             'start': 6, 'xvfb': True}
    cli_args = ['-skipintro', '-connect', H, '-netsync', '-netdebug', '-padscript', 'file:pad.txt'] + (['-renderer', renderer] if renderer != 'Software' else [])
    if predl:
        cli_args += sum([['-file', m] for m in mods], [])
    cli = ps2('cli', EMU1, cli_args, files={'pad.txt': pad(*crosses(200, 900, 60)) + ',' + walk(1, 1000, tics * 3, seed=3)},
              cfg=CFG_SYNC + ('fpscap "Match refresh rate"\n' if renderer == 'Hardware' else ''), start=joinwait)
    if predl:
        cli['copy'] = {m: (TESTS / m).as_posix() for m in mods}
    mine(name, {'timeout': 1500, 'nodes': [srv, pccli, cli], 'abort_on': [{'node': 'srv', 'text': 'left the game (Connection timeout)'}],
                'until': [{'node': 'cli', 'text': 'NETSYNC gametic=', 'min': tics}, {'node': 'srv', 'text': 'NETSYNC gametic=', 'min': tics}], 'grace': 3})


def ps2srv(name, mods, tics=3600, joindelay=50, pcfile=False, renderer='Software'):
    """the PS2 is the server (and a player: the pad script walks it), the mod builds up state; a PC client joins joindelay seconds after the PS2 server printed its address and takes the savegame
    that the PS2 wrote (LUA_Archive on the EE, read by the PC)."""
    # (the PC client reaches the PS2 server through the emulator's NAT: the server sends "punch" packets to the client port before the client joins, net_specs.py)
    srv = ps2('srv', EMU1, ['-server', '-netsync', '-netdebug', '-padscript', 'file:pad.txt', '-netcmd', 'file:cmd.txt'] + sum([['-file', m] for m in mods], []) + (['-renderer', renderer] if renderer != 'Software' else []),
              files={'pad.txt': walk(1, 500, tics * 3), 'cmd.txt': S.punches(5030, 120, tics * 3)}, cfg=CFG_SYNC, map='MAP01')
    srv['copy'] = {m: (TESTS / m).as_posix() for m in mods}
    cli = {'id': 'cli', 'kind': 'pc', 'exe': S.PC, 'cwd': S.PCDIR, 'args': ['-connect', H, '-clientport', '5030', '-nomusic', '-nosound', '-netsync', '-home', S.HOME2, '+nettimeout', '2100', '+jointimeout', '2100']
           + (sum([['-file', (TESTS / m).as_posix()] for m in mods], []) if pcfile else []),
           'start_when': {'node': 'srv', 'text': 'PS2 net: address', 'delay': joindelay}}
    mine(name, {'timeout': 1500, 'nodes': [srv, cli], 'abort_on': [{'node': 'srv', 'text': 'left the game (Connection timeout)'}],
                'until': [{'node': 'cli', 'text': 'NETSYNC gametic=', 'min': tics}, {'node': 'srv', 'text': 'NETSYNC gametic=', 'min': tics}], 'grace': 3})


scenario('lua-join-dl', ['lm_net.lua'])
ps2srv('lua-ps2srv-pre', ['lm_net.lua'], pcfile=True)
ps2srv('lua-ps2srv-dl', ['lm_net.lua'], pcfile=False)
scenario('lua-join-pre', ['lm_net.lua'], predl=True)
scenario('lua-join-hw', ['lm_net.lua'], renderer='Hardware')
scenario('lua-join-rain', ['lm_net.lua'], warp='MAP32')   # a level with weather: the client's own rain mobjs are removed by the savegame (P_RemoveSavegameMobj)
scenario('lua-join-rain-hw', ['lm_net.lua'], warp='MAP32', renderer='Hardware')

if __name__ == '__main__':
    import json
    names = [n for n in T.ARGS_NAMES if not n.startswith('-') and n in MINE14]
    for n, s in MINE14.items():
        if not names or n in names:
            (S.OUT / f'{n}.json').write_text(json.dumps(s, indent=1))
            print('spec', n)
