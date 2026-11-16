#!/usr/bin/env python3
"""Check actual optional TEXC device selection and reopen after transient faults."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

from resource_stream_opt17_hosttest import function

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'tools/ps2'


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--pak', required=True, type=Path)
    ap.add_argument('--out', required=True, type=Path)
    a = ap.parse_args()
    a.pak = a.pak.resolve()
    a.out = a.out.resolve()
    a.out.mkdir(parents=True, exist_ok=True)
    source = (ROOT / 'src/ps2/ps2_texc.c').read_text()
    state_start = source.index('static struct\n{\n\tint state;')
    state_end = source.index('} TC = { 0 };', state_start) + len('} TC = { 0 };')
    opening = source[state_start:state_end] + '\n' + ''.join(
        function(source, name) for name in
        ['static int TC_CmpKey(', 'static boolean TC_ParseKey(', 'boolean PS2TexC_Present('])
    fetch = a.out / 'fetch.inc'
    fetch.write_text(function(source, 'boolean PS2TexC_Fetch('))
    sdk = Path(os.environ.get('PS2SDK', '/workspace/.onboarding/ps2dev-v2/ps2dev/ps2sdk'))
    flags = ['cc', '-std=gnu17', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
             '-Wno-unused-function', '-Wno-sign-compare', '-Wno-unused-parameter',
             '-DPS2_PROFILE', '-DNOHW', '-DNDEBUG', '-DRESOURCE_BASELINE=0',
             '-I' + str(ROOT / 'src'), '-I' + str(ROOT / 'build/host-gen'),
             '-I' + str(sdk / 'ports/include'),
             '-DRESOURCE_FETCH_SOURCE="' + str(fetch) + '"']
    report = {'sources': {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in [ROOT / 'src/ps2/ps2_texc.c', ROOT / 'src/w_pack.c',
                                    TOOLS / 'texc_path_hosttest.c', Path(__file__).resolve()]},
              'pak_sha256': hashlib.sha256(a.pak.read_bytes()).hexdigest()}
    cases = [('primary-usb', 'mass:/games/SRB2', 'missing-home', 'usb'),
             ('fallback-sd', 'missing-primary', 'mx4sio:/games/SRB2', 'sd'),
             ('primary-dvd', 'host:/games/SRB2', 'missing-home', 'dvd')]
    work = a.out / 'filesystem'
    work.mkdir(exist_ok=True)
    for _, primary, home, _ in cases:
        selected = primary if not primary.startswith('missing') else home
        directory = work / selected
        directory.mkdir(parents=True, exist_ok=True)
        target = directory / 'TEXC.PAK'
        if not target.exists():
            target.symlink_to(a.pak)
    assert not (work / 'TEXC.PAK').exists()

    def test(label, text, pack, sanitize=False, reject=False):
        inc = a.out / (label + '-open.inc')
        inc.write_text(text)
        exe = a.out / label
        command = [*flags, '-DRESOURCE_PACK_SOURCE="' + str(pack) + '"',
                   '-DRESOURCE_TEXC_OPEN_SOURCE="' + str(inc) + '"',
                   str(TOOLS / 'texc_path_hosttest.c'), '-Wl,-l:liblz4.so.1', '-lz', '-o', str(exe)]
        if sanitize:
            command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie']
        p = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
        (a.out / (label + '-build.log')).write_text(p.stdout + p.stderr)
        assert p.returncode == 0, (label, p.stderr)
        rows = []
        for name, primary, home, policy in cases:
            selected = primary if not primary.startswith('missing') else home
            p = subprocess.run([str(exe), primary, home, policy, selected + '/TEXC.PAK'],
                               cwd=work, capture_output=True, text=True, timeout=30)
            (a.out / (label + '-' + name + '.log')).write_text(p.stdout + p.stderr)
            if reject:
                assert p.returncode == 1 and 'FAIL' in p.stderr, (label, name, p.returncode, p.stdout, p.stderr)
            else:
                assert p.returncode == 0 and 'PASS checks=' in p.stdout, (label, name, p.returncode, p.stdout, p.stderr)
            rows.append({'case': name, 'rc': p.returncode, 'stdout': p.stdout.strip(), 'stderr': p.stderr.strip()})
        report[label] = rows
        print(label, json.dumps(rows), flush=True)

    pack = ROOT / 'src/w_pack.c'
    test('plain', opening, pack)
    test('asan', opening, pack, sanitize=True)
    assert report['plain'] == report['asan']
    old = opening.replace('WPack_SetupHandleEx(f, path)', 'WPack_SetupHandle(f)').replace(
        'WPack_GetLumps(f, path,', 'WPack_GetLumps(f, "TEXC.PAK",')
    assert old != opening
    test('negative-lost-path', old, pack, reject=True)
    no_reopen = a.out / 'no-reopen-pack.c'
    pack_text = pack.read_text()
    changed = pack_text.replace('if (!s->path[0])\n\t\treturn false;', 'return false;')
    assert changed != pack_text
    no_reopen.write_text(changed)
    test('negative-no-reopen', opening, no_reopen, reject=True)
    (a.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
