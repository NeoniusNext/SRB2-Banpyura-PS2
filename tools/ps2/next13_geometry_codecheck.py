#!/usr/bin/env python3
"""Resolve actual geometry bodies/callers, full RELAs, data, frames and SDK.

Reuses the preceding accepted strict linked-body resolver and its binary
negative controls. No compiler, linker, emulator or production edit occurs.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

import next12_pb_scratch_codecheck as strict
from next12_pb_scratch_finalbind import exact_build,FINAL_ELF,sha
import next13_geometry_variant as variant


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--baseline',type=Path,default=variant.ROOT/'build/nov15-next12/final/hardware')
    p.add_argument('--candidate',type=Path,default=variant.WORK/'geometry-context-v2')
    p.add_argument('--inventory',type=Path,default=variant.WORK/'geometry-inventory-v2/report.json')
    p.add_argument('--baseline-linker-relas',type=Path,default=variant.ROOT/'build/nov15-next12/final/pb-binding/final-linker-relas')
    p.add_argument('--candidate-linker-relas',type=Path,default=variant.WORK/'geometry-linker-relas-v2')
    p.add_argument('--out',type=Path,default=variant.WORK/'geometry-codecheck-v2')
    p.add_argument('--negative-controls',action='store_true')
    a=p.parse_args();out=a.out.resolve()
    if out.exists() or not out.is_relative_to(variant.WORK):raise RuntimeError('Fresh next13 proof output required')
    raw=json.loads(a.inventory.read_text())
    if raw['variant']!='v2' or len(raw['actual_changed_instruction_bodies'])!=4:
        raise RuntimeError('Exact geometry-v2 inventory required')
    sources=variant.expected('v2');pins={n:hashlib.sha256(b).hexdigest() for n,b in sources.items()}
    actual={p.relative_to(a.candidate/'snapshot').as_posix():sha(p) for p in (a.candidate/'snapshot/src').rglob('*') if p.is_file()}
    if actual!=pins:raise RuntimeError('Actual complete compiled geometry source differs')
    builds={'baseline':a.baseline.resolve(),'candidate':a.candidate.resolve()}
    linkers={'baseline':a.baseline_linker_relas.resolve(),'candidate':a.candidate_linker_relas.resolve()}
    admissions={}
    for tag,build in builds.items():
        for name,entry in raw['artifacts'][tag].items():
            if sha(build/name)!=entry['sha256']:raise RuntimeError('Inventoried actual artifact changed')
        admissions[tag]=exact_build(build,variant.WORK/'geometry-baseline-v1' if tag=='baseline' else build/'snapshot',
            FINAL_ELF if tag=='baseline' else raw['artifacts'][tag]['SRB2.ELF']['sha256'])
    inventory={'changed_code_sections_or_bindings':raw['changed_code_sections_or_bindings'],
               'direct_actual_callers':sorted(set(raw['direct_actual_callers'])|{'.text.HWR_RenderBatches'})}
    out.mkdir(parents=True);(out/'pinned-tools').mkdir()
    toolnames=('next13_geometry_codecheck.py','next13_geometry_inventory.py','next13_geometry_variant.py',
        'next12_pb_scratch_codecheck.py','next12_pb_scratch_finalbind.py','next12_pb_scratch_objectcheck.py',
        'next11_sw_checkplane_codecheck.py','next11_sw_linker_relas.py','next10_sw_bounds_codecheck.py',
        'next7_render_fog_codecheck.py','next8_sw_codecheck.py','next11_local_rng.py')
    tools={}
    for name in toolnames:
        path=variant.ROOT/'tools/ps2'/name;target=out/'pinned-tools'/name;shutil.copyfile(path,target)
        tools[name]={'original':str(path),'snapshot':str(target),'sha256':sha(target)}
    report={'kind':__doc__,'status':'RUNNING','complete_build_admissions':admissions,
        'complete_source_overlay_sha256':pins,'object_inventory':str(a.inventory.resolve()),'object_inventory_sha256':sha(a.inventory),
        'proof_tools':tools,'retained_linker_replays':{},'variants':{}}
    binders={}
    for tag,build in builds.items():
        replay=linkers[tag]/'linker-rela-report.json';record=json.loads(replay.read_text())
        if record['original_ELF_sha256']!=sha(build/'SRB2.ELF') or record['original_engine_sha256']!=sha(build/'obj/engine_lto.o'):
            raise RuntimeError('Retained exact linker replay belongs to another actual context')
        for name,digest in record['all_pinned_linker_inputs_unchanged'].items():
            if sha(Path(name))!=digest:raise RuntimeError('An exact original linker replay input changed')
        report['retained_linker_replays'][tag]={'path':str(replay),'sha256':sha(replay),'content':record}
        value,result=strict.run_variant(tag,build,linkers[tag],inventory,out)
        binders[tag]=value;report['variants'][tag]=result
    report['actual_instruction_vs_binding_inventory']=strict.instruction_inventory(binders['baseline'],binders['candidate'],inventory)
    names=set(report['variants']['baseline']['complete_bound_text'])&set(report['variants']['candidate']['complete_bound_text'])
    report['all_bound_complete_frame_save_metadata_equal']=all(
        report['variants']['baseline']['complete_bound_text'][name]['exact_frame']==
        report['variants']['candidate']['complete_bound_text'][name]['exact_frame'] for name in names)
    if not report['all_bound_complete_frame_save_metadata_equal']:raise RuntimeError('Actual complete frame/save metadata differs')
    if a.negative_controls:
        report['binary_negative_controls']=strict.binary_controls(out,binders['candidate'],report['variants']['candidate'])
    for entry in tools.values():
        if sha(Path(entry['original']))!=entry['sha256'] or sha(Path(entry['snapshot']))!=entry['sha256']:
            raise RuntimeError('A proof tool changed during exact binding')
    if sha(a.inventory)!=report['object_inventory_sha256']:raise RuntimeError('Frozen inventory changed during binding')
    for tag,build in builds.items():
        if exact_build(build,variant.WORK/'geometry-baseline-v1' if tag=='baseline' else build/'snapshot',
           raw['artifacts'][tag]['SRB2.ELF']['sha256'])!=admissions[tag]:raise RuntimeError('Actual build changed during binding')
    report['status']='PASS complete actual default Hardware geometry body/caller/RELA/data/literal/frame/SDK binding; native cost and integration pending'
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'status':report['status'],'report':str(out/'report.json'),
        'bound_functions':{tag:len(row['complete_bound_text']) for tag,row in report['variants'].items()},
        'negative_controls':len(report.get('binary_negative_controls',[]))},indent=2))


if __name__=='__main__':main()
