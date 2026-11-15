#!/usr/bin/env python3
"""Bind the measured geometry graph to the selected actual all-up Hardware ELF.

Read-only: no EE compiler, linker, emulator or production mutation. Canonical
full-source release compilation and its binding remain a later separate gate.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import shutil
import struct

from audio_storage_next7_finalbind import Object
from next12_pb_scratch_finalbind import exact_build,exact_source,FINAL_ELF,sha
from next12_pb_scratch_objectcheck import pdr
import next12_pb_scratch_codecheck as strict
import next13_geometry_variant as variant

ROOT=variant.ROOT
WORK=variant.WORK
WRITERS=('HWR_PBAdd.lto_priv.0','HWR_PBSprQuad','HWR_ProcessPolygon.part.0','HWR_ProcessPolygonSlow')
SELECTED={
    'src/hardware/hw_pbatch.inc':'9d025beaed5c982180f8d8fbb94bcc692e132f753f3100b4b1bdff59c1f51aee',
    'src/hardware/hw_main.c':'25ab400933e147ae202648629b088999f7f2b3d84a70a839aeed471cffcdd3a6',
    'src/hardware/hw_cache.c':'869c84455d07bbf55af24d0f0fc48914809c5261e68c2a72d8baebce9551a857',
    'src/ps2/hw/ps2_hw_tex.inc':'091e790814cf00002765b633f0bf12aa6d875880eefec59685f6ba5fb02401ea',
}


def sources(path):return {p.relative_to(path).as_posix():sha(p) for p in (path/'src').rglob('*') if p.is_file()}


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build',type=Path,default=WORK/'combined-classic-v1')
    p.add_argument('--source',type=Path)
    p.add_argument('--linker-relas',type=Path)
    p.add_argument('--out',type=Path,default=WORK/'combined-classic-v1/geometry-binding-v1')
    p.add_argument('--negative-controls',action='store_true')
    a=p.parse_args();build,out=a.build.resolve(),a.out.resolve()
    source=(a.source or build/'snapshot').resolve();linker=(a.linker_relas or build/'linker-relas').resolve()
    if out.exists() or not out.is_relative_to(WORK):raise RuntimeError('Fresh isolated geometry binding output required')
    base=ROOT/'build/nov15-next12/final/source';baseline_build=ROOT/'build/nov15-next12/final/hardware'
    baseline={n:r['sha256'] for n,r in exact_source(base).items()}
    if baseline!={n:r['sha256'] for n,r in exact_source(base,'353cc94103aeb38cef87d26151c779302135d0e9').items()}:
        raise RuntimeError('Tested/published complete source baseline differs')
    expected={**baseline,**SELECTED};actual=sources(source)
    if len(actual)!=602 or actual!=expected:raise RuntimeError('Selected exact602 all-up source differs')
    report_path=build/'build-report.json';build_report=json.loads(report_path.read_text())
    context=json.loads((build/'context-report.json').read_text())
    if context['complete_source_overlay_sha256']!=expected or not context['whole_LTO_option_section_bytes_identical_all_four_units']:
        raise RuntimeError('Actual all-up source/options admission differs')
    if len(context['complete_actual_IR_objects'])!=205 or not context['all_other201_complete_IR_inputs_identical']:
        raise RuntimeError('Complete actual205-input all-up graph required')
    inputs=list(context['complete_actual_IR_objects'])
    command=context['lto_command'];actual_order=[arg for arg in command if arg in context['complete_actual_IR_objects']]
    canonical_order=[str(build/'obj'/Path(name).name) for name in context['canonical_IR_objects']]
    if len(actual_order)!=205 or actual_order!=inputs or actual_order!=canonical_order:raise RuntimeError('Exact205 input ordering differs')
    options=json.loads((build/'actual-unit-options.json').read_text())
    if sha(build/'actual-unit-options.json')!=context['actual_unit_options_sha256'] or len(options)!=4:
        raise RuntimeError('All four complete actual option records required')
    for unit,row in options.items():
        if not row['whole_options_equal_canonical']:raise RuntimeError('An actual whole option section differs')
        if row['actual_existing_override']!=(['-O3'] if unit=='src/ps2/hw/ps2_hwd.c' else []):
            raise RuntimeError('Existing per-unit flag policy differs')
    admissions={'baseline':exact_build(baseline_build,base,FINAL_ELF),
                'all_up':exact_build(build,source,context['ELF_sha256'])}
    measured=WORK/'geometry-context-v2'
    measured_proof_path=WORK/'geometry-codecheck-v2/report.json';measured_proof=json.loads(measured_proof_path.read_text())
    measured_fp_path=WORK/'geometry-fp-proof-v2/report.json';measured_fp=json.loads(measured_fp_path.read_text())
    if sha(measured/'obj/engine_lto.o')!=measured_proof['variants']['candidate']['engine_sha256']:
        raise RuntimeError('Measured geometry native input changed')
    objects={'measured_geometry':Object(measured/'obj/engine_lto.o'),'all_up':Object(build/'obj/engine_lto.o'),
             'baseline':Object(baseline_build/'obj/engine_lto.o')}
    procedures={tag:pdr(obj) for tag,obj in objects.items()}
    writer_identity={}
    for name in WRITERS:
        section='.text.'+name;old,new=objects['measured_geometry'],objects['all_up']
        if old.signature(section)!=new.signature(section):raise RuntimeError('Complete measured writer code changed: '+name)
        if procedures['measured_geometry']['records'][section]['raw_words']!=procedures['all_up']['records'][section]['raw_words']:
            raise RuntimeError('Actual measured writer frame/save words differ')
        raw=new.content(new.sections[section]);controls=[];fp=[]
        for i,word in enumerate(struct.unpack('<'+'I'*(len(raw)//4),raw)):
            if word>>26==17:
                if (word>>21)&31 in (2,6):controls.append({'offset':4*i,'word_hex':f'{word:08x}'})
                if (word>>21)&31==16:fp.append({'offset':4*i,'word_hex':f'{word:08x}','function':word&63,
                    'fs':(word>>11)&31,'ft':(word>>16)&31,'fd':(word>>6)&31})
        if controls or fp!=measured_fp['complete_actual_bodies']['candidate'][name]['ordered_scalar_FP_instructions']:
            raise RuntimeError('Exact actual floating/control order differs from reviewed proof')
        writer_identity[name]={'whole_input_instruction_bytes_identical':True,'whole_input_body_sha256':hashlib.sha256(raw).hexdigest(),
            'whole_frame_save_words_equal':True,'actual_frame':procedures['all_up']['records'][section],
            'measured_complete_ordered_RELA_records':old.relocations.get(section,[]),
            'all_up_complete_ordered_RELA_records':new.relocations.get(section,[]),'FCSR_accesses':controls,
            'exact_scalar_FP_order_equal':True,'ordered_scalar_FP_instructions':fp}
    origin_path=WORK/'geometry-inventory-v2/report.json';origin=json.loads(origin_path.read_text())
    changed=set(origin['changed_code_sections_or_bindings']);callers=set(origin['direct_actual_callers'])|{'.text.HWR_RenderBatches'}
    for obj in (objects['baseline'],objects['all_up']):
        for owner,records in obj.relocations.items():
            if owner in obj.sections and obj.sections[owner][2]&4 and any(row['target']['section'] in changed for row in records):callers.add(owner)
    inventory={'changed_code_sections_or_bindings':sorted(changed),'direct_actual_callers':sorted(callers)}
    out.mkdir(parents=True);(out/'pinned-tools').mkdir()
    toolnames=('next13_geometry_combined_bind.py','next12_pb_scratch_codecheck.py','next12_pb_scratch_finalbind.py',
        'next12_pb_scratch_objectcheck.py','next11_sw_checkplane_codecheck.py','next11_sw_linker_relas.py',
        'next10_sw_bounds_codecheck.py','next7_render_fog_codecheck.py','next8_sw_codecheck.py','next11_local_rng.py')
    tools={}
    for name in toolnames:
        old=ROOT/'tools/ps2'/name;new=out/'pinned-tools'/name;shutil.copyfile(old,new)
        tools[name]={'original':str(old),'snapshot':str(new),'sha256':sha(new)}
    report={'status':'RUNNING','scope':'Actual selected all-up geometry graph; canonical full-source release build/binding pending',
        'complete_selected602_source_sha256':expected,'exact_actual_build_admissions':admissions,
        'actual205_order':actual_order,'actual_all_four_options_sha256':sha(build/'actual-unit-options.json'),
        'measured_geometry_proof_sha256':sha(measured_proof_path),'reviewed_FP_proof_sha256':sha(measured_fp_path),
        'origin_geometry_inventory_sha256':sha(origin_path),'actual_geometry_graph_inventory':inventory,
        'complete_measured_writer_to_all_up_identity':writer_identity,'proof_tools':tools,'variants':{}}
    binders={}
    for tag,role_build,role_linker in [('baseline',baseline_build,ROOT/'build/nov15-next12/final/pb-binding/final-linker-relas'),
                                     ('all_up',build,linker)]:
        replay_path=role_linker/'linker-rela-report.json';replay=json.loads(replay_path.read_text())
        if replay['original_ELF_sha256']!=sha(role_build/'SRB2.ELF') or replay['original_engine_sha256']!=sha(role_build/'obj/engine_lto.o'):
            raise RuntimeError('Exact retained linker replay belongs to another graph')
        for path,digest in replay['all_pinned_linker_inputs_unchanged'].items():
            if sha(Path(path))!=digest:raise RuntimeError('A retained replay input changed')
        value,bound=strict.run_variant(tag,role_build,role_linker,inventory,out)
        binders[tag]=value;report['variants'][tag]=bound
    report['actual_instruction_vs_binding_inventory']=strict.instruction_inventory(binders['baseline'],binders['all_up'],inventory)
    common=set(report['variants']['baseline']['complete_bound_text'])&set(report['variants']['all_up']['complete_bound_text'])
    report['all_recorded_selected_frame_differences']={name:{tag:report['variants'][tag]['complete_bound_text'][name]['exact_frame'] for tag in ('baseline','all_up')}
        for name in sorted(common) if report['variants']['baseline']['complete_bound_text'][name]['exact_frame']!=
        report['variants']['all_up']['complete_bound_text'][name]['exact_frame']}
    for name in WRITERS:
        actual_bound=report['variants']['all_up']['complete_bound_text'][name]
        writer_identity[name]['complete_all_up_resolved_body_sha256']=actual_bound['resolved_text_sha256']
        writer_identity[name]['complete_all_up_frame_instruction_witness']=actual_bound['frame_instruction_witness']
    if a.negative_controls:report['binary_negative_controls']=strict.binary_controls(out,binders['all_up'],report['variants']['all_up'])
    if sources(source)!=expected or exact_build(build,source,context['ELF_sha256'])!=admissions['all_up']:
        raise RuntimeError('Actual selected graph/source changed during binding')
    for row in tools.values():
        if sha(Path(row['original']))!=row['sha256'] or sha(Path(row['snapshot']))!=row['sha256']:raise RuntimeError('A proof tool changed')
    report['status']='PASS actual selected all-up geometry/RELA/data/literal/PDR/SDK and floating-proof binding; canonical release binding and runtime admission pending'
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'status':report['status'],'report':str(out/'report.json'),
        'bound_functions':{tag:len(row['complete_bound_text']) for tag,row in report['variants'].items()},
        'selected_frame_differences':list(report['all_recorded_selected_frame_differences']),
        'measured_writer_input_bodies_exact':len(writer_identity),'negative_controls':len(report.get('binary_negative_controls',[]))},indent=2))


if __name__=='__main__':main()
