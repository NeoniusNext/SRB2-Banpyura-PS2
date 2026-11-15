#!/usr/bin/env python3
"""Compare music codec storage with a saved complete source directory.

The reference is read from --baseline/src/ps2, never from git HEAD. Both paths
serialize every sequencer field and every used track/cache in the old fixed
32-track layout; unused tracks are zero. No PCM/state/read normalization is
applied. --ee builds a paired EE proof with real codecs and embedded selected
inputs, without running an emulator. Host tests include all stock MIDI/Ogg
music and MP3 tones. Allocation probes test actual requested bytes and guard
the complete initialized storage; they do not substitute an allocator model.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import zipfile

from audio_lossless_check import MIDI_PROBE as MIDI_STATE_PROBE, MUSIC_APIS, MUSIC_PROBE as MUSIC_STATE_PROBE, ROOT, run

MUSIC_PROBE = MUSIC_STATE_PROBE + r'''
void PS2_TestMusicFlipCursor(ps2_music *m) { m->cursor ^= 1; }
unsigned PS2_TestMusicStep(const ps2_music *m) { return m->step; }
size_t PS2_TestOggBytes(void) {
#ifdef PS2_AUDIO_VORBIS
    return sizeof(OggVorbis_File);
#else
    return 0;
#endif
}
size_t PS2_TestMusicAllocationBytes(unsigned ogg) {
    size_t n = sizeof(ps2_music);
#if defined(PS2_AUDIO_VORBIS) && MUSIC_TEST_FLEX_OGG
    if (ogg) n += sizeof(OggVorbis_File);
#else
    (void)ogg;
#endif
    return n;
}
int PS2_TestMusicBufferZero(const ps2_music *m) {
    size_t i;
    if ((uintptr_t)m->buffer & 15) return 0;
    for (i = 0; i < DECODE_FRAMES * 2; i++) if (m->buffer[i]) return 0;
    return 1;
}
'''

STORAGE_MUTATIONS = [('tail-zero', 'memset(m, 0, bytes);', 'memset(m, 0, sizeof *m);', 2),
                     ('tail-size', 'bytes += sizeof m->ogg[0];', 'bytes += sizeof m->ogg[0] - 16;', 2)]

MIDI_PROBE = MIDI_STATE_PROBE + r'''
size_t PS2_TestMidiBytes(unsigned count) { return MIDI_TEST_BYTES(count); }
size_t PS2_TestMidiTrackBytes(void) { return sizeof(midi_track); }
'''


def ogg_pages(data, *, serial_xor=0):
    """Recompute real Ogg page CRCs after a bounded identification mutation."""
    data = bytearray(data)
    pos = 0
    while pos < len(data):
        if data[pos:pos + 4] != b'OggS' or pos + 27 > len(data):
            raise SystemExit('invalid source Ogg page')
        segments = data[pos + 26]
        end = pos + 27 + segments + sum(data[pos + 27:pos + 27 + segments])
        if end > len(data):
            raise SystemExit('truncated source Ogg page')
        if serial_xor:
            serial = int.from_bytes(data[pos + 14:pos + 18], 'little') ^ serial_xor
            data[pos + 14:pos + 18] = serial.to_bytes(4, 'little')
        data[pos + 22:pos + 26] = b'\0' * 4
        crc = 0
        for value in data[pos:end]:
            crc ^= value << 24
            for _ in range(8):
                crc = ((crc << 1) ^ (0x04c11db7 if crc & 0x80000000 else 0)) & 0xffffffff
        data[pos + 22:pos + 26] = crc.to_bytes(4, 'little')
        pos = end
    return bytes(data)


def malformed_ogg(data):
    marker = data.find(b'\x01vorbis')
    if marker < 0:
        raise SystemExit('source identification packet missing')
    result = [('truncated-%d.ogg' % n, data[:n]) for n in (12, 27, 64, 256, 1024, len(data) // 2, len(data) - 1)]
    for label, offset, value in [('version', 7, (1).to_bytes(4, 'little')),
                                  ('channels', 11, b'\x03'), ('rate', 12, (3000).to_bytes(4, 'little'))]:
        changed = bytearray(data)
        changed[marker + offset:marker + offset + len(value)] = value
        result.append(('invalid-%s.ogg' % label, ogg_pages(changed)))
    result.append(('chained.ogg', data + ogg_pages(data, serial_xor=1)))
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--baseline', required=True, type=Path)
    ap.add_argument('--candidate', type=Path, default=ROOT, help='root containing candidate src/ps2 (default: checkout)')
    ap.add_argument('--out', type=Path, default=ROOT / 'build/nov14-next7/audio-storage')
    ap.add_argument('--assets', type=Path, default=Path(os.environ.get('SRB2_AUDIO_ASSETS', '/workspace/.onboarding/srb2-assets')))
    ap.add_argument('--sdk', type=Path, default=Path(os.environ.get('PS2SDK', '/workspace/.onboarding/ps2dev-v2/ps2dev/ps2sdk')))
    ap.add_argument('--sanitize', action='store_true')
    ap.add_argument('--ee', action='store_true')
    ap.add_argument('--ee-only', action='store_true', help='build target proof without rerunning host corpus')
    a = ap.parse_args()
    if a.ee_only:
        a.ee = True
    a.out = a.out.resolve()
    if not a.out.is_relative_to(ROOT / 'build'):
        ap.error('--out must be under the repository build directory')
    a.out.mkdir(parents=True, exist_ok=True)
    reference = a.baseline.resolve() / 'src/ps2'
    candidate = a.candidate.resolve() / 'src/ps2'
    log = ['baseline=' + str(a.baseline.resolve()), 'reference normalization: none']
    names = ('ps2_audio.c', 'ps2_audio.h', 'ps2_music.c', 'ps2_music.h', 'ps2_midi.c', 'ps2_midi.h',
             'ps2_athread.c', 'ps2_athread.h', 'ps2_pcmconv.h')
    manifest = {kind: {name: hashlib.sha256((src / name).read_bytes()).hexdigest() for name in names}
                for kind, src in [('reference', reference), ('candidate', candidate)]}
    for name in names:
        if name != 'ps2_music.c' and manifest['reference'][name] != manifest['candidate'][name]:
            ap.error('surrounding audio source differs: ' + name)
    manifest['fixtures'] = {name: hashlib.sha256((ROOT / 'tools/ps2' / name).read_bytes()).hexdigest()
                            for name in ('audio_storage_next7_check.py', 'audio_storage_next7_hosttest.c', 'audio_refill_next6_hosttest.c',
                                         'audio_lossless_check.py', 'audio_lossless_hosttest.c')}
    manifest['baseline_path'] = str(a.baseline.resolve())
    manifest['candidate_path'] = str(a.candidate.resolve())
    (a.out / 'source-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    headers = ''.join((candidate / n).read_text() for n in ['ps2_music.h', 'ps2_midi.h'])
    api_names = sorted(set(re.findall(r'\bPS2_(?:Music|MIDI)\w+(?=\s*\()', headers)))
    decl = []
    for n in ['ps2_music.h', 'ps2_midi.h']:
        if (reference / n).read_bytes() != (candidate / n).read_bytes():
            ap.error('reference and candidate public headers differ: ' + n)
        for line in (candidate / n).read_text().splitlines():
            if re.match(r'^[^/].*\bPS2_(?:Music|MIDI)\w+\(.*;$', line):
                decl.append(MUSIC_APIS.sub(lambda m: m[0].replace('PS2_', 'Ref_', 1), line))
    (a.out / 'reference.h').write_text('\n'.join(decl) + '\n')
    common = ['-O2', '-g', '-std=gnu17', '-Wall', '-Wextra', '-Werror',
              '-I' + str(candidate), '-I' + str(ROOT / 'tools/ps2'), '-I' + str(a.out),
              '-I' + str(a.sdk / 'ports/include'), '-DPS2_AUDIO_VORBIS', '-DPS2_AUDIO_MP3']
    if a.sanitize:
        common += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie']
    sources = []
    for kind, srcdir in [('reference', reference), ('candidate', candidate)]:
        for unit, probe in [('ps2_music.c', MUSIC_PROBE), ('ps2_midi.c', MIDI_PROBE)]:
            production = (srcdir / unit).read_text()
            log.append(kind + ' ' + unit + ' sha256=' + hashlib.sha256(production.encode()).hexdigest())
            if unit == 'ps2_midi.c':
                flexible = 'midi_track tracks[];' in production
                size_expr = 'sizeof(ps2_midi) + count * sizeof(midi_track)' if flexible else 'sizeof(ps2_midi) + count * 0u'
                track_expr = '(m)->count' if flexible else 'TRACKS'
                probe = '#define MIDI_TEST_BYTES(count) (' + size_expr + ')\n#define MIDI_TEST_TRACKS(m) (' + track_expr + ')\n' + probe
            if unit == 'ps2_music.c':
                flexible = 'OggVorbis_File ogg[];' in production
                probe = '#define MUSIC_TEST_FLEX_OGG ' + str(int(flexible)) + '\n' + probe
            if kind == 'reference':
                production = MUSIC_APIS.sub(lambda m: m[0].replace('PS2_', 'Ref_', 1), production)
                production = '\n'.join('#define %s %s' % (api, api.replace('PS2_', 'Ref_', 1)) for api in api_names) + '\n' + production
                probe = probe.replace('PS2_Test', 'Ref_Test')
            path = a.out / (kind + '_' + unit)
            path.write_text(production + probe)
            sources.append(path)
    harness = ROOT / 'tools/ps2/audio_storage_next7_hosttest.c'
    if not a.ee_only:
        exe = a.out / 'audio_storage_next7'
        libs = ['-l:libvorbisfile.so.3', '-l:libvorbis.so.0', '-l:libogg.so.0', '-l:libmpg123.so.0', '-lm']
        cmd = ['gcc', *common, *sources, candidate / 'ps2_audio.c', harness, '-Wl,--wrap=calloc', '-Wl,--wrap=memalign', '-Wl,--wrap=free', '-Wl,--wrap=ov_open_callbacks', '-Wl,--wrap=ov_clear', '-o', exe, *libs]
        if a.sanitize:
            cmd += ['-no-pie']
        log.append(run(cmd))
        files = []
        ogg_seed = None
        with zipfile.ZipFile(a.assets / 'music.pk3') as z:
            for name in sorted(z.namelist()):
                if not name.startswith('Music/') or name.endswith('/'):
                    continue
                data = z.read(name)
                if not data.startswith((b'MThd', b'OggS')):
                    continue
                if data.startswith(b'OggS') and (ogg_seed is None or len(data) < len(ogg_seed)):
                    ogg_seed = data
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
        for name, data in malformed_ogg(ogg_seed):
            dest = a.out / 'malformed' / name
            dest.parent.mkdir(exist_ok=True)
            dest.write_bytes(data)
            files.append(dest)
        env = os.environ.copy()
        if a.sanitize:
            env['PS2_AUDIO_NO_BENCH'] = '1'
        output = run([exe, *files], env=env)
        print(output, end=''); log.append(output)
        for control, expected in [('--negative-control', 'na == nb && !memcmp(a, b, sizeof a)'),
                                  ('--state-negative-control', '!memcmp(a, b, sizeof a)')]:
            neg = subprocess.run([str(exe), control], capture_output=True, text=True)
            (a.out / (control[2:] + '.log')).write_text(neg.stdout + neg.stderr)
            if neg.returncode != 1 or expected not in neg.stderr:
                raise SystemExit('negative control was not detected: ' + neg.stdout + neg.stderr)
            message = control + ' detected'
            print(message); log.append(message)
        for label, old, new, index in STORAGE_MUTATIONS:
            text = sources[index].read_text()
            if old not in text:
                continue
            if text.count(old) != 1:
                raise SystemExit('ambiguous mutation: ' + label)
            mutated = a.out / (label + '.c')
            mutated.write_text(text.replace(old, new))
            mutated_sources = list(sources)
            mutated_sources[index] = mutated
            target = a.out / (label + '-negative')
            cmd = ['gcc', *common, *mutated_sources, candidate / 'ps2_audio.c', harness,
                   '-Wl,--wrap=calloc', '-Wl,--wrap=memalign', '-Wl,--wrap=free', '-Wl,--wrap=ov_open_callbacks', '-Wl,--wrap=ov_clear', '-o', target, *libs]
            if a.sanitize:
                cmd += ['-no-pie']
            log.append(run(cmd))
            ogg_path = next(path for path in files if path.read_bytes().startswith(b'OggS'))
            result = subprocess.run([str(target), str(ogg_path)], capture_output=True, text=True, timeout=60, env=env)
            (a.out / (label + '-negative.log')).write_text(result.stdout + result.stderr)
            if result.returncode != 1 or 'audio-lossless:' not in result.stderr:
                raise SystemExit('storage mutation was not detected: ' + label + result.stdout + result.stderr)
            message = label + ' source mutation detected'
            print(message); log.append(message)
    if a.ee:
        ee = a.out / 'ee'
        ee.mkdir(exist_ok=True)
        assets = []
        with zipfile.ZipFile(a.assets / 'music.pk3') as archive:
            for magic in (b'MThd', b'OggS'):
                choices = sorted((len(data), name) for name in archive.namelist() if not name.endswith('/')
                                 and (data := archive.read(name)).startswith(magic))
                for i in (0, len(choices) // 2, len(choices) - 1):
                    name = choices[i][1]
                    assets.append((name, archive.read(name)))
        for channels in (1, 2):
            dest = a.out / ('tone%d.mp3' % channels)
            run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', '-f', 'lavfi', '-i',
                 'sine=frequency=337:sample_rate=44100:duration=3.25', '-ac', channels,
                 '-codec:a', 'libmp3lame', '-b:a', '128k', dest])
            assets.append((dest.name, dest.read_bytes()))
        assets.extend(malformed_ogg(next(data for name, data in assets if data.startswith(b'OggS'))))
        with (a.out / 'audio_storage_next7_assets.h').open('w') as header:
            for i, (name, data) in enumerate(assets):
                header.write('static const unsigned char music_asset7_%d[] = {%s};\n' %
                             (i, ','.join(str(value) for value in data)))
            header.write('static const struct { const char *name; const unsigned char *bytes; size_t length; } music_assets7[] = {\n')
            for i, (name, data) in enumerate(assets):
                header.write('{%s,music_asset7_%d,sizeof music_asset7_%d},\n' % (json.dumps(name), i, i))
            header.write('};\n')
        (a.out / 'ee-assets.json').write_text(json.dumps([{'name': name, 'bytes': len(data),
            'sha256': hashlib.sha256(data).hexdigest()} for name, data in assets], indent=2) + '\n')
        flags = ['-O2', '-std=gnu17', '-G0', '-Wall', '-Wextra', '-Werror', '-D_EE', '-DPS2',
                 '-I' + str(candidate), '-I' + str(ROOT / 'tools/ps2'), '-I' + str(a.out),
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
        for src in [*sources, candidate / 'ps2_audio.c', harness]:
            obj = ee / ('bench_' + src.stem + '.o')
            run(['mips64r5900el-ps2-elf-gcc', *flags, '-DPS2_AUDIO_VORBIS', '-DPS2_AUDIO_MP3',
                 '-DPS2_STORAGE_NEXT7_ASSETS', '-c', src, '-o', obj])
            objects.append(obj)
        elf = ee / 'AUDIO_STORAGE_NEXT7.ELF'
        link_flags = ['mips64r5900el-ps2-elf-gcc', '-T' + str(a.sdk / 'ee/startup/linkfile'), '-L' + str(a.sdk / 'ee/lib'),
                      '-Wl,-zmax-page-size=128', '-Wl,--defsym,_stack_size=0x60000', '-L' + str(a.sdk / 'ports/lib'),
                      '-Wl,--wrap=calloc', '-Wl,--wrap=memalign', '-Wl,--wrap=free', '-Wl,--wrap=ov_open_callbacks', '-Wl,--wrap=ov_clear']
        libs = ['-ldebug', '-lpatches', '-lvorbisfile', '-lvorbis', '-logg', '-lmpg123', '-lm']
        run([*link_flags, *objects, '-o', elf, *libs])
        for label, old, new, index in STORAGE_MUTATIONS:
            production = sources[index].read_text()
            if production.count(old) != 1:
                raise SystemExit('missing/ambiguous storage mutation: ' + label)
            mutated = ee / (label + '.c')
            mutated.write_text(production.replace(old, new))
            obj = mutated.with_suffix('.o')
            run(['mips64r5900el-ps2-elf-gcc', *flags, '-DPS2_AUDIO_VORBIS', '-DPS2_AUDIO_MP3', '-c', mutated, '-o', obj])
            mutated_objects = list(objects); mutated_objects[index] = obj
            run([*link_flags, *mutated_objects, '-o', ee / (label + '.ELF'), *libs])
        message = 'EE real-codec proof ELF (not executed by this script): ' + str(elf)
        print(message); log.append(message)
        log.append('EE ELF sha256=' + hashlib.sha256(elf.read_bytes()).hexdigest())
    (a.out / 'report.txt').write_text('\n'.join(log))
    print('Report:', a.out / 'report.txt')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
