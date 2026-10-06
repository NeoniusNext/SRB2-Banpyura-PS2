"""Build and run tools/ps2/ogg_conv_test.c (PS2-41: ov_read vs ov_read_float + PS2_FloatToS16 on a real Ogg, in PCSX2).

usage: python tools/ps2/build_ogg_test.py [--out DIR] [--lump O_GFZ1] [--seconds 4] [--no-run]
Extracts the music lump from srb2-assets/music.pk3 into <out>/test.ogg, builds <out>/OGGTEST.ELF with the production
flags (tools/ps2/build.py), runs it through tools/ps2/run_pcsx2.py (machine-wide emulator lock) and prints OGGCONV lines."""
import argparse
import os
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/ps2'))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=str(ROOT / 'build/opt-s/ogg-test'))
    ap.add_argument('--lump', default='O_GFZ1')
    ap.add_argument('--seconds', type=int, default=4)
    ap.add_argument('--no-run', action='store_true')
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    os.environ['SRB2_PS2_OUT'] = str(out / 'obj-tmp')
    import build as b
    z = zipfile.ZipFile(ROOT / 'srb2-assets/music.pk3')
    name = next(n for n in z.namelist() if n.split('/')[-1].upper() == a.lump)
    (out / 'test.ogg').write_bytes(z.read(name))
    flags = [f for f in b.CFLAGS if f not in ('-MMD', '-MP', '-DPS2_AUDIO_VORBIS', '-DPS2_AUDIO_MP3')]
    cmd = [str(b.CC), *flags, *b.INCS, *b.LDFLAGS, str(ROOT / 'tools/ps2/ogg_conv_test.c'), '-o', str(out / 'OGGTEST.ELF'),
           '-lvorbisfile', '-lvorbis', '-logg', '-lps2_drivers', '-lpatches', '-ldebug', '-lm']
    cmd = [c for c in cmd if c != '-Wl,--wrap=W_LumpLength']
    p = subprocess.run(cmd, env=b.ENV, capture_output=True, text=True, cwd=ROOT)
    print(p.stdout + p.stderr)
    if p.returncode:
        return p.returncode
    if a.no_run:
        return 0
    log = out / 'pcsx2.log'
    r = subprocess.run([sys.executable, str(ROOT / 'tools/ps2/run_pcsx2.py'), '--elf', str(out / 'OGGTEST.ELF'), '--log', str(log),
                        '--args=host:test.ogg ' + str(a.seconds), '--timeout', '240', '--until', 'OGGCONV PASS'], capture_output=True, text=True)
    print(r.stdout + r.stderr)
    text = log.read_text(errors='replace') if log.exists() else ''
    for l in text.splitlines():
        if 'OGGCONV' in l:
            print(l)
    return 0 if 'OGGCONV PASS' in text else 1


if __name__ == '__main__':
    sys.exit(main())
