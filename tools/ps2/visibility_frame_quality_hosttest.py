#!/usr/bin/env python3
"""Execute positive and defect controls for the bounded native-frame comparator."""
import argparse
import json
from pathlib import Path
from types import SimpleNamespace

import numpy as np

from visibility_frame_quality import compare_ppm, directory_pairs, read_ppm


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=False)
    bounds = dict(max_rgb=0, max_mean_rgb=0, max_changed_fraction=0,
                  max_tile_mean_rgb=0, max_large_error_component=0,
                  max_edge_unmatched_fraction=0, spatial_error=8,
                  edge_contrast=32, edge_radius=0, tile=16)
    cases = []

    def ppm(name, image):
        path = a.out / (name + '.ppm')
        h, w, _ = image.shape
        path.write_bytes(f'P6\n{w} {h}\n255\n'.encode() + image.tobytes())
        return path

    image = np.full((64, 64, 3), 48, dtype=np.uint8)
    image[16:48, 16:48] = 160
    baseline = ppm('baseline', image)

    def check(name, candidate, accepted, expected=None, **limits):
        row = compare_ppm(baseline, ppm(name, candidate), SimpleNamespace(**(bounds | limits)))
        if row['within_bounds'] != accepted:
            raise AssertionError(name + ': unexpected decision')
        if expected and not any(v.startswith(expected + ':') for v in row['violations']):
            raise AssertionError(name + ': required sentinel did not reject')
        cases.append({'case': name, 'expected_within_bounds': accepted, 'comparison': row})
        return row

    row = check('exact', image.copy(), True)
    assert row['metrics']['exact_pixels'] == 4096 and row['metrics']['changed_pixels'] == 0
    row = check('small_uniform_error', image + 1, True, max_rgb=1, max_mean_rgb=1,
                max_tile_mean_rgb=1, max_changed_fraction=1)
    assert row['metrics']['max_rgb_error'] == 1 and row['metrics']['mean_rgb_error'] == 1
    assert row['metrics']['changed_pixels'] == 4096 and row['metrics']['spatial']['pixels'] == 0
    check('strict_still_exact', image + 1, False, 'max_rgb')
    spike = image.copy()
    spike[0, 0, 0] = 255
    row = check('one_pixel_defect', spike, False, 'max_rgb')
    assert row['metrics']['changed_pixels'] == 1 and row['metrics']['max_rgb_error'] == 207
    assert row['metrics']['mean_rgb_error'] == 207 / (4096 * 3)
    generous = dict(max_rgb=255, max_mean_rgb=255, max_changed_fraction=1,
                    max_tile_mean_rgb=255, max_edge_unmatched_fraction=1)
    patch = image.copy()
    patch[24:32, 24:32] = 0
    row = check('contiguous_hole', patch, False, 'max_large_error_component',
                **generous, max_large_error_component=63)
    assert row['metrics']['spatial']['components']['largest_pixels'] == 64
    assert row['metrics']['spatial']['components']['largest_bbox_xyxy'] == [24, 24, 32, 32]
    check('diluted_local_defect', patch, False, 'max_tile_mean_rgb',
          **(generous | dict(max_tile_mean_rgb=1)), max_large_error_component=4096)
    shifted = np.roll(image, 1, axis=1)
    row = check('one_pixel_geometry_shift', shifted, False, 'max_edge_unmatched_fraction',
                **(generous | dict(max_edge_unmatched_fraction=0)), max_large_error_component=4096)
    assert row['metrics']['spatial']['edges']['horizontal_pairs']['missing'] == 64
    check('explicit_one_pixel_edge_radius', shifted, True,
          **(generous | dict(max_edge_unmatched_fraction=0)),
          max_large_error_component=4096, edge_radius=1)

    rejected = []
    for name, raw in [('truncated', baseline.read_bytes()[:-1]),
                      ('extra_payload', baseline.read_bytes() + b'X'),
                      ('unsupported_header', b'P3\n64 64\n255\n'),
                      ('bad_dimensions', b'P6\n0 64\n255\n')]:
        path = a.out / (name + '.ppm')
        path.write_bytes(raw)
        try:
            read_ppm(path)
        except ValueError:
            rejected.append(name)
        else:
            raise AssertionError(name + ': invalid PPM accepted')
    # A payload beginning in whitespace must not be consumed as header padding.
    for value in (9, 10, 13, 32):
        raw = np.full((1, 1, 3), value, dtype=np.uint8)
        assert np.array_equal(read_ppm(ppm('binary-first-' + str(value), raw))[1], raw)
    try:
        compare_ppm(baseline, ppm('wrong-size', image[:32]), SimpleNamespace(**bounds))
    except ValueError:
        rejected.append('dimension_mismatch')
    else:
        raise AssertionError('Different dimensions accepted')
    for name in ('empty-a', 'empty-b'):
        (a.out / name).mkdir()
    try:
        directory_pairs(a.out / 'empty-a', a.out / 'empty-b', '', '*.ppm')
    except ValueError:
        rejected.append('empty_frame_set')
    else:
        raise AssertionError('Empty frame set accepted')
    report = {'passed': True, 'pixel_and_spatial_controls': cases,
              'invalid_inputs_rejected': rejected, 'binary_first_bytes_checked': [9, 10, 13, 32]}
    (a.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'passed': True, 'pixel_and_spatial_controls': len(cases),
                      'invalid_inputs_rejected': rejected}))


if __name__ == '__main__':
    main()
