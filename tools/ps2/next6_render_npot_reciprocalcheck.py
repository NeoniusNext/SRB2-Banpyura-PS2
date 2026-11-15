"""Exhaust every signed 16-bit coordinate and UINT16 flat dimension.

Both production helpers are extracted without rewriting their arithmetic. The
independent oracle uses ordinary integer division; no renderer is reimplemented.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

from next6_render_npot_hosttest import ROOT, TOOLS, function


DRIVER = r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "next6_render_npot_stub.h"
UINT16 ds_flatwidth, ds_flatheight;
static UINT32 snapshots[2][6];
static UINT64 cache_cases;
static void fixture_cache_snapshot(unsigned variant, UINT16 x0, UINT16 x1, UINT8 xn,
                                   UINT16 y0, UINT16 y1, UINT8 yn)
{
    snapshots[variant][0] = x0; snapshots[variant][1] = x1; snapshots[variant][2] = xn;
    snapshots[variant][3] = y0; snapshots[variant][4] = y1; snapshots[variant][5] = yn;
}
void I_Error(const char *message, ...)
{
    va_list args;
    va_start(args, message);
    vfprintf(stderr, message, args);
    va_end(args);
    exit(2);
}
#include "helpers.inc"
static void checked_cache(UINT32 *x, UINT32 *y)
{
    struct libdivide_u32_t reference_x, reference_y;
    reference_R_NPO2SlopeDividers(&reference_x, &reference_y);
    R_NPO2SlopeReciprocals(x, y);
    if (memcmp(snapshots[0], snapshots[1], sizeof snapshots[0])
        || *x != libdivide_u32_do(0x7fffffffU, &reference_x) + 1U
        || *y != libdivide_u32_do(0x7fffffffU, &reference_y) + 1U)
    {
        printf("RECIPROCAL FAIL replacement width=%u height=%u\n", ds_flatwidth, ds_flatheight);
        exit(1);
    }
    cache_cases++;
}
int main(void)
{
    static const UINT16 sequence[][2] = {
        {63,47}, {257,17}, {63,47}, {319,241}, {257,17}, {63,47},
        {65535,1}, {1,65535}, {65535,1}, {32768,32767}, {1,65535}, {65535,1},
        {63,47}, {257,17}, {257,17}, {63,47},
    };
    UINT32 d, n;
    UINT64 pairs = 0;
    for (d = 0; d < sizeof sequence / sizeof sequence[0]; d++)
    {
        UINT32 x, y;
        ds_flatwidth = sequence[d][0]; ds_flatheight = sequence[d][1];
        checked_cache(&x, &y);
    }
    for (d = 1; d <= 65535; d++)
    {
        UINT32 x, y, hitx, hity;
        ds_flatwidth = (UINT16)d;
        ds_flatheight = (UINT16)(65536 - d);
        checked_cache(&x, &y);
        checked_cache(&hitx, &hity);
        if (x != hitx || y != hity || x != 0x7fffffffU / d + 1U
            || y != 0x7fffffffU / (65536 - d) + 1U)
        {
            printf("RECIPROCAL FAIL cache d=%u\n", d);
            return 1;
        }
        for (n = 0; n <= 32767; n++)
        {
            const UINT32 remainder = n - (n / d) * d;
            const UINT32 positive = R_NPO2SlopeWrap((fixed_t)n, (UINT16)d, x);
            const UINT32 negative = R_NPO2SlopeWrap(-(fixed_t)n - 1, (UINT16)d, x);
            if (positive != remainder || negative != d - 1 - remainder)
            {
                printf("RECIPROCAL FAIL d=%u n=%u positive=%u negative=%u remainder=%u\n",
                       d, n, positive, negative, remainder);
                return 1;
            }
            pairs += 2;
        }
    }
    printf("RECIPROCAL PASS dimensions=65535 magnitudes=32768 signed_pairs=%llu cache_cases=%llu\n",
           (unsigned long long)pairs, (unsigned long long)cache_cases);
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', required=True, type=Path)
    p.add_argument('--base-src', required=True, type=Path)
    p.add_argument('--cc', default=os.environ.get('CC', 'gcc'))
    p.add_argument('--negative-controls', action='store_true')
    a = p.parse_args()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    source = (ROOT / 'src/r_draw8_npo2.c').read_text()
    baseline = (a.base_src / 'r_draw8_npo2.c').read_text()
    reference_cache = function(baseline, 'R_NPO2SlopeDividers')
    current_cache = function(source, 'R_NPO2SlopeReciprocals')
    snapshots = ('\n\tfixture_cache_snapshot(VARIANT, xcached[0].dimension, xcached[1].dimension, xnext,\n'
                 '\t                       ycached[0].dimension, ycached[1].dimension, ynext);\n')
    # Append observations at function exit without changing lookup/generation/replacement arithmetic.
    reference_cache = reference_cache.rstrip()[:-1] + snapshots.replace('VARIANT', '0') + '}\n'
    current_cache = current_cache.rstrip()[:-1] + snapshots.replace('VARIANT', '1') + '}\n'
    helpers = (reference_cache.replace('R_NPO2SlopeDividers', 'reference_R_NPO2SlopeDividers')
               + current_cache + function(source, 'R_NPO2SlopeWrap'))
    (out / 'helpers.inc').write_text(helpers)
    (out / 'driver.c').write_text(DRIVER)
    flags = [a.cc, '-O3', '-std=gnu17', '-fwrapv', '-DPS2_PROFILE', '-Wall', '-Wextra', '-Werror',
             '-I' + str(out), '-I' + str(TOOLS), '-I' + str(ROOT / 'src')]
    report = {'source_sha256': hashlib.sha256(source.encode()).hexdigest(),
              'baseline_sha256': hashlib.sha256(baseline.encode()).hexdigest(),
              'cache_observation': 'Only append slot dimensions/replacement cursor snapshots at actual helper exits',
              'checks': {}}
    policies = [('actual', helpers, 0)]
    if a.negative_controls:
        policies.extend([
            ('wrong-shift', helpers.replace('magnitude << 1', 'magnitude << 0'), 1),
            ('wrong-sign', helpers.replace('(UINT32)(coordinate >> 31)', '(UINT32)0'), 1),
            ('wrong-reciprocal', helpers.replace(current_cache, current_cache.replace(' + 1U', ' + 0U')), 1),
            ('wrong-replacement', helpers.replace(current_cache, current_cache.replace('xnext ^= 1', 'xnext ^= 0')), 1),
        ])
    for policy, content, expected in policies:
        if policy != 'actual' and content == helpers:
            raise SystemExit('Negative control did not mutate actual helper: ' + policy)
        (out / 'helpers.inc').write_text(content)
        binary = out / policy
        build = subprocess.run([*flags, str(out / 'driver.c'), '-o', str(binary)], capture_output=True, text=True)
        (out / (policy + '-build.log')).write_text(build.stdout + build.stderr)
        if build.returncode:
            raise SystemExit(build.stderr)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        (out / (policy + '-test.log')).write_text(result.stdout + result.stderr)
        if result.returncode != expected:
            raise SystemExit(policy + ': unexpected exit ' + str(result.returncode))
        report['checks'][policy] = {'exit': result.returncode, 'output': result.stdout.strip()}
        print(policy + ': ' + result.stdout.strip(), flush=True)
    (out / 'helpers.inc').write_text(helpers)
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
