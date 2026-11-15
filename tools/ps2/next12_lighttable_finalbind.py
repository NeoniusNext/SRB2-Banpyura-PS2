"""Bind final published-source Hardware code/data/SDK graph and both seed ELFs.

Pure read-only checking plus archived binary negative controls. Exact retained
linker RELAs are required; no capacity or instruction field is normalized.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import shutil
import struct

from next7_render_fog_codecheck import Elf32
from next8_sw_codecheck import linked_symbols
from next10_sw_bounds_codecheck import code, frame, output_bytes
from next11_sw_checkplane_codecheck import canonical_data
from next12_pb_scratch_codecheck import binder as strict_binder
from next12_lighttable_codecheck import signature, controls, copy_binder
from next12_lighttable_allocator import SDK
from next12_lighttable_packed import witness
from next12_local_rng import verify_patch, stub_words

ROOT = Path(__file__).resolve().parents[2]
STORE = 'lt_store.lto_priv.0'


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def admission(build, snapshot, component_source):
    manifest_path = snapshot / 'manifest.json'; manifest = json.loads(manifest_path.read_text())
    observed = {str(p.relative_to(snapshot)): sha(p) for p in sorted((snapshot / 'src').rglob('*')) if p.is_file()}
    component = {str(p.relative_to(component_source)): sha(p) for p in sorted((component_source / 'src').rglob('*')) if p.is_file()}
    if observed != manifest['source_sha256'] or observed != component or len(observed) != 602:
        raise SystemExit('All602 final sources must exactly equal the executed combined component sources')
    report = json.loads((build / 'build-report.json').read_text()); checked = {}
    for name, value in report['input_sha256'].items():
        path = Path(name)
        if path.is_relative_to(ROOT / 'src'):
            relative = 'src/' + str(path.relative_to(ROOT / 'src'))
            if observed.get(relative) != value: raise SystemExit('Actual canonical compiled source/header differs from frozen publication')
            checked[name] = value
    if not report['hardware'] or len(report['sources']) != 203 or report['compiled'] != 203:
        raise SystemExit('Fresh complete default203-C plus2-VU canonical Hardware build required')
    if any(report['input_sha256'].get(str(ROOT / name)) != observed[name] for name in report['sources']):
        raise SystemExit('A canonical primary C source is not pinned to the final602 tree')
    for name in report['sources']:
        path = build / 'obj' / name.replace('/', '__').replace('.c', '.o')
        if not path.is_file(): raise SystemExit('A complete canonical IR input is missing')
    if any(not (build / 'obj' / (name + '_vsm.o')).is_file() for name in ('ps2_hw_vu1', 'ps2_hw_vu1_spr')):
        raise SystemExit('Both complete actual VU inputs required')
    return {'source_revision': manifest['revision'], 'manifest_sha256': sha(manifest_path),
            'all602_source_sha256': observed, 'actual_compiled_source_and_header_pins': checked,
            'all203_C_and2_VU_IR_inputs_present': True, 'build_report_sha256': sha(build / 'build-report.json')}


def data_signature(value):
    result = []
    for name, entry in sorted(value.data.items()):
        index = value.obj.index[name]; section = value.obj.sections[index]
        records = [(r['offset'], r['kind'], r['exact_symbol'], r['explicit_addend']) for r in entry['ordered_RELA_records']]
        result.append((name, section[1], section[2], section[5], section[8],
                       None if section[1] == 8 else value.obj.bytes(section).hex(), records))
    return result


def bind_diagnostic(original, patch, path, names, allocator, allocator_dir):
    if sha(path) != patch['ELF_sha256']: raise SystemExit('Actual final diagnostic ELF pin differs')
    payload = struct.pack('<10I', *stub_words(patch['seed_uint32'], patch['actual_M_RandomSeed_address']))
    verify_patch(original.elf.data, path.read_bytes(), patch['patched_file_offset'], patch['owned_symbol_bytes'], payload)
    value = copy_binder(original, elf=Elf32(path)); value.literal_uses = []
    roots = {name: value.text(name) for name in names}; sdk = {}
    for function, record in allocator['actual_complete_SDK_member_bindings'].items():
        member = allocator_dir / record['member']
        if sha(member) != record['member_sha256']: raise SystemExit('Final SDK member pin differs')
        sdk[function] = SDK(Elf32(member), value, record['archive'], record['member']).section_text(function)
    return {'ELF': str(path), 'ELF_sha256': sha(path), 'seed': patch['seed_hex'],
            'only_original_initializer_extent_changed': True, 'complete_selected_function_bindings': roots,
            'complete_final_SDK_bindings': sdk, 'all_complete_referenced_data': list(value.data.values()),
            'every_merged_literal_use': value.literal_uses,
            'scope': 'Final actual canonical helper/caller/SDK graph fully bound into this exact layout-preserving diagnostic ELF; staged-run admission is recorded separately'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'build/nov15-next12/final/hardware')
    parser.add_argument('--snapshot', type=Path, default=ROOT / 'build/nov15-next12/final/source')
    parser.add_argument('--linker', type=Path, required=True); parser.add_argument('--allocator', type=Path, required=True)
    parser.add_argument('--component', type=Path, default=ROOT / 'build/nov15-next12/lighttable-combined-v2')
    parser.add_argument('--rng', type=Path, default=ROOT / 'build/nov15-next12/local-rng/final-hardware')
    parser.add_argument('--optout-build', type=Path); parser.add_argument('--optout-linker', type=Path)
    parser.add_argument('--original-build', type=Path); parser.add_argument('--original-linker', type=Path)
    parser.add_argument('--out', type=Path, required=True); parser.add_argument('--negative-controls', action='store_true')
    args = parser.parse_args(); build = args.build.resolve(); component = args.component.resolve(); out = args.out.resolve()
    if out.exists() or not out.is_relative_to(ROOT / 'build'): raise SystemExit('Fresh immutable final binding stage required')
    out.mkdir(parents=True); pinned = out / 'pinned-tools'; pinned.mkdir()
    for name in ('next12_lighttable_finalbind.py', 'next12_lighttable_codecheck.py', 'next12_lighttable_allocator.py',
                 'next12_lighttable_packed.py', 'next12_pb_scratch_codecheck.py', 'next11_sw_checkplane_codecheck.py',
                 'next11_sw_linker_relas.py', 'next12_local_rng.py', 'next10_sw_bounds_codecheck.py'):
        shutil.copyfile(ROOT / 'tools/ps2' / name, pinned / name)
    admitted = admission(build, args.snapshot.resolve(), component / 'candidate-source')
    actual = strict_binder(build, args.linker.resolve())
    executed = strict_binder(component / 'candidate-hardware', component / 'link-rela-candidate-v1')
    component_report_path = component / 'binding-v1/binding-report.json'
    component_report = json.loads(component_report_path.read_text())
    names = list(component_report['versions']['candidate']['actual_linked_function_bindings'])
    allocator_dir = args.allocator.resolve(); allocator_path = allocator_dir / 'allocator-report.json'; allocator = json.loads(allocator_path.read_text())
    if allocator['ELF_sha256'] != sha(build / 'SRB2.ELF'): raise SystemExit('Allocator proof belongs to another canonical game')
    names = list(dict.fromkeys([*names, *allocator['actual_complete_engine_function_bindings']]))
    roots = {}; previous = {}
    for name in names:
        if signature(actual.obj, name) != signature(executed.obj, name) or frame(actual.obj, name) != frame(executed.obj, name):
            raise SystemExit('Complete final/component instructions/targets/addends/frame differ: ' + name)
        roots[name] = actual.text(name); previous[name] = executed.text(name)
    if data_signature(actual) != data_signature(executed):
        raise SystemExit('Complete final/component referenced raw data/capacities/RELA records differ')
    original = linked_symbols(Elf32(ROOT / 'build/nov15-next12/baseline/hardware/SRB2.ELF'))
    final = linked_symbols(actual.elf)
    if any(original[n]['value'] != final[n]['value'] for n in ('_end', '_gp')):
        raise SystemExit('Final actual baseline end/GP capacity differs')
    patches_path = args.rng.resolve() / 'patch-report.json'; patches = json.loads(patches_path.read_text())
    if patches['original_inputs']['elf']['sha256'] != sha(build / 'SRB2.ELF'):
        raise SystemExit('Diagnostic source is not the final actual canonical game')
    diagnostic = {seed: bind_diagnostic(actual, record, Path(record['ELF']), names, allocator, allocator_dir)
                  for seed, record in patches['variants'].items()}
    if set(diagnostic) != {'01b2d3f5', '9e3779b9'}: raise SystemExit('Both exact local seeds required')
    report = {'scope': 'Final committed full-default Hardware graph, all literal uses/data capacities/frames and exact actual SDK graph; no capacity normalization',
        'source_and_actual_full_build_admission': admitted, 'shipping_ELF_sha256': sha(build / 'SRB2.ELF'),
        'engine_sha256': sha(build / 'obj/engine_lto.o'), 'map_sha256': sha(build / 'SRB2.map'),
        'component_binding_report_sha256': sha(component_report_path), 'allocator_report_sha256': sha(allocator_path),
        'table_candidate_roots': len(component_report['versions']['candidate']['actual_linked_function_bindings']),
        'additional_spill_roots': len(allocator['actual_complete_engine_function_bindings']),
        'all_complete_final_and_executed_component_code_RELA_targets_addends_frames_and_referenced_data_equal': True,
        'actual_final_function_bindings': roots, 'actual_component_function_bindings': previous,
        'all_actual_final_referenced_data': list(actual.data.values()), 'every_final_merged_literal_use': actual.literal_uses,
        'exact_baseline_static_boundaries': {n: final[n]['value'] for n in ('_end', '_gp')},
        'actual_final_packed_load': witness(code(actual.obj, STORE)['words']),
        'both_actual_final_diagnostic_ELF_bindings': diagnostic, 'RNG_patch_report_sha256': sha(patches_path)}
    if args.optout_build or args.optout_linker:
        if not args.optout_build or not args.optout_linker: raise SystemExit('Complete opt-out build/linker pair required')
        opt = strict_binder(args.optout_build.resolve(), args.optout_linker.resolve())
        if not args.original_build or not args.original_linker:
            raise SystemExit('Opt-out binding requires the matched current-generated-header original-five-source control')
        reference_build = args.original_build.resolve()
        reference = strict_binder(reference_build, args.original_linker.resolve())
        old_names = ['HWR_GetLightTableID','HWR_CreateLightTable','HWR_UpdateLightTable',STORE,
            'hw_CreateLightTable','hw_UpdateLightTable','HWR_Lighting','HWR_ClearLightTables',
            'hw_ClearLightTables','hw_SetScreenPalette','clut_alloc','clut_build_into',
            'clut_fixed_refresh.lto_priv.0','clut_get.lto_priv.0','PS2HWD_FillDriver',
            'ov_flush.part.0.lto_priv.0','__wrap_memcpy','__wrap_memset',
            'pk_alloc.lto_priv.0','pk_flush.lto_priv.0','vu_chunk_end.lto_priv.0']
        function_names = {n[6:] for n in reference.obj.names if n.startswith('.text.')}
        if function_names != {n[6:] for n in opt.obj.names if n.startswith('.text.')}:
            raise SystemExit('Matched original/opt-out complete function inventories differ')
        for name in sorted(function_names):
            if signature(opt.obj,name) != signature(reference.obj,name):
                raise SystemExit('Matched original/opt-out complete whole-engine instructions/targets/addends differ: '+name)
        old = {}; control = {}
        for name in old_names:
            if signature(opt.obj, name) != signature(reference.obj, name) or frame(opt.obj, name) != frame(reference.obj, name):
                raise SystemExit('Final PB-active opt-out original graph differs: ' + name)
            old[name] = reference.text(name); control[name] = opt.text(name)
        if data_signature(opt) != data_signature(reference):
            raise SystemExit('Final PB-active opt-out original data/capacities differ')
        report['current_header_PB_active_optout_original_graph'] = {'original_reference': old, 'final_context_control': control,
            'matched_current_generated_header_original_source_build':str(reference_build),
            'all_whole_engine_function_sections_and_ordered_RELA_records_equal':True,'whole_engine_function_sections':len(function_names),
            'historical_source85_CLUT_inlining_identity_is_not_claimed':True,
            'all_complete_original_code_targets_addends_frames_and_data_capacities_retained': True,
            'no_new_indexed_API_is_linked': all(n not in linked_symbols(opt.elf) for n in ('PS2HWD_CreateIndexedLightTable','PS2HWD_UpdateIndexedLightTable'))}
    if args.negative_controls:
        report['executed_final_binary_mutation_controls'] = controls(out / 'binary-controls', actual, 'HWR_Lighting', STORE)
    report['pinned_tool_sha256'] = {p.name: sha(p) for p in pinned.iterdir()}
    (out / 'final-binding-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Bound final shipping Hardware graph, SDK, exact source and both diagnostic ELFs: ' + str(out))


if __name__ == '__main__': main()
