"""Linux (gcc) driver of the audio host tests that audio_check.py / audio_thread_check.py / audio_equiv_check.py build with MSVC.

usage: python3 tools/ps2/audio_hosttests.py [core] [thread] [backend] [ring] [ring-neg] [equiv] [all]
(ring-neg: negative controls of the ring test, built from mutated copies of i_sound.c: the old pump without the silence flush must be caught
playing stale audio, and a pump without the 4-byte guard must be caught filling the ring completely)
Builds into build/audio-host/<test> and runs: core (audio_hosttest.c: mixer, music, MIDI), thread (audio_thread_hosttest.c: the
engine control plane with real threads), backend (audio_backend_hosttest.c: the production i_sound.c with engine and audsrv mocked),
ring (audio_ring_hosttest.c: i_sound.c against a model of the stock audsrv IOP ring, PS2-300/301), equiv (audio_equiv_hosttest.c).
No codec libraries are linked (the PS2_AUDIO_VORBIS/MP3 paths are tested by the codec checks); exit code 0 = all requested passed.
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/audio-host'
SDK = Path('/opt/ps2dev-x/ps2dev/ps2sdk')
SRC = ['src/ps2/ps2_audio.c', 'src/ps2/ps2_athread.c', 'src/ps2/ps2_music.c', 'src/ps2/ps2_midi.c']
CFLAGS = ['-O2', '-g', '-std=gnu17', '-w', '-DNDEBUG', '-I' + str(ROOT / 'src/ps2'), '-I' + str(ROOT / 'tools/ps2'),
          '-I' + str(SDK / 'ee/include'), '-I' + str(SDK / 'common/include')]


def host_isound(dest):
    text = (ROOT / 'src/ps2/i_sound.c').read_text()
    text = re.sub(r'^#include "(?:\.\./[^"]+|ps2_boot\.h)"\n', '', text, flags=re.M)
    dest.mkdir(parents=True, exist_ok=True)
    (dest / 'i_sound_host.c').write_text(text)


def build(name, extra_src, extra_flags=(), libs=('-lm', '-lpthread')):
    d = OUT / name
    d.mkdir(parents=True, exist_ok=True)
    exe = d / name
    cmd = ['gcc', *CFLAGS, '-I' + str(d), *extra_flags, *[str(ROOT / s) for s in extra_src], '-o', str(exe), *libs]
    r = subprocess.run(cmd, capture_output=True, text=True)
    (d / 'build.log').write_text(' '.join(cmd) + '\n' + r.stdout + r.stderr)
    if r.returncode:
        print(r.stdout + r.stderr)
        raise SystemExit('build failed: ' + name)
    return exe


def run(name, exe, args=()):
    r = subprocess.run([str(exe), *args], capture_output=True, text=True, cwd=OUT / name)
    text = r.stdout + r.stderr
    (OUT / name / 'test.log').write_text(text)
    print(text, end='')
    print('%s: %s' % (name, 'PASS' if r.returncode == 0 else 'FAIL (exit %d)' % r.returncode))
    return r.returncode


def main(argv):
    want = set(argv) or {'all'}
    if 'all' in want:
        want = {'core', 'thread', 'backend', 'ring', 'equiv'}
    rc = 0
    if 'core' in want:
        rc |= run('core', build('core', SRC + ['tools/ps2/audio_hosttest.c']))
    if 'thread' in want:
        rc |= run('thread', build('thread', SRC + ['tools/ps2/audio_thread_hosttest.c']))
    if 'backend' in want:
        host_isound(OUT / 'backend')
        rc |= run('backend', build('backend', SRC + ['tools/ps2/audio_backend_hosttest.c']))
    if 'ring' in want and (ROOT / 'tools/ps2/audio_ring_hosttest.c').exists():
        host_isound(OUT / 'ring')
        rc |= run('ring', build('ring', SRC + ['tools/ps2/audio_ring_hosttest.c']))
    if 'ring-neg' in want:
        for name, old, new, args, must_fail in [
                ('ring_noflush', 'flush_left = ring_bytes;', 'flush_left = 0;', ['--expect-stale'], False),
                ('ring_noguard', '#define RING_GUARD 4 ', '#define RING_GUARD 0 ', [], True)]:
            d = OUT / name
            host_isound(d)
            text = (d / 'i_sound_host.c').read_text()
            assert old in text, old
            (d / 'i_sound_host.c').write_text(text.replace(old, new))
            exe = build(name, SRC + ['tools/ps2/audio_ring_hosttest.c'])
            r = subprocess.run([str(exe), *args], capture_output=True, text=True, cwd=d)
            (d / 'test.log').write_text(r.stdout + r.stderr)
            ok = (r.returncode != 0) if must_fail else (r.returncode == 0)
            print('%s: negative control %s' % (name, 'DETECTED (as required)' if ok else 'NOT DETECTED -- the test is blind'))
            rc |= 0 if ok else 1
    if 'equiv' in want and (ROOT / 'tools/ps2/audio_equiv_hosttest.c').exists():
        rc |= run('equiv', build('equiv', ['src/ps2/ps2_audio.c', 'tools/ps2/audio_equiv_hosttest.c']))
    return rc


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
