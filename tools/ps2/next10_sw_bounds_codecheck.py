"""Bind complete timed bounds code, actual callers, data and linked fields.

Engine-defined addresses come from the exact input-section map contribution,
so repeated local symbol spellings in SDK objects cannot redirect a binding.
Merged string addresses must have a unique complete literal in final rodata.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
from next7_render_fog_codecheck import Elf32
from next8_sw_codecheck import linked_symbols
from next10_sw_bounds_report import report as native_report

NAME = 'R_PlaneBounds'


def symbol_tables(elf):
    if hasattr(elf,'next10_symbol_tables'):return elf.next10_symbol_tables
    tables={}
    for i,section in enumerate(elf.sections):
        if section[1]!=2:continue
        strings=elf.bytes(elf.sections[section[6]]);symbols=[]
        for at in range(section[4],section[4]+section[5],section[9]):
            raw=struct.unpack_from('<IIIBBH',elf.data,at)
            name=strings[raw[0]:].split(b'\0',1)[0].decode()
            if not name and raw[3]&15==3:name=elf.names[raw[5]]
            symbols.append({'name':name,'value':raw[1],'size':raw[2],'type':raw[3]&15,
                            'binding':raw[3]>>4,'section':raw[5]})
        tables[i]=symbols
    elf.next10_symbol_tables=tables
    return tables


def relocations(elf,name):
    index=elf.index['.text.'+name];tables=symbol_tables(elf);result=[]
    for section in elf.sections:
        if section[7]!=index or section[1] not in (4,9):continue
        if section[1]!=4:raise SystemExit('Explicit RELAs required')
        for at in range(section[4],section[4]+section[5],section[9]):
            location,info,addend=struct.unpack_from('<IIi',elf.data,at)
            result.append({'offset':location,'kind':info&255,'symbol':tables[section[6]][info>>8],
                           'addend':addend,'record_file_offset':at,'symbol_table':section[6]})
    if len({r['offset'] for r in result})!=len(result):raise SystemExit('Duplicate RELA offsets')
    return result


def code(elf,name):
    data=elf.bytes(elf.sections[elf.index['.text.'+name]])
    if len(data)%4:raise SystemExit('Unaligned complete text')
    words=list(struct.unpack('<'+'I'*(len(data)//4),data));records=[]
    for r in relocations(elf,name):
        mask=0x03ffffff if r['kind']==4 else 0xffff
        if r['kind'] not in (4,5,6,7) or words[r['offset']//4]&mask:
            raise SystemExit('Unexpected encoded RELA field')
        target=r['symbol'];index=target['section']
        records.append((r['offset'],r['kind'],target['name'],r['addend'],target['type'],
                        elf.names[index] if index<len(elf.names) else index,target['value']))
    return {'words':words,'ordered_relocations':records,'text_bytes':len(data)}


def contributions(map_path):
    pattern=r'^\s+(\.[^\s]+)\s+(?:\n\s+)?0x([a-f0-9]+)\s+0x([a-f0-9]+)\s+\S*engine_lto\.o\s*$'
    result={}
    for name,address,size in re.findall(pattern,map_path.read_text(),re.M):
        if name in result:raise SystemExit('Duplicate engine input contribution '+name)
        result[name]=(int(address,16),int(size,16))
    return result


def output_bytes(elf,address,size):
    matches=[s for s in elf.sections if s[1]!=8 and s[2]&2 and s[3]<=address and address+size<=s[3]+s[5]]
    if len(matches)!=1:raise SystemExit('Expected one allocated final section at address')
    s=matches[0];return elf.bytes(s)[address-s[3]:address-s[3]+size]


def frame(elf,name):
    index=elf.index['.pdr'];data=elf.bytes(elf.sections[index]);tables=symbol_tables(elf);matches=[]
    for section in elf.sections:
        if section[1]!=4 or section[7]!=index:continue
        for at in range(section[4],section[4]+section[5],section[9]):
            offset,info,addend=struct.unpack_from('<IIi',elf.data,at)
            target=tables[section[6]][info>>8]['name']
            if target in (name,'.text.'+name):
                if info&255!=2 or addend!=0 or offset%32:raise SystemExit('Unexpected exact PDR record')
                raw=data[offset:offset+32]
                matches.append({'stack_frame_bytes':struct.unpack_from('<I',raw,20)[0],
                                'register_mask':struct.unpack_from('<I',raw,4)[0],
                                'frame_record_hex':raw.hex()})
    if len(matches)!=1:raise SystemExit('Missing/ambiguous exact frame record '+name)
    return matches[0]


def bind(engine,executable,map_path,name,verify_input_hash=True):
    maps=contributions(map_path);symbols=linked_symbols(executable)
    build_report_path=map_path.parent/'build-report.json'
    build_report=json.loads(build_report_path.read_text())
    if 'bounds_context_control' in build_report:
        control=build_report['bounds_context_control'];archive=control['generated_input_archive']
        archive_path=Path(archive['path'])
        if hashlib.sha256(archive_path.read_bytes()).hexdigest()!=archive['sha256'] or json.loads(archive_path.read_text())!=archive['details']:
            raise SystemExit('Context generated-input archive metadata changed')
        for entry in archive['details']['entries']:
            digest=entry['original_pinned_sha256']
            if control['external_generated_inputs'][entry['original_path']]!=digest or entry['archive_sha256']!=digest or hashlib.sha256(Path(entry['archive_path']).read_bytes()).hexdigest()!=digest:
                raise SystemExit('Context archived generated bytes differ from original pin')
    if verify_input_hash and build_report['elf_sha256']!=hashlib.sha256(executable.data).hexdigest():
        raise SystemExit('Build report does not pin exact final ELF')
    command=build_report['link_command']
    if not isinstance(command,list):raise SystemExit('Structured final link command required')
    wrappers={arg[len('-Wl,--wrap='):] for arg in command if arg.startswith('-Wl,--wrap=')}
    gp=struct.unpack_from('<I',engine.bytes(engine.sections[engine.index['.reginfo']]),20)[0]
    data=bytearray(engine.bytes(engine.sections[engine.index['.text.'+name]]))
    section_name='.text.'+name
    if section_name not in maps or maps[section_name][1]!=len(data):
        raise SystemExit('Complete function map contribution differs')
    base=maps[section_name][0]
    if name in symbols and symbols[name]['value']!=base:raise SystemExit('Named function/map disagreement')
    checked=[];references={}
    for r in relocations(engine,name):
        symbol=r['symbol'];index=symbol['section'];addend=r['addend'];kind=r['kind']
        resolved_symbol=symbol['name']
        if index==0:
            # Undefined SDK/global names have one global or weak definition.
            if resolved_symbol in wrappers:resolved_symbol='__wrap_'+resolved_symbol
            elif resolved_symbol.startswith('__real_') and resolved_symbol[7:] in wrappers:
                resolved_symbol=resolved_symbol[7:]
            matches=[s for table in symbol_tables(executable).values() for s in table
                     if s['name']==resolved_symbol and s['binding'] in (1,2) and s['section']!=0]
            addresses={s['value'] for s in matches}
            if len(addresses)!=1:raise SystemExit('Missing/ambiguous external exact target '+symbol['name'])
            address=addresses.pop()+addend
        else:
            section=engine.sections[index];target=engine.names[index]
            position=symbol['value']+addend+(gp if kind==7 and symbol['type']==3 else 0)
            if section[2]&0x30==0x30:
                if not 0<=position<section[5]:raise SystemExit('Merged string offset outside input')
                literal=engine.bytes(section)[position:].split(b'\0',1)[0]+b'\0'
                locations=[]
                for final_index,final_section in enumerate(executable.sections):
                    if not executable.names[final_index].startswith('.rodata') or not final_section[2]&2:continue
                    raw=executable.bytes(final_section);at=raw.find(literal)
                    while at>=0:
                        address_candidate=final_section[3]+at
                        if (address_candidate-position)%section[8]==0:
                            locations.append(address_candidate)
                        at=raw.find(literal,at+1)
                if len(locations)!=1:raise SystemExit('Merged literal lacks unique exact final address')
                address=locations[0]
                references[(target,position)]={'input_section':target,'offset':position,
                    'literal_hex':literal.hex(),'linked_address':address,'input_alignment':section[8],
                    'unique_complete_literal_at_required_alignment':True}
            else:
                if target not in maps:raise SystemExit('Missing input contribution '+target)
                mapped,size=maps[target]
                if size!=section[5]:raise SystemExit('Mapped input section size differs '+target)
                address=mapped+position
                if section[1]!=8 and not target.startswith('.text'):
                    # Plain input data are bound literally; data with their own
                    # RELAs require a separate resolver rather than zero masks.
                    if any(s[7]==index and s[1] in (4,9) for s in engine.sections):
                        raise SystemExit('Referenced data RELAs need explicit resolution '+target)
                    raw=engine.bytes(section)
                    if output_bytes(executable,mapped,len(raw))!=raw:
                        raise SystemExit('Referenced input data bytes differ '+target)
                    references[(target,0)]={'input_section':target,'complete_bytes':len(raw),
                        'sha256':hashlib.sha256(raw).hexdigest(),'linked_address':mapped,'literal_identity':True}
                elif section[1]==8:
                    references[(target,0)]={'input_section':target,'zero_initialized_bytes':section[5],
                        'linked_address':mapped,'unchanged_capacity':True}
        address&=0xffffffff
        if kind==4:value=(address>>2)&0x03ffffff
        elif kind==5:value=((address+0x8000)>>16)&0xffff
        elif kind==6:value=address&0xffff
        elif kind==7:
            displacement=address-symbols['_gp']['value']
            if not -32768<=displacement<=32767:raise SystemExit('GP field outside signed16')
            value=displacement&0xffff
        else:raise SystemExit('Unexpected code RELA type')
        word=struct.unpack_from('<I',data,r['offset'])[0]
        if word&(0x03ffffff if kind==4 else 0xffff):raise SystemExit('Nonzero encoded field')
        struct.pack_into('<I',data,r['offset'],word|value)
        checked.append({'offset':r['offset'],'kind':kind,'exact_symbol':symbol['name'],
                        'explicit_addend':addend,'resolved_symbol':resolved_symbol,'resolved_address':address})
    actual=output_bytes(executable,base,len(data))
    if bytes(data)!=actual:
        differences=[{'offset':at,'expected':hex(struct.unpack_from('<I',data,at)[0]),
                      'actual':hex(struct.unpack_from('<I',actual,at)[0])}
                     for at in range(0,len(data),4) if data[at:at+4]!=actual[at:at+4]]
        raise SystemExit('Fully resolved complete linked body/padding differs '+name+': '+str(differences))
    return {'complete_text_bytes':len(data),'all_fully_resolved_linked_bytes_equal':True,
            'exact_frame_record':frame(engine,name),
            'ordered_RELA_records':checked,'referenced_data':list(references.values()),
            'engine_sha256':hashlib.sha256(engine.data).hexdigest(),
            'ELF_sha256':hashlib.sha256(executable.data).hexdigest(),
            'map_sha256':hashlib.sha256(map_path.read_bytes()).hexdigest(),
            'exact_link_command_wrappers':sorted(wrappers),
            'build_report_sha256':hashlib.sha256(build_report_path.read_bytes()).hexdigest()}


def callers(engine):
    result={};tables=symbol_tables(engine)
    for section in engine.sections:
        name=engine.names[section[7]]
        if section[1]!=4 or not name.startswith('.text.'):continue
        for at in range(section[4],section[4]+section[5],section[9]):
            location,info,_=struct.unpack_from('<IIi',engine.data,at)
            target=tables[section[6]][info>>8]['name']
            if target in (NAME,'.text.'+NAME):result.setdefault(name[6:],[]).append(location)
    return result


def check(fixture,variant,engine,executable,map_path):
    if code(fixture,variant+'_'+NAME)!=code(engine,NAME):
        raise SystemExit('Complete fixture/default-LTO instructions/ordered RELAs differ: '+variant)
    if frame(fixture,variant+'_'+NAME)!=frame(engine,NAME):
        raise SystemExit('Complete fixture/default-LTO frame record differs: '+variant)
    return bind(engine,executable,map_path,NAME)


def timed_bodies(fixture,executable):
    symbols=linked_symbols(executable);result={}
    for variant in ('reference','production'):
        name=variant+'_'+NAME
        if relocations(fixture,name):raise SystemExit('Expected zero RELAs for direct timed body binding')
        raw=fixture.bytes(fixture.sections[fixture.index['.text.'+name]])
        symbol=symbols[name]
        if symbol['size']!=len(raw) or output_bytes(executable,symbol['value'],len(raw))!=raw:
            raise SystemExit('Complete functions object/executed ELF body or symbol size differs '+name)
        result[name]={'complete_body_and_padding_bytes':len(raw),'exact_symbol_size':symbol['size'],
                      'executed_address':symbol['value'],'all_bytes_equal':True,'sha256':hashlib.sha256(raw).hexdigest()}
    return result


def execution(fixture,variant):
    folder=fixture.path.parent;path=folder/'run-manifest.json'
    if not path.is_file():raise SystemExit('Production binding requires an explicit staged execution manifest')
    run=json.loads(path.read_text())
    before=run['inputs_before'];after=run['inputs_after']
    if before!=after or not run['all_original_and_staged_inputs_unchanged'] or not run['validation_passed'] or run['runner_returncode']!=0:
        raise SystemExit('Staged execution input/outcome gate failed')
    for name,digest in before.items():
        if hashlib.sha256((folder/name).read_bytes()).hexdigest()!=digest:
            raise SystemExit('Executed frozen input bytes changed '+name)
    if run['executed_ELF']!=str(folder/'BOUNDS.ELF'):
        raise SystemExit('Executed ELF path differs from measured fixture')
    command=run['runner_command']
    if command[command.index('--elf')+1]!=run['executed_ELF'] or command[command.index('--until')+1]!='BOUNDS DONE':
        raise SystemExit('Staged runner arguments differ from complete timing execution')
    for name in ('run.log','runner.log'):
        if hashlib.sha256((folder/name).read_bytes()).hexdigest()!=run[name.replace('.','_')+'_sha256']:
            raise SystemExit('Staged execution log changed '+name)
    fresh=native_report(folder,persist=False,quiet=True)
    if fresh['functions_sha256']!=hashlib.sha256(fixture.data).hexdigest() or fresh['ELF_sha256']!=before['BOUNDS.ELF']:
        raise SystemExit('Executed ELF/functions object differ from exact bound fixture')
    summary={k:v for k,v in fresh['variants'][variant].items() if k!='timings'}
    if summary['rows']!=196 or summary['slower']!=0 or summary['equally_weighted_summed_per_call_cost_reduction_percent']<=0:
        raise SystemExit('Production native timing acceptance gate failed')
    bodies=timed_bodies(fixture,Elf32(folder/'BOUNDS.ELF'))
    return {'run_manifest':str(path),'run_manifest_sha256':hashlib.sha256(path.read_bytes()).hexdigest(),
            'exact_executed_ELF_sha256':fresh['ELF_sha256'],'exact_executed_functions_sha256':fresh['functions_sha256'],
            'all_before_after_staged_inputs_equal':True,'strict_native_log_reparse_passed':True,
            'complete_function_object_to_executed_ELF_body_binding':bodies,
            'native_exactness':fresh['exactness'],'native_timing_summary':summary}


def negatives(out,fixture,variant,engine,executable,map_path):
    controls=[];caller='R_DrawMasked';caller_records=relocations(engine,caller)
    for title in ('helper_register','helper_mmi_opcode','helper_frame','caller_register','caller_frame','caller_RELA_target',
                  'caller_RELA_addend','caller_RELA_type','caller_RELA_location'):
        raw=bytearray(engine.data)
        helper_section=engine.sections[engine.index['.text.'+NAME]]
        caller_section=engine.sections[engine.index['.text.'+caller]]
        record=caller_records[0]
        if title.endswith('_frame'):
            target_name=NAME if title=='helper_frame' else caller
            pdr_index=engine.index['.pdr'];pdr=engine.sections[pdr_index];offsets=[]
            for section in engine.sections:
                if section[1]!=4 or section[7]!=pdr_index:continue
                for at in range(section[4],section[4]+section[5],section[9]):
                    location,info,_=struct.unpack_from('<IIi',engine.data,at)
                    if symbol_tables(engine)[section[6]][info>>8]['name'] in (target_name,'.text.'+target_name):
                        offsets.append(pdr[4]+location+20)
            if len(offsets)!=1:raise SystemExit('Expected one actual frame-field mutation site')
            at=offsets[0];value=struct.unpack_from('<I',raw,at)[0];struct.pack_into('<I',raw,at,value^16)
        elif title in ('helper_register','helper_mmi_opcode','caller_register'):
            section=caller_section if title=='caller_register' else helper_section
            offset=0
            if title=='helper_mmi_opcode':
                # pminw has the exact MMI1 function/subfunction encoding.
                matches=[at for at in range(0,section[5],4)
                         if struct.unpack_from('<I',raw,section[4]+at)[0]&0xfc0007ff==0x700000e8]
                if len(matches)!=1:raise SystemExit('Expected one actual pminw site')
                offset=matches[0]
            at=section[4]+offset;word=struct.unpack_from('<I',raw,at)[0]
            struct.pack_into('<I',raw,at,word^(0x20 if title=='helper_mmi_opcode' else 0x10000))
        elif title=='caller_RELA_target':
            table=symbol_tables(engine)[record['symbol_table']]
            other=next(i for i,s in enumerate(table) if s['name']=='free')
            info=struct.unpack_from('<I',raw,record['record_file_offset']+4)[0]
            struct.pack_into('<I',raw,record['record_file_offset']+4,(other<<8)|(info&255))
        elif title=='caller_RELA_addend':struct.pack_into('<i',raw,record['record_file_offset']+8,4)
        elif title=='caller_RELA_type':
            info=struct.unpack_from('<I',raw,record['record_file_offset']+4)[0]
            struct.pack_into('<I',raw,record['record_file_offset']+4,(info&~255)|5)
        else:struct.pack_into('<I',raw,record['record_file_offset'],record['offset']+4)
        path=out/('negative-'+title+'.o');path.write_bytes(raw);modified=Elf32(path)
        try:
            if title.startswith('helper_'):
                if code(fixture,variant+'_'+NAME)==code(modified,NAME) and frame(fixture,variant+'_'+NAME)==frame(modified,NAME):
                    raise AssertionError('Helper mutation escaped full code check')
            else:
                if code(engine,caller)==code(modified,caller) and frame(engine,caller)==frame(modified,caller):
                    raise AssertionError('Caller mutation escaped full code check')
        except SystemExit:
            pass
        controls.append({'mutation':title,'rejected':True,'file':str(path),
                         'sha256':hashlib.sha256(raw).hexdigest()})
    maps=contributions(map_path)
    original=bind(engine,executable,map_path,caller)
    literal=next(r for r in original['referenced_data'] if 'literal_hex' in r)
    for title in ('linked_helper_register','linked_caller_register','linked_caller_resolved_pointer','linked_literal_byte'):
        raw=bytearray(executable.data)
        if title=='linked_literal_byte':address=literal['linked_address'];bit=1
        elif title=='linked_helper_register':address=maps['.text.'+NAME][0];bit=0x10000
        else:
            address=maps['.text.'+caller][0]+(caller_records[0]['offset'] if title=='linked_caller_resolved_pointer' else 0)
            bit=1 if title=='linked_caller_resolved_pointer' else 0x10000
        section=next(s for s in executable.sections if s[1]!=8 and s[2]&2 and s[3]<=address<s[3]+s[5])
        at=section[4]+address-section[3]
        if title=='linked_literal_byte':raw[at]^=bit
        else:
            word=struct.unpack_from('<I',raw,at)[0];struct.pack_into('<I',raw,at,word^bit)
        path=out/('negative-'+title+'.ELF');path.write_bytes(raw)
        try:
            bind(engine,Elf32(path),map_path,NAME if title=='linked_helper_register' else caller,False)
        except SystemExit as error:
            controls.append({'mutation':title,'rejected':True,'reason':str(error),'file':str(path),
                             'sha256':hashlib.sha256(raw).hexdigest()})
        else:raise SystemExit('Linked-byte mutation escaped exact binding '+title)
    native=Elf32(fixture.path.parent/'BOUNDS.ELF');symbols=linked_symbols(native);symbol=symbols['production_'+NAME]
    for title in ('executed_helper_register','executed_helper_symbol_size'):
        raw=bytearray(native.data)
        if title=='executed_helper_register':
            section=native.sections[symbol['section']];at=section[4]+symbol['value']-section[3]
            value=struct.unpack_from('<I',raw,at)[0];struct.pack_into('<I',raw,at,value^0x10000)
        else:
            sites=[]
            for section in native.sections:
                if section[1]!=2:continue
                strings=native.bytes(native.sections[section[6]])
                for at in range(section[4],section[4]+section[5],section[9]):
                    name_offset=struct.unpack_from('<I',raw,at)[0]
                    name=strings[name_offset:].split(b'\0',1)[0].decode()
                    if name=='production_'+NAME:sites.append(at+8)
            if len(sites)!=1:raise SystemExit('Expected one executed helper symbol-size site')
            struct.pack_into('<I',raw,sites[0],symbol['size']-4)
        path=out/('negative-'+title+'.ELF');path.write_bytes(raw)
        try:timed_bodies(fixture,Elf32(path))
        except SystemExit as error:
            controls.append({'mutation':title,'rejected':True,'reason':str(error),'file':str(path),
                             'sha256':hashlib.sha256(raw).hexdigest()})
        else:raise SystemExit('Executed-helper mutation escaped exact body binding '+title)
    return controls


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',required=True,type=Path)
    p.add_argument('--fixture-object',required=True,type=Path)
    for version in ('baseline','candidate','historical'):
        for kind in ('engine','elf','map'):p.add_argument('--'+version+'-'+kind,type=Path,required=version=='baseline')
    p.add_argument('--candidate-variant',default='production')
    p.add_argument('--context-label',default='Current-header old-bounds reference; full caller code/data/frame equality is checked directly')
    p.add_argument('--negative-controls',action='store_true')
    a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
    fixture=Elf32(a.fixture_object.resolve())
    baseline=Elf32(a.baseline_engine.resolve());baseline_elf=Elf32(a.baseline_elf.resolve())
    manifest=json.loads((fixture.path.parent/'source-hashes.json').read_text())
    for name,digest in manifest['variants'].items():
        if hashlib.sha256((fixture.path.parent/(name+'.inc')).read_bytes()).hexdigest()!=digest:
            raise SystemExit('Frozen extracted source changed '+name)
    for name in ('functions.c','driver.c'):
        if hashlib.sha256((fixture.path.parent/name).read_bytes()).hexdigest()!=manifest[name]:
            raise SystemExit('Frozen fixture source changed '+name)
    for name,digest in manifest.get('current_header_snapshot',{}).items():
        if hashlib.sha256((fixture.path.parent/'frozen-src'/name).read_bytes()).hexdigest()!=digest:
            raise SystemExit('Frozen actual header changed '+name)
    reference_build_report=json.loads((a.baseline_map.resolve().parent/'build-report.json').read_text())
    reference_control=reference_build_report.get('bounds_context_control',{})
    result={'fixture_sha256':hashlib.sha256(fixture.data).hexdigest(),'source_inputs':manifest,
            'current_header_reference':check(fixture,'reference',baseline,baseline_elf,a.baseline_map.resolve()),
            'caller_comparison_context':a.context_label,
            'caller_reference_build_flags':reference_build_report['flags'],
            'caller_reference_added_definition':reference_control.get('only_added_definition'),
            'caller_reference_additional_checked_IR_replacements':reference_control.get('additional_checked_IR_replacements',[])}
    if a.candidate_engine:
        if not a.candidate_elf or not a.candidate_map:raise SystemExit('Candidate engine requires ELF/map')
        if not a.historical_engine or not a.historical_elf or not a.historical_map:
            raise SystemExit('Candidate requires separate historical helper object/ELF/map pins')
        if a.candidate_variant=='production':result['exact_staged_execution_binding']=execution(fixture,a.candidate_variant)
        result['historical_helper_reference']=check(fixture,'reference',Elf32(a.historical_engine.resolve()),
            Elf32(a.historical_elf.resolve()),a.historical_map.resolve())
        candidate=Elf32(a.candidate_engine.resolve());candidate_elf=Elf32(a.candidate_elf.resolve())
        result['candidate']=check(fixture,a.candidate_variant,candidate,candidate_elf,a.candidate_map.resolve())
        before=callers(baseline);after=callers(candidate)
        if before!=after or set(before)!={'R_DrawMasked'} or len(before['R_DrawMasked'])!=3:
            raise SystemExit('Complete caller graph/call sites differ')
        result['callers']={}
        for name,sites in before.items():
            if code(baseline,name)!=code(candidate,name):raise SystemExit('Complete caller code/ordered RELAs differ '+name)
            if frame(baseline,name)!=frame(candidate,name):raise SystemExit('Complete caller frame differs '+name)
            left=bind(baseline,baseline_elf,a.baseline_map.resolve(),name)
            right=bind(candidate,candidate_elf,a.candidate_map.resolve(),name)
            canonical=lambda rows:[{k:v for k,v in row.items() if k!='linked_address'} for row in rows]
            if canonical(left['referenced_data'])!=canonical(right['referenced_data']):
                raise SystemExit('Complete referenced caller data/capacities differ '+name)
            result['callers'][name]={'call_sites':sites,'complete_code_and_ordered_RELA_identity':True,
                'complete_frame_and_referenced_data_identity':True,'reference':left,'candidate':right}
        if a.negative_controls:
            result['binary_gate_negative_controls']=negatives(out,fixture,a.candidate_variant,candidate,candidate_elf,a.candidate_map.resolve())
    elif a.negative_controls:raise SystemExit('Binary controls require candidate object/ELF/map')
    (out/'code-report.json').write_text(json.dumps(result,indent=2)+'\n')
    print('Complete timed code, ordered RELAs, exact data and fully resolved linked bodies pass')


if __name__=='__main__':main()
