"""OPT9-M: PCSX2 run with USB keyboard/mouse input injected into the emulator's own window (the "USB stand").

usage: usb_run.py --elf FILE --log FILE [--args "..."] [--script "T:cmd[,cmd...];T:cmd"] [--ready TEXT] [--until TEXT]
                  [--timeout SEC] [--marker TEXT] [--exe PCSX2.EXE]

What it does
 * Starts D:/PCSX2-usb/pcsx2-qt.exe (a private copy of the test emulator whose PCSX2.ini has [USB1] Type = hidkbd and [USB2] Type = hidmouse,
   host mouse raw input OFF) under its own lock file (the other emulator slots are never touched), window hidden like run_pcsx2.py does.
 * Finds the emulator's display window by PID (class Qt...QWindowIcon, title is not "PCSX2 v...") and drives it ONLY with PostMessageW to that
   HWND: WM_KEYDOWN/WM_KEYUP, WM_MOUSEMOVE, WM_xBUTTONDOWN/UP, WM_MOUSEWHEEL. No SendInput, no keybd_event/mouse_event, no SetCursorPos, no
   foreground/focus change, no message to any window of another process: the user's desktop, keyboard and mouse are never touched.
   Qt turns the posted messages into key/pointer events of the display widget, PCSX2's InputManager feeds them to the emulated HID devices.
 * The script is a list of steps "T:cmd,cmd;T:cmd": T seconds after the --ready text appeared in the log (default: 3 s after the window exists),
   cmds (several per step separated by ',', arguments after ':'... see below).
     key:NAME[:down|up]      tap (default) or press/release a key. NAME: a-z 0-9 enter esc space tab bksp up down left right home end pgup pgdn
                             ins del f1..f12 lshift rshift lctrl rctrl lalt ralt (or 0x41-style VK code)
     type:TEXT               taps the characters of TEXT (letters, digits, space, '.', '-', '/', ':' ...), 50 ms apart
     btn:left|right|middle[:down|up]   emulated mouse button (tap by default): the stand profile binds the HID mouse buttons of [USB2] to the
                             host keys F13/F14/F15 (hidmouse_LeftButton = Keyboard/F13 ...), so this posts WM_KEYDOWN/UP VK_F13..F15
     sleep:SEC               extra pause inside one step
     mv:DX,DY / mvto:X,Y / wheel:N     ONLY with --pointer (see below): pointer motion and wheel
   Mouse MOTION and WHEEL cannot be injected safely: PCSX2's HID mouse takes them from its "Pointer" binding, which switches the display window
   into relative-mouse mode, where every mouse-move message makes PCSX2 itself read the REAL cursor (GetCursorPos) and warp it back to the window
   centre (SetCursorPos), and the start of the run warps it once. That moves the user's real pointer, so the default profile has no Pointer binding.
   --pointer adds hidmouse_Pointer = Pointer-0 for the run (restored afterwards) and is meant for a machine nobody is using; the engine's -usbtest
   mode (tools/ps2/usb_mouse_test.py) feeds recorded HID reports through the engine's own parser instead.
 * --nopad sets [Pad1] Type = None for the run (restored afterwards): the emulator's default keyboard bindings of the virtual DualShock (arrows,
   A/S/D/W, Return ...) would otherwise reach the engine as pad input at the same time as the USB keyboard.
 * Output: the emulator log (PCSX2 console + engine printf) and a transcript of what was posted on stdout. Exit code 0 when the process exited
   by itself or --until appeared; 2 on timeout (the emulator is closed politely, then killed).

Why not the real input path: raw mouse input needs real WM_INPUT handles which cannot be posted; with the raw source off, mouse motion
comes from Qt's absolute pointer position (see docs/GATES/g1/opt9-M.md for the measured behaviour and the limits).
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import os
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_pcsx2 as R  # noqa: E402  (lock helpers)

u32 = ctypes.windll.user32
u32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
u32.PostMessageW.restype = wt.BOOL
u32.MapVirtualKeyW.argtypes = [wt.UINT, wt.UINT]

EXE = os.environ.get('SRB2_PCSX2_USB', 'D:/PCSX2-usb/pcsx2-qt.exe')

WM_KEYDOWN, WM_KEYUP, WM_MOUSEMOVE = 0x100, 0x101, 0x200
WM_LBUTTONDOWN, WM_LBUTTONUP, WM_RBUTTONDOWN, WM_RBUTTONUP, WM_MBUTTONDOWN, WM_MBUTTONUP = 0x201, 0x202, 0x204, 0x205, 0x207, 0x208
WM_MOUSEWHEEL = 0x20A
MK_LBUTTON, MK_RBUTTON, MK_MBUTTON = 1, 2, 16

VK = {'enter': 0x0D, 'esc': 0x1B, 'space': 0x20, 'tab': 0x09, 'bksp': 0x08, 'left': 0x25, 'up': 0x26, 'right': 0x27, 'down': 0x28,
      'home': 0x24, 'end': 0x23, 'pgup': 0x21, 'pgdn': 0x22, 'ins': 0x2D, 'del': 0x2E, 'lshift': 0xA0, 'rshift': 0xA1, 'lctrl': 0xA2,
      'rctrl': 0xA3, 'lalt': 0xA4, 'ralt': 0xA5, 'capslock': 0x14, 'numlock': 0x90, 'pause': 0x13}
for _i in range(24):
    VK['f%d' % (_i + 1)] = 0x70 + _i
EXTENDED = {0x25, 0x26, 0x27, 0x28, 0x24, 0x23, 0x21, 0x22, 0x2D, 0x2E, 0xA3, 0xA5}
CHARVK = {' ': 0x20, '.': 0xBE, '-': 0xBD, '/': 0xBF, ':': 0xBA, ',': 0xBC, ';': 0xBA, '=': 0xBB, '_': 0xBD}


def vk_of(name):
    n = name.lower()
    if n in VK:
        return VK[n]
    if n.startswith('0x'):
        return int(n, 16)
    if len(n) == 1 and n.isalpha():
        return ord(n.upper())
    if len(n) == 1 and n.isdigit():
        return ord(n)
    raise SystemExit('usb_run: unknown key ' + name)


def mk(x, y):
    return ((y & 0xFFFF) << 16) | (x & 0xFFFF)


class Stand:
    """One emulator process and its display window; every injection goes to this HWND only."""

    def __init__(self, hwnd, pid, say=print, pointer=False):
        self.hwnd, self.pid, self.say, self.pointer = hwnd, pid, say, pointer
        self.px = self.py = None
        self.buttons = 0
        r = wt.RECT()
        u32.GetClientRect(hwnd, ctypes.byref(r))
        self.w, self.h = max(r.right, 64), max(r.bottom, 64)

    def refresh(self):
        # the display window can be re-created when the guest starts: look it up again before every step (same PID only)
        hw = find_display(self.pid)
        if hw and hw[0] != self.hwnd:
            self.hwnd = hw[0]
            r = wt.RECT()
            u32.GetClientRect(self.hwnd, ctypes.byref(r))
            self.w, self.h = max(r.right, 64), max(r.bottom, 64)
            self.say('  display window now %d %r %dx%d' % (self.hwnd, hw[1], self.w, self.h))

    def _post(self, msg, wp, lp):
        # the only call that reaches the emulator: a message to its own HWND
        if not u32.PostMessageW(self.hwnd, msg, wp, lp):
            self.say('  PostMessage failed %d' % ctypes.GetLastError())

    def key(self, name, how='tap'):
        vk = vk_of(name)
        sc = u32.MapVirtualKeyW(vk, 0)
        ext = (1 << 24) if vk in EXTENDED else 0
        down = 1 | (sc << 16) | ext
        up = 1 | (sc << 16) | ext | 0xC0000000
        if how in ('tap', 'down'):
            self._post(WM_KEYDOWN, vk, down)
        if how == 'tap':
            time.sleep(0.05)
        if how in ('tap', 'up'):
            self._post(WM_KEYUP, vk, up)
        self.say('  key %s %s (vk 0x%02X)' % (name, how, vk))

    def type(self, text):
        for ch in text:
            shift = ch.isupper() or ch in ':_'
            if shift:
                self.key('lshift', 'down')
            code = CHARVK.get(ch) or vk_of(ch)
            self._post(WM_KEYDOWN, code, 1 | (u32.MapVirtualKeyW(code, 0) << 16))
            time.sleep(0.03)
            self._post(WM_KEYUP, code, 1 | (u32.MapVirtualKeyW(code, 0) << 16) | 0xC0000000)
            if shift:
                self.key('lshift', 'up')
            time.sleep(0.05)
        self.say('  type %r' % text)

    def _mods(self):
        return self.buttons

    def _need_pointer(self, what):
        if not self.pointer:
            raise SystemExit('usb_run: %s needs --pointer (it makes PCSX2 warp the real cursor, see the docstring)' % what)

    def mvto(self, x, y):
        self._need_pointer('mvto')
        x = min(max(int(x), 0), self.w - 1)
        y = min(max(int(y), 0), self.h - 1)
        self._post(WM_MOUSEMOVE, self._mods(), mk(x, y))
        self.px, self.py = x, y

    def mv(self, dx, dy):
        self._need_pointer('mv')
        if self.px is None:
            self.mvto(self.w // 2, self.h // 2)
            time.sleep(0.3)
        steps = max(1, int(max(abs(dx), abs(dy)) // 8))
        tx, ty = self.px + dx, self.py + dy
        sx, sy = self.px, self.py
        for i in range(1, steps + 1):
            self.mvto(sx + (tx - sx) * i / steps, sy + (ty - sy) * i / steps)
            time.sleep(0.01)
        self.say('  mv %+d,%+d -> pointer %d,%d' % (dx, dy, self.px, self.py))

    def btn(self, which, how='tap'):
        if not self.pointer:
            name = {'left': 'f13', 'right': 'f14', 'middle': 'f15'}[which]
            if how == 'tap':
                # the engine reads the buttons by polling once per tic (~70 ms of host time at the emulator's speed): hold a click long enough
                self.key(name, 'down')
                time.sleep(0.4)
                self.key(name, 'up')
            else:
                self.key(name, how)
            self.say('  (mouse %s button = host key %s)' % (which, name.upper()))
            return
        if self.px is None:
            self.mvto(self.w // 2, self.h // 2)
        d, u, mk_ = {'left': (WM_LBUTTONDOWN, WM_LBUTTONUP, MK_LBUTTON), 'right': (WM_RBUTTONDOWN, WM_RBUTTONUP, MK_RBUTTON),
                     'middle': (WM_MBUTTONDOWN, WM_MBUTTONUP, MK_MBUTTON)}[which]
        if how in ('tap', 'down'):
            self.buttons |= mk_
            self._post(d, self.buttons, mk(self.px, self.py))
        if how == 'tap':
            time.sleep(0.15)
        if how in ('tap', 'up'):
            self.buttons &= ~mk_
            self._post(u, self.buttons, mk(self.px, self.py))
        self.say('  btn %s %s' % (which, how))

    def wheel(self, n):
        self._need_pointer('wheel')
        if self.px is None:
            self.mvto(self.w // 2, self.h // 2)
        # WM_MOUSEWHEEL: lParam is in SCREEN coordinates, but Qt posts it to the window from msg.lParam only for the position: any value is accepted
        for _ in range(abs(int(n))):
            delta = (120 if n > 0 else -120) & 0xFFFF
            self._post(WM_MOUSEWHEEL, (delta << 16) | self.buttons, mk(self.px, self.py))
            time.sleep(0.05)
        self.say('  wheel %+d' % n)

    def run(self, cmd):
        self.refresh()
        name, _, arg = cmd.partition(':')
        name = name.strip()
        if name == 'key':
            k, _, how = arg.partition(':')
            self.key(k, how or 'tap')
        elif name == 'type':
            self.type(arg)
        elif name == 'mv':
            dx, dy = arg.split(',')
            self.mv(int(dx), int(dy))
        elif name == 'mvto':
            x, y = arg.split(',')
            self.mvto(int(x), int(y))
        elif name == 'btn':
            b, _, how = arg.partition(':')
            self.btn(b, how or 'tap')
        elif name == 'wheel':
            self.wheel(int(arg))
        elif name == 'sleep':
            time.sleep(float(arg))
        else:
            raise SystemExit('usb_run: unknown command ' + cmd)


def find_display(pid):
    """The emulator's display window: top-level window of this process, Qt window class, title not 'PCSX2 v...'."""
    found = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(h, _):
        q = wt.DWORD()
        u32.GetWindowThreadProcessId(h, ctypes.byref(q))
        if q.value == pid:
            title = ctypes.create_unicode_buffer(256)
            cls = ctypes.create_unicode_buffer(256)
            u32.GetWindowTextW(h, title, 256)
            u32.GetClassNameW(h, cls, 256)
            if cls.value.startswith('Qt6') and cls.value.endswith('QWindowIcon') and title.value and not title.value.startswith('PCSX2 v') and title.value != 'pcsx2-qt':
                found.append((h, title.value))
        return True
    u32.EnumWindows(cb, 0)
    return found[0] if found else None  # (hwnd, title)


class IniPatch:
    """Temporary edit of the stand's PCSX2.ini (our private copy, held under its lock); restored in close()."""

    def __init__(self, exe):
        self.path = Path(exe).parent / 'inis' / 'PCSX2.ini'
        self.orig = self.path.read_bytes()
        self.lines = self.orig.decode('utf-8').splitlines()
        self.crlf = bytes([13]) in self.orig

    def set(self, section, key, value):
        head = '[%s]' % section
        want = '%s = %s' % (key, value)
        L = self.lines
        if head not in [x.strip() for x in L]:
            L += ['', head, want, '']
            return
        i = [x.strip() for x in L].index(head) + 1
        j = i
        while j < len(L) and not L[j].startswith('['):
            if L[j].split('=')[0].strip().lower() == key.lower():
                L[j] = want
                return
            j += 1
        L.insert(i, want)

    def apply(self):
        sep = chr(13) + chr(10) if self.crlf else chr(10)
        self.path.write_bytes((sep.join(self.lines) + sep).encode('utf-8'))

    def close(self):
        self.path.write_bytes(self.orig)


def parse_script(text):
    steps = []
    for chunk in [c for c in text.replace('\n', ';').split(';') if c.strip()]:
        t, _, cmds = chunk.strip().partition(':')
        steps.append((float(t), [c for c in re.split(r',(?=\w+:)', cmds) if c.strip()]))
    steps.sort(key=lambda s: s[0])
    return steps


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', required=True)
    ap.add_argument('--log', required=True)
    ap.add_argument('--args', default='')
    ap.add_argument('--script', default='')
    ap.add_argument('--script-file', default='')
    ap.add_argument('--ready', default='', help='log text after which the script clock starts (default: window found + 3 s)')
    ap.add_argument('--until', default='')
    ap.add_argument('--timeout', type=float, default=120)
    ap.add_argument('--lock-wait', type=float, default=900)
    ap.add_argument('--marker', default='')
    ap.add_argument('--exe', default=EXE)
    ap.add_argument('--pointer', action='store_true', help='bind the HID mouse Pointer for the run (motion + wheel); PCSX2 then warps the real cursor!')
    ap.add_argument('--set', action='append', default=[], metavar='SECTION.KEY=VALUE', help='temporary PCSX2.ini edit for the run (restored afterwards), repeatable')
    ap.add_argument('--nopad', action='store_true', help='[Pad1] Type = None for the run: the keyboard keys do not also drive the virtual DualShock')
    a = ap.parse_args()
    script = a.script + (';' + Path(a.script_file).read_text() if a.script_file else '')
    steps = parse_script(script)
    log = Path(a.log).resolve()
    log.parent.mkdir(parents=True, exist_ok=True)
    exe, lock = R.acquire_exe(a.exe, a.lock_wait)
    ini = IniPatch(exe)
    if a.pointer:
        ini.set('USB2', 'hidmouse_Pointer', 'Pointer-0')
    if a.nopad:
        ini.set('Pad1', 'Type', 'None')
    for item in a.set:
        sk, _, v = item.partition('=')
        sec, _, k = sk.partition('.')
        ini.set(sec, k, v)
    if a.pointer or a.nopad or a.set:
        ini.apply()
    try:
        log.unlink()
    except OSError:
        pass
    cmd = [exe, '-portable', '-batch', '-nogui', '-fastboot', '-elf', str(Path(a.elf).resolve()), '-logfile', str(log)]
    if a.args:
        cmd += ['-gameargs', a.args]
    code, seen = 1, False
    p = None
    try:
        si = subprocess.STARTUPINFO()
        si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        si.wShowWindow = 0
        p = subprocess.Popen(cmd, startupinfo=si, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        t0 = time.time()
        end = t0 + a.timeout
        stand = None
        clock0 = None
        nxt = 0
        while time.time() < end:
            if p.poll() is not None:
                code = p.returncode
                break
            text = log.read_text(errors='replace') if log.exists() else ''
            if stand is None:
                hw = find_display(p.pid)
                if hw:
                    h, title = hw
                    stand = Stand(h, p.pid, lambda s: print(s, flush=True), a.pointer)
                    print('usb_run: pid %d display window %d %r client %dx%d' % (p.pid, h, title, stand.w, stand.h), flush=True)
                    win_t = time.time()
            if stand is not None and clock0 is None:
                if (a.ready and a.ready in text) or (not a.ready and time.time() - win_t > 3):
                    clock0 = time.time()
                    print('usb_run: script clock starts', flush=True)
            if clock0 is not None and nxt < len(steps) and time.time() - clock0 >= steps[nxt][0]:
                t, cmds = steps[nxt]
                nxt += 1
                print('usb_run: t=%.1f %s' % (time.time() - clock0, ','.join(cmds)), flush=True)
                for c in cmds:
                    stand.run(c)
                continue
            if a.until and a.until in text:
                seen = True
                break
            time.sleep(0.05)
        if p.poll() is None:
            subprocess.run(['taskkill', '/PID', str(p.pid)], capture_output=True)
            try:
                p.wait(8)
            except subprocess.TimeoutExpired:
                p.terminate()
                try:
                    p.wait(10)
                except subprocess.TimeoutExpired:
                    p.kill()
        if seen:
            code = 0
        elif time.time() >= end:
            code = 2
        elif p.poll() is not None:
            code = p.returncode
    finally:
        if p is not None and p.poll() is None:
            p.kill()
        ini.close()  # the ini is back to the stand profile before the lock is released
        try:
            lock.unlink()
        except OSError:
            pass
    text = log.read_text(errors='replace') if log.exists() else ''
    if a.marker:
        print('\n'.join(l for l in text.splitlines() if a.marker in l))
    print('pcsx2-usb exit code', code, '(2 = timeout)')
    return code


if __name__ == '__main__':
    sys.exit(main())
