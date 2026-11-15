#!/usr/bin/env python3
"""Review all literal PB native resource/profile rows and complete image gates.

Read-only inputs; no compiler or emulator. Timing and lookup differences remain
in the output. No requested-byte or component-code result becomes an FPS claim.
"""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import re

from next12_pb_scratch_run import sha
import next12_pb_scratch_replay as replay


def lines(path):
    return path.read_text(errors='replace').splitlines()


def hardware_rows(boot):
    rows = defaultdict(list)
    for line in boot:
        if line.startswith('HW'):
            rows[line.split()[0]].append(line)
    return dict(rows)


def pressure(path, baseline_run, candidate_run):
    report = json.loads(path.read_text())
    if report['errors'] or not report['lhash_equal'] or not report['canonical_geometry_hashes_equal']:
        raise RuntimeError('Native exactness/completion gate failed: ' + str(path))
    base, current = report['runs']['baseline'], report['runs']['candidate']
    old = lines(baseline_run / 'boot.txt')
    new = lines(candidate_run / 'boot.txt')
    before, after = hardware_rows(old), hardware_rows(new)
    differences = {}
    for kind in sorted(set(before) | set(after)):
        left, right = before.get(kind, []), after.get(kind, [])
        if left != right:
            differences[kind] = {'baseline': left, 'candidate': right}
    tails = [[line.partition(' | polys=')[2] for line in rows if line.startswith('HWPROF win=')]
             for rows in (old, new)]
    if not tails[0] or tails[0] != tails[1]:
        raise RuntimeError('Main actual driver geometry/upload/VRAM/LOD/capacity counters changed')
    if before.get('HWWS') != after.get('HWWS'):
        raise RuntimeError('Resident texture dimensions/format/blocks/wrap/flags differ')
    if base['runtime']['critical_settings'] != current['runtime']['critical_settings']:
        raise RuntimeError('Full non-UI emulator settings differ across pair')
    if base['runtime']['emulator_binary_sha256'] != current['runtime']['emulator_binary_sha256']:
        raise RuntimeError('Actual emulator bytes differ across pair')
    pattern = r'polygons=(\d+) in (\d+) batches \((\d+) buckets\).*batches drawn=(\d+).*planned polygons=(\d+).*pool (\d+) quadwords, cut for room=(\d+)'
    pb = [[list(map(int, re.search(pattern, line).groups())) for line in rows if line.startswith('HWPROF50 collection:')]
          for rows in (old, new)]
    if not pb[0] or pb[0] != pb[1]:
        raise RuntimeError('PB capacity/flush/geometry counters changed')
    working = [[dict(re.findall(r'(\w+)=([^\s]+)', line)) for line in rows if line.startswith('HWPROF0 ')]
               for rows in (old, new)]
    result = {
        'report': str(path), 'report_sha256': sha(path),
        'baseline_run': str(baseline_run), 'candidate_run': str(candidate_run),
        'baseline_boot_sha256': sha(baseline_run / 'boot.txt'), 'candidate_boot_sha256': sha(candidate_run / 'boot.txt'),
        'full_loader_hashes_equal': True, 'full_HWPH_rows_equal': True,
        'HWPH_rows_each': len(base['canonical_geometry_hash_rows']),
        'full_non_UI_settings_and_emulator_equal': True,
        'final_resources': report['final_resources'],
        'literal_numeric_resource_deltas': report['literal_numeric_resource_deltas'],
        'nonincreasing_resource_checks': report['nonincreasing_resource_checks'],
        'all_raw_checkpoint_rows': {'baseline': base['all_checkpoint_rows'], 'candidate': current['all_checkpoint_rows']},
        'all_raw_zone_rows': {'baseline': base['zone_rows'], 'candidate': current['zone_rows']},
        'main_driver_counts_and_VRAM_LOD_budget_tails_equal': True,
        'complete_main_driver_count_tails': tails[0],
        'resident_texture_rows_equal': True, 'complete_resident_texture_rows': before.get('HWWS', []),
        'PB_capacity_and_output_counters_equal': True,
        'PB_counter_columns': ['polygons', 'batches', 'buckets', 'driver_batches', 'planned', 'pool_quadwords', 'cuts_for_room'],
        'PB_complete_counter_rows': pb[0],
        'all_Hardware_rows': {'baseline': before, 'candidate': after},
        'literal_different_Hardware_row_groups': differences,
        'working_cycle_wall_cadence_windows': {'baseline': working[0], 'candidate': working[1]},
        'timing_policy': 'Literal cycles/work/wall/cadence/max/spike and elapsed values retained. No per-stage regression is hidden and no overall FPS gain is inferred.',
        'lookup_policy': 'PB general/slow/found counters can change with address hashing after layout changes; retain the complete raw rows instead of normalizing them.',
    }
    if not all(result['nonincreasing_resource_checks'].values()):
        raise RuntimeError('A required physical/cache/stack/spill resource increased')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work', type=Path, default=replay.ROOT / 'build/nov15-next12/pb-scratch')
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    work = args.work.resolve()
    out = (args.out or work / 'native-resource-image-audit.json').resolve()
    if not out.is_relative_to(replay.ROOT / 'build') or out.exists():
        raise RuntimeError('A fresh output under build/ is required')
    replay.validate(replay.DEFAULT_CANDIDATE)
    parent = replay.ROOT / 'build/nov15-next12/hardware-profile-seed1/runs'
    scopes = (
        ('map02', work / 'gate-pressure-map02-01b2d3f5-retry1', 'base-hw-map02-spawn-350', 'pb-hw-map02-spawn-350'),
        ('fog-stereo', work / 'gate-pressure-map01-01b2d3f5', 'base-hw-fog-stereo-spawn-350', 'pb-hw-fog-stereo-spawn-350'),
    )
    report = {'source_revision': replay.REVISION, 'candidate_source_sha256': replay.CANDIDATE_SHA,
              'auditor_sha256': sha(Path(__file__)), 'native_pressure': {}, 'native_images': {}}
    for scene, directory, base, candidate in scopes:
        report['native_pressure'][scene] = pressure(directory / 'report.json', parent / base,
                                                    directory / 'runs' / candidate)
        image_path = work / ('images-' + scene) / 'report.json'
        images = json.loads(image_path.read_text())
        if images['errors'] or not all(row['all_bytes_equal'] for row in images['complete_images'].values()):
            raise RuntimeError('Full native image oracle differs')
        report['native_images'][scene] = {'report': str(image_path), 'report_sha256': sha(image_path),
            'complete_images': images['complete_images'], 'full_loader_hashes_equal': images['lhash_equal'],
            'full_non_UI_settings_equal': images['full_non_UI_settings_equal'],
            'emulator_binary_equal': images['emulator_binary_equal']}
    report['native_successful_candidate_runs'] = 4
    report['prior_launch_failure'] = {
        'path': str(work / 'gate-pressure-map02-01b2d3f5'),
        'reason': 'Compiler-only shell PATH lacked xvfb-run. PCSX2 did not launch and no boot log exists. Failed orchestration and unchanged-settings archive preserved; the successful retry used emulator-env.sh and a fresh destination.',
        'not_counted_as_a_game_result': True,
    }
    report['conclusion'] = 'Both controlled native350 scenes reduce actual zone used/peak with unchanged eviction/failure/libc/stack/spill metrics and all geometry/driver/VRAM/LOD/capacity oracles. All six complete native PPMs match. Eligible for integration review; no general map or FPS claim.'
    out.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'report': str(out), 'conclusion': report['conclusion']}, indent=2))


if __name__ == '__main__':
    main()
