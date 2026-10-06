"""Host audio tests and strict EE checks, isolated in build/ps2-audio-check.

python tools/ps2/audio_check.py [--host-only | --ee-only]
Does not change the production source/build lists. Codec options are checked both
disabled and enabled. --ee-only also links a standalone audsrv/decoder smoke ELF.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import re

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/ps2-audio-check'
SOURCES = ['src/ps2/ps2_audio.c', 'src/ps2/ps2_athread.c', 'src/ps2/ps2_music.c', 'src/ps2/ps2_midi.c']


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    modes = ap.add_mutually_exclusive_group()
    modes.add_argument('--host-only', action='store_true')
    modes.add_argument('--ee-only', action='store_true')
    a = ap.parse_args()
    if not OUT.parent.is_dir():
        raise SystemExit(f'missing output parent: {OUT.parent}')
    OUT.mkdir(exist_ok=True)
    if not a.ee_only:
        import math_common as c
        units = [(ROOT / p, Path(p).stem + '.obj', ['/I' + str(ROOT / 'src/ps2')])
                 for p in [*SOURCES, 'tools/ps2/audio_hosttest.c']]
        exe = c.msvc_build(OUT / 'host', 'audio_hosttest', units, cl_extra=['/W4', '/WX', '/wd4324'])
        rc, text = c.run(exe, log=OUT / 'host/test.log')
        print(text, end='')
        if rc:
            return rc
        backend = OUT / 'backend'
        backend.mkdir(exist_ok=True)
        text = (ROOT / 'src/ps2/i_sound.c').read_text()
        text = re.sub(r'^#include "(?:\.\./[^\"]+|ps2_boot\.h)"\n', '', text, flags=re.M)
        (backend / 'i_sound_host.c').write_text(text)
        flags = ['/I' + str(ROOT / 'src/ps2'), '/I' + str(ROOT / 'tools/ps2'),
                 '/I' + str(backend), '/I' + 'D:/ps2dev/ps2sdk/ee/include']
        units = [(ROOT / p, Path(p).stem + '.obj', flags)
                 for p in [*SOURCES, 'tools/ps2/audio_backend_hosttest.c']]
        exe = c.msvc_build(backend, 'audio_backend_hosttest', units, cl_extra=['/W4', '/WX', '/wd4200', '/wd4324'])
        rc, text = c.run(exe, log=backend / 'test.log')
        print(text, end='')
        if rc:
            return rc
    if not a.host_only:
        os.environ['SRB2_PS2_OUT'] = str(OUT / 'ee')
        spec = importlib.util.spec_from_file_location('audio_build_config', ROOT / 'tools/ps2/build.py')
        b = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(b)
        b.OBJ.mkdir(parents=True, exist_ok=True)
        b.gen_config()
        flags = [f for f in b.CFLAGS if f not in
                 ('-MMD', '-MP', '-DPS2_AUDIO_VORBIS', '-DPS2_AUDIO_MP3')] + ['-Werror']
        log = []
        for codec in [[], ['-DPS2_AUDIO_VORBIS', '-DPS2_AUDIO_MP3']]:
            for p in [*SOURCES, 'src/ps2/i_sound.c', 'src/ps2/ps2_boot.c', 'tools/ps2/audio_smoke.c']:
                cmd = [str(b.CC), *flags, *codec, *b.INCS, '-fsyntax-only', str(ROOT / p)]
                r = subprocess.run(cmd, env=b.ENV, cwd=ROOT, capture_output=True, text=True)
                log.append(' '.join(cmd) + '\n' + r.stdout + r.stderr)
                if r.returncode:
                    (OUT / 'ee/syntax.log').write_text('\n'.join(log))
                    print(r.stdout + r.stderr)
                    return r.returncode
        (OUT / 'ee/syntax.log').write_text('\n'.join(log))
        print('EE: 12 strict syntax checks PASS (codecs enabled/disabled)')
        objects = []
        for p in [*SOURCES, 'tools/ps2/audio_smoke.c']:
            obj = OUT / 'ee' / (Path(p).stem + '.o')
            cmd = [str(b.CC), *flags, '-DPS2_AUDIO_VORBIS', '-DPS2_AUDIO_MP3', *b.INCS,
                   '-c', str(ROOT / p), '-o', str(obj)]
            r = subprocess.run(cmd, env=b.ENV, cwd=ROOT, capture_output=True, text=True)
            if r.returncode:
                print(r.stdout + r.stderr)
                return r.returncode
            objects.append(str(obj))
        cmd = [str(b.CC), *b.LDFLAGS, *objects, '-o', str(OUT / 'ee/AUDIO.ELF'),
               '-lps2_drivers', '-laudsrv', '-lpatches', '-lvorbisfile', '-lvorbis', '-logg', '-lmpg123', '-lm']
        r = subprocess.run(cmd, env=b.ENV, cwd=ROOT, capture_output=True, text=True)
        (OUT / 'ee/link.log').write_text(' '.join(cmd) + '\n' + r.stdout + r.stderr)
        if r.returncode:
            print(r.stdout + r.stderr)
            return r.returncode
        print('EE: real audsrv + Vorbis/Ogg + mpg123 + MIDI smoke ELF link PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
