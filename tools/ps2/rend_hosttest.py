"""Host (MSVC) test of src/ps2/ps2_rdraw.h helpers against the original expressions; --negative-control must fail.

usage: python tools/ps2/rend_hosttest.py --out build/opt-r/rend-host [--negative-control]
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import math_common as C  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--negative-control', action='store_true')
    a = ap.parse_args()
    out = a.out.resolve()
    ok = True
    for name, flags in (('main', []), ('broken', ['/DRT_BROKEN'])):
        if name == 'broken' and not a.negative_control:
            continue
        work = out / name
        work.mkdir(parents=True, exist_ok=True)
        (work / 'stubs.c').write_text('#include <string.h>\nvoid *M_Memcpy(void *a, const void *b, size_t n) { return memcpy(a, b, n); }\n')
        inc = ['/DPS2_PROFILE', '/I' + str(C.ROOT / 'src'), '/I' + str(C.ROOT / 'src/ps2')]
        exe = C.msvc_build(work, 'rend_hosttest', [(C.ROOT / 'tools/ps2/rend_hosttest.c', 'main.obj', inc + flags),
                                                   (C.ROOT / 'src/m_fixed.c', 'fixed.obj', inc),
                                                   (work / 'stubs.c', 'stubs.obj', [])],
                           cl_extra=['/wd4146'])
        rc, text = C.run(exe, [], log=work / 'test.log')
        print(name, text.strip())
        if name == 'main':
            ok &= rc == 0 and 'failures=0' in text.splitlines()[-1]
        else:
            ok &= rc != 0
            print('negative control', 'detected' if rc else 'NOT DETECTED')
    print('RESULT', 'PASS' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
