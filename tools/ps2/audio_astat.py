"""Collect the ASTAT audio lines of PS2 engine runs (boot.txt written by opt_run.py) into one table / JSON.

usage: python tools/ps2/audio_astat.py <run dir> [<run dir> ...] [--json out.json]
A run dir is build/.../run/<name> (contains boot.txt). The last "ASTAT end" line (or the last ASTAT line of any kind) is the
result of the run; every "ASTAT evt" line (an underrun was observed) is listed with its position in the log.
Fields (new engine; the old single-thread code prints calls/maxint_ms = longest gap between two I_UpdateSound calls):
underruns = times the audsrv queue was found empty while audio was being produced, gapmax_ms/gaptotal_ms = how long
it stayed empty, emptyobs = empty observations (including those with a zero measured gap), mstarve = music blocks the mixer
needed and the decoder had not delivered, maxint_ms = longest time between two refills, minq_ms = smallest queue seen.
"""
import json
import re
import sys
from pathlib import Path


def parse(boot):
    lines = Path(boot).read_text(errors='replace').splitlines() if Path(boot).exists() else []
    result = {'events': 0, 'last': None, 'end': None, 'threads': None, 'probe': [], 'level_frames': None, 'reloads': 0}
    for line in lines:
        if line.startswith('ASTAT probe'):
            result['probe'].append(line)
        m = re.match(r'ASTAT (\w+) (.*)', line)
        if not m:
            continue
        tag, rest = m.groups()
        if tag == 'reload':
            result['reloads'] += 1
            continue
        if 'threads prio' in rest:
            result['threads'] = rest
            continue
        kv = dict(x.split('=', 1) for x in rest.split() if '=' in x)
        if 'underruns' not in kv:
            continue
        kv['mode'] = kv.get('mode', '?')
        if tag == 'evt':
            result['events'] += 1
        result['last'] = kv
        if tag in ('end', 'final'):
            result['end'] = kv
    return result


def main(argv):
    out = None
    dirs = []
    i = 0
    while i < len(argv):
        if argv[i] == '--json':
            out = argv[i + 1]
            i += 2
            continue
        dirs.append(argv[i])
        i += 1
    table = {}
    cols = ['mode', 'target', 'underruns', 'emptyobs', 'gapmax_ms', 'gaptotal_ms', 'mstarve', 'maxint_ms', 'maingap_ms', 'gaps100',
            'minq_ms', 'blocks', 'mixavg_cyc', 'hmis', 'decmax_us']
    print('%-28s ' % 'run' + ' '.join('%11s' % c for c in cols) + '  evts')
    for d in dirs:
        r = parse(Path(d) / 'boot.txt')
        k = r['end'] or r['last']
        table[Path(d).name] = r
        if not k:
            print('%-28s no ASTAT line' % Path(d).name)
            continue
        print('%-28s ' % Path(d).name + ' '.join('%11s' % k.get(c, '-') for c in cols) + '  %d' % r['events'])
        if r['threads']:
            print('%-28s %s' % ('', r['threads']))
    if out:
        Path(out).write_text(json.dumps(table, indent=1))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
