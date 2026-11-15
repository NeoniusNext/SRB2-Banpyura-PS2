#!/usr/bin/env python3
"""Bind actual dec_level, complete callers, wrappers, data, literals and frames."""
import argparse
import hashlib
import json
from pathlib import Path
import re

from next13_texture_variant import ROOT
from next12_pb_scratch_codecheck import binder, source_relas, sdk_memory, boundary_records
from next10_sw_bounds_codecheck import frame


def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def inventory(value):
    result={}
    for name in value.obj.names:
        if not name.startswith('.text.'):continue
        index=value.obj.index[name]
        records=[]
        for row in source_relas(value.obj,index):
            symbol=dict(row['symbol']);symbol.pop('size')
            if symbol['section']<len(value.obj.names):symbol['section']=value.obj.names[symbol['section']]
            records.append({'offset':row['offset'],'kind':row['kind'],'addend':row['addend'],'symbol':symbol})
        result[name]={'bytes':value.obj.bytes(value.obj.sections[index]),'records':records}
    return result


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--base',type=Path,default=ROOT/'build/nov15-next12/final/hardware')
    p.add_argument('--base-relas',type=Path,default=ROOT/'build/nov15-next12/final/pb-binding/final-linker-relas')
    p.add_argument('--candidate',type=Path,required=True)
    p.add_argument('--candidate-relas',type=Path)
    p.add_argument('--out',type=Path,required=True)
    a=p.parse_args()
    if a.out.exists():raise SystemExit('Fresh exact code-binding output required')
    a.out.mkdir(parents=True)
    builds={'base':a.base.resolve(),'cur':a.candidate.resolve()}
    relas={'base':a.base_relas.resolve(),'cur':(a.candidate_relas or a.candidate/'linker-relas').resolve()}
    values={tag:binder(builds[tag],relas[tag]) for tag in builds}
    inv={tag:inventory(value) for tag,value in values.items()}
    if set(inv['base'])!=set(inv['cur']):raise SystemExit('Unexpected complete code section set difference')
    differences={}
    for name in inv['base']:
        left,right=inv['base'][name],inv['cur'][name]
        if left!=right:
            differences[name]={'instruction_or_padding_bytes_changed':left['bytes']!=right['bytes'],
                'ordered_RELA_semantics_except_target_size_changed':left['records']!=right['records'],
                'baseline_bytes':len(left['bytes']),'candidate_bytes':len(right['bytes'])}
    direct=set()
    for tag,value in values.items():
        for section in value.obj.names:
            if not section.startswith('.text.'):continue
            for record in source_relas(value.obj,value.obj.index[section]):
                target=record['symbol'];target_section=value.obj.names[target['section']] if target['section']<len(value.obj.names) else ''
                if target['name']=='dec_level' or target_section=='.text.dec_level':direct.add(section[6:])
    mandatory={name[6:] for name in differences}|direct|{'dec_level','dec_level_ref','__wrap_memcpy','__wrap_memset'}
    reports={}
    for tag,value in values.items():
        checked={};discarded=[]
        for name in sorted(mandatory):
            section='.text.'+name
            if section not in value.maps or not value.maps[section][0] or name not in value.symbols:
                discarded.append({'name':name,'frame':frame(value.obj,name),'reason':'No live out-of-line body in this actual game'})
                continue
            checked[name]=value.text(name)
        directory=a.out/(tag+'-sdk');directory.mkdir()
        sdk=sdk_memory(value,builds[tag],directory)
        helper=checked['dec_level']
        if any(r['kind']==4 for r in helper['ordered_RELA_records']):raise SystemExit('Unexpected actual helper callee')
        reports[tag]={'build':str(builds[tag]),'engine_sha256':sha(builds[tag]/'obj/engine_lto.o'),
          'ELF_sha256':sha(builds[tag]/'SRB2.ELF'),'map_sha256':sha(builds[tag]/'SRB2.map'),
          'build_report_sha256':sha(builds[tag]/'build-report.json'),'complete_bound_code':checked,
          'complete_referenced_data':value.data,'complete_literals':list(value.literals.values()),
          'every_literal_RELA_use_and_exact_anchor':value.literal_uses,'discarded_out_of_line_helpers':discarded,
          'actual_reached_ABI_boundaries':boundary_records(value,checked),'actual_SDK_memory_bodies':sdk,
          'helper_has_no_callee_or_wrapper_reference':True,'direct_dec_level_callers':sorted(direct),
          'capacity_symbols':{name:value.symbols[name] for name in ('_end','_gp','_stack_size','_stack','_heap_size') if name in value.symbols},
          'all_complete_resolved_body_data_literal_initializer_capacity_and_frame_checks_passed':True}
    result={'code_section_differences':differences,'direct_dec_level_callers':sorted(direct),
       'actual_bound_code_union':sorted(mandatory),'variants':reports,
       'source_scope':'Only isolated dec_level score selection, complete602 source freeze bound separately',
       'tool_sha256':sha(Path(__file__))}
    (a.out/'code-binding.json').write_text(json.dumps(result,indent=2)+'\n')
    print('Bound actual helper/caller/wrapper/data/literal/frame graph; changed sections='+str(len(differences)))


if __name__=='__main__':main()
