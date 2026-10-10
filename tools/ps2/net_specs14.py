"""OPT14-CHAT scenarios for tools/ps2/net_session.py (docs/GATES/g1/opt14-CHAT.md): the "Chat" item of the pause menu, the on-screen keyboard on the chat line,
the chat window in both renderers.

usage: python3 tools/ps2/net_specs14.py [--elf ELF] [--base build/opt14-chat] [name ...]
Writes <base>/specs/<name>.json. Run: python3 tools/ps2/net_session.py <base>/specs/<name>.json
Builds on the helpers of net_specs9.py (pcsrv, ps2, pad, crosses, walk ...). The PS2 pad script numbers are poll counts (= displayed frames); the pad keys act as the
port maps them: start = menu / OSK "OK", cross = Enter / type, circle = Escape / close the OSK, triangle = backspace on the OSK, square = shift, select = the talk key,
d-pad = arrows.
 chat-base-srv     (baseline, before the work) a PS2 server alone: "say" from -netcmd, vidshot of the chat
"""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import net_env  # noqa: E402

net_env.PAK = 'build/pak2'  # SRP2 v2 packs (docs/OPT13_IMPL_BRIEF.md)
net_env.BASE = 'build/opt14-chat'
_argv = sys.argv
if '--base' not in _argv:
    sys.argv = [_argv[0], '--base', net_env.BASE] + _argv[1:]
ARGS_NAMES = [a for a in _argv[1:]]
import net_specs9 as S  # noqa: E402

ROOT = S.ROOT
H = S.H
write = S.write
pcsrv = S.pcsrv
ps2 = S.ps2
pad = S.pad
walk = S.walk
crosses = S.crosses
EMU1, EMU2 = S.EMU1, S.EMU2
CFG_SYNC = S.CFG_SYNC
SPECS = S.SPECS
MINE = {}


def mine(name, spec):
    write(name, spec)
    MINE[name] = SPECS[name]


def shots(*frames):
    return ','.join(f'f{n}' for n in frames)


# baseline: a PS2 server alone (netgame without a second player), the chat of "say" from the command list
def base_srv(name, renderer='Software'):
    args = ['-server', '-netcmd', 'file:cmd.txt', '-vidshot', shots(600, 700, 800)] + (['-renderer', renderer] if renderer != 'Software' else [])
    node = ps2('srv', EMU1, args, map='MAP01', files={'cmd.txt': '300:say Hello from the console|420:say A second line of the chat to see how it wraps around the box, it is long enough'},
               may_exit=True)
    mine(name, {'timeout': 600, 'nodes': [node], 'until': [{'node': 'srv', 'text': 'VIDSHOT COMPLETE'}], 'grace': 2})


base_srv('chat-base-srv')
base_srv('chat-base-srv-hw', 'Hardware')

if __name__ == '__main__':
    names = [n for n in ARGS_NAMES if not n.startswith('-') and n in MINE]
    for n, s in MINE.items():
        if not names or n in names:
            (S.OUT / f'{n}.json').write_text(json.dumps(s, indent=1))
            print('spec', n)
