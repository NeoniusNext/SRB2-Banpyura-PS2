"""Join timeline of a PS2 client from the host-clock stamps (<node>/boot.txt.ts) that net_session.py writes: seconds from the first engine line.

usage: python3 tools/ps2/net_join.py RUNDIR [node=cli]
Prints the milestones of a connection: network modules, link, DHCP, the ready screen, "Contacting the server", the savegame download, "Loading savegame", the map, "joined the game", the first NETSYNC.
"""
import sys
from pathlib import Path

KEYS = [('Loading the network modules', 'modules'), ('Waiting for the Ethernet link', 'link wait'), ('Link is up', 'link up'), ('Waiting for the DHCP server', 'dhcp wait'),
        ('DHCP lease received', 'dhcp lease'), ('NETUI ready at', 'ready screen'), ('receive thread started', 'rx thread'), ('Contacting the server', 'contacting'),
        ('Sending join request', 'join request'), ('Join accepted', 'join accepted'), ('Downloading addon', 'download start'), ('Finished download', 'download end'),
        ('Loading savegame', 'loading save'), ('Map is now', 'map loaded'), ('has joined the game', 'joined'), ('NETSYNC gametic=35 ', 'first netsync')]


def main():
    run = Path(sys.argv[1])
    node = sys.argv[2] if len(sys.argv) > 2 else 'cli'
    f = run / node / ('boot.txt.ts' if (run / node / 'boot.txt.ts').exists() else 'out.txt.ts')
    lines = f.read_text(errors='replace').splitlines()
    t0 = float(lines[0].split()[0])
    seen = set()
    prev = None
    for ln in lines:
        ts, _, rest = ln.partition(' ')
        for k, name in KEYS:
            if k in rest and name not in seen:
                seen.add(name)
                t = float(ts) - t0
                print(f'{t:8.2f} s  (+{(t - prev) if prev is not None else 0:6.2f})  {name}')
                prev = t


if __name__ == '__main__':
    main()
