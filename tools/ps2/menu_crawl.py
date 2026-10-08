"""PS2-339 (OPT11 NETUI): walks every menu and every item with the button hints on and checks that no hint stands on anything the menu draws.

usage: python3 tools/ps2/menu_crawl.py --elf build/out1/SRB2.ELF --label sw-320x200 [--hw] [--args '-width 640 -height 480'] [--warp 1] [--out FILE.md]
The engine (src/ps2/ps2_menuhints.c) has the console command ps2_menucrawl: it brings up each menu definition the way the game does (m_menu.c M_PS2MenuEnter), moves the
cursor over every selectable item and prints one MHCHECK line per item:
  MHCHECK menu=ID item=I/N kind=K how=LR plates=P [x,y,wxh]... WxH menu_pixels=M under=U within2=W ok|OVERLAP
how: how the left and the right group were placed (L one line at the bottom corner, R one line higher in the corner, S two stacked lines, I icons only, M moved along
the row towards the middle, m icons moved, - not drawn).
menu_pixels / under / within2 are measured on the real pixels of the frame (software renderer only): the menu is drawn on a flat colour without the hints and the pixels
it covers are counted inside the plates (under) and within 2 px of them (within2). The hardware renderer has the same layout code but no pixel readback here.
A menu whose drawing hangs or crashes the emulator (a draw routine that needs state that its entry function sets) is skipped and the walk goes on with the next one:
the run is repeated with ps2_menucrawl <next>; such menus are listed as "not drawn" and have to be checked by hand.
The result is a table "menu -> items checked / groups by placement / overlaps" in Markdown.
"""
import argparse
import re
import subprocess
import sys
from collections import OrderedDict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def run_once(name, elf, first, hw, extra, timeout, keep_args, warp, cfg=(), last=''):
    cmd = [sys.executable, str(ROOT / 'tools/ps2/netui_run.py'), '--name', name, '--elf', elf, '--timeout', str(timeout), '--until', 'MHCRAWL done', '--stall', 'MHC=75', '--', '-skipintro', '-menuhintscheck']
    if hw:
        cmd += ['-renderer', 'Hardware', '-zreserve', '1536']
    if warp:
        cmd += ['-warp', str(warp)]
    cmd += keep_args + extra
    for line in cfg:
        cmd[cmd.index('--') :cmd.index('--')] = ['--cfg', line]
    cmd += ['-netcmd', '%d:ps2_menucrawl %d %s' % (150 if warp else 60, first, last)]
    subprocess.run(cmd, check=False, stdout=subprocess.DEVNULL)
    return (ROOT / 'build/runs' / name / 'boot.txt').read_text(errors='replace')


CONTROLS = ['forward', 'backward', 'strafeleft', 'straferight', 'turnleft', 'turnright', 'weaponnext', 'weaponprev'] + ['weapon%d' % i for i in range(1, 11)] + \
           ['fire', 'firenormal', 'tossflag', 'spin', 'camtoggle', 'camreset', 'lookup', 'lookdown', 'centerview', 'mouseaiming', 'talkkey', 'teamtalkkey', 'scores', 'jump',
            'console', 'pause', 'systemmenu', 'screenshot', 'recordgif', 'viewpoint', 'viewpointprev', 'custom1', 'custom2', 'custom3']


def stress_lines():
    """setcontrol lines: every control gets a long key name, 16 of them (the first rows, Jump/Spin, Look up/down, the camera) also one of the 16 buttons of a pad (the 11 px shoulder buttons, the 7 px ones, the D-pad). setcontrol
    takes a key away from the control that had it (G_CheckDoubleUsage), so every name is used once per player."""
    names = (['space', 'backspace', 'tab', 'caps lock', 'numlock', 'scrolllock', 'leftwin', 'rightwin', 'menu', 'lshift', 'rshift', 'lctrl', 'rctrl', 'lalt', 'ralt',
              'right arrow', 'left arrow', 'down arrow', 'up arrow', 'keypad /', 'keypad -', 'keypad +', 'keypad .'] + ['keypad %d' % i for i in range(10)] +
             ['home', 'end', 'pgup', 'pgdn', 'ins', 'del'] + ['f%d' % i for i in range(1, 13)] + ['pause/break'] + ['mouse%d' % i for i in range(1, 9)] +
             ['wheel 1 up', 'wheel 1 down', 'wheel 2 up', 'wheel 2 down'] + [chr(c) for c in range(ord('a'), ord('z') + 1)])
    assert len(names) >= 2 * len(CONTROLS)
    pads = ['joy11', 'joy12', 'joy5', 'joy6', 'joy9', 'joy10', 'joy7', 'joy8', 'joy1', 'joy2', 'joy3', 'joy4', 'hatup', 'hatdown', 'hatleft', 'hatright']
    # the controls that get a pad button as the second binding: the ones of the first rows and the ones next to each other (Jump/Spin, Look up/down, the camera)
    padded = ['forward', 'backward', 'strafeleft', 'straferight', 'jump', 'spin', 'lookup', 'lookdown', 'centerview', 'camtoggle', 'camreset', 'fire', 'firenormal', 'weaponnext',
              'weaponprev', 'console']
    out = []
    for i, c in enumerate(CONTROLS):
        second = ' "%s"' % pads[padded.index(c)] if c in padded else ''
        out.append('setcontrol "%s" "%s"%s' % (c, names[i], second))
    for i, c in enumerate(CONTROLS):
        second = ' "sec_%s"' % pads[(padded.index(c) + 3) % len(pads)] if c in padded else ''
        out.append('setcontrol2 "%s" "%s"%s' % (c, names[len(CONTROLS) + i], second))
    return out


def parse(text, menus):
    cur = None
    done = False
    for line in text.split('\n'):
        m = re.match(r'MHCRAWL menu (\d+)/(\d+) (\S+(?: \S+)*?) id=(\d+) items=(\d+) title=(\S+)', line)
        if m:
            idx = int(m.group(1))
            cur = menus.setdefault(idx, {'name': m.group(3), 'id': m.group(4), 'items': int(m.group(5)), 'checks': [], 'seen': True})
            continue
        if line.startswith('MHCRAWL done'):
            done = True
        m = re.match(r'MHCHECK menu=(\d+) item=(\d+)/(\d+) kind=(\d+) how=(.)(.) plates=(\d+)(.*)', line)
        if m and cur is not None:
            rest = m.group(8)
            pm = re.search(r'(\d+)x(\d+) menu_pixels=(\d+) under=(\d+) within2=(\d+) (ok|OVERLAP)', rest)
            im = re.search(r'icons=(\d+) on_text=(\d+) icon_icon=(\d+) text_on_icon=(\d+)(?: ink=(\d+) touch=(\d+))? (iconok|ICONOVERLAP)(?: x=(\d+)\.\.(\d+))?', rest)
            plates = re.findall(r'\[(-?\d+),(-?\d+),(\d+)x(\d+)\]', rest)
            cur['checks'].append({'item': int(m.group(2)), 'kind': int(m.group(4)), 'how': m.group(5) + m.group(6), 'plates': [tuple(map(int, p)) for p in plates],
                                  'size': (int(pm.group(1)), int(pm.group(2))) if pm else None, 'pixels': int(pm.group(3)) if pm else None,
                                  'under': int(pm.group(4)) if pm else None, 'near': int(pm.group(5)) if pm else None, 'overlap': bool(pm and pm.group(6) == 'OVERLAP'),
                                  'icons': int(im.group(1)) if im else 0, 'icon_bad': bool(im and im.group(7) == 'ICONOVERLAP'), 'hints': 0, 'dups': 0,
                                  'xl': int(im.group(8)) if im and im.group(8) else None, 'xr': int(im.group(9)) if im and im.group(9) else None})
            continue
        m = re.match(r'HINTCHK menu=(\d+) item=(\d+)/(\d+) hints=(\d+) dup=(\d+)', line)
        if m and cur is not None and cur['checks']:
            cur['checks'][-1]['hints'] = int(m.group(4))
            cur['checks'][-1]['dups'] = int(m.group(5))
    return done, cur


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', required=True)
    ap.add_argument('--label', required=True)
    ap.add_argument('--hw', action='store_true')
    ap.add_argument('--args', default='')
    ap.add_argument('--warp', type=int, default=0)
    ap.add_argument('--timeout', type=int, default=420)
    ap.add_argument('--out', default='')
    ap.add_argument('--first', type=int, default=0, help='the first menu of the list')
    ap.add_argument('--last', default='', help='the last menu of the list')
    ap.add_argument('--cfg', action='append', default=[], help='a line of the config of the run, e.g. \'menuhints "Off"\'')
    ap.add_argument('--stress-controls', action='store_true', help='every control of Setup Controls gets a long keyboard name and a pad button of the widest kind (both players)')
    a = ap.parse_args()
    keep = a.args.split()
    cfg = list(a.cfg) + (stress_lines() if a.stress_controls else [])
    menus = OrderedDict()
    skipped = []
    first = a.first
    for attempt in range(40):
        text = run_once('crawl-' + a.label, a.elf, first, a.hw, [], a.timeout, keep, a.warp, cfg, a.last)
        done, cur = parse(text, menus)
        lastidx = max(menus) if menus else -1
        if done:
            break
        if lastidx < first:
            print('no progress at menu %d: stopping' % first, file=sys.stderr)
            break
        # the last menu that was started did not finish: it hung or crashed the run; its items already seen stay
        last = menus[lastidx]
        total_items = last['items']
        if len(last['checks']) < max(1, min(total_items, 1)):
            skipped.append((lastidx, last['name']))
        first = lastidx + 1
        print('run %d stopped in menu %d (%s): go on with %d' % (attempt, lastidx, last['name'], first), file=sys.stderr)
    # the report
    rows = []
    totals = {'items': 0, 'overlap': 0, 'L': 0, 'R': 0, 'S': 0, 'I': 0, 'M': 0, '-': 0}
    for idx, m in menus.items():
        cnt = {'L': 0, 'R': 0, 'S': 0, 'I': 0, 'M': 0, '-': 0}
        ov = 0
        for c in m['checks']:
            for h in c['how']:
                h = 'M' if h == 'm' else h
                if h in cnt:
                    cnt[h] += 1
            ov += 1 if c['overlap'] else 0
        n = len(m['checks'])
        ic = sum(c['icons'] for c in m['checks'])
        icbad = sum(1 for c in m['checks'] if c['icon_bad'])
        hn = sum(c['hints'] for c in m['checks'])
        hd = sum(1 for c in m['checks'] if c['dups'])
        totals['items'] += n
        totals['overlap'] += ov
        totals['icons'] = totals.get('icons', 0) + ic
        totals['icon_bad'] = totals.get('icon_bad', 0) + icbad
        totals['hints'] = totals.get('hints', 0) + hn
        totals['dups'] = totals.get('dups', 0) + hd
        for k in cnt:
            totals[k] += cnt[k]
        xs = [c['xl'] for c in m['checks'] if c.get('xl') is not None and c['icons']]
        xe = [c['xr'] for c in m['checks'] if c.get('xr') is not None and c['icons']]
        xrange_ = '%d..%d' % (min(xs), max(xe)) if xs and xe else '-'
        rows.append((idx, m['name'], n, cnt, ov, ic, icbad, hn, hd, xrange_))
    lines = ['| # | menu | items | one line | raised | stacked | icons | moved | hidden | overlaps | icons in text | icon overlaps | hints | duplicates | x range (states with icons) |', '|--:|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|']
    for idx, name, n, cnt, ov, ic, icbad, hn, hd, xrange_ in rows:
        verdict = ('%d/%d' % (ov, n)) if n else 'not drawn'
        lines.append('| %d | %s | %d | %d | %d | %d | %d | %d | %d | %s | %d | %s | %d | %s | %s |' % (idx, name, n, cnt['L'], cnt['R'], cnt['S'], cnt['I'], cnt['M'], cnt['-'], verdict, ic, ('%d/%d' % (icbad, n)) if n else '-', hn, ('%d/%d' % (hd, n)) if n else '-', xrange_))
    lines.append('| | **all** | %d | %d | %d | %d | %d | %d | %d | **%d/%d** | %d | **%d/%d** | %d | **%d/%d** | |' % (totals['items'], totals['L'], totals['R'], totals['S'], totals['I'], totals['M'], totals['-'], totals['overlap'], totals['items'], totals.get('icons', 0), totals.get('icon_bad', 0), totals['items'], totals.get('hints', 0), totals.get('dups', 0), totals['items']))
    if skipped:
        lines.append('')
        lines.append('not drawn by the crawler (hang / crash): ' + ', '.join('%d %s' % s for s in skipped))
    out = '\n'.join(lines)
    print(out)
    if a.out:
        Path(a.out).write_text('<!-- menu_crawl.py --label %s -->\n' % a.label + out + '\n')
    return 0 if totals['overlap'] == 0 and totals.get('icon_bad', 0) == 0 and totals.get('dups', 0) == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
