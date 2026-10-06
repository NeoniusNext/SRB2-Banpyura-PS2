"""Host equivalence of the optimised audio code (MSVC): python tools/ps2/audio_equiv_check.py [--negative-control]
Builds ps2_audio.c + ps2_music.c + ps2_midi.c + tools/ps2/audio_equiv_hosttest.c into build/opt-s/audio-equiv (or
$SRB2_PS2_OPT_OUT/audio-equiv) and runs it. With --negative-control one sample is perturbed and the run must fail."""
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import math_common as c

ROOT = c.ROOT
OUT = Path(os.environ.get('SRB2_PS2_OPT_OUT', ROOT / 'build/opt-s')) / 'audio-equiv'


def main():
    inc = ['/I' + str(ROOT / 'src/ps2')]
    units = [(ROOT / p, Path(p).stem + '.obj', inc) for p in
             ['src/ps2/ps2_audio.c', 'src/ps2/ps2_music.c', 'src/ps2/ps2_midi.c', 'tools/ps2/audio_equiv_hosttest.c']]
    exe = c.msvc_build(OUT, 'audio_equiv_hosttest', units, cl_extra=['/W4', '/WX', '/wd4244', '/wd4267'])
    neg = '--negative-control' in sys.argv
    rc, text = c.run(exe, args=['--negative-control'] if neg else [], log=OUT / ('negative.log' if neg else 'test.log'))
    print(text, end='')
    if neg:
        print('negative control', 'FAILED AS REQUIRED' if rc else 'DID NOT FAIL (test is blind)')
        return 0 if rc else 1
    return rc


if __name__ == '__main__':
    sys.exit(main())
