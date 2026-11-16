#!/usr/bin/env python3
"""Observe the existing sampled absolute libc break without changing ELF layout.

Fresh copies of the canonical seeded pressure ELFs receive one NOP in the
out-of-line PS2Mem_LibcPeak return expression. Its sole actual caller is the
final ZSTAT. All inline relative reports and every preceding instruction stay
unchanged. Original pressure failures remain immutable evidence.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import sys

from next7_render_fog_codecheck import Elf32
from next8_sw_codecheck import linked_symbols
from next10_sw_bounds_codecheck import output_bytes, relocations
from next12_runtime import run_verified
from next13_flats_pressure import admit_elf
import next12_pb_scratch_codecheck as strict

ROOT = Path(__file__).resolve().parents[2]
RELATIVE = re.compile(r'^ps2_mem: C heap peak above the arena (\d+) B; main stack used (\d+) B of (\d+) B \(0 = -zstack off\)$', re.M)
BREAK = re.compile(r'^ps2_mem: brk 0x([0-9a-f]+), heap start 0x([0-9a-f]+), RAM (\d+) B, main stack (\d+) B$', re.M)
TIME_FIELDS = {'cycles', 't_ms'}


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def read_json(path):
    return json.loads(Path(path).read_text())


def replace_option(command, name, value):
    require(command.count(name) == 1, 'Unique canonical option required: ' + name)
    result = list(command)
    result[result.index(name) + 1] = str(value)
    return result


def streams(boot):
    result = {prefix: [line for line in boot.splitlines() if line.startswith(prefix)]
              for prefix in ('LHASH ', 'HWPH ', 'HWLT ', 'HWWS ')}
    result['HWPROF resource tail'] = [line.partition(' | polys=')[2]
        for line in boot.splitlines() if line.startswith('HWPROF win=')]
    # These additional existing non-time rows may be absent in a given run.
    for prefix in ('HW mem ', '[zreg]', '[spill] '):
        result[prefix] = [line for line in boot.splitlines() if line.startswith(prefix)]
    return result


def read_run(run):
    summary = read_json(run / 'summary.json')
    require(not summary['rc'] and summary['done'] and not summary['errors']
            and len(summary['zstat']) == 1, 'Incomplete native run: ' + str(run))
    boot = (run / 'boot.txt').read_text(errors='replace')
    require('HWE startup renderer=Hardware chosen=2 hardware=1' in boot
            and not re.search(r'Switching to the Software renderer|HARDWARE -> SOFTWARE', boot),
            'Actual Hardware renderer absent')
    require(not any(map(int, re.findall(r'(?:underruns|hmis|herr)=(\d+)', boot)))
            and 'WARNING:' not in boot, 'Unexpected native warning/audio error')
    relative, brk = list(RELATIVE.finditer(boot)), list(BREAK.finditer(boot))
    require(len(relative) == len(brk) == 1, 'Exactly one final relative report/break required')
    zstat = {key: int(value) for key, value in summary['zstat'][0].items()}
    require(zstat['frames'] == zstat['levelframes'] == 350 and zstat['ram'] == 33554432,
            'Matched 350-frame retail32 run required')
    return {'ZSTAT': zstat, 'streams': streams(boot),
            'relative_report': relative[0].group(0), 'relative_peak': int(relative[0][1]),
            'logged_brk': int(brk[0][1], 16), 'heap_start': int(brk[0][2], 16),
            'break_report': brk[0].group(0),
            'libc_report': re.findall(r'^ps2_mem: libc heap .*$', boot, re.M),
            'boot_sha256': sha(run / 'boot.txt'), 'summary_sha256': sha(run / 'summary.json'),
            'config_sha256': sha(run / 'reference.cfg')}


def bind_and_patch(build, seeded, out):
    binder = strict.binder(build, None)
    bound = {name: binder.text(name) for name in
             ('PS2Mem_LibcPeak', 'ZA_InitMem', 'ZA_InitHeap')}
    peak = binder.symbols['PS2Mem_LibcPeak']
    require(peak['size'] == 104 and bound['PS2Mem_LibcPeak']['exact_frame']['stack_frame_bytes'] == 16,
            'Actual peak body/frame changed')
    calls = []
    for name in binder.obj.names:
        if name.startswith('.text.'):
            for row in relocations(binder.obj, name[6:]):
                if row['symbol']['name'] == 'PS2Mem_LibcPeak':
                    calls.append({'owner': name[6:], 'offset': row['offset'],
                                  'kind': row['kind'], 'addend': row['addend']})
    require(calls == [{'owner': 'PS2Mem_Frame', 'offset': 2696, 'kind': 4, 'addend': 0}],
            'Final ZSTAT must be the only noninline caller')
    actual_calls = []
    for name, section in zip(binder.elf.names, binder.elf.sections):
        if name != '.text':  # .vutext contains VU instructions, not EE JALs.
            continue
        raw = binder.elf.bytes(section)
        for offset in range(0, len(raw) - 3, 4):
            word = struct.unpack_from('<I', raw, offset)[0]
            address = section[3] + offset
            if word >> 26 == 3 and ((address + 4) & 0xf0000000 | (word & 0x3ffffff) << 2) == peak['value']:
                actual_calls.append(address)
    caller = binder.symbols['PS2Mem_Frame']
    require(actual_calls == [caller['value'] + 2696], 'Linked JAL inventory differs')
    source = (build.parent / 'source/src/ps2/ps2_mem.c').read_text()
    require('za_brk_max - (size_t)za_end : 0' in source
            and 'za_end = za_base + bytes;' in source, 'Bound source origin formula changed')
    require(source.count('"ZSTAT ram=') == 1, 'Unique final ZSTAT producer required')
    raw = seeded.read_bytes()
    parsed = Elf32(seeded)
    require(linked_symbols(parsed) == binder.symbols, 'Seeded symbols/layout changed')
    require(output_bytes(parsed, peak['value'], peak['size']) ==
            output_bytes(binder.elf, peak['value'], peak['size']), 'Seed patch changed peak observer')
    address = peak['value'] + 0x44
    section = parsed.sections[peak['section']]
    offset = section[4] + address - section[3]
    require(struct.unpack_from('<I', raw, offset)[0] == 0x00431023,
            'Expected exact SUBU v0,v0,v1 at Peak+0x44')
    changed = bytearray(raw)
    struct.pack_into('<I', changed, offset, 0)
    require(bytes(changed[:offset]) == raw[:offset] and bytes(changed[offset + 4:]) == raw[offset + 4:]
            and len(changed) == len(raw), 'Observer changed another ELF byte')
    path = out / 'ABSOLUTE-SAMPLED-PEAK.ELF'
    path.write_bytes(changed)
    probe = Elf32(path)
    require(linked_symbols(probe) == binder.symbols, 'Observer changed defined symbols/extent')
    report_body = binder.symbols['PS2Mem_Report']
    require(output_bytes(probe, report_body['value'], report_body['size']) ==
            output_bytes(parsed, report_body['value'], report_body['size']), 'Inline relative report changed')
    return path, {'complete_actual_bound_bodies': bound, 'complete_bound_data': binder.data,
        'engine_RELA_callers': calls, 'linked_JAL_call_addresses': actual_calls,
        'sole_caller_is_final_ZSTAT': True, 'peak_address': peak['value'],
        'instruction_address': address, 'instruction_file_offset': offset,
        'old_word': '00431023', 'new_word': '00000000', 'same_file_bytes': len(raw),
        'only_this_word_changed': True, 'all_symbols_addresses_and_sizes_unchanged': True,
        'relative_report_complete_body_unchanged': True,
        'seeded_ELF_sha256': sha(seeded), 'observer_ELF_sha256': sha(path),
        'source_sha256': sha(build.parent / 'source/src/ps2/ps2_mem.c'),
        'return_scope': 'Existing ready/max>arena-end guards retained; positive path returns recorded sampled max. All updates unchanged.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--canonical-pressure', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    canonical, out = args.canonical_pressure.resolve(), args.out.resolve()
    require(out.is_relative_to(ROOT / 'build') and out != ROOT / 'build' and not out.exists(),
            'Fresh child of build/ required')
    ready = read_json(canonical / 'readiness.json')
    old_report = read_json(canonical / 'report.json')
    require(ready['frames'] == 350 and old_report['errors'] == ['Memory pressure increased: libcpeak'],
            'Expected preserved canonical RED raw peak failure')
    for path, digest in ready['input_sha256'].items():
        asset = Path(path)
        if asset.suffix == '.PAK' or asset.name == 'FINEACON.DAT':
            require(sha(asset) == digest, 'Canonical asset changed: ' + path)
    out.mkdir(parents=True)
    shutil.copyfile(__file__, out / Path(__file__).name)
    pins = {canonical / 'readiness.json', canonical / 'report.json', Path(__file__).resolve(),
            out / Path(__file__).name}
    report = {'diagnostic_only': True, 'original_gate_report_unchanged': True,
        'original_pressure_status': 'RED: Memory pressure increased: libcpeak',
        'original_pressure_report': str(canonical / 'report.json'),
        'original_raw_libcpeak_delta': old_report['numeric_deltas']['libcpeak'],
        'runs': {}, 'errors': [], 'sampled_peak_scope': 'Existing frame/checkpoint sbrk samples, not every malloc or physical PS2 timing.'}
    plans = []
    for role in ('baseline', 'candidate'):
        admission = ready['admissions'][role]
        build, seeded = Path(admission['build']), Path(admission['diagnostic_ELF'])
        require(admit_elf(build, seeded, ready['seed']) == admission, 'Canonical seed admission differs')
        directory = out / role
        directory.mkdir()
        probe, patch = bind_and_patch(build, seeded, directory)
        pre = canonical / 'runtime' / role / 'run-pre.json'
        original_command = read_json(pre)['command']
        require(Path(original_command[original_command.index('--elf') + 1]) == seeded,
                'Canonical command/seed ELF mismatch')
        command = replace_option(original_command, '--elf', probe)
        command = replace_option(command, '--out', out / 'runs')
        restored = replace_option(replace_option(command, '--elf', seeded), '--out',
                                  original_command[original_command.index('--out') + 1])
        require(restored == original_command, 'Other canonical arguments changed')
        original_run = canonical / 'runs' / role
        before = read_run(original_run)
        require(before['ZSTAT'] == old_report['runs'][role]['ZSTAT'],
                'Canonical original counters changed')
        require(before['relative_peak'] == before['ZSTAT']['libcpeak'] > 0,
                'Positive canonical native peak formula path required')
        require({k: before['streams'][k] for k in old_report['runs'][role]['streams']} ==
                old_report['runs'][role]['streams'], 'Canonical ordered streams changed')
        pins.update([pre, probe, seeded, build / 'SRB2.ELF', build / 'SRB2.map',
                     build / 'build-report.json', build / 'obj/engine_lto.o',
                     build.parent / 'source/src/ps2/ps2_mem.c'])
        pins.update(original_run / name for name in
                    ('boot.txt', 'summary.json', 'ps2args', 'reference.cfg', 'SRB2.ELF'))
        pak = Path(command[command.index('--pak') + 1])
        pins.update([*pak.glob('*.PAK'), pak / 'FINEACON.DAT'])
        plans.append((role, command, original_run, probe, before, patch))
    manifest = {str(path): sha(path) for path in sorted(pins)}
    (out / 'readiness.json').write_text(json.dumps({'canonical': str(canonical),
        'immutable_input_sha256': manifest, 'commands': {p[0]: p[1] for p in plans}}, indent=2) + '\n')
    pins.add(out / 'readiness.json')
    for role, command, original_run, probe, before, patch in plans:
        print('START absolute sampled-peak probe ' + role, flush=True)
        result = run_verified(command, out / 'runtime' / role, sorted(pins))
        require(result.returncode == 0, 'Official observer replay failed: ' + role)
        original_pre = read_json(canonical / 'runtime' / role / 'run-pre.json')
        current_pre = read_json(out / 'runtime' / role / 'run-pre.json')
        for key in ('critical_settings', 'emulator_binary_sha256'):
            require(current_pre[key] == original_pre[key], 'Canonical native runtime changed: ' + key)
        run = out / 'runs' / role
        after = read_run(run)
        require(sha(run / 'SRB2.ELF') == sha(probe), 'Staged observer ELF differs')
        require((run / 'ps2args').read_bytes() == (original_run / 'ps2args').read_bytes(),
                'Actual staged arguments differ')
        require(after['config_sha256'] == before['config_sha256'], 'Saved native config differs')
        for path, digest in manifest.items():
            asset = Path(path)
            if asset.suffix == '.PAK' or asset.name == 'FINEACON.DAT':
                require(sha(run / asset.name) == digest, 'Staged canonical asset differs: ' + asset.name)
        errors = []
        require(set(before['ZSTAT']) == set(after['ZSTAT']), 'Native counter schema changed')
        deltas = {key: after['ZSTAT'][key] - value for key, value in before['ZSTAT'].items()}
        changed = {key: delta for key, delta in deltas.items()
                   if key not in TIME_FIELDS | {'libcpeak'} and delta}
        if changed:
            errors.append('Other non-time native counters changed: ' + str(changed))
        equality = {key: value == after['streams'][key] for key, value in before['streams'].items()}
        if not all(equality.values()):
            errors.append('Other ordered non-time resource streams changed')
        relative_same = before['relative_report'] == after['relative_report']
        if not relative_same or before['break_report'] != after['break_report'] or before['libc_report'] != after['libc_report']:
            errors.append('Original inline relative/break/libc report changed')
        absolute = after['ZSTAT']['libcpeak']
        if absolute != after['logged_brk']:
            errors.append('Direct sampled max differs from logged report break; origin derivation withheld')
        origin = absolute - after['relative_peak'] if relative_same and absolute == after['logged_brk'] else None
        report['runs'][role] = {'patch': patch, 'canonical': before, 'observer': after,
            'non_time_ZSTAT_other_changes': changed, 'all_ZSTAT_deltas': deltas,
            'ordered_stream_equality': {key: {'equal': equal, 'rows': len(before['streams'][key])}
                                        for key, equal in equality.items()},
            'relative_inline_report_retained': relative_same, 'direct_sampled_absolute_peak': absolute,
            'derived_runtime_arena_end': origin,
            'derivation': 'E=M-R from bound positive-path formula, only after observed M==logged report break and unchanged inline R.',
            'errors': errors}
        report['errors'].extend(role + ': ' + error for error in errors)
        print(role + ': absolute sampled peak=' + str(absolute) + ' relative=' + str(after['relative_peak'])
              + ' derived origin=' + str(origin) + ' errors=' + str(errors), flush=True)
    left, right = (report['runs'][role] for role in ('baseline', 'candidate'))
    report['direct_absolute_peak_delta'] = right['direct_sampled_absolute_peak'] - left['direct_sampled_absolute_peak']
    report['derived_origin_delta'] = (right['derived_runtime_arena_end'] - left['derived_runtime_arena_end']
        if left['derived_runtime_arena_end'] is not None and right['derived_runtime_arena_end'] is not None else None)
    if report['direct_absolute_peak_delta']:
        report['errors'].append('Direct sampled absolute peak changed')
    if report['derived_origin_delta'] != -report['original_raw_libcpeak_delta']:
        report['errors'].append('Measured peak/origin relation does not explain the raw relative delta')
    require(manifest == {path: sha(path) for path in manifest}, 'Original frozen evidence/input changed')
    report['all_frozen_inputs_unchanged'] = True
    report['input_sha256'] = manifest
    report['passed'] = not report['errors']
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'passed': report['passed'], 'absolute_peak_delta': report['direct_absolute_peak_delta'],
        'derived_origin_delta': report['derived_origin_delta'], 'original_pressure_remains_RED': True,
        'errors': report['errors'], 'report': str(out / 'report.json')}, indent=2), flush=True)
    return int(bool(report['errors']))


if __name__ == '__main__':
    sys.exit(main())
