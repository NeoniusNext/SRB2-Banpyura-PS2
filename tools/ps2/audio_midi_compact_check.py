#!/usr/bin/env python3
"""Compare MIDI storage/gain changes with a saved complete source directory.

The reference is read from --baseline/src/ps2, never from git HEAD. Both paths
serialize every sequencer field and every used track/cache in the old fixed
32-track layout; unused tracks are zero. No PCM/state/read normalization is
applied. --ee builds a synthetic paired EE test/benchmark ELF, without running
an emulator. Host tests include the stock MIDI/Ogg music archive and MP3 tones.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess
import zipfile

from audio_lossless_check import MIDI_PROBE as MIDI_STATE_PROBE, MUSIC_APIS, MUSIC_PROBE, ROOT, run

MIDI_PROBE = MIDI_STATE_PROBE + r'''
size_t PS2_TestMidiBytes(unsigned count) { return MIDI_TEST_BYTES(count); }
size_t PS2_TestMidiTrackBytes(void) { return sizeof(midi_track); }
void PS2_TestMidiFlipPhase(ps2_midi *m) { m->notes[0].phase ^= 1; }
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--baseline', required=True, type=Path)
    ap.add_argument('--out', type=Path, default=ROOT / 'build/audio-midi-compact-20261114')
    ap.add_argument('--assets', type=Path, default=Path(os.environ.get('SRB2_AUDIO_ASSETS', '/workspace/.onboarding/srb2-assets')))
    ap.add_argument('--sdk', type=Path, default=Path(os.environ.get('PS2SDK', '/workspace/.onboarding/ps2dev-v2/ps2dev/ps2sdk')))
    ap.add_argument('--sanitize', action='store_true')
    ap.add_argument('--ee', action='store_true')
    a = ap.parse_args()
    a.out = a.out.resolve()
    if not a.out.is_relative_to(ROOT / 'build'):
        ap.error('--out must be under the repository build directory')
    a.out.mkdir(parents=True, exist_ok=True)
    reference = a.baseline.resolve() / 'src/ps2'
    log = ['baseline=' + str(a.baseline.resolve()), 'reference normalization: none']
    headers = ''.join((ROOT / 'src/ps2' / n).read_text() for n in ['ps2_music.h', 'ps2_midi.h'])
    api_names = sorted(set(re.findall(r'\bPS2_(?:Music|MIDI)\w+(?=\s*\()', headers)))
    decl = []
    for n in ['ps2_music.h', 'ps2_midi.h']:
        if (reference / n).read_bytes() != (ROOT / 'src/ps2' / n).read_bytes():
            ap.error('reference and candidate public headers differ: ' + n)
        for line in (ROOT / 'src/ps2' / n).read_text().splitlines():
            if re.match(r'^[^/].*\bPS2_(?:Music|MIDI)\w+\(.*;$', line):
                decl.append(MUSIC_APIS.sub(lambda m: m[0].replace('PS2_', 'Ref_', 1), line))
    (a.out / 'reference.h').write_text('\n'.join(decl) + '\n')
    common = ['-O2', '-g', '-std=gnu17', '-Wall', '-Wextra', '-Werror',
              '-I' + str(ROOT / 'src/ps2'), '-I' + str(ROOT / 'tools/ps2'), '-I' + str(a.out),
              '-I' + str(a.sdk / 'ports/include'), '-DPS2_AUDIO_VORBIS', '-DPS2_AUDIO_MP3']
    if a.sanitize:
        common += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie']
    sources = []
    for kind, srcdir in [('reference', reference), ('candidate', ROOT / 'src/ps2')]:
        for unit, probe in [('ps2_music.c', MUSIC_PROBE), ('ps2_midi.c', MIDI_PROBE)]:
            production = (srcdir / unit).read_text()
            log.append(kind + ' ' + unit + ' sha256=' + hashlib.sha256(production.encode()).hexdigest())
            if unit == 'ps2_midi.c':
                flexible = 'midi_track tracks[];' in production
                size_expr = 'sizeof(ps2_midi) + count * sizeof(midi_track)' if flexible else 'sizeof(ps2_midi) + count * 0u'
                track_expr = '(m)->count' if flexible else 'TRACKS'
                probe = '#define MIDI_TEST_BYTES(count) (' + size_expr + ')\n#define MIDI_TEST_TRACKS(m) (' + track_expr + ')\n' + probe
            if kind == 'reference':
                production = MUSIC_APIS.sub(lambda m: m[0].replace('PS2_', 'Ref_', 1), production)
                production = '\n'.join('#define %s %s' % (api, api.replace('PS2_', 'Ref_', 1)) for api in api_names) + '\n' + production
                probe = probe.replace('PS2_Test', 'Ref_Test')
            path = a.out / (kind + '_' + unit)
            path.write_text(production + probe)
            sources.append(path)
    harness = ROOT / 'tools/ps2/audio_midi_compact_hosttest.c'
    exe = a.out / 'audio_midi_compact'
    libs = ['-l:libvorbisfile.so.3', '-l:libvorbis.so.0', '-l:libogg.so.0', '-l:libmpg123.so.0', '-lm']
    cmd = ['gcc', *common, *sources, ROOT / 'src/ps2/ps2_audio.c', harness, '-Wl,--wrap=calloc', '-o', exe, *libs]
    if a.sanitize:
        cmd += ['-no-pie']
    log.append(run(cmd))
    files = []
    with zipfile.ZipFile(a.assets / 'music.pk3') as z:
        for name in sorted(z.namelist()):
            if not name.startswith('Music/') or name.endswith('/'):
                continue
            data = z.read(name)
            if not data.startswith((b'MThd', b'OggS')):
                continue
            dest = a.out / 'assets' / Path(name).name
            dest.parent.mkdir(exist_ok=True)
            dest.write_bytes(data)
            files.append(dest)
    for channels in [1, 2]:
        dest = a.out / ('tone%d.mp3' % channels)
        run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', '-f', 'lavfi', '-i',
             'sine=frequency=337:sample_rate=44100:duration=3.25', '-ac', channels,
             '-codec:a', 'libmp3lame', '-b:a', '128k', dest])
        files.append(dest)
    env = os.environ.copy()
    if a.sanitize:
        env['PS2_AUDIO_NO_BENCH'] = '1'
    output = run([exe, *files], env=env)
    print(output, end=''); log.append(output)
    for control, expected in [('--negative-control', 'na == nb && !memcmp(a, b, sizeof a)'),
                              ('--state-negative-control', 'na == nb && na <= sizeof sa && !memcmp(sa, sb, na)')]:
        neg = subprocess.run([str(exe), control], capture_output=True, text=True)
        (a.out / (control[2:] + '.log')).write_text(neg.stdout + neg.stderr)
        if neg.returncode != 1 or expected not in neg.stderr:
            raise SystemExit('negative control was not detected: ' + neg.stdout + neg.stderr)
        message = control + ' detected'
        print(message); log.append(message)
    if a.ee:
        ee = a.out / 'ee'
        ee.mkdir(exist_ok=True)
        flags = ['-O2', '-std=gnu17', '-G0', '-Wall', '-Wextra', '-Werror', '-D_EE', '-DPS2',
                 '-I' + str(ROOT / 'src/ps2'), '-I' + str(ROOT / 'tools/ps2'), '-I' + str(a.out),
                 '-I' + str(a.sdk / 'ee/include'), '-I' + str(a.sdk / 'common/include'), '-I' + str(a.sdk / 'ports/include')]
        sizes = []
        for src in sources:
            production = src.read_text().split('size_t PS2_Test')[0].split('size_t Ref_Test')[0].split('#define MIDI_TEST_BYTES')[0]
            clean = ee / src.name
            clean.write_text(production)
            for codecs in [False, True]:
                obj = clean.with_suffix('.codecs.o' if codecs else '.o')
                run(['mips64r5900el-ps2-elf-gcc', *flags, *(['-DPS2_AUDIO_VORBIS', '-DPS2_AUDIO_MP3'] if codecs else []), '-c', clean, '-o', obj])
                sizes.append(obj)
                disasm = run(['mips64r5900el-ps2-elf-objdump', '-dr', obj])
                obj.with_suffix('.dis').write_text(disasm)
        size_text = run(['mips64r5900el-ps2-elf-size', *sizes])
        print(size_text, end=''); log.append(size_text)
        objects = []
        for src in [*sources, ROOT / 'src/ps2/ps2_audio.c', harness]:
            obj = ee / ('bench_' + src.stem + '.o')
            run(['mips64r5900el-ps2-elf-gcc', *flags, '-c', src, '-o', obj])
            objects.append(obj)
        elf = ee / 'AUDIO_MIDI_COMPACT.ELF'
        run(['mips64r5900el-ps2-elf-gcc', '-T' + str(a.sdk / 'ee/startup/linkfile'), '-L' + str(a.sdk / 'ee/lib'),
             '-Wl,-zmax-page-size=128', '-Wl,--defsym,_stack_size=0x60000', '-Wl,--wrap=calloc', *objects, '-o', elf, '-ldebug', '-lpatches', '-lm'])
        message = 'EE synthetic ELF (not executed by this script): ' + str(elf)
        print(message); log.append(message)
        log.append('EE ELF sha256=' + hashlib.sha256(elf.read_bytes()).hexdigest())
    (a.out / 'report.txt').write_text('\n'.join(log))
    print('Report:', a.out / 'report.txt')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
