"""Real codec host checks using bundled x64 DLLs and stock PK3 audio assets.

No downloads. Builds import libraries only inside build/ps2-audio-check/codecs.
Checks all music Ogg/MIDI lumps and all stock DMX/WAV/Ogg effects.
The MP3 fixture is 32 valid MPEG-1 layer-III silent frames (128 kb/s, 44.1 kHz).
"""
import ctypes
import os
from pathlib import Path
import subprocess
import sys
import zipfile

import math_common as C
from audio_check import SOURCES

ROOT = C.ROOT
OUT = ROOT / 'build/ps2-audio-check/codecs'
DLL = ROOT / 'libs/SDL2_mixer/lib/x64'
EXPORTS = {
    'libvorbisfile-3': 'ov_open_callbacks ov_info ov_streams ov_time_total ov_comment ov_clear ov_pcm_seek ov_read ov_halfrate',
    'libvorbis-0': 'vorbis_comment_query',
    'libmpg123-0': 'mpg123_init mpg123_new mpg123_param mpg123_format_none mpg123_format mpg123_replace_reader_handle mpg123_open_handle mpg123_getformat mpg123_length mpg123_seek mpg123_delete mpg123_read',
}


def main():
    if not OUT.parent.is_dir():
        raise SystemExit(f'missing output parent: {OUT.parent}; run audio_check.py first')
    OUT.mkdir(exist_ok=True)
    os.environ['PATH'] = str(DLL) + os.pathsep + os.environ['PATH']
    lines = ['@echo off', f'call "{C.VCVARS}" x64 >nul 2>&1', 'if errorlevel 1 exit /b 1']
    with os.add_dll_directory(str(DLL)):
        for name, symbols in EXPORTS.items():
            lib = ctypes.CDLL(str(DLL / (name + '.dll')))
            for symbol in symbols.split():
                getattr(lib, symbol)  # prove exports exist before generating imports
            (OUT / (name + '.def')).write_text('LIBRARY ' + name + '.dll\nEXPORTS\n' + '\n'.join(symbols.split()) + '\n')
            lines += [f'lib /nologo /machine:x64 /def:{name}.def /out:{name}.lib', 'if errorlevel 1 exit /b 1']
    bat = OUT / 'imports.bat'
    bat.write_text('\n'.join(lines) + '\n')
    r = subprocess.run(['cmd', '/c', str(bat)], cwd=OUT, capture_output=True, text=True)
    (OUT / 'imports.log').write_text(r.stdout + r.stderr)
    if r.returncode:
        print(r.stdout + r.stderr)
        return r.returncode
    flags = ['/I' + str(ROOT / 'src/ps2'), '/ID:/ps2dev/ps2sdk/ports/include',
             '/DPS2_AUDIO_VORBIS', '/DPS2_AUDIO_MP3', '/DMPG123_ENUM_API']
    units = [(ROOT / p, Path(p).stem + '.obj', flags)
             for p in [*SOURCES, 'tools/ps2/audio_codec_hosttest.c']]
    exe = C.msvc_build(OUT, 'audio_codec_hosttest', units,
                       link_extra=[str(OUT / (n + '.lib')) for n in EXPORTS], cl_extra=['/W4', '/WX', '/wd4324'])
    backend = ROOT / 'build/ps2-audio-check/backend'
    if not (backend / 'i_sound_host.c').is_file():
        raise SystemExit('run audio_check.py first (backend test source required)')
    backend_flags = [*flags, '/I' + str(ROOT / 'tools/ps2'), '/I' + str(backend),
                     '/ID:/ps2dev/ps2sdk/ee/include']
    units = [(ROOT / p, Path(p).stem + '-backend.obj', backend_flags)
             for p in [*SOURCES, 'tools/ps2/audio_backend_hosttest.c']]
    effect_exe = C.msvc_build(OUT, 'audio_effect_hosttest', units,
                             link_extra=[str(OUT / (n + '.lib')) for n in EXPORTS],
                             cl_extra=['/W4', '/WX', '/wd4200', '/wd4324'])
    fixtures = []
    effects = []
    for archive in ['music.pk3', 'srb2.pk3']:
        with zipfile.ZipFile(ROOT / 'srb2-assets' / archive) as z:
            candidates = []
            for name in z.namelist():
                if name.endswith('/'):
                    continue
                if archive == 'srb2.pk3' and not name.lower().startswith('sounds/'):
                    continue
                data = z.read(name)
                if data[:4] in (b'OggS', b'MThd') or (archive == 'srb2.pk3' and
                                                     (data[:4] == b'RIFF' or data[:2] == b'\x03\0')):
                    candidates.append((name, data))
            for i, (name, data) in enumerate(candidates):
                p = OUT / f'{archive}-{i}.audio'
                p.write_bytes(data)
                (effects if archive == 'srb2.pk3' else fixtures).append(p)
    mp3 = OUT / 'silent.mp3'
    mp3.write_bytes((b'\xff\xfb\x90\x00' + bytes(413)) * 32)
    fixtures.append(mp3)
    # Avoid the Windows command-line length limit by running batches.
    logs = []
    for first in range(0, len(fixtures), 60):
        rc, text = C.run(exe, [str(p) for p in fixtures[first:first+60]])
        logs.append(text)
        print(text, end='')
        if rc:
            (OUT / 'test.log').write_text(''.join(logs))
            return rc
    for first in range(0, len(effects), 60):
        rc, text = C.run(effect_exe, [str(p) for p in effects[first:first+60]])
        logs.append(text)
        print(text, end='')
        if rc:
            (OUT / 'test.log').write_text(''.join(logs))
            return rc
    (OUT / 'test.log').write_text(''.join(logs))
    return 0


if __name__ == '__main__':
    sys.exit(main())
