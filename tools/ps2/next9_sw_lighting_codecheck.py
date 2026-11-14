"""Bind complete measured slope-light helpers to default LTO and final ELF.

Only relocation fields and symbol spelling normalize. Literal relocations are
compared by their complete float bytes. Final-link fields are fully reconstructed
from actual symbols, _gp and the map's checked engine .sdata contribution.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
from next7_render_fog_codecheck import Elf32
from next8_sw_codecheck import linked_symbols


def helper_name(elf):
    names=[name[6:] for name in elf.names if re.fullmatch(r'\.text\.R_CalcSlopeLight(?:\.lto_priv\.\d+)?',name)]
    if len(names)!=1: raise SystemExit('Expected one complete actual R_CalcSlopeLight section')
    return names[0]


def records(elf,name):
    index=elf.index['.text.'+name]; result=[]
    for section in elf.sections:
        if section[7]!=index or section[1] not in (4,9): continue
        if section[1]!=4: raise SystemExit('Explicit RELA records required')
        for at in range(section[4],section[4]+section[5],section[9]):
            location,info,addend=struct.unpack_from('<IIi',elf.data,at)
            result.append((location,info&255,elf.symbols[section[6]][info>>8],addend))
    if not result or len({r[0] for r in result})!=len(result):
        raise SystemExit('Missing or duplicate-offset ordered RELAs')
    return result


def binding(engine,executable,map_path,name=None):
    name=name or helper_name(engine); syms=linked_symbols(executable)
    symbol=syms[name]; section=executable.sections[symbol['section']]
    code=engine.bytes(engine.sections[engine.index['.text.'+name]])
    offset=symbol['value']-section[3]
    if offset<0 or offset+len(code)>section[5]: raise SystemExit('Complete linked body/padding outside section')
    actual=executable.bytes(section)[offset:offset+len(code)]
    expected=list(struct.unpack('<'+'I'*(len(code)//4),code))
    pool=engine.bytes(engine.sections[engine.index['.sdata']])
    gp=struct.unpack_from('<I',engine.bytes(engine.sections[engine.index['.reginfo']]),20)[0]
    contributors=re.findall(r'^\s+\.sdata\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+\S*engine_lto\.o\s*$',map_path.read_text(),re.M)
    if len(contributors)!=1: raise SystemExit('Expected one engine .sdata map contribution')
    pool_address,pool_size=(int(v,16) for v in contributors[0])
    if pool_size!=len(pool): raise SystemExit('Map/input literal pool size differs')
    resolved_pool=bytearray(pool);data_relocations=[]
    for data_section in engine.sections:
        if data_section[7]!=engine.index['.sdata'] or data_section[1] not in (4,9):continue
        if data_section[1]!=4:raise SystemExit('Explicit mapped-pool RELAs required')
        for at in range(data_section[4],data_section[4]+data_section[5],data_section[9]):
            location,info,addend=struct.unpack_from('<IIi',engine.data,at)
            kind,target=info&255,engine.symbols[data_section[6]][info>>8]
            if kind!=2 or target not in syms or location+4>len(pool) or struct.unpack_from('<I',pool,location)[0]!=0:
                raise SystemExit('Unexpected mapped-pool R_MIPS_32 RELA')
            address=(syms[target]['value']+addend)&0xffffffff
            struct.pack_into('<I',resolved_pool,location,address)
            data_relocations.append({'offset':location,'kind':kind,'exact_symbol':target,'explicit_addend':addend,'resolved_address':address})
    output_pool=executable.sections[executable.index['.sdata']]
    pool_offset=pool_address-output_pool[3]
    if pool_offset<0 or pool_offset+len(pool)>output_pool[5] or executable.bytes(output_pool)[pool_offset:pool_offset+len(pool)]!=resolved_pool:
        raise SystemExit('Complete mapped engine literal-pool bytes differ')
    checked=[]
    for location,kind,target,addend in records(engine,name):
        mask=0x03ffffff if kind==4 else 0xffff
        if expected[location//4]&mask: raise SystemExit('Expected zero encoded RELA field')
        if target=='.sdata':
            literal_offset=addend+gp
            if kind!=7 or literal_offset<0 or literal_offset+4>len(pool):
                raise SystemExit('Unexpected helper literal relocation')
            address=pool_address+literal_offset
        else:
            if target not in syms: raise SystemExit('Exact linked target symbol missing: '+target)
            address=(syms[target]['value']+addend)&0xffffffff
        if kind==4: value=(address>>2)&0x03ffffff
        elif kind==5: value=((address+0x8000)>>16)&0xffff
        elif kind==6: value=address&0xffff
        elif kind==7:
            displacement=address-syms['_gp']['value']
            if not -32768<=displacement<=32767: raise SystemExit('Resolved GP displacement outside signed16')
            value=displacement&0xffff
        else: raise SystemExit('Unexpected MIPS text relocation')
        expected[location//4]|=value
        checked.append({'offset':location,'kind':kind,'exact_symbol':target,'explicit_addend':addend,'resolved_address':address})
    expected=struct.pack('<'+'I'*len(expected),*expected)
    if expected!=actual: raise SystemExit('Fully resolved linked body bytes differ')
    return {'all_linked_body_and_padding_bytes_equal':True,'complete_text_bytes':len(code),
            'symbol_body_bytes':symbol['size'],'ordered_relocations':checked,
            'complete_mapped_sdata_bytes':len(pool),'sdata_sha256':hashlib.sha256(pool).hexdigest(),
            'sdata_R_MIPS_32_relocations':data_relocations,'resolved_sdata_sha256':hashlib.sha256(resolved_pool).hexdigest(),
            'elf':str(executable.path),'elf_sha256':hashlib.sha256(executable.data).hexdigest(),
            'map':str(map_path),'map_sha256':hashlib.sha256(map_path.read_bytes()).hexdigest()}


def check(fixture,variant,engine,executable,map_path):
    name=helper_name(engine)
    expected=fixture.instructions(variant+'_R_CalcSlopeLight',False,True)
    actual=engine.instructions(name,True,True)
    expected_order=[offset for offset,_,_,_ in records(fixture,variant+'_R_CalcSlopeLight')]
    actual_order=[offset for offset,_,_,_ in records(engine,name)]
    if expected!=actual or expected_order!=actual_order:
        raise SystemExit('Complete helper instruction/register/branch/RELA/literal/padding mismatch: '+variant)
    return {'complete_instructions_and_ordered_RELA_stream_equal':True,'text_bytes':actual['text_bytes'],
            'ordered_RELA_offsets':actual_order,'literal_bytes':sorted({v[1] for v in actual['relocations'].values() if v[1].startswith('float32_bytes:')}),
            'engine':str(engine.path),'engine_sha256':hashlib.sha256(engine.data).hexdigest(),
            'linked_binding':binding(engine,executable,map_path)}


def callers(engine):
    result=set();helper=helper_name(engine)
    for section in engine.sections:
        if section[1]!=4 or not engine.names[section[7]].startswith('.text.'):continue
        for at in range(section[4],section[4]+section[5],section[9]):
            _,info,_=struct.unpack_from('<IIi',engine.data,at)
            target=engine.symbols[section[6]][info>>8]
            if target in (helper,'.text.'+helper):result.add(engine.names[section[7]][6:])
    return result


def caller_audit(baseline,baseline_elf,baseline_map,candidate,candidate_elf,candidate_map):
    before=callers(baseline);after=callers(candidate)
    if before!=after or len(before)!=14:raise SystemExit('Unexpected actual slope-helper caller graph')
    result={}
    for name in sorted(before):
        left=baseline.instructions(name,True,True);right=candidate.instructions(name,True,True)
        left_order=[r[0] for r in records(baseline,name)];right_order=[r[0] for r in records(candidate,name)]
        if left!=right or left_order!=right_order:
            raise SystemExit('Complete actual caller instructions/ordered RELAs/literal bytes differ: '+name)
        result[name]={'complete_instruction_register_branch_RELA_literal_padding_identity':True,
                      'text_bytes':left['text_bytes'],'ordered_RELA_count':len(left_order),
                      'baseline_linked_bytes':binding(baseline,baseline_elf,baseline_map,name),
                      'candidate_linked_bytes':binding(candidate,candidate_elf,candidate_map,name)}
    return result


def negatives(out,fixture,variant,engine,executable,map_path):
    name=helper_name(engine);index=engine.index['.text.'+name]
    section=engine.sections[index]
    relocation=next(s for s in engine.sections if s[1]==4 and s[7]==index)
    record=relocation[4]
    pool=engine.sections[engine.index['.sdata']]
    gp=struct.unpack_from('<I',engine.bytes(engine.sections[engine.index['.reginfo']]),20)[0]
    literal=next(r[3]+gp for r in records(engine,name) if r[2]=='.sdata')
    controls=[]
    for title in ('instruction_register','RELA_target','RELA_addend','RELA_type','RELA_location','object_literal_byte'):
        data=bytearray(engine.data)
        if title=='instruction_register':
            word=struct.unpack_from('<I',data,section[4])[0]
            struct.pack_into('<I',data,section[4],word^0x10000)
        elif title=='RELA_target':
            names=engine.symbols[relocation[6]]
            other=next(i for i,n in enumerate(names) if n=='ds_x1')
            info=struct.unpack_from('<I',data,record+4)[0]
            struct.pack_into('<I',data,record+4,(other<<8)|(info&255))
        elif title=='RELA_addend':struct.pack_into('<i',data,record+8,4)
        elif title=='RELA_type':
            info=struct.unpack_from('<I',data,record+4)[0]
            struct.pack_into('<I',data,record+4,(info&~255)|6)
        elif title=='RELA_location':struct.pack_into('<I',data,record,16)
        else:data[pool[4]+literal]^=1
        path=out/('negative-'+title+'.o');path.write_bytes(data)
        try:check(fixture,variant,Elf32(path),executable,map_path)
        except SystemExit as error:
            if not str(error).startswith('Complete helper instruction/register/branch/RELA/literal/padding mismatch'):
                raise
            controls.append({'mutation':title,'rejected':True,'reason':str(error),'file':str(path),'sha256':hashlib.sha256(data).hexdigest()})
        else:raise SystemExit('Missed actual object negative: '+title)
    data_relocations=[s for s in engine.sections if s[1]==4 and s[7]==engine.index['.sdata']]
    if data_relocations:
        data_section=data_relocations[0];data_record=data_section[4]
        for title in ('data_RELA_target','data_RELA_addend'):
            data=bytearray(engine.data)
            if title=='data_RELA_target':
                names=engine.symbols[data_section[6]]
                other=next(i for i,n in enumerate(names) if n=='ds_slopelight')
                info=struct.unpack_from('<I',data,data_record+4)[0]
                struct.pack_into('<I',data,data_record+4,(other<<8)|(info&255))
            else:
                addend=struct.unpack_from('<i',data,data_record+8)[0]
                struct.pack_into('<i',data,data_record+8,addend+4)
            path=out/('negative-'+title+'.o');path.write_bytes(data)
            try:binding(Elf32(path),executable,map_path)
            except SystemExit as error:
                if str(error)!='Complete mapped engine literal-pool bytes differ':raise
                controls.append({'mutation':title,'rejected':True,'reason':str(error),'file':str(path),'sha256':hashlib.sha256(data).hexdigest()})
            else:raise SystemExit('Missed actual mapped-pool data-RELA negative: '+title)
    syms=linked_symbols(executable);symbol=syms[name];text=executable.sections[symbol['section']]
    start=text[4]+symbol['value']-text[3]
    contributors=re.findall(r'^\s+\.sdata\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+\S*engine_lto\.o\s*$',map_path.read_text(),re.M)
    pool_address=int(contributors[0][0],16)
    output_pool=executable.sections[executable.index['.sdata']]
    for title in ('linked_register','linked_resolved_pointer','linked_mapped_literal_byte'):
        data=bytearray(executable.data)
        if title=='linked_mapped_literal_byte':data[output_pool[4]+pool_address-output_pool[3]+literal]^=1
        else:
            word=struct.unpack_from('<I',data,start)[0]
            struct.pack_into('<I',data,start,word^(0x10000 if title=='linked_register' else 1))
        path=out/('negative-'+title+'.ELF');path.write_bytes(data)
        try:binding(engine,Elf32(path),map_path)
        except SystemExit as error:
            if str(error) not in ('Fully resolved linked body bytes differ','Complete mapped engine literal-pool bytes differ'):raise
            controls.append({'mutation':title,'rejected':True,'reason':str(error),'file':str(path),'sha256':hashlib.sha256(data).hexdigest()})
        else:raise SystemExit('Missed actual linked negative: '+title)
    return controls


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',required=True,type=Path)
    p.add_argument('--fixture-object',required=True,type=Path)
    p.add_argument('--baseline-engine',required=True,type=Path)
    p.add_argument('--baseline-elf',required=True,type=Path)
    p.add_argument('--baseline-map',required=True,type=Path)
    p.add_argument('--candidate-engine',type=Path)
    p.add_argument('--candidate-elf',type=Path)
    p.add_argument('--candidate-map',type=Path)
    p.add_argument('--candidate-variant',default='countupper')
    p.add_argument('--negative-controls',action='store_true')
    a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
    fixture=Elf32(a.fixture_object.resolve())
    inputs=json.loads((fixture.path.parent/'source-hashes.json').read_text())
    for name,digest in inputs['variants'].items():
        if hashlib.sha256((fixture.path.parent/(name+'.inc')).read_bytes()).hexdigest()!=digest:
            raise SystemExit('Frozen helper input changed: '+name)
    for name in ('functions.c','driver.c','next9_sw_lighting.h'):
        if hashlib.sha256((fixture.path.parent/name).read_bytes()).hexdigest()!=inputs[name]:
            raise SystemExit('Frozen fixture input changed: '+name)
    result={'fixture_object':str(fixture.path),'fixture_sha256':hashlib.sha256(fixture.data).hexdigest(),
            'source_inputs':inputs,'reference':check(fixture,'reference',Elf32(a.baseline_engine.resolve()),Elf32(a.baseline_elf.resolve()),a.baseline_map.resolve())}
    if a.candidate_engine:
        if not a.candidate_elf or not a.candidate_map: raise SystemExit('Candidate engine requires exact ELF and map')
        result['candidate']=check(fixture,a.candidate_variant,Elf32(a.candidate_engine.resolve()),Elf32(a.candidate_elf.resolve()),a.candidate_map.resolve())
        result['fourteen_complete_actual_callers']=caller_audit(Elf32(a.baseline_engine.resolve()),Elf32(a.baseline_elf.resolve()),a.baseline_map.resolve(),
                                                               Elf32(a.candidate_engine.resolve()),Elf32(a.candidate_elf.resolve()),a.candidate_map.resolve())
    if a.negative_controls:
        if not a.candidate_engine:raise SystemExit('Negatives require actual candidate engine/ELF/map')
        result['binary_gate_negative_controls']=negatives(out,fixture,a.candidate_variant,Elf32(a.candidate_engine.resolve()),Elf32(a.candidate_elf.resolve()),a.candidate_map.resolve())
    (out/'code-report.json').write_text(json.dumps(result,indent=2)+'\n')
    print('Complete helper instructions, ordered RELAs, literals and resolved linked bytes match')


if __name__=='__main__': main()
