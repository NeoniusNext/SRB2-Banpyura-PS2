#!/usr/bin/env python3
"""Bind the cut-V2 flat surface to its own canonical602/205 final Hardware build.

Read-only proof; no compiler/emulator. Replays the whole component-selected
surface on the actual final object/ELF, its direct interfaces, data, literals,
RELAs, frames and capacities. SDK closures have separately named reports.
"""
import argparse
import json
from pathlib import Path
import shutil

from audio_storage_next7_finalbind import Object
from next12_pb_scratch_objectcheck import allocated, pdr
import next12_pb_scratch_codecheck as strict
from next13_flats_cold_codecheck import actual_binder, controls
from next13_flats_pressure import ROOT, admit_elf, sha


REVISION = '5481bbbdc5aa3167c046836f3e51e4dbb31ad807'
CACHE_SHA = '869c84455d07bbf55af24d0f0fc48914809c5261e68c2a72d8baebce9551a857'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate', type=Path, default=ROOT / 'build/nov15-next13/final/hardware')
    parser.add_argument('--baseline', type=Path, default=ROOT / 'build/nov15-next12/final/hardware')
    parser.add_argument('--source', type=Path, default=ROOT / 'build/nov15-next13/final/source')
    parser.add_argument('--source-admission', type=Path, default=ROOT / 'build/nov15-next13/final/hardware-source-admission-v1/report.json')
    parser.add_argument('--component', type=Path, default=ROOT / 'build/nov15-next13/flats-cut-binding-v2/report.json')
    parser.add_argument('--diagnostics', type=Path, default=ROOT / 'build/nov15-next13/local-rng/final-hardware/patch-report.json')
    parser.add_argument('--out', type=Path, default=ROOT / 'build/nov15-next13/final/flats-binding-v1')
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    admission_path = args.candidate / 'canonical-build-admission.json'
    admission = json.loads(admission_path.read_text())
    source = {p.relative_to(args.source).as_posix(): sha(p) for p in (args.source / 'src').rglob('*') if p.is_file()}
    source_admission = json.loads(args.source_admission.read_text())
    if len(source) != 602 or source != admission['all602_source_sha256'] or source != source_admission['source_sha256']:
        raise RuntimeError('Actual final canonical602 source authority differs')
    if admission['source_commit'] != REVISION or source['src/hardware/hw_cache.c'] != CACHE_SHA:
        raise RuntimeError('Actual final source revision or selected cut-V2 source differs')
    if sha(args.source_admission) != admission['source_admission_sha256']:
        raise RuntimeError('Actual canonical source admission changed')
    if not admission['all203_C_freshly_compiled'] or not admission['all203_whole_LTO_option_sections_match_original_default']:
        raise RuntimeError('Actual fresh canonical whole-default options authority absent')
    ir = admission['complete_actual205_IR_objects']
    if len(ir) != 205 or any(sha(path) != digest for path, digest in ir.items()):
        raise RuntimeError('Complete actual205 fresh canonical IR changed')
    if sha(args.candidate / 'SRB2.ELF') != admission['ELF_sha256'] or sha(args.candidate / 'obj/engine_lto.o') != admission['engine_sha256']:
        raise RuntimeError('Actual canonical engine/ELF admission changed')
    component = json.loads(args.component.read_text())
    selected = set(component['selected_complete_body_sections'])
    selected |= {'.text.' + name for name in ('HWR_GetLevelFlat', 'HWR_LoadMapTextures', 'HWR_FreeMapTextures',
        'HWR_PS2_RegenerateMipmap', 'HWR_PS2_StealData', 'HWR_GetTexture')}
    tools = [Path(__file__), ROOT / 'tools/ps2/next13_flats_cold_codecheck.py', ROOT / 'tools/ps2/next13_flats_cut_codecheck.py',
        ROOT / 'tools/ps2/next12_pb_scratch_codecheck.py', ROOT / 'tools/ps2/next12_pb_scratch_objectcheck.py',
        ROOT / 'tools/ps2/next10_sw_bounds_codecheck.py', ROOT / 'tools/ps2/next11_sw_checkplane_codecheck.py',
        ROOT / 'tools/ps2/next7_render_fog_codecheck.py', ROOT / 'tools/ps2/next8_sw_codecheck.py',
        ROOT / 'tools/ps2/audio_storage_next7_finalbind.py', ROOT / 'tools/ps2/next13_flats_pressure.py']
    pinned = out / 'pinned-tools'
    pinned.mkdir()
    for tool in tools:
        shutil.copyfile(tool, pinned / tool.name)
    report = {'status': 'RUNNING final canonical flat binding', 'source_commit': REVISION, 'all602_source_sha256': source,
        'complete_actual205_IR_objects': ir, 'canonical_build_admission_sha256': sha(admission_path),
        'canonical_source_admission_sha256': sha(args.source_admission), 'inherited_component_report_sha256': sha(args.component),
        'inherited_component_NoOPT_scope': 'Original isolated cut-V2 full ELF/IR/PDR restore; no final combined opt-out compilation claimed',
        'selected_complete_body_sections': sorted(selected), 'tools': {str(t): sha(t) for t in tools}, 'variants': {}}
    binders = {}
    for role, build, linker in [('baseline', args.baseline, ROOT / 'build/nov15-next12/final/pb-binding/final-linker-relas'),
                                 ('candidate', args.candidate, args.candidate / 'linker-relas')]:
        value = actual_binder(build.resolve(), linker.resolve())
        binders[role] = value
        obj = Object(build / 'obj/engine_lto.o')
        frames = pdr(obj)
        checked, discarded = {}, []
        for section in sorted(selected):
            if section not in value.obj.index:
                discarded.append({'section': section, 'body_present': False})
            elif section not in value.maps or not value.maps[section][0]:
                discarded.append({'section': section, 'body_present': True,
                    'complete_input_signature': obj.signature(section), 'complete_ordered_RELAs': obj.relocations.get(section, []),
                    'exact_PDR': frames['records'].get(section)})
            else:
                checked[value.section_function(section)['name']] = value.text(value.section_function(section)['name'])
        report['variants'][role] = {'complete_bound_text': checked, 'discarded_bodies': discarded,
            'complete_referenced_data': value.data, 'complete_literals': list(value.literals.values()),
            'every_exact_literal_use': value.literal_uses, 'actual_call_boundaries': strict.boundary_records(value, checked),
            'complete_input_PDRs_and_ordered_RELAs': frames, 'ELF_sha256': sha(build / 'SRB2.ELF'),
            'engine_sha256': sha(build / 'obj/engine_lto.o'), 'map_sha256': sha(build / 'SRB2.map'),
            'memory_symbols': {name: value.symbols[name] for name in ('_end', '_gp', '_fbss', '_etext') if name in value.symbols},
            'retained_linker_report_sha256': sha(linker / 'linker-rela-report.json')}
    report['binary_negative_controls'] = controls(out / 'binary-negatives', binders['candidate'], report['variants']['candidate']['complete_bound_text'])
    old, new = [report['variants'][role]['memory_symbols'] for role in ('baseline', 'candidate')]
    report['actual_static_boundaries'] = {name: {'baseline': old[name]['value'], 'candidate': new[name]['value'],
        'delta': new[name]['value'] - old[name]['value']} for name in ('_end', '_gp')}
    if any(row['delta'] for row in report['actual_static_boundaries'].values()):
        raise RuntimeError('Final actual static end/GP differ')
    patches = json.loads(args.diagnostics.read_text())
    report['both_actual_final_diagnostic_admissions'] = {}
    for seed, patch in patches['variants'].items():
        admitted = admit_elf(args.candidate.resolve(), Path(patch['ELF']), int(seed, 16))
        # admit_elf verifies every byte outside the complete212B initializer,
        # not merely the selected function spans or an external hash report.
        report['both_actual_final_diagnostic_admissions'][seed] = admitted
    report['diagnostic_patch_report_sha256'] = sha(args.diagnostics)
    report['closure_scope'] = 'Whole component-selected flat/load/free/parser/wrapper/Spill/PB/caller surface and exact direct call interfaces; separate reports supply full SDK/callee closures'
    report['no_compiler_or_emulator_invoked'] = True
    report['status'] = 'PASS complete actual canonical cut-V2 flat surface/data/literal/RELA/PDR/capacity binding and both diagnostic byte admissions'
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'status': report['status'], 'bound_bodies': {r: len(v['complete_bound_text']) for r, v in report['variants'].items()},
        'binary_controls': len(report['binary_negative_controls']), 'report_sha256': sha(out / 'report.json')}))


if __name__ == '__main__':
    main()
