"""OPT14 (PS2-LUA-4): checks the boot.txt of a PS2 run of tools/ps2/luatests/lt_heap.lua: after every round of garbage that the script dropped and collected, the Lua heap of the zone ("Lua heap" line of
memfree) must be near what Lua says is live ("LT heap <tag> <KB>" lines), not the high-water mark of the earlier rounds.

usage: lua_heapcheck.py boot.txt [--slack 1.35]      exit 0: every "..._dropped" / "again_" round has heap <= live * slack + 512 KB
"""
import re
import sys


def main():
    path = sys.argv[1]
    slack = float(sys.argv[sys.argv.index('--slack') + 1]) if '--slack' in sys.argv else 1.35
    lines = open(path, errors='replace').read().splitlines()
    rows = []
    live = None
    for l in lines:
        m = re.match(r'LT heap (\S+) (\d+)', l)
        if m:
            live = (m.group(1), int(m.group(2)))
            continue
        m = re.match(r'Lua heap\s*:\s*(\d+) KB', l)
        if m and live:
            rows.append((live[0], live[1], int(m.group(1))))
            live = None
    bad = 0
    for tag, lua_kb, zone_kb in rows:
        limit = lua_kb * slack + 512
        ok = zone_kb <= limit or not (tag.endswith('dropped') or tag.startswith('again'))
        print(f'{tag:24s} lua live {lua_kb:6d} KB   zone {zone_kb:6d} KB   limit {limit:8.0f} KB   {"ok" if ok else "TOO MUCH"}')
        bad += 0 if ok else 1
    if not rows:
        print('no rows: run lt_heap.lua with memfree on the PS2 build')
        return 2
    print('RESULT', 'OK' if not bad else 'FAIL')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
