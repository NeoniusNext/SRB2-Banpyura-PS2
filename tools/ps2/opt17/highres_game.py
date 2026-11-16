#!/usr/bin/env python3
"""Run one whole-game high-resolution scenario through the standard32MiB wrapper.

This tool retains all skipped-draw diagnostics. Integration success, strict
missing-draw success and timing evidence are separate fields, never FPS claims.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tools/ps2'))
from next12_runtime import run_verified


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--pak', type=Path, default=ROOT / 'build/pak')
    p.add_argument('--case', choices=[a+'-'+b for a in ('smoke', 'switch', 'heat', 'wipe', 'heat-wipe') for b in ('pal', '480p')], required=True)
    p.add_argument('--timeout', type=int, default=420)
    p.add_argument('--hwstats', type=int, default=35, help='Existing driver counters cadence; use1 for a short diagnostic')
    a = p.parse_args()
    out = a.out.resolve()
    if not out.is_relative_to(ROOT / 'build') or out == ROOT / 'build' or out.exists():
        p.error('Choose a fresh child of build/')
    out.mkdir(parents=True)
    shutil.copyfile(Path(__file__), out / 'runner-snapshot.py')
    build, pak = a.build.resolve(), a.pak.resolve()
    elf = build / 'SRB2.ELF'
    kind, output = a.case.rsplit('-', 1)
    if kind == 'smoke':
        extra = ['-zquit', '14', '-vidshot', 'l4=vid_mode~7,l8,l12,l9999']
    elif kind == 'switch':
        spec = 'l4=vid_mode~7,l14=vid_mode~9,l26=vid_mode~10,l38=vid_mode~8,l50=showmem~2,l62=showmem~3,l74=gr_paletterendering~Off,l86=gr_paletterendering~On;showmem~0,l98=renderer~Software;vid_mode~7,l116=vid_mode~10;renderer~Hardware,l138=vid_mode~0;showmem~1,l158,l9999'
        extra = ['-zquit', '175', '-vidshot', spec]
    else:
        extra = ['-zquit', '45', '-vidshot', 'l5,l25,l40,l9999', '-vidcmd', 'vid_mode~10' if output == 'pal' else 'vid_mode~9']
        if kind == 'heat-wipe':
            extra[1] = '12'
            extra[3] = 'l5,l15,l27,l9999'
            extra += ['-zchain', '02,01']
        elif kind == 'wipe':
            extra[3] = 'l5,l25,l40,l60,l100,l9999'
            extra += ['-zchain', '02,01']
    cmd = [sys.executable, '-B', str(ROOT / 'tools/ps2/opt_run.py'), '--name', a.case,
           '--elf', str(elf), '--ram', '32', '--pak', str(pak), '--out', str(out / 'runs'),
           '--map', 'MAP01' if kind in ('smoke', 'switch') else 'MAP02', '--timeout', str(a.timeout),
           '--cfg', 'showmem "On"', '--cfg', 'showfps "Full"',
           '--until', 'I_ShutdownSystem(): end of logstream.']
    inputs = [elf, build / 'build-report.json', Path(__file__), out / 'runner-snapshot.py', *sorted(pak.glob('*.PAK')), pak / 'FINEACON.DAT']
    if kind.startswith('heat'):
        script = out / 'heat.lua'
        script.write_text('addHook("MapLoad", function()\n for sec in sectors.iterate do sec.flags = $ | MSF_HEATWAVE end\n print("OPT17 HEATWAVE READY")\nend)\n')
        cmd += ['--home-file', str(script) + '=heat.lua']
        extra += ['-file', 'host:/.srb2/heat.lua']
        inputs += [script]
    cmd += ['--', '-pal' if output == 'pal' else '-480p', '-singletics', '-renderer', 'Hardware',
            '-zck', '-zstack', '-zheap', '35', '-astat', '-ps2prof', '-hwfb', '-hwstats', str(a.hwstats), *extra]
    result = run_verified(cmd, out / 'runtime', inputs)
    run = out / 'runs' / a.case
    boot = (run / 'boot.txt').read_text(errors='replace')
    summary = json.loads((run / 'summary.json').read_text())
    captures = {}
    for f in sorted(run.glob('vidshot-*.ppm')):
        raw = f.read_bytes()
        header = raw.split(b'\n', 3)
        w, h = map(int, header[1].split())
        captures[f.name] = {'sha256': sha(f), 'width': w, 'height': h, 'bytes': len(raw),
            'payload_valid': header[0] == b'P6' and header[2] == b'255' and len(header[3]) == w*h*3}
    report = {'ELF_sha256': sha(elf), 'build_report_sha256': sha(build / 'build-report.json'),
        'case': a.case, 'command': cmd, 'wrapper_exit': result.returncode, 'captures': captures,
        'fatal': [x for x in boot.splitlines() if re.search(r'I_Error|HEAP CHECK FAILED|Out of memory|WATCHDOG|^ERROR:|is not a possible value|Lua Error', x)],
        'warnings': [x for x in boot.splitlines() if 'WARNING:' in x],
        'automatic_fallback': re.findall(r'^.*HARDWARE -> SOFTWARE.*$', boot, re.M),
        'skips': [x for x in boot.splitlines() if x.startswith(('HWSKIP ', 'HWD texture not resident'))],
        'audio_errors': re.findall(r'(?:underruns|hmis|herr)=([1-9][0-9]*)', boot),
        'renderer_modes': [x for x in boot.splitlines() if re.search(r'HWE |Switching to the .*renderer|HWSTREAM |VIDSHOT |^ZCHAIN ', x)],
        'profile': [x for x in boot.splitlines() if x.startswith(('HWPROF ', 'HWSPIKE ', 'PROF ', 'TICK '))],
        'activity': re.findall(r'^SHOWMEM activity: .+$', boot, re.M), 'ZSTAT': summary['zstat'],
        'heat_ready': 'OPT17 HEATWAVE READY' in boot, 'boot_sha256': sha(run / 'boot.txt'),
        'hardware_startup': 'HWE startup renderer=Hardware chosen=2 hardware=1' in boot,
        'complete_shutdown': 'I_ShutdownSystem(): end of logstream.' in boot,
        'physical_PS2_tested': False, 'FPS_claimed': False}
    expected = 12 if kind == 'switch' else 5 if kind == 'wipe' else 3
    report['integration_passed'] = (result.returncode == 0 and not report['fatal'] and not report['automatic_fallback']
        and not report['warnings'] and report['hardware_startup'] and not report['audio_errors']
        and len(captures) == expected and all(c['payload_valid'] for c in captures.values())
        and report['complete_shutdown'] and (not kind.startswith('heat') or report['heat_ready']))
    report['strict_missing_draw_passed'] = not report['skips']
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: report[k] for k in ('case', 'ELF_sha256', 'wrapper_exit', 'integration_passed', 'strict_missing_draw_passed', 'fatal', 'automatic_fallback')}, indent=2))
    return int(not report['integration_passed'])


if __name__ == '__main__':
    raise SystemExit(main())
