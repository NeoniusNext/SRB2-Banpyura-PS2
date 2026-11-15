#!/usr/bin/env python3
"""Inventory complete actual geometry LTO bodies, RELAs, frames and ELF costs.

Read-only. Static instruction counts are not native elapsed costs. Final linked
RELA/data/literal/SDK resolution is a separate stage.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess

from audio_storage_next7_finalbind import Object
from next12_pb_scratch_objectcheck import allocated,pdr
from next12_pb_scratch_finalbind import exact_build,object_delta,FINAL_ELF,sha
from next7_render_fog_codecheck import Elf32
from next10_sw_bounds_codecheck import symbol_tables
import next13_geometry_variant as variant


def inspect(baseline,candidate,out,version='v2'):
    baseline,candidate,out=(Path(x).resolve() for x in (baseline,candidate,out))
    if out.exists() or not out.is_relative_to(variant.WORK):raise RuntimeError('Fresh next13 inventory required')
    context=json.loads((candidate/'context-report.json').read_text())
    candidate_pins=variant.expected(version)
    pins={name:hashlib.sha256(raw).hexdigest() for name,raw in candidate_pins.items()}
    actual={p.relative_to(candidate/'snapshot').as_posix():sha(p) for p in (candidate/'snapshot/src').rglob('*') if p.is_file()}
    if actual!=pins or context['complete_source_overlay_sha256']!=pins:raise RuntimeError('Actual frozen compiled source differs')
    if len(context['complete_actual_IR_objects'])!=205 or context['changed_actual_IR_objects']!=['src__hardware__hw_batching.o']:
        raise RuntimeError('Complete205-input one-unit native context required')
    for name,digest in context['complete_actual_IR_objects'].items():
        if sha(Path(name))!=digest:raise RuntimeError('Actual compiled IR input changed')
    admissions={'baseline':exact_build(baseline,variant.WORK/'geometry-baseline-v1',FINAL_ELF),
                'candidate':exact_build(candidate,candidate/'snapshot',context['ELF_sha256'])}
    objects={tag:Object(path/'obj/engine_lto.o') for tag,path in [('baseline',baseline),('candidate',candidate)]}
    signatures={tag:allocated(obj) for tag,obj in objects.items()};procedures={tag:pdr(obj) for tag,obj in objects.items()}
    delta=object_delta(objects['baseline'],objects['candidate'])
    out.mkdir(parents=True);shutil.copyfile(Path(__file__),out/Path(__file__).name)
    frames_equal={name:procedures['baseline']['records'].get(name,{}).get('raw_words')==
        procedures['candidate']['records'].get(name,{}).get('raw_words')
        for name in set(procedures['baseline']['records'])|set(procedures['candidate']['records'])}
    if not all(frames_equal.values()):raise RuntimeError('Complete actual PDR frame/save words differ')
    detail={};bodies=[]
    for section in delta['complete_changed_allocated_sections']:
        detail[section]={}
        for tag,obj in objects.items():
            detail[section][tag]={'signature':obj.signature(section),'complete_input_bytes_hex':obj.content(obj.sections[section]).hex(),
                'complete_ordered_RELA_records':obj.relocations.get(section,[]),'exact_frame':procedures[tag]['records'].get(section)}
        if objects['baseline'].signature(section)!=objects['candidate'].signature(section):bodies.append(section)
    disassembler=Path(json.loads((baseline/'build-report.json').read_text())['link_command'][0]).with_name('mips64r5900el-ps2-elf-objdump')
    instruction_counts={};elf_costs={}
    for tag,path in [('baseline',baseline),('candidate',candidate)]:
        obj=objects[tag];instruction_counts[tag]={}
        for section in bodies:
            raw=obj.content(obj.sections[section]);words=struct.unpack('<'+'I'*(len(raw)//4),raw)
            selfops=[];minmax=0
            for at,word in enumerate(words):
                if word>>26==17 and ((word>>21)&31)==16 and (word&63) in (40,41):
                    minmax+=1
                    fs,ft,fd=(word>>11)&31,(word>>16)&31,(word>>6)&31
                    if fs==ft:selfops.append({'input_offset':at*4,'word_hex':f'{word:08x}','operation':'max.s' if word&63==40 else 'min.s','fs':fs,'ft':ft,'fd':fd})
            name=section[6:];disasm=out/(tag+'-'+name+'.disasm')
            result=subprocess.run([str(disassembler),'-d','--disassemble='+name,str(path/'SRB2.ELF')],capture_output=True,text=True,check=True)
            disasm.write_text(result.stdout)
            instruction_counts[tag][section]={'complete_bytes':len(raw),'complete_input_instruction_slots':len(words),
                'minmax_slots':minmax,'same_operand_minmax_slots':selfops,'actual_ELF_disassembly':str(disasm),'disassembly_sha256':sha(disasm)}
        elf=Elf32(path/'SRB2.ELF')
        elf_costs[tag]={'allocated_sections':{name:{'type':s[1],'flags':s[2],'address':s[3],'bytes':s[5],'alignment':s[8]}
            for name,s in zip(elf.names,elf.sections) if s[2]&2},'symbols':{s['name']:{'address':s['value'],'bytes':s['size']}
            for table in symbol_tables(elf).values() for s in table if s['name'] in ('_end','_gp')}}
        (out/(tag+'-complete-allocated.json')).write_text(json.dumps(signatures[tag],indent=2)+'\n')
        (out/(tag+'-complete-pdr.json')).write_text(json.dumps(procedures[tag],indent=2)+'\n')
    artifacts={tag:{name:{'path':str(path/name),'sha256':sha(path/name),'bytes':(path/name).stat().st_size}
        for name in ('obj/engine_lto.o','SRB2.ELF','SRB2.map','build-report.json')}
        for tag,path in [('baseline',baseline),('candidate',candidate)]}
    report={'status':'PASS complete actual default-LTO object/frame/cost inventory; final resolved binding and native cost pending',
        'variant':version,'artifacts':artifacts,'complete_build_admissions':admissions,
        'changed_allocated_sections':delta['complete_changed_allocated_sections'],
        'changed_code_sections_or_bindings':delta['changed_code_sections_or_bindings'],
        'direct_actual_callers':delta['direct_actual_callers'],'actual_changed_instruction_bodies':bodies,
        'complete_changed_section_details':detail,'complete_all_procedure_words_equal':True,
        'actual_object_procedure_records':len(frames_equal),'actual_instruction_inventory':instruction_counts,
        'actual_ELF_costs':elf_costs,'disassembler':str(disassembler),'disassembler_sha256':sha(disassembler),
        'checker_sha256':sha(Path(__file__)),'no_EE_compiler_emulator_or_production_edit':True}
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    return report


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--baseline',type=Path,default=variant.ROOT/'build/nov15-next12/final/hardware')
    p.add_argument('--candidate',type=Path,default=variant.WORK/'geometry-context-v2')
    p.add_argument('--version',choices=('v1','v2'),default='v2')
    p.add_argument('--out',type=Path,default=variant.WORK/'geometry-inventory-v2')
    a=p.parse_args();r=inspect(a.baseline,a.candidate,a.out,a.version)
    print(json.dumps({'status':r['status'],'report':str(a.out/'report.json'),
        'changed_code_bodies':r['actual_changed_instruction_bodies'],
        'frame_records_equal':r['actual_object_procedure_records'],
        'same_operand_minmax_sites':{tag:sum(len(row['same_operand_minmax_slots']) for row in rows.values())
                                   for tag,rows in r['actual_instruction_inventory'].items()}},indent=2))


if __name__=='__main__':main()
