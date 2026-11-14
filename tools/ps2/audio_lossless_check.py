#!/usr/bin/env python3
"""Compare music/MIDI against a fixed git baseline, including private state.

Uses the host Vorbis/mpg123 shared libraries, SDK public codec headers, and
non-silent ffmpeg mono/stereo MP3 fixtures. Does not change the checkout/build
configuration or run an emulator. Optional --sanitize checks the same cases
under ASan/UBSan; --ee compiles strict EE objects and a synthetic equivalence /
COP0 MIDI benchmark ELF (no codecs or asset files needed by the EE fixture).
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import signal
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[2]
MUSIC_APIS = re.compile(r'\bPS2_(?:Music|MIDI)\w+')
MUSIC_PROBE = '''
size_t PS2_TestMusicBytes(void) { return sizeof(ps2_music); }
size_t PS2_TestMusicState(const ps2_music *m, uint64_t *out) {
    size_t n = 0, i;
    if (!m) return 0;
#define FIELD(x) out[n++] = (uint64_t)m->x
    FIELD(type); FIELD(pcm.offset); FIELD(pcm.frames); FIELD(pcm.rate); FIELD(pcm.bits); FIELD(pcm.channels);
    FIELD(io_pos); FIELD(decoded); FIELD(position); FIELD(rate); FIELD(length_ms); FIELD(loop_ms); FIELD(fraction); FIELD(step);
    FIELD(channels); FIELD(playing); FIELD(paused); FIELD(looping); FIELD(error); FIELD(buffered); FIELD(cursor); FIELD(skipped);
#undef FIELD
    out[n] = 0;
    if (!m->error) for (i = 0; i < m->buffered * 2; i++) out[n] = out[n] * 16777619u ^ (uint16_t)m->buffer[i];
    return n + 1;
}
'''
MIDI_PROBE = '''
typedef struct {
    ps2_audio_input in;
    midi_track tracks[TRACKS];
    midi_channel channels[16];
    midi_note notes[NOTES];
    uint32_t steps[128];
    int16_t sine[256];
    unsigned count, division, tempo, failed, events;
    uint64_t tick, us, remainder, frame, length_frames;
} midi_test_state;
size_t PS2_TestMidiState(const ps2_midi *m, unsigned char *out) {
    memset(out, 0, sizeof(midi_test_state));
#define COPY(x) memcpy(out + offsetof(midi_test_state, x), &m->x, sizeof m->x)
    COPY(in);
    memcpy(out + offsetof(midi_test_state, tracks), m->tracks, MIDI_TEST_TRACKS(m) * sizeof m->tracks[0]);
    COPY(channels); COPY(notes); COPY(steps); COPY(sine);
    COPY(count); COPY(division); COPY(tempo); COPY(failed); COPY(events);
    COPY(tick); COPY(us); COPY(remainder); COPY(frame); COPY(length_frames);
#undef COPY
    return sizeof(midi_test_state);
}
'''


def run(cmd, *, env=None, cwd=ROOT, expected=0):
    p = subprocess.run([str(v) for v in cmd], capture_output=True, text=True, env=env, cwd=cwd)
    if p.returncode != expected:
        shown = list(cmd) if len(cmd) <= 16 else [*cmd[:12], '... (%d further arguments)' % (len(cmd) - 12)]
        print(' '.join(str(v) for v in shown))
        print(p.stdout + p.stderr)
        raise SystemExit('unexpected command status %d (expected %d)' % (p.returncode, expected))
    return p.stdout + p.stderr


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--baseline', default='fc7b98fe6')
    ap.add_argument('--out', type=Path, default=ROOT / 'build/audio-lossless-20261114')
    ap.add_argument('--assets', type=Path, default=Path(os.environ.get('SRB2_AUDIO_ASSETS', '/opt/srb2-assets')))
    ap.add_argument('--sdk', type=Path, default=Path(os.environ.get('PS2SDK', '/opt/ps2dev/ps2sdk')))
    ap.add_argument('--sanitize', action='store_true')
    ap.add_argument('--ee', action='store_true')
    a = ap.parse_args()
    a.out = a.out.resolve()
    if not a.out.is_relative_to(ROOT / 'build'):
        ap.error('--out must be under the repository build directory')
    a.out.mkdir(parents=True, exist_ok=True)
    log = []
    versions = run(['git', 'rev-parse', a.baseline]).strip()
    log.append('baseline=' + versions)
    decl = []
    for name in ['ps2_music.h', 'ps2_midi.h']:
        for line in (ROOT / 'src/ps2' / name).read_text().splitlines():
            if re.match(r'^[^/].*\bPS2_(?:Music|MIDI)\w+\(.*;$', line):
                decl.append(MUSIC_APIS.sub(lambda m: m[0].replace('PS2_', 'Ref_', 1), line))
    (a.out / 'reference.h').write_text('\n'.join(decl) + '\n')
    common = ['-O2', '-g', '-std=gnu17', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'src/ps2'), '-I' + str(a.out),
              '-I' + str(a.sdk / 'ports/include'), '-DPS2_AUDIO_VORBIS', '-DPS2_AUDIO_MP3']
    if a.sanitize:
        common += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie']
    sources = []
    api_names = set(re.findall(r'\bPS2_(?:Music|MIDI)\w+(?=\s*\()', (ROOT / 'src/ps2/ps2_music.h').read_text() + (ROOT / 'src/ps2/ps2_midi.h').read_text()))
    for kind in ['reference', 'candidate']:
        for unit, probe in [('ps2_music.c', MUSIC_PROBE), ('ps2_midi.c', MIDI_PROBE)]:
            old = run(['git', 'show', versions + ':src/ps2/' + unit]) if kind == 'reference' else (ROOT / 'src/ps2' / unit).read_text()
            if unit == 'ps2_midi.c':
                # Compare all old slots against zero-filled absent tail slots,
                # while preserving every field/cache byte of validated tracks.
                tracks = '(m)->count' if 'midi_track tracks[];' in old else 'TRACKS'
                probe = '#define MIDI_TEST_TRACKS(m) (' + tracks + ')\n' + probe
            if kind == 'reference':
                if unit == 'ps2_music.c':
                    conv = 'PS2_FloatsToS16Stereo(m->buffer, pcm[0], m->channels == 2 ? pcm[1] : pcm[0], n);'
                    if '\t\t' + conv in old:
                        # The baseline dereferences a NULL pcm at zero-frame EOF.
                        # This guard affects no decoded sample, cursor or read.
                        old = old.replace('\t\t' + conv, '\t\tif (n) ' + conv)
                        log.append('reference normalization: guard zero-frame Ogg conversion (NULL pcm at EOF)')
                old = MUSIC_APIS.sub(lambda m: m[0].replace('PS2_', 'Ref_', 1), old)
                # Rename declarations from the included production headers too.
                old = '\n'.join('#define %s %s' % (api, api.replace('PS2_', 'Ref_', 1)) for api in sorted(api_names)) + '\n' + old
                probe = probe.replace('PS2_Test', 'Ref_Test')
            src = a.out / (kind + '_' + unit)
            src.write_text(old + probe)
            sources.append(src)
            log.append(src.name + ' sha256=' + hashlib.sha256(src.read_bytes()).hexdigest())
    harness = ROOT / 'tools/ps2/audio_lossless_hosttest.c'
    exe = a.out / 'audio_lossless'
    libs = ['-l:libvorbisfile.so.3', '-l:libvorbis.so.0', '-l:libogg.so.0', '-l:libmpg123.so.0', '-lm']
    cmd = ['gcc', *common, *sources, ROOT / 'src/ps2/ps2_audio.c', harness, '-o', exe, *libs]
    if a.sanitize:
        cmd += ['-no-pie']
    log.append(run(cmd))
    files = []
    for archive, prefix in [('music.pk3', 'Music/')]:
        with zipfile.ZipFile(a.assets / archive) as z:
            for name in sorted(z.namelist()):
                if not name.startswith(prefix) or name.endswith('/'):
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
             'sine=frequency=337:sample_rate=44100:duration=3.25', '-ac', channels, '-codec:a', 'libmp3lame', '-b:a', '128k', dest])
        files.append(dest)
    # Prove the original bug in isolation. Only the codec's zero-frame pointer
    # result is substituted; setup, normal output, looping and seek use libvorbis.
    ogg = next(path for path in files if path.read_bytes()[:4] == b'OggS')
    original_music = a.out / 'unguarded_ps2_music.c'
    original_music.write_text(run(['git', 'show', versions + ':src/ps2/ps2_music.c']))
    for label, music in [('original', original_music), ('guarded', ROOT / 'src/ps2/ps2_music.c')]:
        target = a.out / ('ogg_eof_' + label)
        cmd = ['gcc', *common, music, ROOT / 'src/ps2/ps2_midi.c', ROOT / 'src/ps2/ps2_audio.c',
               ROOT / 'tools/ps2/audio_ogg_eof_hosttest.c', '-Wl,--wrap=ov_read_float', '-o', target, *libs]
        if a.sanitize:
            cmd += ['-no-pie']
        run(cmd)
        result = subprocess.run([str(target), str(ogg)], capture_output=True, text=True)
        (a.out / ('ogg-eof-' + label + '.log')).write_text(result.stdout + result.stderr)
        if label == 'original':
            null_failure = result.returncode == -signal.SIGSEGV or (
                result.returncode != 0 and 'runtime error: load of null pointer' in result.stderr)
            if not null_failure or 'forcing zero-frame Ogg result with NULL PCM' not in result.stderr:
                raise SystemExit('unguarded Ogg EOF negative control did not fail')
            message = 'Original Ogg NULL-PCM EOF crash detected (isolated negative control)'
        else:
            if result.returncode or 'loop PCM PASS' not in result.stdout:
                raise SystemExit(result.stdout + result.stderr)
            message = result.stdout.strip()
        print(message); log.append(message)
    output = run([exe, *files])
    print(output, end='')
    log.append(output)
    neg = subprocess.run([str(exe), '--negative-control'], capture_output=True, text=True)
    if neg.returncode != 1 or 'na == nb && !memcmp(a, b, sizeof a)' not in neg.stderr:
        raise SystemExit('negative control was not detected: ' + neg.stdout + neg.stderr)
    log.append('PCM bit-flip negative control detected\n')
    print('PCM bit-flip negative control detected')
    if a.ee:
        ee = a.out / 'ee'
        ee.mkdir(exist_ok=True)
        flags = ['-O2', '-std=gnu17', '-G0', '-Wall', '-Wextra', '-Werror', '-D_EE', '-DPS2',
                 '-I' + str(ROOT / 'src/ps2'), '-I' + str(a.sdk / 'ee/include'), '-I' + str(a.sdk / 'common/include'), '-I' + str(a.sdk / 'ports/include')]
        sizes = []
        for src in sources:
            # Compile production code without the private host probes.
            text = src.read_text().split('size_t PS2_Test')[0].split('size_t Ref_Test')[0]
            clean = ee / src.name
            clean.write_text(text)
            for codecs in [False, True]:
                obj = clean.with_suffix('.codecs.o' if codecs else '.o')
                run(['mips64r5900el-ps2-elf-gcc', *flags, *(['-DPS2_AUDIO_VORBIS', '-DPS2_AUDIO_MP3'] if codecs else []), '-c', clean, '-o', obj])
                sizes.append(obj)
        size_text = run(['mips64r5900el-ps2-elf-size', *sizes])
        print(size_text, end=''); log.append(size_text)
        objects = []
        for src in [*sources, ROOT / 'src/ps2/ps2_audio.c', harness]:
            obj = ee / ('bench_' + src.stem + '.o')
            run(['mips64r5900el-ps2-elf-gcc', *flags, '-I' + str(a.out), '-c', src, '-o', obj])
            objects.append(obj)
        elf = ee / 'AUDIO_LOSSLESS.ELF'
        run(['mips64r5900el-ps2-elf-gcc', '-T' + str(a.sdk / 'ee/startup/linkfile'), '-L' + str(a.sdk / 'ee/lib'),
             '-Wl,-zmax-page-size=128', '-Wl,--defsym,_stack_size=0x60000', *objects, '-o', elf, '-ldebug', '-lpatches', '-lm'])
        message = 'EE synthetic equivalence/benchmark ELF (not executed by this script): ' + str(elf)
        print(message); log.append(message)
        log.append('EE ELF sha256=' + hashlib.sha256(elf.read_bytes()).hexdigest())
    (a.out / 'report.txt').write_text('\n'.join(log))
    print('Report:', a.out / 'report.txt')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
