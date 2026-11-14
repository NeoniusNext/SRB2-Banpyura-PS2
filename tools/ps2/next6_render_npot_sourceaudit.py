"""Invert only the bounded NPOT wrapping change and check the frozen full file."""
import argparse
import difflib
import hashlib
import json
from pathlib import Path
import re

from next6_render_npot_hosttest import ROOT


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--base-src', required=True, type=Path)
    p.add_argument('--out', required=True, type=Path)
    a = p.parse_args()
    out = a.out.resolve()
    (out / 'src').mkdir(parents=True, exist_ok=True)
    baseline = (a.base_src / 'r_draw8_npo2.c').read_text()
    candidate = (ROOT / 'src/r_draw8_npo2.c').read_text()
    reconstructed = candidate
    begin = reconstructed.index('#ifdef PS2_OPT_DRAW\n')
    end = reconstructed.index('/**\t\\brief The R_DrawSpan_NPO2_8')
    base_begin = baseline.index('#ifdef PS2_OPT_DRAW\n')
    base_end = baseline.index('/**\t\\brief The R_DrawSpan_NPO2_8')
    reconstructed = reconstructed[:begin] + baseline[base_begin:base_end] + reconstructed[end:]
    current_setup = ('\tUINT32 x_reciprocal, y_reciprocal;\n'
                     '\tR_NPO2SlopeReciprocals(&x_reciprocal, &y_reciprocal);')
    prior_setup = ('\tstruct libdivide_u32_t x_divider, y_divider;\n'
                   '\tR_NPO2SlopeDividers(&x_divider, &y_divider);')
    if reconstructed.count(current_setup) != 7:
        raise SystemExit('Expected all seven reciprocal setup sites')
    reconstructed = reconstructed.replace(current_setup, prior_setup)
    pattern = (r'#ifdef PS2_OPT_DRAW\n(?P<i>\t+)x = R_NPO2SlopeWrap\(x, NPO2_TILT_WIDTH, x_reciprocal\);\n'
               r'(?P=i)y = R_NPO2SlopeWrap\(y, NPO2_TILT_HEIGHT, y_reciprocal\);\n#else\n'
               r'(?P<original>.*?)#endif\n')
    reconstructed, count = re.subn(pattern, lambda m: m['original'], reconstructed, flags=re.S)
    if count != 26 or reconstructed != baseline:
        raise SystemExit('Inverse transformation did not reconstruct the frozen complete source')
    siblings = {}
    for name in ('r_draw.c', 'r_draw8.c', 'v_video.c'):
        before = (a.base_src / name).read_bytes()
        after = (ROOT / 'src' / name).read_bytes()
        if before != after:
            raise SystemExit('Unexpected sibling change: ' + name)
        siblings[name] = sha(after)
    (out / 'src/r_draw8_npo2.c').write_text(candidate)
    (out / 'reconstructed.c').write_text(reconstructed)
    (out / 'change.diff').write_text(''.join(difflib.unified_diff(
        baseline.splitlines(keepends=True), candidate.splitlines(keepends=True),
        fromfile='baseline/r_draw8_npo2.c', tofile='candidate/r_draw8_npo2.c')))
    report = {'baseline_sha256': sha(baseline.encode()), 'candidate_sha256': sha(candidate.encode()),
              'inverse_sha256': sha(reconstructed.encode()), 'normalization_sites': count,
              'setup_sites': 7, 'complete_inverse_matches': True, 'unchanged_siblings': siblings}
    (out / 'change-audit.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
