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


# ---- 2b. more of the PS2 server alone
def alone(name, args, files, frames, renderer='Software', cfg='', mapname='MAP01', timeout=900):
    a = ['-server', '-vidshot', shots(*frames)] + (['-renderer', renderer] if renderer != 'Software' else []) + args
    node = ps2('srv', EMU1, a, map=mapname, files=files, cfg=cfg, may_exit=True)
    mine(name, {'timeout': timeout, 'nodes': [node], 'until': [{'node': 'srv', 'text': 'VIDSHOT COMPLETE'}], 'grace': 2})


for _r, _sfx in (('Software', ''), ('Hardware', '-hw')):
    # the button hints (cvar menuhints) of the pause menu on "Chat" and on "Continue": the quick button of the chat is told
    alone('chat-hints-srv' + _sfx, ['-padscript', 'file:pad.txt'], {'pad.txt': pad((700, 'start', 5), (760, 'down', 5))}, [740, 800, 880], _r, cfg='menuhints "On"\n')
    # the pause menu with messages in the chat window under it (the window is not drawn over the menu's text: it is dimmed by the menu's fade, like the HUD)
    alone('chat-overlap-srv' + _sfx, ['-padscript', 'file:pad.txt', '-netcmd', 'file:cmd.txt'],
          {'pad.txt': pad((900, 'start', 5)), 'cmd.txt': '300:say First message|330:say Second message|360:say Third message that is a bit longer than the others to wrap'}, [880, 950, 1000], _r)
    # the netgame paused by the server (the "pause" command): the chat window goes on being drawn and fading
    alone('chat-paused-srv' + _sfx, ['-netcmd', 'file:cmd.txt'], {'cmd.txt': '250:pause|300:say Said while the game is paused|520:pause'}, [450, 550, 700, 900], _r)
    # every key of the symbols layer on the chat line (and the shift layer): the line must show them all, none may be refused by the font
    sym_text = ".,!?'\":;-_()[]{}<>=+@#$%&*/\\|~"
    items, end = chat_pad(700, sym_text, 'menu', False, 8)
    alone('chat-sym-srv' + _sfx, ['-padscript', 'file:pad.txt'], {'pad.txt': pad(*items)}, [end - 10, end + 60], _r, timeout=1200)
    # a long message wraps in the line (the input line grows upward over the log) and in the window; the text cursor moves with L2 / R2; Cancel drops the line
    items, end = chat_pad(700, 'the quick brown fox jumps over the lazy dog again and again', 'menu', False, 8)
    alone('chat-long-srv' + _sfx, ['-padscript', 'file:pad.txt'], {'pad.txt': pad(*items)}, [end - 20, end + 60, end + 160], _r, timeout=1500)

# ---- 2c. without a netgame: the pause menu of the single player and of the local split screen has no chat items; a netgame in split screen (the engine allows it with
# "debug 1" only) keeps the console chat of the original there
def local(name, args, files, frames, renderer='Software', mapname='MAP01', netgame=False, timeout=900):
    a = (['-server'] if netgame else []) + ['-vidshot', shots(*frames)] + (['-renderer', renderer] if renderer != 'Software' else []) + args
    node = ps2('srv', EMU1, a, map=mapname, files=files, may_exit=True)
    mine(name, {'timeout': timeout, 'nodes': [node], 'until': [{'node': 'srv', 'text': 'VIDSHOT COMPLETE'}], 'grace': 2})


for _r, _sfx in (('Software', '-so'), ('Hardware', '-ha')):
    local('chat-single' + _sfx, ['-padscript', 'file:pad.txt'], {'pad.txt': pad((300, 'start', 5))}, [280, 360], _r, timeout=600)
    local('chat-localsplit' + _sfx, ['-padscript', 'file:pad.txt', '-netcmd', 'file:cmd.txt'], {'pad.txt': pad((420, 'start', 5)), 'cmd.txt': '100:splitscreen 1'}, [400, 500], _r, timeout=600)
    local('chat-netsplit' + _sfx, ['-netcmd', 'file:cmd.txt'], {'cmd.txt': '200:debug 1|220:splitscreen 1|420:say Said in the split screen|700:say And a second line of it'}, [380, 500, 620, 800], _r, netgame=True, timeout=900)

# ---- 3. PC dedicated server <-> PS2 client
# how the three counters of a PS2 run relate (fitted on chat-pc-sw / chat-pc-hw of this work): the pad script counts pad POLLS, -vidshot fN counts displayed FRAMES, the
# game counts TICS (leveltime). Software: a poll is a frame, 29 frames a second against 35 tics a second. Hardware ("Match refresh rate", PCSX2 at ~0.75 of real time):
# the pad is polled about once per tic, frames come 1.36 times as fast.
COUNTERS = {'Software': {'tic': lambda poll: int(1.19 * poll + 25), 'frame': lambda poll: int(poll)},
            'Hardware': {'tic': lambda poll: int(1.19 * poll + 25), 'frame': lambda poll: int(poll)}}  # (refitted below once measured)


def pc_client(name, renderer='Software', via='menu', team=False, text='hello from ps2', first=1700):
    """The PS2 joins a PC dedicated server by pad (Cross on the join screens); a PC client (xvfb, build/pc-net, -diagsay: src/netcode/d_clisrv.c) and the server
    itself talk at given game tics; the PS2 pad then opens the chat (the pause menu, or Select) and types TEXT. The engine log of the PS2 gets every chat line
    (-chatlog), the server's out.txt every line it sees."""
    items, end = chat_pad(first, text, via, team)
    tic, frame = COUNTERS[renderer]['tic'], COUNTERS[renderer]['frame']
    p1, p2, p3 = first - 300, end + 120, end + 500  # (polls) the PC client talks, the PS2 is typing nothing; the PC client answers; the server talks
    say1, say2, say3 = tic(p1), tic(p2), tic(p3)
    # the shots: the message of the PC client in the mini chat, the keyboard with the history under it, the sent message, the answers
    polls = [p1 + 60, p1 + 150, first + 40, first + 100, first + 200, end + 30, end + 80, p2 + 60, p2 + 130, p3 + 60]
    frames = [frame(x) for x in polls]
    cli_args = ['-skipintro', '-connect', H, '-chatlog', '-padscript', 'file:pad.txt', '-vidshot', shots(*frames)] + (['-renderer', renderer] if renderer != 'Software' else [])
    srv = pcsrv(start=0, extra=['-diagsay', f'{say3}:Server says goodbye'])
    pcc = {'id': 'pcc', 'kind': 'pc', 'exe': S.PC, 'cwd': S.PCDIR, 'start': 6,
           'args': ['-connect', H, '-nomusic', '-nosound', '-netsync', '-home', S.HOME2, '-diagsay', f'{say1}:Hello from the PC client|{say2}:Reply from the PC client']}
    cli = ps2('cli', EMU1, cli_args, files={'pad.txt': pad(*(crosses(200, first - 300, 60) + items))},
              cfg=CFG_SYNC, may_exit=True, start=8)  # (fpscap 35 of the harness: the chat window does not depend on the frame rate)
    mine(name, {'timeout': 2400, 'nodes': [srv, pcc, cli],
                'until': [{'node': 'cli', 'text': 'VIDSHOT COMPLETE'}, {'node': 'srv', 'text': text, 'file': 'out.txt'}, {'node': 'cli', 'text': 'Server says goodbye'}], 'grace': 3})
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
