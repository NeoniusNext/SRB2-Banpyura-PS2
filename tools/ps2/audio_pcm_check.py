"""PCM continuity check of a dump taken by the PS2 engine (-adump <frames>) against a host decode of the same music file.

usage: python tools/ps2/audio_pcm_check.py --dump <apcm.raw> --music <O_xxx.ogg|mid|...> [--loop-ms N] [--frames N] [--out DIR]
The dump holds exactly the PCM the mixer sent to audsrv (22050 Hz, signed 16-bit stereo, interleaved). The reference is the same
file decoded by the engine's own ps2_music.c on the host (real Vorbis DLL), in 512-frame blocks, looping. The check aligns the first
non-silent frame of the dump with the start of the reference and compares block by block:
  * equal blocks            - the stream is the music, in order
  * a block that equals the reference shifted by d frames (|d| <= 4096) - a hole (d>0) or a repeat (d<0) in the stream
  * other blocks            - SFX mixed in, a different music state (fade, pause) or a defect; listed
Needs the host libraries of the codec check (build/ps2-audio-check/codecs, created by tools/ps2/audio_codec_check.py).
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import math_common as C

ROOT = C.ROOT
CODECS = ROOT / 'build/ps2-audio-check/codecs'
DLL = ROOT / 'libs/SDL2_mixer/lib/x64'
LIBS = ['libvorbisfile-3', 'libvorbis-0', 'libmpg123-0']
BLOCK = 512


def build_reference(out):
    flags = ['/I' + str(ROOT / 'src/ps2'), '/ID:/ps2dev/ps2sdk/ports/include', '/DPS2_AUDIO_VORBIS', '/DPS2_AUDIO_MP3',
             '/DMPG123_ENUM_API']
    for n in LIBS:
        if not (CODECS / (n + '.lib')).is_file():
            raise SystemExit('run tools/ps2/audio_codec_check.py once first (import libraries missing: %s)' % n)
    units = [(ROOT / p, Path(p).stem + '-ref.obj', flags) for p in
             ['src/ps2/ps2_audio.c', 'src/ps2/ps2_music.c', 'src/ps2/ps2_midi.c', 'tools/ps2/audio_dump_ref.c']]
    return C.msvc_build(out, 'audio_dump_ref', units, link_extra=[str(CODECS / (n + '.lib')) for n in LIBS],
                        cl_extra=['/W3', '/wd4244', '/wd4267'])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dump', required=True)
    ap.add_argument('--music', required=True)
    ap.add_argument('--loop-ms', type=int, default=0)
    ap.add_argument('--frames', type=int, default=0, help='reference length (default: the dump length plus a margin)')
    ap.add_argument('--out', default=str(ROOT / 'build/opt2-a/pcm-check'))
    a = ap.parse_args()
    import numpy as np
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    dump = np.fromfile(a.dump, dtype='<i2').reshape(-1, 2)
    exe = build_reference(out)
    frames = a.frames or (len(dump) + 20000)
    ref_path = out / 'ref.raw'
    env = dict(os.environ, PATH=str(DLL) + os.pathsep + os.environ['PATH'])
    args = [str(exe), a.music, str(frames), str(ref_path)] + ([str(a.loop_ms)] if a.loop_ms else [])
    r = subprocess.run(args, capture_output=True, text=True, env=env)
    print(r.stdout.strip(), r.stderr.strip())
    if r.returncode:
        return r.returncode
    ref = np.fromfile(ref_path, dtype='<i2').reshape(-1, 2)
    nz = np.flatnonzero((dump != 0).any(axis=1))
    if not len(nz):
        print('dump is silent')
        return 1
    start = int(nz[0])
    rnz = np.flatnonzero((ref != 0).any(axis=1))
    rstart = int(rnz[0])
    print('dump %d frames (%.2f s), first non-silent frame %d; reference first non-silent frame %d' % (len(dump), len(dump) / 22050, start, rstart))
    n = (len(dump) - start) // BLOCK
    equal = shifted = other = 0
    problems = []
    for b in range(n):
        d = dump[start + b * BLOCK:start + (b + 1) * BLOCK]
        r0 = rstart + b * BLOCK
        if r0 + BLOCK > len(ref):
            break
        if np.array_equal(d, ref[r0:r0 + BLOCK]):
            equal += 1
            continue
        found = None
        for shift in range(1, 4097):
            for sign in (1, -1):
                s = r0 + sign * shift
                if s >= 0 and s + BLOCK <= len(ref) and np.array_equal(d, ref[s:s + BLOCK]):
                    found = sign * shift
                    break
            if found:
                break
        if found:
            shifted += 1
            problems.append((b, 'shifted by %+d frames (%s)' % (found, 'hole' if found > 0 else 'repeat')))
        else:
            other += 1
            diff = int((d != ref[r0:r0 + BLOCK]).any(axis=1).sum())
            problems.append((b, 'differs in %d of %d frames (SFX, fade or defect)' % (diff, BLOCK)))
    print('blocks compared %d: equal %d, shifted (hole/repeat) %d, other %d' % (equal + shifted + other, equal, shifted, other))
    for b, msg in problems[:40]:
        print('  block %d (frame %d): %s' % (b, start + b * BLOCK, msg))
    return 0 if shifted == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
