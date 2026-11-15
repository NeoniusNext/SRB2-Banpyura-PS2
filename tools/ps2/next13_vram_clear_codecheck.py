#!/usr/bin/env python3
"""Read-only complete clear/caller/lifecycle binding to actual whole game ELFs.

No instruction/RELA field is masked. Uses the official strict binder and exact
retained linker RELAs, and reports literal MIPS frames/static capacity changes.
This does not establish runtime cache-history equality or physical GS speed.
"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess

from audio_storage_next7_finalbind import Object
from next12_pb_scratch_codecheck import binder
from next12_pb_scratch_objectcheck import allocated,pdr
from next10_sw_bounds_codecheck import symbol_tables
from next7_render_fog_codecheck import Elf32

ROOT=Path(__file__).resolve().parents[2]
WORK=ROOT/'build/nov15-next13/vram-clear/stages'


def sha(path):
    return __import__('hashlib').sha256(Path(path).read_bytes()).hexdigest()


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--baseline',type=Path,default=ROOT/'build/nov15-next12/final/hardware')
    ap.add_argument('--candidate',type=Path,default=WORK/'context-v1')
    ap.add_argument('--out',type=Path,required=True)
    a=ap.parse_args();base,candidate,out=(p.resolve() for p in (a.baseline,a.candidate,a.out))
    if out.exists() or not out.is_relative_to(WORK):raise SystemExit('Fresh isolated audit stage required')
    context_path=candidate/'context-report.json';context=json.loads(context_path.read_text())
    if context['changed_actual_IR_objects']!=['src__ps2__hw__ps2_hwd.o'] or context['preserved_default_unit_flags']!=['-O3']:
        raise SystemExit('Actual one-unit O3-hwd graph required')
    for name,digest in context['complete_actual_IR_objects'].items():
        if sha(name)!=digest:raise SystemExit('Actual candidate IR changed')
    out.mkdir(parents=True)
    for tool in ('next13_vram_clear_codecheck.py','next12_pb_scratch_codecheck.py','next11_sw_checkplane_codecheck.py',
                 'next12_pb_scratch_objectcheck.py','next11_sw_linker_relas.py','audio_storage_next7_finalbind.py'):
        shutil.copy2(ROOT/'tools/ps2'/tool,out/tool)
    paths={'baseline':base,'candidate':candidate}
    objects={tag:Object(path/'obj/engine_lto.o') for tag,path in paths.items()}
    allocations={tag:allocated(obj) for tag,obj in objects.items()}
    changed=sorted(name for name in allocations['baseline'].keys()|allocations['candidate'].keys()
                   if allocations['baseline'].get(name)!=allocations['candidate'].get(name))
    changed_code=[name for name in changed if any(name in obj.sections and obj.sections[name][2]&4 for obj in objects.values())]
    mandatory=set(changed_code)|{'.text.gs_fill_all.lto_priv.0','.text.pk_flush.lto_priv.0',
        '.text.frame_prepare.lto_priv.0','.text.rel_release_buf.lto_priv.0','.text.dc_slot','.text.hw_ClearBuffer'}
    result={'context_report_sha256':sha(context_path),'actual_default_hwd_O3_preserved':True,
            'all_changed_allocated_sections':changed,'changed_code_sections_or_binding_metadata':changed_code,
            'variants':{},'scope':'Exact complete whole-game linked code/RELA/data/literal/frame/static audit. No runtime source-cache equivalence or GS performance claim.'}
    for tag,path in paths.items():
        value=binder(path,candidate/(tag+'-linker-relas'));obj=objects[tag]
        names=sorted(name[6:] for name in mandatory if name in obj.sections)
        bodies={name:value.text(name) for name in names}
        elf=Elf32(path/'SRB2.ELF');symbols=[s for table in symbol_tables(elf).values() for s in table]
        static={s['name']:{'address':s['value'],'bytes':s['size']} for s in symbols
                if s['name'] in ('_end','_gp') or s['name'].startswith('H.lto_priv.')}
        section_capacity={name:{'type':s[1],'flags':s[2],'address':s[3],'bytes':s[5],'alignment':s[8]}
                          for name,s in zip(elf.names,elf.sections) if s[2]&2}
        result['variants'][tag]={'ELF_sha256':sha(path/'SRB2.ELF'),'engine_sha256':sha(path/'obj/engine_lto.o'),
            'complete_bound_functions':bodies,'all_referenced_data':value.data,'all_referenced_literals':value.literals,
            'complete_literal_uses':value.literal_uses,'final_static_symbols':static,'all_final_allocated_section_capacities':section_capacity,
            'literal_clear_PDR':pdr(obj)['records']['.text.gs_fill_all.lto_priv.0']}
        # Binutils only: no target compilation or execution.
        compiler=json.loads((path/'build-report.json').read_text())['link_command'][0]
        dump=Path(compiler).with_name(Path(compiler).name.replace('-gcc','-objdump'))
        command=[str(dump),'-dr','-j','.text.gs_fill_all.lto_priv.0',str(path/'obj/engine_lto.o')]
        output=subprocess.check_output(command,text=True)
        (out/(tag+'-clear-disassembly.txt')).write_text(output)
    x,y=objects['baseline'],objects['candidate']
    raw_changed=[name for name in changed_code if name not in x.sections or name not in y.sections
                 or x.content(x.sections[name])!=y.content(y.sections[name])]
    if raw_changed!=['.text.gs_fill_all.lto_priv.0']:raise SystemExit('Another complete allocated code body changed')
    result['only_raw_code_body_change']=raw_changed
    result['clear_text_bytes']={tag:obj.sections['.text.gs_fill_all.lto_priv.0'][5] for tag,obj in objects.items()}
    result['clear_frame_bytes']={tag:result['variants'][tag]['literal_clear_PDR']['frame_bytes'] for tag in paths}
    result['final_capacity_deltas']={name:result['variants']['candidate']['all_final_allocated_section_capacities'][name]['bytes']-row['bytes']
        for name,row in result['variants']['baseline']['all_final_allocated_section_capacities'].items()}
    result['static_end_address_delta']=result['variants']['candidate']['final_static_symbols']['_end']['address']-result['variants']['baseline']['final_static_symbols']['_end']['address']
    # Tuple-key literal dictionaries are represented by the fully recorded use
    # records plus their explicit canonical input section/offset keys.
    for variant in result['variants'].values():
        variant['all_referenced_literals']=[{'key':list(key),**literal} for key,literal in variant['all_referenced_literals'].items()]
    result['status']='PASS exact actual O3-hwd clear/callers/lifecycle linked-body binding; runtime candidate not accepted'
    (out/'report.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({k:v for k,v in result.items() if k in ('status','clear_text_bytes','clear_frame_bytes','final_capacity_deltas','static_end_address_delta','only_raw_code_body_change')},indent=2))


if __name__=='__main__':main()
