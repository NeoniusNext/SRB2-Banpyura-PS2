"""OPT14-CHAT scenarios for tools/ps2/net_session.py (docs/GATES/g1/opt14-CHAT.md): the "Chat" item of the pause menu, the on-screen keyboard on the chat line,
the chat window in both renderers.

usage: python3 tools/ps2/net_specs14.py [--elf ELF] [--base build/opt14-chat] [name ...]
Writes <base>/specs/<name>.json. Run: python3 tools/ps2/net_session.py <base>/specs/<name>.json
Builds on the helpers of net_specs9.py (pcsrv, ps2, pad, crosses, walk ...). The PS2 pad script numbers are poll counts (= displayed frames); the pad keys act as the
port maps them: start = menu / OSK "OK", cross = Enter / type, circle = Escape / close the OSK, triangle = backspace on the OSK, square = shift, select = the talk key
(the symbols layer on the OSK), d-pad = arrows / the OSK cursor.

 chat-base-srv[-hw]   a PS2 server alone: "say" from -netcmd, shots of the chat window (the window chat of this work; the PS2 had the console chat before)
 chat-pause-srv[-hw]  a PS2 server alone: the pad opens the pause menu, takes "Chat", types a message on the keyboard, sends it (shots of every step)
 chat-pc-sw / -hw     PC dedicated server <-> PS2 client: the same by pad; the server's stdin "say" answers; the message must reach the server log and the
                      server's must show on the PS2 (shots)
 chat-select-sw/-hw   the quick button (Select) instead of the menu
 chat-kbd-srv[-hw]    a USB keyboard (-kbdscript): 't' opens the line, the keys type, Enter sends, no on-screen keyboard
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

LOWER = ['1234567890', 'qwertyuiop', 'asdfghjkl.', 'zxcvbnm:/-']
UPPER = ['!@#$%^&*()', 'QWERTYUIOP', 'ASDFGHJKL_', 'ZXCVBNM;?+']
SYM = ['.,!?\'":;-_', '()[]{}<>=+', '@#$%&*/\\|~', '1234567890']


def mine(name, spec):
    write(name, spec)
    MINE[name] = SPECS[name]


def shots(*frames):
    return ','.join(f'f{n}' for n in frames)


def osk_buttons(text, start=(0, 0)):
    """the buttons to press on the on-screen keyboard (src/ps2/ps2_osk.c) to type TEXT from the cursor position START: list of button names.
    The cursor wraps (10 columns, 5 rows: four of characters and the command row); Square = shift (one letter), Select = the symbols layer (sticky)."""
    x, y = start
    out = []
    sym = False

    def go(tx, ty):
        nonlocal x, y
        dx = (tx - x) % 10
        out.extend(['right'] * dx if dx <= 5 else ['left'] * (10 - dx))
        dy = (ty - y) % 5
        out.extend(['down'] * dy if dy <= 2 else ['up'] * (5 - dy))
        x, y = tx, ty

    for ch in text:
        if ch == ' ':
            go(2, 4)  # the Space button spans columns 2..3 of the command row
            out.append('cross')
            continue
        for rows, shift, issym in ((LOWER, False, False), (UPPER, True, False), (SYM, False, True)):
            hit = [(r, row.index(ch)) for r, row in enumerate(rows) if ch in row]
            if hit:
                break
        else:
            raise SystemExit(f'no key for {ch!r} on the keyboard')
        if issym != sym:
            out.append('select')
            sym = issym
        if shift:
            out.append('square')
        go(hit[0][1], hit[0][0])
        out.append('cross')
    if sym:
        out.append('select')
    return out


def seq(start, buttons, step=9, hold=4):
    """(frame, button, hold) items, one press every STEP polls from START; returns (items, next free frame)"""
    items = [(start + i * step, b, hold) for i, b in enumerate(buttons)]
    return items, start + len(buttons) * step


def chat_pad(first, text, via='menu', team=False, step=9):
    """the pad items that open the chat (via the pause menu: Start, Down [Down], Cross; via Select) and type TEXT, send with Start. Returns (items, end frame)."""
    items = []
    t = first
    if via == 'menu':
        items += [(t, 'start', 5)]
        t += 60
        items += [(t, 'down', 5)]
        t += 25
        if team:
            items += [(t, 'down', 5)]
            t += 25
        items += [(t, 'cross', 5)]
        t += 45
    else:
        items += [(t, 'select', 5)]
        t += 45
    typed, t = seq(t, osk_buttons(text), step)
    items += typed
    t += 20
    items += [(t, 'start', 5)]
    return items, t + 5


# ---- 1. baseline: a PS2 server alone (a netgame without a second player), the chat of "say" from the command list
def base_srv(name, renderer='Software'):
    args = ['-server', '-netcmd', 'file:cmd.txt', '-vidshot', shots(450, 600, 700, 800)] + (['-renderer', renderer] if renderer != 'Software' else [])
    node = ps2('srv', EMU1, args, map='MAP01', files={'cmd.txt': '300:say Hello from the console|420:say A second line of the chat to see how it wraps around the box, it is long enough'},
               may_exit=True)
    mine(name, {'timeout': 600, 'nodes': [node], 'until': [{'node': 'srv', 'text': 'VIDSHOT COMPLETE'}], 'grace': 2})


base_srv('chat-base-srv')
base_srv('chat-base-srv-hw', 'Hardware')


# ---- 2. a PS2 server alone: pause menu > Chat > keyboard > send
def pause_srv(name, renderer='Software', via='menu', team=False, text='hi there', first=700, extra=None, mapname='MAP01', mode=None):
    items, end = chat_pad(first, text, via, team)
    frames = [first + 40, first + 100, first + 150, first + 200, first + 300, end + 40, end + 120]
    args = ['-server', '-padscript', 'file:pad.txt', '-vidshot', shots(*frames)] + (['-renderer', renderer] if renderer != 'Software' else []) + (extra or [])
    if mode:
        args += ['-gametype', mode]
    node = ps2('srv', EMU1, args, map=mapname, files={'pad.txt': pad(*items)}, may_exit=True)
    mine(name, {'timeout': 900, 'nodes': [node], 'until': [{'node': 'srv', 'text': 'VIDSHOT COMPLETE'}], 'grace': 2})
    return frames, end


pause_srv('chat-pause-srv')
pause_srv('chat-pause-srv-hw', 'Hardware')
pause_srv('chat-pause-srv-team', team=True, mode='ctf', mapname='MAPM0')


# ---- 3. PC dedicated server <-> PS2 client
def pc_client(name, renderer='Software', via='menu', team=False, text='hello from ps2', first=1700, ratio=None):
    """The PS2 joins a PC dedicated server by pad (Cross on the join screens); a PC client (xvfb, build/pc-net, -diagsay: src/netcode/d_clisrv.c) and the server
    itself talk at given game tics; the PS2 pad then opens the chat (the pause menu, or Select) and types TEXT. The engine log of the PS2 gets every chat line
    (-chatlog), the server's out.txt every line it sees. RATIO: displayed PS2 frames per game tic (about 1 in Software, more in Hardware: PCSX2 runs it slower
    than the game's 35 tics, the frame counter runs on at the display rate)."""
    ratio = ratio or (1.0 if renderer == 'Software' else 1.35)
    items, end = chat_pad(first, text, via, team)

    def tic(poll):
        return max(100, int(poll / ratio) - 90)
    say1, say2, say3 = tic(first - 300), tic(end + 120), tic(end + 700)
    # the shots: the message of the PC client in the mini chat, the keyboard with the history under it, the sent message, the answers
    frames = [first - 300 + 120, first - 300 + 220, first + 40, first + 100, first + 200, end + 40, end + 100, end + 120 + 80, end + 120 + 180, end + 700 + 80]
    cli_args = ['-skipintro', '-connect', H, '-chatlog', '-padscript', 'file:pad.txt', '-vidshot', shots(*frames)] + (['-renderer', renderer] if renderer != 'Software' else [])
    srv = pcsrv(start=0, extra=['-diagsay', f'{say3}:Server says goodbye'])
    pcc = {'id': 'pcc', 'kind': 'pc', 'exe': S.PC, 'cwd': S.PCDIR, 'start': 6,
           'args': ['-connect', H, '-nomusic', '-nosound', '-netsync', '-home', S.HOME2, '-diagsay', f'{say1}:Hello from the PC client|{say2}:Reply from the PC client']}
    cli = ps2('cli', EMU1, cli_args, files={'pad.txt': pad(*(crosses(200, first - 300, 60) + items))},
              cfg=CFG_SYNC + ('fpscap "Match refresh rate"\n' if renderer == 'Hardware' else ''), may_exit=True, start=8)
    mine(name, {'timeout': 1800, 'nodes': [srv, pcc, cli],
                'until': [{'node': 'cli', 'text': 'VIDSHOT COMPLETE'}, {'node': 'srv', 'text': text, 'file': 'out.txt'}, {'node': 'cli', 'text': 'Reply from the PC client'}], 'grace': 3})
    return frames, end


pc_client('chat-pc-sw')
pc_client('chat-pc-hw', 'Hardware')
pc_client('chat-select-sw', via='select', text='quick one')
pc_client('chat-select-hw', 'Hardware', via='select', text='quick one')


# ---- 4. a USB keyboard (the script of ps2_kbd.c): 't' opens the line, Enter sends; no on-screen keyboard comes up
def kbd_script(first, text):
    keys = {' ': 'space', '.': 'period', ',': 'comma'}
    steps = [f'{first}:t']
    t = first + 20
    for ch in text:
        steps.append(f'{t}:{keys.get(ch, ch)}')
        t += 8
    steps.append(f'{t + 10}:enter')
    return ','.join(steps), t + 10


def kbd_srv(name, renderer='Software', text='typed on usb'):
    script, end = kbd_script(700, text)
    frames = [740, 800, end - 10, end + 40, end + 120]
    args = ['-server', '-kbdscript', 'file:kbd.txt', '-vidshot', shots(*frames)] + (['-renderer', renderer] if renderer != 'Software' else [])
    node = ps2('srv', EMU1, args, map='MAP01', files={'kbd.txt': script}, may_exit=True)
    mine(name, {'timeout': 900, 'nodes': [node], 'until': [{'node': 'srv', 'text': 'VIDSHOT COMPLETE'}], 'grace': 2})


kbd_srv('chat-kbd-srv')
kbd_srv('chat-kbd-srv-hw', 'Hardware')

if __name__ == '__main__':
    names = [n for n in ARGS_NAMES if not n.startswith('-') and n in MINE]
    for n, s in MINE.items():
        if not names or n in names:
            (S.OUT / f'{n}.json').write_text(json.dumps(s, indent=1))
            print('spec', n)
