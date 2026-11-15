#!/usr/bin/env python3
"""Bind PB bodies/data/frames/literals to the actual combined final Hardware ELF.

This is a separate final-release stage. It does not rewrite the frozen PB-only
checker, inventory, linked proof, RNG copies or runtime evidence. No compiler
or emulator is invoked. The narrow PB opt-out keeps the final light table active.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

from audio_storage_next7_finalbind import Object
import next12_pb_scratch_codecheck as component
from next12_pb_scratch_objectcheck import allocated, pdr

ROOT = component.ROOT
WORK = ROOT / 'build/nov15-next12'
REVISION = 'd4fbc7c319f19edc70eda220c3323d0350504c98'
FINAL_ELF = 'efc963c7876a5f2a1c395baf5ba9982b68c6db1e56811d7a5e8a690ba840ab20'
COMPONENT_CHECKER = 'd1d4fd6d3565ead4987b6ce1a7ab03751bede72bdfd2a0ba6300233347607df4'
COMPONENT_REPORT = '6ab36c2ee73e894e62c080e3b64197b0ff5f7ea74a3f36a0568e52342caa27fe'
FINAL_DIAGNOSTICS = {
    '01b2d3f5': '6766d019d20ef841ee12604011c466231c4cb9ba213e0467ed92f313bbdfb122',
    '9e3779b9': '6abf462952a14316408645966dd845f0178e568b24ff4179dd795fd4e5dde47d',
}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def exact_source(source, revision=REVISION):
    """Verify all 602 source files against their exact committed Git blobs."""
    raw = subprocess.check_output(['git', 'ls-tree', '-rz', '--full-tree', revision, '--', 'src'], cwd=ROOT)
    expected = {}
    for record in raw.split(b'\0'):
        if not record:
            continue
        metadata, name = record.split(b'\t', 1)
        mode, kind, blob = metadata.decode().split()
        if kind != 'blob' or mode != '100644':
            raise SystemExit('An exact regular source blob is required')
        expected[name.decode()] = blob
    paths = {p.relative_to(source).as_posix(): p for p in (source / 'src').rglob('*') if p.is_file()}
    if len(expected) != 602 or set(paths) != set(expected):
        raise SystemExit('The complete exact602 committed source set differs')
    result = {}
    for name, path in sorted(paths.items()):
        payload = path.read_bytes()
        blob = hashlib.sha1(b'blob ' + str(len(payload)).encode() + b'\0' + payload).hexdigest()
        if blob != expected[name]:
            raise SystemExit('A frozen source differs from its exact committed Git blob: ' + name)
        result[name] = {'git_blob': blob, 'sha256': hashlib.sha256(payload).hexdigest(), 'bytes': len(payload)}
    if result['src/hardware/hw_pbatch.inc']['sha256'] != component.replay.CANDIDATE_SHA:
        raise SystemExit('Final PB include differs from the accepted component')
    return result


def exact_build(build, source, expected_elf=None, optout=False):
    report_path = build / 'build-report.json'
    report = json.loads(report_path.read_text())
    if not report['hardware'] or len(report['sources']) != 203 or report['diagnostic_build']:
        raise SystemExit('Complete default release203-C/2-VU Hardware context required')
    flags = report['flags']
    if not {'-O2', '-G8', '-flto', '-DPS2', '-DPS2_PROFILE', '-DHWRENDER', '-Werror'} <= set(flags):
        raise SystemExit('Actual default release/O2/G8/LTO/Profile Hardware flags differ')
    if ('-DPS2_NOOPT_PB_SCRATCH' in flags) != optout:
        raise SystemExit('Narrow final PB opt-out flag differs')
    if sha(build / 'SRB2.ELF') != report['elf_sha256'] or (expected_elf and report['elf_sha256'] != expected_elf):
        raise SystemExit('Actual final shipping ELF/report admission differs')
    inputs = {}
    for name, digest in report['input_sha256'].items():
        path = Path(name)
        actual = source / path.relative_to(ROOT) if path.is_relative_to(ROOT / 'src') else path
        # An opt-out context records its already frozen snapshot paths directly.
        if sha(actual) != digest:
            raise SystemExit('Actual complete source/generated/external build input changed: ' + str(actual))
        inputs[name] = {'checked_path': str(actual), 'sha256': digest}
    objects = [build / 'obj' / name.replace('/', '__').replace('.c', '.o') for name in report['sources']]
    objects += [build / 'obj' / (name + '_vsm.o') for name in ('ps2_hw_vu1', 'ps2_hw_vu1_spr')]
    if len(objects) != 205 or len(set(objects)) != 205:
        raise SystemExit('Exactly205 actual full LTO inputs are required')
    artifacts = {name: {'path': str(build / name), 'sha256': sha(build / name), 'bytes': (build / name).stat().st_size}
                 for name in ('obj/engine_lto.o', 'SRB2.ELF', 'SRB2.map', 'build-report.json')}
    return {'build': str(build), 'artifacts': artifacts, 'complete_actual_IR_objects': {str(p): sha(p) for p in objects},
            'all_actual_build_inputs': inputs, 'actual_flags': flags, 'actual_link_command': report['link_command']}


def object_delta(before, after):
    left, right = allocated(before), allocated(after)
    changed = sorted(name for name in set(left) | set(right) if left.get(name) != right.get(name))
    code = [name for name in changed if (name in before.sections and before.sections[name][2] & 4)
            or (name in after.sections and after.sections[name][2] & 4)]
    callers = {}
    for tag, obj in (('old_PB', before), ('final_PB', after)):
        for owner, records in obj.relocations.items():
            if owner not in obj.sections or not obj.sections[owner][2] & 4:
                continue
            uses = [r for r in records if r['target']['section'] in code]
            if uses:
                callers.setdefault(owner, {})[tag] = {'complete_signature': obj.signature(owner),
                    'complete_ordered_relocations': records, 'changed_target_uses': uses}
    return {'complete_changed_allocated_sections': changed, 'changed_code_sections_or_bindings': code,
            'direct_actual_callers': callers}


def pin_tools(out):
    names = ('next12_pb_scratch_finalbind.py', 'next12_pb_scratch_finalcontext.py',
        'next12_pb_scratch_codecheck.py', 'next12_pb_scratch_objectcheck.py', 'next12_pb_scratch_context.py',
        'next12_pb_scratch_replay.py', 'next11_sw_checkplane_codecheck.py', 'next11_sw_linker_relas.py',
        'next10_sw_bounds_codecheck.py', 'next10_sw_bounds_context.py', 'next7_render_fog_codecheck.py',
        'next8_sw_codecheck.py', 'next11_local_rng.py', 'audio_storage_next7_finalbind.py')
    folder = out / 'pinned-tools'
    folder.mkdir()
    result = {}
    for name in names:
        source = ROOT / 'tools/ps2' / name
        target = folder / name
        shutil.copyfile(source, target)
        result[name] = {'original': str(source), 'snapshot': str(target), 'sha256': sha(target)}
    (folder / 'manifest.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--final-build', type=Path, default=WORK / 'final/hardware')
    parser.add_argument('--final-source', type=Path, default=WORK / 'final/source')
    parser.add_argument('--final-linker-relas', type=Path, default=WORK / 'final/pb-binding/final-linker-relas')
    parser.add_argument('--control-build', type=Path, default=WORK / 'final/pb-noopt-hardware')
    parser.add_argument('--control-linker-relas', type=Path, default=WORK / 'final/pb-binding/control-linker-relas')
    parser.add_argument('--final-rng', type=Path, default=WORK / 'local-rng/final-hardware')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--negative-controls', action='store_true')
    args = parser.parse_args()
    out = args.out.resolve()
    if not out.is_relative_to(ROOT / 'build') or out == ROOT / 'build' or out.exists():
        raise SystemExit('A fresh isolated child of build/ is required')
    if sha(Path(component.__file__)) != COMPONENT_CHECKER:
        raise SystemExit('The frozen component checker changed')
    component_report_path = WORK / 'pb-scratch/resolved-binding/final-codecheck-v2/report.json'
    if sha(component_report_path) != COMPONENT_REPORT:
        raise SystemExit('The frozen component resolved report changed')
    component_report = json.loads(component_report_path.read_text())
    for name, digest in component_report['helper_sha256'].items():
        if sha(ROOT / 'tools/ps2' / name) != digest:
            raise SystemExit('A frozen component binding helper changed: ' + name)
    source = args.final_source.resolve()
    source_pins = exact_source(source)
    final = args.final_build.resolve()
    control = args.control_build.resolve()
    admissions = {'final': exact_build(final, source, FINAL_ELF),
                  'control': exact_build(control, control / 'snapshot', optout=True)}
    control_source = exact_source(control / 'snapshot')
    if control_source != source_pins:
        raise SystemExit('Final control source must remain exact final source; only the narrow compiler flag changes')
    context_path = control / 'context-report.json'
    context = json.loads(context_path.read_text())
    if not context['all_other_complete_IR_inputs_identical'] or context['only_recompiled_unit'] != 'src/hardware/hw_batching.c':
        raise SystemExit('Final control does not retain the other204 actual IR objects')
    final_by_name = {Path(name).name: digest for name, digest in admissions['final']['complete_actual_IR_objects'].items()}
    control_by_name = {Path(name).name: digest for name, digest in admissions['control']['complete_actual_IR_objects'].items()}
    changed_IR = sorted(name for name in final_by_name if final_by_name[name] != control_by_name[name])
    if changed_IR != ['src__hardware__hw_batching.o']:
        raise SystemExit('Final PB opt-out changed other actual full LTO inputs')
    final_obj, control_obj = Object(final / 'obj/engine_lto.o'), Object(control / 'obj/engine_lto.o')
    delta = object_delta(control_obj, final_obj)
    original_path = WORK / 'pb-scratch/objectcheck/report.json'
    if sha(original_path) != component_report['object_inventory_sha256']:
        raise SystemExit('The original complete PB object/root inventory changed')
    original = json.loads(original_path.read_text())
    inventory = {'changed_code_sections_or_bindings': sorted(set(original['changed_code_sections_or_bindings'])
                    | set(delta['changed_code_sections_or_bindings'])),
                 'direct_actual_callers': sorted(set(original['direct_actual_callers']) | set(delta['direct_actual_callers']))}
    out.mkdir(parents=True)
    report = {'kind': __doc__, 'checker_sha256': sha(Path(__file__)), 'source_revision': REVISION,
        'candidate_source_sha256': component.replay.CANDIDATE_SHA, 'complete_final_source': source_pins,
        'exact_final_and_control_admission': admissions, 'control_context_report_sha256': sha(context_path),
        'control_source_equals_final_source': True, 'control_changed_IR_inputs': changed_IR,
        'actual_final_control_object_inventory': delta, 'expanded_binding_inventory': inventory,
        'PB_attribution_with_final_light_table_active': {
            'changed_allocated_section_names_equal_original_component':
                delta['complete_changed_allocated_sections'] == original['changed_allocated_sections'],
            'changed_code_or_binding_names_equal_original_component':
                delta['changed_code_sections_or_bindings'] == original['changed_code_sections_or_bindings'],
            'direct_actual_caller_names_equal_original_component':
                sorted(delta['direct_actual_callers']) == sorted(original['direct_actual_callers']),
            'comparison_rule': 'Exact complete allocated-section type/flags/size/alignment/entry-size/bytes signatures and ordered full target/type/location/addend records; no capacity or instruction normalization'},
        'frozen_component_checker_sha256': COMPONENT_CHECKER,
        'frozen_component_resolved_report_sha256': COMPONENT_REPORT,
        'scope': 'Actual final shipping Hardware and final-source narrow PB opt-out with light table active. Each complete selected body/frame/data/literal is bound to its own actual game ELF. The earlier component proof is retained, and no component-to-final identity is inferred.',
        'variants': {}, 'proof_tools': pin_tools(out), 'retained_linker_replays': {}}
    binders = {}
    for tag, build, linker in (('final', final, args.final_linker_relas.resolve()),
                                ('control', control, args.control_linker_relas.resolve())):
        linker_report = linker / 'linker-rela-report.json'
        retained = json.loads(linker_report.read_text())
        for name, digest in retained['all_pinned_linker_inputs_unchanged'].items():
            if sha(Path(name)) != digest:
                raise SystemExit('An exact original final retained-RELA link input changed: ' + name)
        report['retained_linker_replays'][tag] = {'report_path': str(linker_report),
            'report_sha256': sha(linker_report), 'all_original_link_inputs_rechecked': retained['all_pinned_linker_inputs_unchanged']}
        value, result = component.run_variant(tag, build, linker, inventory, out)
        binders[tag] = value
        if tag == 'final':
            result['actual_RNG_diagnostic_copies'] = component.diagnostic_copies(value, args.final_rng.resolve(), result['complete_bound_text'])
            if {seed: item['sha256'] for seed, item in result['actual_RNG_diagnostic_copies'].items()} != FINAL_DIAGNOSTICS:
                raise SystemExit('The exact two final Hardware diagnostic-copy admissions differ')
        else:
            result['execution_scope'] = 'Read-only opt-out attribution control; no execution or CPU/FPS claim'
        report['variants'][tag] = result
    report['actual_final_PB_instruction_inventory'] = component.instruction_inventory(binders['control'], binders['final'], inventory)
    frames = {'final': pdr(final_obj), 'control': pdr(control_obj)}
    report['complete_actual_input_procedure_metadata'] = frames
    common = set(report['variants']['final']['complete_bound_text']) & set(report['variants']['control']['complete_bound_text'])
    report['exact_selected_control_final_frame_metadata_equal'] = all(
        report['variants']['final']['complete_bound_text'][name]['exact_frame'] ==
        report['variants']['control']['complete_bound_text'][name]['exact_frame'] for name in common)
    # Record real frame differences, never normalize or borrow a component frame.
    report['selected_control_final_frame_differences'] = {
        name: {tag: report['variants'][tag]['complete_bound_text'][name]['exact_frame'] for tag in ('control', 'final')}
        for name in common if report['variants']['final']['complete_bound_text'][name]['exact_frame'] !=
        report['variants']['control']['complete_bound_text'][name]['exact_frame']}
    if args.negative_controls:
        report['binary_negative_controls'] = component.binary_controls(out, binders['final'], report['variants']['final'])
    # Recheck all admissions after the entire proof, including SDK extraction.
    if admissions != {'final': exact_build(final, source, FINAL_ELF),
                      'control': exact_build(control, control / 'snapshot', optout=True)} or exact_source(source) != source_pins:
        raise SystemExit('Actual final/control/source input changed during proof')
    for entry in report['proof_tools'].values():
        if sha(Path(entry['original'])) != entry['sha256'] or sha(Path(entry['snapshot'])) != entry['sha256']:
            raise SystemExit('An exact final proof helper changed during verification')
    for entry in report['retained_linker_replays'].values():
        if sha(Path(entry['report_path'])) != entry['report_sha256'] or any(
                sha(Path(name)) != digest for name, digest in entry['all_original_link_inputs_rechecked'].items()):
            raise SystemExit('An exact final retained-RELA link input changed during verification')
    report['status'] = 'PASS actual final shipping PB resolved code/data/literal/frame/SDK binding; no new execution or FPS claim'
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'status': report['status'], 'report': str(out / 'report.json'),
        'functions': {tag: len(value['complete_bound_text']) for tag, value in report['variants'].items()},
        'negative_controls': len(report.get('binary_negative_controls', []))}, indent=2))


if __name__ == '__main__':
    main()
