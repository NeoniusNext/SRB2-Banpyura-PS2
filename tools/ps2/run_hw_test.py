"""Build and run the GS renderer driver hardware test (tools/ps2/hw_test.c) in PCSX2 and summarise the result.

usage: SRB2_PS2_OUT=<dir> python tools/ps2/run_hw_test.py [--no-build] [--tag NAME] [--timeout SEC] [--png] [-- test args...]
  test args (see the head of hw_test.c): fb32, negctl=N, soak=N, dump, quick
The PCSX2 log is <dir>/hwt-<tag>.log; the test's own lines (prefix 'H0 ') are copied to <dir>/hwt-<tag>.txt.
Exit code 0 only if the run reached 'H0 COMPLETE' and every check passed (with negctl the expected failures are the point:
the exit code is then the number of failed checks, and the caller looks for the specific FAIL line).
With --png the ppm dumps (the 'dump' argument) next to the ELF are converted to PNG.
"""
import argparse
import os
import subprocess
import sys
import hashlib
import json
import re
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = Path(os.environ.get('SRB2_PS2_OUT', ROOT / 'build/ps2'))


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def engine_run(a):
    if not a.pak or not a.engine_run:
        raise SystemExit('--engine-elf requires --pak and a unique --engine-run directory')
    run = Path(a.engine_run).resolve()
    if run.exists() and any(run.iterdir()):
        raise SystemExit(f'engine run directory is not empty: {run}')
    elf, pak = Path(a.engine_elf).resolve(), Path(a.pak).resolve()
    packs = [pak / name for name in ('SRB2.PAK', 'ZONES.PAK', 'CHARS.PAK', 'MUSIC.PAK')]
    before = {str(p): sha(p) for p in packs}
    run.mkdir(parents=True, exist_ok=True)
    (run / '.srb2').mkdir()
    (run / 'refout').mkdir()
    shutil.copy2(elf, run / 'SRB2.ELF')
    for p in packs:
        shutil.copy2(p, run / p.name)
    cfg = ('fpscap "35"\nfullscreen "Off"\nshowfps "No"\nshowping "Off"\n'
           'rollingdemos "Off"\nrenderer "Hardware"\n')
    (run / '.srb2/hwtest.cfg').write_text(cfg, encoding='utf-8')
    args = ['-logfile', 'boot.txt', '-config', 'hwtest.cfg', '-skipintro', '-nolog', '-noendtxt',
            '-ntsc', '-renderer', 'Hardware', '-ps2ref', 'host:/refout',
            '-hwdump', str(a.engine_frame), '-hwstats', '35', *a.test_args]
    log = run / 'pcsx2.log'
    cmd = [sys.executable, str(ROOT / 'tools/ps2/run_pcsx2.py'), '--elf', str(run / 'SRB2.ELF'),
           '--log', str(log), '--timeout', str(a.timeout), '--wait-for-exit', '--args=' + ' '.join(args)]
    print(subprocess.list2cmdline(cmd))
    p = subprocess.run(cmd, capture_output=True, text=True)
    (run / 'wrapper.txt').write_text(p.stdout + p.stderr, encoding='utf-8')
    boot = (run / 'boot.txt').read_text(errors='replace') if (run / 'boot.txt').exists() else ''
    text = log.read_text(errors='replace') if log.exists() else ''
    alltext = boot + '\n' + text
    images = list(run.glob(f'hwdump-{a.engine_frame}.ppm'))
    errors = []
    if p.returncode:
        errors.append(f'PCSX2 wrapper exit {p.returncode}')
    if not re.search(r'HWE startup renderer=Hardware chosen=2 hardware=1', alltext):
        errors.append('Hardware startup not confirmed')
    signature = re.search(r'HWE title readback renderer=Hardware driver=ps2_hwd title=1 frame=(\d+) total_hw=(\d+) gs_frames=(\d+) polys=(\d+) uploads=(\d+) missing=(\d+) timeouts=(\d+) limitations=0x([0-9a-f]+)', alltext)
    if signature is None:
        errors.append('real HW title/readback signature missing')
    elif int(signature[1]) != a.engine_frame or min(int(signature[i]) for i in (3, 4, 5)) <= 0 or int(signature[6]) or int(signature[7]):
        errors.append('HW title signature reports incomplete/failed rendering')
    if f'HWE COMPLETE title Hardware frame={a.engine_frame}' not in alltext:
        errors.append('title completion missing')
    if 'PS2BOOT exit code=0 poweroff=1' not in text:
        errors.append('successful engine poweroff missing')
    if re.search(r'I_Error\(\):|HWE FATAL:|PS2BOOT exit code=-', alltext):
        errors.append('fatal engine error in logs')
    image_info = None
    if len(images) != 1:
        errors.append('screenshot missing')
    else:
        image = images[0].read_bytes()
        header = b'P6\n320 200\n255\n'
        pixels = image[len(header):]
        if not image.startswith(header) or len(pixels) != 320 * 200 * 3:
            errors.append('invalid screenshot dimensions/payload')
        colours = len({pixels[i:i+3] for i in range(0, len(pixels), 3)})
        if colours < 16:
            errors.append('screenshot is blank or nearly uniform')
        image_info = {'path': str(images[0]), 'sha256': sha(images[0]), 'bytes': len(image), 'colours': colours}
    after = {str(p): sha(p) for p in packs}
    if before != after:
        errors.append('source cooked packs changed')
    report = {'command': cmd, 'wrapper_exit': p.returncode, 'elf_sha256': sha(run / 'SRB2.ELF'),
              'elf_bytes': (run / 'SRB2.ELF').stat().st_size, 'config_input': cfg,
              'packs_before': before, 'packs_after': after, 'signature': signature.group(0) if signature else None,
              'image': image_info, 'errors': errors, 'passed': not errors,
              'scope': 'engine HW title/readback, not PC HW parity'}
    build_report = elf.parent / 'build-report.json'
    if build_report.exists():
        shutil.copy2(build_report, run / 'engine-build-report.json')
    (run / 'engine-report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    for line in boot.splitlines():
        if any(marker in line for marker in ('HWE ', 'HW mem ', 'I_Error():', 'PS2 GS ', '.PAK')):
            print(line)
    print(json.dumps({'passed': report['passed'], 'errors': errors, 'image': image_info}, indent=2))
    return 1 if errors else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--tag', default='run')
    ap.add_argument('--timeout', type=float, default=600)
    ap.add_argument('--png', action='store_true')
    ap.add_argument('--engine-elf', help='run a full HW engine ELF against cooked packs; no golden software comparison')
    ap.add_argument('--engine-run', help='new isolated engine run directory (must be empty)')
    ap.add_argument('--pak', help='cooked pack input directory, read-only')
    ap.add_argument('--engine-frame', type=int, default=105)
    ap.add_argument('test_args', nargs='*')
    a = ap.parse_args()
    if a.engine_elf:
        return engine_run(a)
    env = dict(os.environ, SRB2_PS2_HW='1')
    if not a.no_build:
        p = subprocess.run([sys.executable, str(ROOT / 'tools/ps2/build_hw_test.py')], env=env)
        if p.returncode:
            return 1
    log = OUT / f'hwt-{a.tag}.log'
    cmd = [sys.executable, str(ROOT / 'tools/ps2/run_pcsx2.py'), '--elf', str(OUT / 'HW_TEST.ELF'), '--log', str(log),
           '--timeout', str(a.timeout), '--until', 'H0 COMPLETE', '--args', ' '.join(a.test_args)]
    p = subprocess.run(cmd, env=env, capture_output=True, text=True)
    text = log.read_text(errors='replace') if log.exists() else ''
    lines = [l.split('] ', 1)[-1] for l in text.splitlines() if ' H0 ' in l]
    (OUT / f'hwt-{a.tag}.txt').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    for l in lines:
        print(l[:300])
    if a.png:
        try:
            from PIL import Image
            for ppm in OUT.glob('hw-*.ppm'):
                Image.open(ppm).save(ppm.with_suffix('.png'))
        except ImportError:
            print('PIL missing: no PNG conversion')
    done = [l for l in lines if l.startswith('H0 COMPLETE')]
    (OUT / f'hwt-{a.tag}.json').write_text(json.dumps({
        'command': cmd, 'wrapper_exit': p.returncode,
        'complete': done, 'pass': [l for l in lines if l.startswith('H0 PASS ')],
        'fail': [l for l in lines if l.startswith('H0 FAIL ')],
        'elf_sha256': hashlib.sha256((OUT / 'HW_TEST.ELF').read_bytes()).hexdigest(),
    }, indent=2), encoding='utf-8')
    if not done:
        print('NO COMPLETE LINE (timeout or crash); pcsx2 wrapper exit', p.returncode)
        return 2
    match = re.fullmatch(r'H0 COMPLETE checks=(\d+) failures=(\d+)', done[0])
    if match is None or len(done) != 1:
        print('invalid completion marker')
        return 2
    checks, fails = map(int, match.groups())
    if checks != len([l for l in lines if l.startswith(('H0 PASS ', 'H0 FAIL '))]) or fails != len([l for l in lines if l.startswith('H0 FAIL ')]):
        print('inconsistent test totals')
        return 2
    if p.returncode:
        print('PCSX2 wrapper failed despite completion marker:', p.returncode)
        return 2
    return 0 if fails == 0 else min(fails, 100)


if __name__ == '__main__':
    sys.exit(main())
