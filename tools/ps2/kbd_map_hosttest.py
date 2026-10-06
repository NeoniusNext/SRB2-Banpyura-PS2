"""PS2-151 host test: the USB HID usage -> engine key table of src/ps2/ps2_kbd_map.h against the SDL port, and the US-layout text rules.

python tools/ps2/kbd_map_hosttest.py [--negative-controls]
Reference for keys: the switch of Impl_SDL_Scancode_To_Keycode in src/sdl/i_video.c is PARSED from the source (SDL scancode names), the numbers of the
SDL scancodes are the USB HID keyboard-page usages (SDL_scancode.h: SDL_SCANCODE_A = 4 ... SDL_SCANCODE_RGUI = 231). The KEY_* values come from src/keys.h.
Reference for text: written out independently below as the character each key types on a US keyboard.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VCVARS = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat")
WORK = ROOT / 'build/opt9-k/host'

# SDL_scancode.h values (USB HID keyboard usages) of the names that Impl_SDL_Scancode_To_Keycode mentions
SDL = {'RETURN': 40, 'ESCAPE': 41, 'BACKSPACE': 42, 'TAB': 43, 'SPACE': 44, 'MINUS': 45, 'EQUALS': 46, 'LEFTBRACKET': 47, 'RIGHTBRACKET': 48,
       'BACKSLASH': 49, 'NONUSHASH': 50, 'SEMICOLON': 51, 'APOSTROPHE': 52, 'GRAVE': 53, 'COMMA': 54, 'PERIOD': 55, 'SLASH': 56, 'CAPSLOCK': 57,
       'PRINTSCREEN': 70, 'SCROLLLOCK': 71, 'PAUSE': 72, 'INSERT': 73, 'HOME': 74, 'PAGEUP': 75, 'DELETE': 76, 'END': 77, 'PAGEDOWN': 78,
       'RIGHT': 79, 'LEFT': 80, 'DOWN': 81, 'UP': 82, 'NUMLOCKCLEAR': 83, 'KP_DIVIDE': 84, 'KP_MULTIPLY': 85, 'KP_MINUS': 86, 'KP_PLUS': 87,
       'KP_ENTER': 88, 'KP_PERIOD': 99, 'NONUSBACKSLASH': 100, 'LCTRL': 224, 'LSHIFT': 225, 'LALT': 226, 'LGUI': 227, 'RCTRL': 228, 'RSHIFT': 229,
       'RALT': 230, 'RGUI': 231, 'F11': 68, 'F12': 69}
for i in range(10):
    SDL['KP_%d' % i] = 98 if i == 0 else 88 + i
for i in range(1, 11):
    SDL['F%d' % i] = 57 + i


def key_constants():
    consts = {}
    text = (ROOT / 'src/keys.h').read_text()
    for m in re.finditer(r'#define\s+(KEY_\w+)\s+(.+?)\s*(?://.*)?$', text, re.M):
        expr = m.group(2).strip()
        try:
            expr2 = re.sub(r"'(.)'", lambda mm: str(ord(mm.group(1))), expr)
            consts[m.group(1)] = eval(expr2, {}, dict(consts))
        except Exception:
            pass
    return consts


def sdl_reference(consts):
    """usage -> key from the SDL source, as the function would return it."""
    src = (ROOT / 'src/sdl/i_video.c').read_text()
    a = src.index('static INT32 Impl_SDL_Scancode_To_Keycode')
    body = src[a:src.index('static boolean ShouldIgnoreMouse', a)]
    ref = {}
    for i in range(26):
        ref[4 + i] = ord('a') + i
    for i in range(9):
        ref[30 + i] = ord('1') + i
    ref[39] = ord('0')
    for i in range(10):
        ref[SDL['F1'] + i] = consts['KEY_F1'] + i
    for m in re.finditer(r"case SDL_SCANCODE_(\w+):\s*return\s+('(?:\\.|[^'\\])'|[^;]+?);", body):
        name, expr = m.group(1), m.group(2).strip()
        expr = re.sub(r"'(\\.|[^\\'])'", lambda mm: str(ord(mm.group(1).encode().decode('unicode_escape'))), expr)
        expr = re.sub(r'//.*$', '', expr).strip()
        ref[SDL[name]] = eval(expr, {}, consts)
    return ref


def text_reference(u, shift, caps, num):
    """US keyboard: what the key types (0 = nothing)."""
    if 4 <= u <= 29:
        return ord('A' if shift != caps else 'a') + (u - 4)
    if 30 <= u <= 38:
        return ord('!@#$%^&*('[u - 30]) if shift else ord('1') + (u - 30)
    if u == 39:
        return ord(')') if shift else ord('0')
    if u == 44:
        return 32
    base = {45: '-_', 46: '=+', 47: '[{', 48: ']}', 49: '\\|', 50: '#~', 51: ';:', 52: "'\"", 53: '`~', 54: ',<', 55: '.>', 56: '/?', 100: '\\|'}
    if u in base:
        return ord(base[u][1 if shift else 0])
    if u in (84, 85, 86, 87):
        return ord('/*-+'[u - 84])
    if num and not shift:
        if 89 <= u <= 97:
            return ord('1') + (u - 89)
        if u == 98:
            return ord('0')
        if u == 99:
            return ord('.')
    return 0


def build():
    WORK.mkdir(parents=True, exist_ok=True)
    src = ROOT / 'tools/ps2/kbd_map_hosttest.c'
    exe = WORK / 'kbd_map.exe'
    bat = WORK / 'build_map.bat'
    bat.write_text('@echo off\r\ncall "%s" x64 >nul 2>&1\r\ncl /nologo /std:c17 /W4 /WX /D_CRT_SECURE_NO_WARNINGS /I"%s" "%s" /Fo:"%s" /Fe:"%s"\r\n'
                   % (VCVARS, ROOT / 'src', src, WORK / 'kbd_map.obj', exe))
    r = subprocess.run(['cmd', '/c', str(bat)], cwd=WORK, capture_output=True, text=True, encoding='oem', errors='replace')
    if r.returncode:
        print(r.stdout + r.stderr)
        raise SystemExit('build failed')
    return exe


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--negative-controls', action='store_true')
    a = ap.parse_args()
    exe = build()
    out = subprocess.run([str(exe)], capture_output=True, text=True).stdout.split('\n')
    keys, texts = {}, {}
    for l in out:
        p = l.split()
        if not p:
            continue
        if p[0] == 'K':
            keys[int(p[1])] = int(p[2])
        else:
            texts[tuple(int(x) for x in p[1:5])] = int(p[5])
    consts = key_constants()
    ref = sdl_reference(consts)
    ref[101] = consts['KEY_MENU']  # the one deliberate addition: the application key
    bad = 0
    for u in range(256):
        want = ref.get(u, 0)
        if keys[u] != want:
            print('KEY MISMATCH usage 0x%02X: ours %d, SDL %d' % (u, keys[u], want))
            bad += 1
    nt = 0
    for (u, shift, caps, num), got in sorted(texts.items()):
        want = text_reference(u, shift, caps, num)
        nt += 1
        if got != want:
            print('TEXT MISMATCH usage 0x%02X shift %d caps %d num %d: ours %d, want %d' % (u, shift, caps, num, got, want))
            bad += 1
    print('kbd_map: %d key usages (%d mapped like SDL), %d text cases, %d mismatches' % (len(keys), len(ref), nt, bad))
    if a.negative_controls:
        # the reference must be able to fail: flip one entry and one text rule and see that the comparison notices
        ref2 = dict(ref)
        ref2[4] = ord('b')
        caught = sum(1 for u in range(256) if keys[u] != ref2.get(u, 0))
        t_caught = text_reference(30, 1, 0, 0) != texts[(30, 0, 0, 0)]
        print('negative controls: key table flip caught %d (want 1), text shift/plain differ %s' % (caught, t_caught))
        if caught != 1 or not t_caught:
            bad += 1
    print('ALL PASS' if not bad else 'FAIL')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
