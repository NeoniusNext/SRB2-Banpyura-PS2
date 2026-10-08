"""PS2-339 (OPT11 NETUI): the table "menu -> overlaps 0/N" of the crawl tables of tools/ps2/menu_crawl.py, one column per video mode, for the menus of the player's way.

usage: python3 tools/ps2/menu_crawl_matrix.py [DIR]      (DIR holds crawl-<label>.md, default build)
Cells: overlaps / states checked (SW: counted on the pixels; HW: the same layout, "-" for pixels), then in brackets what was drawn at 320x200: groups placed in the corner (L/R), icons (I),
moved along the row (M), not drawn (-).
"""
import re
import sys
from pathlib import Path

MENUS = ['MainDef', 'SP_MainDef', 'SP_LoadDef', 'SP_PlayerDef', 'SP_LevelSelectDef', 'SP_TimeAttackDef', 'MP_MainDef', 'MP_ServerDef', 'MP_ConnectDef', 'MP_RoomDef', 'MP_PlayerSetupDef',
         'MISC_AddonsDef', 'OP_MainDef', 'OP_ChangeControlsDef', 'OP_P1ControlsDef', 'OP_JoystickSetDef', 'OP_CameraOptionsDef', 'OP_PlaystyleDef', 'OP_VideoOptionsDef', 'OP_VideoModeDef',
         'OP_ColorOptionsDef', 'OP_SoundOptionsDef', 'OP_ServerOptionsDef', 'OP_DataOptionsDef', 'OP_BanpyuraOptionsDef', 'OP_ScreenshotOptionsDef', 'OP_EraseDataDef', 'SR_MainDef',
         'SR_SoundTestDef', 'MISC_HelpDef', 'SPauseDef', 'MPauseDef', 'MessageDef short', 'MessageDef long', 'MessageDef yes/no', 'MessageDef capture']
MODES = [('sw-320x200', '320x200'), ('sw-320x256-pal', '320x256 PAL'), ('sw-640x480', '640x480'), ('sw-640x512-pal', '640x512 PAL'), ('hw-ntsc', 'HW NTSC'), ('hw-pal', 'HW PAL')]


def load(d, label):
    rows = {}
    f = d / ('crawl-%s.md' % label)
    for line in f.read_text().split('\n'):
        if re.match(r'\| \d+ \|', line):
            c = [x.strip() for x in line.strip().strip('|').split('|')]
            rows[c[1]] = c
    return rows


def main():
    d = Path(sys.argv[1]) if len(sys.argv) > 1 else Path('build')
    data = {label: load(d, label) for label, _ in MODES}
    print('| Меню | ' + ' | '.join(t for _, t in MODES) + ' | в углу 320x200 (L R I M -) |')
    print('|---|' + '--:|' * len(MODES) + '---|')
    tot = {label: [0, 0] for label, _ in MODES}
    for name in MENUS:
        cells = []
        for label, _ in MODES:
            r = data[label].get(name)
            if not r:
                cells.append('')
                continue
            ov = r[9]
            n = ov.split('/')[1] if '/' in ov else '0'
            if label.startswith('hw'):
                cells.append('- /%s' % n)
            else:
                cells.append(ov)
        r = data['sw-320x200'].get(name)
        # columns of the md: # | menu | items | one line | raised | stacked | icons | moved | hidden | overlaps
        place = 'L%s R%s I%s M%s -%s' % (r[3], r[4], r[6], r[7], r[8]) if r else ''
        print('| %s | %s | %s |' % (name, ' | '.join(cells), place))
    for label, _ in MODES:
        for r in data[label].values():
            if '/' in r[9]:
                a, b = r[9].split('/')
                tot[label][0] += int(a)
                tot[label][1] += int(b)
    print('| **все 70 определений меню** | ' + ' | '.join(('%d/%d' % tuple(tot[l])) if l.startswith('sw') else '- /%d' % tot[l][1] for l, _ in MODES) + ' | |')


if __name__ == '__main__':
    main()
