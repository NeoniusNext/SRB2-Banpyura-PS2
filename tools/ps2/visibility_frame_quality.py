#!/usr/bin/env python3
"""Measure complete P6 frame pairs with explicit bounded RGB/spatial limits.

This is a separate diagnostic/admission tool. It never edits the strict frame,
resource-stream or GS/VU0 gates, and does not infer semantic geometry equality.
Default limits require exact RGB pixels. All nonzero limits must be supplied
explicitly and are retained in the immutable JSON report.

Inputs can be two PPMs, matching directories of PPMs (recursive), or an existing
visibility_memory_integration.py output supplied with --integration.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import sys

import numpy as np


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def read_ppm(path):
    raw = path.read_bytes()
    # These are the complete uncompressed native readbacks, not screenshots or
    # rescaled/colour-managed images. Validate the entire payload before reshape.
    header = re.match(rb'P6\s+([1-9][0-9]*)\s+([1-9][0-9]*)\s+255(?:\r\n|\n|[ \t])', raw)
    if not header:
        raise ValueError('Unsupported native P6 header: ' + str(path))
    w, h = map(int, header.groups())
    if len(raw) != header.end() + w * h * 3:
        raise ValueError('Incomplete or extra P6 pixel payload: ' + str(path))
    return raw, np.frombuffer(raw, dtype=np.uint8, offset=header.end()).reshape(h, w, 3)


def box(mask):
    yy, xx = np.nonzero(mask)
    return [int(xx.min()), int(yy.min()), int(xx.max()) + 1, int(yy.max()) + 1] if len(xx) else None


def components(mask):
    """Four-connected significant-error components, with bounded image memory."""
    todo = mask.copy()
    h, w = todo.shape
    count, largest, largest_box = 0, 0, None
    # Flood fill only visits true pixels once. Coordinates/bounds refer to the
    # original full frame; there is no crop, edge exclusion or area sampling.
    for y, x in zip(*np.nonzero(mask)):
        if not todo[y, x]:
            continue
        count += 1
        todo[y, x] = False
        queue = [(int(y), int(x))]
        area, x0, y0, x1, y1 = 0, int(x), int(y), int(x), int(y)
        while queue:
            py, px = queue.pop()
            area += 1
            x0, y0, x1, y1 = min(x0, px), min(y0, py), max(x1, px), max(y1, py)
            for ny, nx in ((py - 1, px), (py + 1, px), (py, px - 1), (py, px + 1)):
                if 0 <= ny < h and 0 <= nx < w and todo[ny, nx]:
                    todo[ny, nx] = False
                    queue.append((ny, nx))
        if area > largest:
            largest, largest_box = area, [x0, y0, x1 + 1, y1 + 1]
    return {'count': count, 'largest_pixels': largest, 'largest_bbox_xyxy': largest_box}


def dilate(mask, radius):
    if not radius:
        return mask
    h, w = mask.shape
    padded = np.pad(mask, radius)
    result = np.zeros_like(mask)
    for dy in range(radius * 2 + 1):
        for dx in range(radius * 2 + 1):
            result |= padded[dy:dy + h, dx:dx + w]
    return result


def edges(image, contrast):
    signed = image.astype(np.int16)
    return (np.max(np.abs(signed[:, 1:] - signed[:, :-1]), axis=2) >= contrast,
            np.max(np.abs(signed[1:] - signed[:-1]), axis=2) >= contrast)


def compare_ppm(baseline, candidate, a):
    raw_a, before = read_ppm(baseline)
    raw_b, after = read_ppm(candidate)
    if before.shape != after.shape:
        raise ValueError('Frame dimensions differ: ' + str(baseline) + ' / ' + str(candidate))
    signed = after.astype(np.int16) - before.astype(np.int16)
    error = np.abs(signed)
    pixel_error = error.max(axis=2)
    changed = pixel_error != 0
    significant = pixel_error > a.spatial_error
    h, w, _ = before.shape
    total, changed_count = w * h, int(changed.sum())
    spatial = {'error_above_rgb': a.spatial_error, 'pixels': int(significant.sum()),
               'bbox_xyxy': box(significant), 'components': components(significant),
               'max_row_pixels': int(significant.sum(axis=1).max()),
               'max_column_pixels': int(significant.sum(axis=0).max())}
    tile_max = {'mean_rgb_error': -1.0}
    tile_changed = 0.0
    for y in range(0, h, a.tile):
        for x in range(0, w, a.tile):
            tile_error = float(error[y:y + a.tile, x:x + a.tile].mean())
            tile_changed = max(tile_changed, float(changed[y:y + a.tile, x:x + a.tile].mean()))
            if tile_error > tile_max['mean_rgb_error']:
                tile_max = {'mean_rgb_error': tile_error,
                            'bbox_xyxy': [x, y, min(x + a.tile, w), min(y + a.tile, h)]}
    spatial['tiles'] = {'size': a.tile, 'worst_mean_rgb': tile_max,
                        'max_changed_fraction': tile_changed}
    edge_report = {}
    for direction, ea, eb in zip(('horizontal_pairs', 'vertical_pairs'),
                                 edges(before, a.edge_contrast), edges(after, a.edge_contrast)):
        missing, added = ea & ~dilate(eb, a.edge_radius), eb & ~dilate(ea, a.edge_radius)
        na, nb = int(ea.sum()), int(eb.sum())
        nm, nn = int(missing.sum()), int(added.sum())
        edge_report[direction] = {'baseline': na, 'candidate': nb, 'missing': nm, 'added': nn,
                                  'missing_fraction': nm / max(1, na), 'added_fraction': nn / max(1, nb)}
    spatial['edges'] = {'contrast_rgb': a.edge_contrast, 'match_radius_pixels': a.edge_radius,
                        **edge_report}
    unmatched = max(v[k] for v in edge_report.values() for k in ('missing_fraction', 'added_fraction'))
    metrics = {'pixels': total, 'exact_pixels': total - changed_count, 'changed_pixels': changed_count,
               'changed_fraction': changed_count / total, 'max_rgb_error': int(error.max()),
               'mean_rgb_error': float(error.mean()),
               'mean_rgb_error_by_channel': error.mean(axis=(0, 1)).tolist(),
               'mean_signed_rgb_by_channel': signed.mean(axis=(0, 1)).tolist(),
               'rms_rgb_error': float(np.sqrt(np.square(error.astype(np.float64)).mean())),
               'changed_bbox_xyxy': box(changed), 'spatial': spatial}
    observed = {'max_rgb': metrics['max_rgb_error'], 'max_mean_rgb': metrics['mean_rgb_error'],
                'max_changed_fraction': metrics['changed_fraction'],
                'max_tile_mean_rgb': tile_max['mean_rgb_error'],
                'max_large_error_component': spatial['components']['largest_pixels'],
                'max_edge_unmatched_fraction': unmatched}
    violations = [key + ': observed ' + str(value) + ' > bound ' + str(getattr(a, key))
                  for key, value in observed.items() if value > getattr(a, key)]
    return {'baseline': str(baseline), 'candidate': str(candidate), 'dimensions': [w, h],
            'baseline_sha256': sha(raw_a), 'candidate_sha256': sha(raw_b),
            'equal_complete_bytes': raw_a == raw_b, 'equal_rgb_pixels': not changed_count,
            'metrics': metrics, 'violations': violations, 'within_bounds': not violations}


def pairs(a):
    if a.integration:
        if a.baseline or a.candidate:
            raise ValueError('--integration cannot be combined with --baseline/--candidate')
        source = a.integration.resolve()
        report = json.loads((source / 'report.json').read_text())
        result = []
        for row in report['pairs']:
            scene = row['scene']
            left, right = source / 'runs' / ('baseline-' + scene), source / 'runs' / ('candidate-' + scene)
            result += directory_pairs(left, right, scene + '/', 'vidshot-*.ppm')
        return result, {'integration_report': str(source / 'report.json'),
                        'integration_report_sha256': sha((source / 'report.json').read_bytes()),
                        'original_errors': report['errors'],
                        'original_strict_equal_frames': all(row['equal_frames'] for row in report['pairs'])}
    if not a.baseline or not a.candidate:
        raise ValueError('Supply --baseline and --candidate, or --integration')
    left, right = a.baseline.resolve(), a.candidate.resolve()
    if left.is_file() and right.is_file():
        return [(left.name, left, right)], {}
    if left.is_dir() and right.is_dir():
        return directory_pairs(left, right, '', '*.ppm'), {}
    raise ValueError('Inputs must both be files or both directories')


def directory_pairs(left, right, prefix, pattern):
    before = {str(p.relative_to(left)): p for p in left.rglob(pattern)}
    after = {str(p.relative_to(right)): p for p in right.rglob(pattern)}
    if not before or before.keys() != after.keys():
        raise ValueError('Empty or unequal frame sets: ' + str(left) + ' / ' + str(right))
    return [(prefix + name, before[name], after[name]) for name in sorted(before)]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--baseline', type=Path)
    p.add_argument('--candidate', type=Path)
    p.add_argument('--integration', type=Path)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--max-rgb', type=int, default=0)
    p.add_argument('--max-mean-rgb', type=float, default=0)
    p.add_argument('--max-changed-fraction', type=float, default=0)
    p.add_argument('--max-tile-mean-rgb', type=float, default=0)
    p.add_argument('--max-large-error-component', type=int, default=0)
    p.add_argument('--max-edge-unmatched-fraction', type=float, default=0)
    p.add_argument('--spatial-error', type=int, default=8)
    p.add_argument('--edge-contrast', type=int, default=32)
    p.add_argument('--edge-radius', type=int, default=0)
    p.add_argument('--tile', type=int, default=16)
    a = p.parse_args()
    values = vars(a)
    if any(not math.isfinite(v) or v < 0 for k, v in values.items()
           if k.startswith('max_') or k in ('spatial_error', 'edge_radius')):
        p.error('Bounds must be finite and nonnegative')
    if not 1 <= a.edge_contrast <= 255 or a.tile < 1 or a.edge_radius > 8:
        p.error('Use contrast 1..255, positive tile size and edge radius 0..8')
    if a.max_changed_fraction > 1 or a.max_edge_unmatched_fraction > 1 or a.max_rgb > 255 or a.spatial_error > 255:
        p.error('Fractions must be <= 1 and RGB values <= 255')
    if a.out.exists():
        p.error('Choose a fresh --out JSON path; existing evidence is immutable')
    try:
        work, original = pairs(a)
        frames = {name: compare_ppm(before, after, a) for name, before, after in work}
    except (OSError, ValueError, KeyError) as error:
        p.error(str(error))
    if not frames:
        p.error('No frame pairs')
    report = {'schema': 1, 'tool_sha256': sha(Path(__file__).read_bytes()),
              'scope': 'Complete native RGB readback diagnostic; image edges/components are spatial sentinels, not proof of semantic geometry equality. Resource, lifecycle and strict GS/VU0 checks remain separate.',
              'limits': {k: v for k, v in values.items() if k.startswith('max_')},
              'parameters': {k: values[k] for k in ('spatial_error', 'edge_contrast', 'edge_radius', 'tile')},
              'source_gate_unchanged': original, 'frames': frames,
              'complete_frame_pairs': len(frames),
              'exact_rgb_frame_pairs': sum(f['equal_rgb_pixels'] for f in frames.values()),
              'exact_byte_frame_pairs': sum(f['equal_complete_bytes'] for f in frames.values()),
              'within_explicit_bounds': all(f['within_bounds'] for f in frames.values())}
    a.out.parent.mkdir(parents=True, exist_ok=True)
    with a.out.open('x') as stream:
        json.dump(report, stream, indent=2, allow_nan=False)
        stream.write('\n')
    print(json.dumps({k: report[k] for k in ('complete_frame_pairs', 'exact_rgb_frame_pairs',
                                           'exact_byte_frame_pairs', 'within_explicit_bounds')}))
    return 0 if report['within_explicit_bounds'] else 1


if __name__ == '__main__':
    sys.exit(main())
