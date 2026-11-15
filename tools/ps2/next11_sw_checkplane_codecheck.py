"""Bind actual complete SW helpers/caller/data to resolved game and timed ELFs.

Every ordered explicit RELA, addend, register, branch, frame and padding byte
is retained. Referenced data-pointer RELAs are resolved rather than masked.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import re
import struct

from next7_render_fog_codecheck import Elf32
from next8_sw_codecheck import linked_symbols
from next10_sw_bounds_codecheck import code,frame,relocations,symbol_tables,output_bytes
from next11_sw_checkplane_report import report as native_report
from next11_sw_linker_relas import verify as verify_linker_replay
from next10_gs_run import critical_settings

NAME='R_CheckPlane'
DEPENDENCY='new_visplane.lto_priv.0'
CALLER='R_StoreWallRange'


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()


class Binder:
    def __init__(self,obj,elf,map_path,link_command,linker_relas=None):
        self.obj=obj;self.elf=elf;self.map_path=map_path;self.link_command=link_command
        pattern=r'^\s+(\.[^\s]+)\s+(?:\n\s+)?0x([a-f0-9]+)\s+0x([a-f0-9]+)\s+\S*'+re.escape(obj.path.name)+r'\s*$'
        self.maps={}
        for name,address,size in re.findall(pattern,map_path.read_text(),re.M):
            if name in self.maps:raise SystemExit('Duplicate exact object/map contribution '+name)
            self.maps[name]=(int(address,16),int(size,16))
        self.symbols=linked_symbols(elf)
        self.gp=struct.unpack_from('<I',obj.bytes(obj.sections[obj.index['.reginfo']]),20)[0]
        self.wrappers={a[len('-Wl,--wrap='):] for a in link_command if a.startswith('-Wl,--wrap=')}
        self.data={};self.literals={}
        self.linker_relas={};self.linker_rela_evidence=None
        if linker_relas:
            path=linker_relas/'linker-rela-report.json';report=json.loads(path.read_text())
            diagnostic=Elf32(linker_relas/'LINKER-RELAS.ELF')
            if report['original_ELF_sha256']!=hashlib.sha256(elf.data).hexdigest() or report['original_engine_sha256']!=hashlib.sha256(obj.data).hexdigest() or report['original_map_sha256']!=sha(map_path) or report['diagnostic_ELF_sha256']!=hashlib.sha256(diagnostic.data).hexdigest():
                raise SystemExit('Retained exact linker RELA replay pins differ')
            verify_linker_replay(elf,diagnostic)
            tables=symbol_tables(diagnostic)
            for rela in diagnostic.sections:
                if rela[1] not in (4,9):continue
                if rela[1]!=4:raise SystemExit('Explicit retained final RELAs required')
                for at in range(rela[4],rela[4]+rela[5],rela[9]):
                    offset,info,addend=struct.unpack_from('<IIi',diagnostic.data,at)
                    symbol=tables[rela[6]][info>>8];key=(offset,info&255)
                    self.linker_relas.setdefault(key,[]).append({'exact_final_symbol':symbol['name'],
                        'exact_final_addend':addend,'resolved_address':(symbol['value']+addend)&0xffffffff})
            self.linker_rela_evidence={'report_sha256':sha(path),'diagnostic_ELF_sha256':hashlib.sha256(diagnostic.data).hexdigest(),
                'all_original_allocated_bytes_and_defined_symbols_equal':True}

    def external(self,name):
        if name in self.wrappers:name='__wrap_'+name
        elif name.startswith('__real_') and name[7:] in self.wrappers:name=name[7:]
        matches=[s for table in symbol_tables(self.elf).values() for s in table
                 if s['name']==name and s['binding'] in (1,2) and s['section']!=0]
        values={s['value'] for s in matches}
        if len(values)!=1:raise SystemExit('Missing/ambiguous exact linked target '+name)
        return name,values.pop()

    def mapped(self,index):
        name=self.obj.names[index];section=self.obj.sections[index]
        if name not in self.maps or self.maps[name][1]!=section[5]:
            raise SystemExit('Complete exact mapped input contribution missing/different '+name)
        return self.maps[name][0]

    def resolve(self,symbol,addend,kind,source_address=None):
        index=symbol['section'];name=symbol['name']
        if index==0:
            target,address=self.external(name)
            return (address+addend)&0xffffffff,target
        section=self.obj.sections[index];section_name=self.obj.names[index]
        position=symbol['value']+addend+(self.gp if kind==7 and symbol['type']==3 else 0)
        if section[2]&0x30==0x30:
            if not 0<=position<section[5]:raise SystemExit('Merged complete literal offset outside input')
            literal=self.obj.bytes(section)[position:].split(b'\0',1)[0]+b'\0';locations=[]
            for final_index,final in enumerate(self.elf.sections):
                if not self.elf.names[final_index].startswith('.rodata') or not final[2]&2:continue
                raw=self.elf.bytes(final);at=raw.find(literal)
                while at>=0:
                    if (final[3]+at-position)%section[8]==0:locations.append(final[3]+at)
                    at=raw.find(literal,at+1)
            anchor=None
            if self.linker_rela_evidence:
                anchors=self.linker_relas.get((source_address,kind),[])
                if len(anchors)!=1:raise SystemExit('One exact retained linker literal RELA anchor required')
                anchor=anchors[0];address=anchor['resolved_address']
                if address not in locations:raise SystemExit('Retained exact literal target/address does not contain complete original bytes')
            else:
                if len(locations)!=1:raise SystemExit('Merged complete literal lacks unique aligned exact final bytes '+section_name+
                    ' offset='+str(position)+' bytes='+literal.hex()+' addresses='+str(locations)+' recent data='+str(list(self.data)[-8:]))
                address=locations[0]
            self.literals[(section_name,position)]={'input_section':section_name,'offset':position,
                'complete_literal_hex':literal.hex(),'input_alignment':section[8],'linked_address':address,
                'retained_exact_linker_RELA_anchor':anchor}
        else:
            address=self.mapped(index)+position
            if not section_name.startswith('.text'):self.bind_data(index)
        return address&0xffffffff,name

    def bind_data(self,index):
        name=self.obj.names[index];section=self.obj.sections[index]
        if name in self.data:return
        address=self.mapped(index)
        entry={'input_section':name,'complete_bytes':section[5],'alignment':section[8],
               'linked_address':address,'ordered_RELA_records':[]}
        self.data[name]=entry
        if section[1]==8:
            entry['zero_initialized_capacity']=True;return
        raw=self.obj.bytes(section);resolved=bytearray(raw)
        entry['input_sha256']=hashlib.sha256(raw).hexdigest()
        for rela in self.obj.sections:
            if rela[7]!=index or rela[1] not in (4,9):continue
            if rela[1]!=4:raise SystemExit('Explicit data RELAs required '+name)
            table=symbol_tables(self.obj)[rela[6]]
            for at in range(rela[4],rela[4]+rela[5],rela[9]):
                offset,info,addend=struct.unpack_from('<IIi',self.obj.data,at);kind=info&255;symbol=table[info>>8]
                if kind!=2 or offset+4>len(raw) or struct.unpack_from('<I',raw,offset)[0]:
                    raise SystemExit('Unexpected nonzero/non-R_MIPS_32 data field '+name)
                value,target=self.resolve(symbol,addend,kind,address+offset);struct.pack_into('<I',resolved,offset,value)
                entry['ordered_RELA_records'].append({'offset':offset,'kind':kind,'exact_symbol':symbol['name'],
                    'explicit_addend':addend,'resolved_symbol':target,'resolved_address':value})
        if output_bytes(self.elf,address,len(resolved))!=resolved:
            raise SystemExit('Fully resolved complete data bytes differ '+name)
        entry['resolved_sha256']=hashlib.sha256(resolved).hexdigest();entry['all_fully_resolved_bytes_equal']=True

    def named_data(self,name):
        matches=[s for table in symbol_tables(self.obj).values() for s in table
                 if s['name']==name and s['type']==1 and s['section']!=0]
        if len(matches)!=1:raise SystemExit('One exact input data object required '+name)
        self.bind_data(matches[0]['section'])

    def text(self,name):
        index=self.obj.index['.text.'+name];raw=self.obj.bytes(self.obj.sections[index]);resolved=bytearray(raw)
        address=self.mapped(index);checked=[]
        symbols=[s for table in symbol_tables(self.obj).values() for s in table
                 if s['name']==name and s['type']==2 and s['section']==index]
        if len(symbols)!=1 or self.symbols[name]['size']!=symbols[0]['size'] or self.symbols[name]['value']!=address:
            raise SystemExit('Exact input/linked function symbol extent/address differs '+name)
        for record in relocations(self.obj,name):
            offset=record['offset'];kind=record['kind'];symbol=record['symbol'];addend=record['addend']
            value,target=self.resolve(symbol,addend,kind,address+offset)
            if kind==4:mask=0x03ffffff;field=(value>>2)&mask
            elif kind==5:mask=0xffff;field=((value+0x8000)>>16)&mask
            elif kind==6:mask=0xffff;field=value&mask
            elif kind==7:
                mask=0xffff;displacement=value-self.symbols['_gp']['value']
                if not -32768<=displacement<=32767:raise SystemExit('Exact GP relocation exceeds signed16')
                field=displacement&mask
            else:raise SystemExit('Unexpected complete code RELA kind')
            word=struct.unpack_from('<I',resolved,offset)[0]
            if word&mask:raise SystemExit('Expected zero encoded explicit RELA field')
            struct.pack_into('<I',resolved,offset,word|field)
            checked.append({'offset':offset,'kind':kind,'exact_symbol':symbol['name'],'explicit_addend':addend,
                            'resolved_symbol':target,'resolved_address':value})
        if output_bytes(self.elf,address,len(resolved))!=resolved:
            raise SystemExit('Fully resolved complete linked text/padding differs '+name)
        return {'name':name,'complete_text_bytes':len(raw),'exact_symbol_bytes':symbols[0]['size'],
                'all_fully_resolved_text_and_padding_bytes_equal':True,'exact_frame':frame(self.obj,name),
                'ordered_RELA_records':checked,'referenced_data':list(self.data.values()),
                'complete_literals':list(self.literals.values()),'input_object_sha256':hashlib.sha256(self.obj.data).hexdigest(),
                'ELF_sha256':hashlib.sha256(self.elf.data).hexdigest(),'map_sha256':sha(self.map_path),
                'actual_memory_wrappers':sorted(self.wrappers),'retained_linker_RELA_evidence':self.linker_rela_evidence}


def build_binder(engine_path,elf_path,map_path,linker_relas=None):
    build=json.loads((map_path.parent/'build-report.json').read_text())
    if sha(elf_path)!=build['elf_sha256']:raise SystemExit('Build report does not pin exact resolved game ELF')
    return Binder(Elf32(engine_path),Elf32(elf_path),map_path,build['link_command'],linker_relas)


def simple_records(obj,name,rename=None):
    rename=rename or {}
    return [(r['offset'],r['kind'],rename.get(r['symbol']['name'],r['symbol']['name']),r['addend'])
            for r in relocations(obj,name)]


def fixture_match(fixture,name,engine,actual,rename=None):
    left=code(fixture,name);right=code(engine,actual)
    if left['words']!=right['words'] or simple_records(fixture,name,rename)!=simple_records(engine,actual):
        raise SystemExit('Complete fixture/actual LTO instructions/ordered exact-target RELAs differ '+name)
    if frame(fixture,name)!=frame(engine,actual):raise SystemExit('Complete fixture/actual frame differs '+name)
    return {'all_complete_instructions_ordered_RELA_targets_addends_and_frame_equal':True,
            'complete_text_bytes':left['text_bytes'],'ordered_RELA_count':len(simple_records(fixture,name)),
            'exact_frame':frame(fixture,name)}


def callers(engine):
    result={};tables=symbol_tables(engine)
    for rela in engine.sections:
        if rela[1]!=4 or not engine.names[rela[7]].startswith('.text.'):continue
        for at in range(rela[4],rela[4]+rela[5],rela[9]):
            offset,info,addend=struct.unpack_from('<IIi',engine.data,at)
            if tables[rela[6]][info>>8]['name']==NAME:
                result.setdefault(engine.names[rela[7]][6:],[]).append((offset,info&255,addend))
    return result


def execution(folder,selected_variant='production'):
    manifest=json.loads((folder/'run-manifest.json').read_text())
    if manifest['inputs_before']!=manifest['inputs_after'] or not manifest['all_original_and_staged_inputs_unchanged'] or not manifest['validation_passed'] or manifest['runner_returncode']:
        raise SystemExit('Executed immutable input/outcome manifest failed')
    if not manifest['every_non_UI_setting_unchanged'] or manifest['critical_settings_before']!=manifest['critical_settings_after']:
        raise SystemExit('Complete non-UI native emulator settings differ')
    for name,digest in manifest['inputs_before'].items():
        if sha(folder/name)!=digest:raise SystemExit('Executed frozen native file changed '+name)
    for name in ('run.log','runner.log'):
        if sha(folder/name)!=manifest[name.replace('.','_')+'_sha256']:raise SystemExit('Executed log changed '+name)
    for name,digest in manifest['proof_tools_before_and_after_sha256'].items():
        if sha(folder/'pinned-tools'/Path(name).name)!=digest:
            raise SystemExit('Archived exact executed proof tool changed '+name)
    for phase,key in (('before','critical_settings_before'),('after','critical_settings_after')):
        for name,settings in manifest[key].items():
            parts=Path(name).parts;starts=[i for i,part in enumerate(parts) if re.fullmatch(r'slot\d+',part)]
            if len(starts)!=1:raise SystemExit('One exact archived emulator slot config path required')
            archived=folder/('run-config-'+phase)/Path(*parts[starts[0]:])
            if critical_settings(archived)!=settings:
                raise SystemExit('Complete archived non-UI emulator settings differ '+name)
    command=manifest['runner_command']
    if manifest['executed_ELF']!=str(folder/'CHECKPLANE.ELF') or command[command.index('--elf')+1]!=manifest['executed_ELF'] or command[command.index('--until')+1]!='CHECKPLANE DONE':
        raise SystemExit('Actual executed complete fixture/marker differs')
    result=native_report(folder,False)
    if selected_variant=='production' and (result['summary']['combined']['slower'] or result['summary']['combined']['equally_weighted_summed_per_call_cost_reduction_percent']<=0):
        raise SystemExit('Native no-regression cost gate failed')
    retained={'run_manifest_sha256':sha(folder/'run-manifest.json'),'exactness':result['exactness'],
            'selected_executed_variant':selected_variant,
            'all_compiled_executed_inputs_and_all_non_UI_settings_pinned':True}
    if selected_variant=='production':retained['summary']=result['summary']
    else:retained['scope']='Original reference side of the exact execution is retained; no new-helper Hardware cost improvement is claimed'
    return retained


def canonical_data(report):
    def clean(value):
        if isinstance(value,dict):return {k:clean(v) for k,v in value.items() if k not in ('linked_address','resolved_address','resolved_sha256','exact_final_addend')}
        if isinstance(value,list):return [clean(v) for v in value]
        return value
    return clean({'data':report['referenced_data'],'literals':report['complete_literals']})


def binary_controls(out,game,folder,selected_variant='production'):
    """Mutate actual binary fields and exercise code/frame/full-resolution gates."""
    results=[]
    def reject(title,raw,suffix,check):
        path=out/('negative-'+title+suffix);path.write_bytes(raw);mutated=Elf32(path)
        try:check(mutated)
        except SystemExit as error:
            results.append({'mutation':title,'rejected':True,'reason':str(error),'file':str(path),'sha256':sha(path)})
        else:raise SystemExit('Actual binary mutation escaped gate '+title)
    def equality(mutated,name):
        if code(game.obj,name)!=code(mutated,name) or frame(game.obj,name)!=frame(mutated,name):
            raise SystemExit('Complete actual instructions/ordered RELAs/frame mismatch '+name)
    def clone(obj=None,elf=None):
        value=copy.copy(game);value.obj=obj or game.obj;value.elf=elf or game.elf
        value.symbols=linked_symbols(value.elf);value.data={};value.literals={}
        return value
    def change_frame(raw,name):
        pdr=game.obj.sections[game.obj.index['.pdr']];sites=[];tables=symbol_tables(game.obj)
        for rela in game.obj.sections:
            if rela[1]!=4 or rela[7]!=game.obj.index['.pdr']:continue
            for at in range(rela[4],rela[4]+rela[5],rela[9]):
                offset,info,addend=struct.unpack_from('<IIi',game.obj.data,at)
                if tables[rela[6]][info>>8]['name'] in (name,'.text.'+name):sites.append(pdr[4]+offset+20)
        if len(sites)!=1:raise SystemExit('One actual PDR mutation field required')
        at=sites[0];struct.pack_into('<I',raw,at,struct.unpack_from('<I',raw,at)[0]^16)
    for label,name in (('helper',NAME),('caller',CALLER),('dependency',DEPENDENCY)):
        for field in ('register','frame'):
            raw=bytearray(game.obj.data)
            if field=='frame':change_frame(raw,name)
            else:
                section=game.obj.sections[game.obj.index['.text.'+name]];at=section[4]
                struct.pack_into('<I',raw,at,struct.unpack_from('<I',raw,at)[0]^0x10000)
            reject(label+'_'+field,raw,'.o',lambda mutated,n=name:equality(mutated,n))
    records=relocations(game.obj,CALLER);record=records[0]
    for field in ('target','addend','type','location'):
        raw=bytearray(game.obj.data);at=record['record_file_offset']
        if field=='target':
            table=symbol_tables(game.obj)[record['symbol_table']]
            other=next(i for i,s in enumerate(table) if s['name']=='free')
            info=struct.unpack_from('<I',raw,at+4)[0];struct.pack_into('<I',raw,at+4,(other<<8)|(info&255))
        elif field=='addend':struct.pack_into('<i',raw,at+8,record['addend']+4)
        elif field=='type':
            info=struct.unpack_from('<I',raw,at+4)[0];struct.pack_into('<I',raw,at+4,(info&~255)|1)
        else:struct.pack_into('<I',raw,at,record['offset']+4)
        reject('caller_RELA_'+field,raw,'.o',lambda mutated:equality(mutated,CALLER))
    data_index=game.obj.index['.sdata.finecosine'];data_relas=[s for s in game.obj.sections if s[1]==4 and s[7]==data_index]
    if len(data_relas)!=1 or data_relas[0][5]!=12:raise SystemExit('One exact finecosine pointer RELA required')
    data_rela=data_relas[0];at=data_rela[4]
    for field in ('target','addend'):
        raw=bytearray(game.obj.data)
        if field=='target':
            table=symbol_tables(game.obj)[data_rela[6]];other=next(i for i,s in enumerate(table) if s['name']=='tantoangle')
            info=struct.unpack_from('<I',raw,at+4)[0];struct.pack_into('<I',raw,at+4,(other<<8)|(info&255))
        else:struct.pack_into('<i',raw,at+8,struct.unpack_from('<i',raw,at+8)[0]+4)
        reject('data_RELA_'+field,raw,'.o',lambda mutated:clone(obj=mutated).text(CALLER))
    literal_index=game.obj.index['.rodata.sideudmf_default'];literal_section=game.obj.sections[literal_index]
    raw=bytearray(game.obj.data);raw[literal_section[4]]^=1
    reject('object_literal_byte',raw,'.o',lambda mutated:clone(obj=mutated).text(CALLER))
    def change_linked(raw,address,bit=1,word=True):
        section=next(s for s in game.elf.sections if s[1]!=8 and s[2]&2 and s[3]<=address<s[3]+s[5])
        at=section[4]+address-section[3]
        if word:struct.pack_into('<I',raw,at,struct.unpack_from('<I',raw,at)[0]^bit)
        else:raw[at]^=bit
    for title,name in (('linked_helper_register',NAME),('linked_caller_register',CALLER),('linked_dependency_register',DEPENDENCY)):
        raw=bytearray(game.elf.data);change_linked(raw,game.maps['.text.'+name][0],0x10000)
        reject(title,raw,'.ELF',lambda mutated,n=name:clone(elf=mutated).text(n))
    call=next(r for r in records if r['kind']==4)
    raw=bytearray(game.elf.data);change_linked(raw,game.maps['.text.'+CALLER][0]+call['offset'])
    reject('linked_resolved_call_pointer',raw,'.ELF',lambda mutated:clone(elf=mutated).text(CALLER))
    raw=bytearray(game.elf.data);change_linked(raw,game.maps['.rodata.sideudmf_default'][0],word=False)
    reject('linked_literal_byte',raw,'.ELF',lambda mutated:clone(elf=mutated).text(CALLER))
    # The same bytes at another valid aligned "0" literal must still fail:
    # exact retained linker RELAs determine the selected pointer address.
    source=next(r for r in game.data['.data.cv_secbright']['ordered_RELA_records']
                if r['exact_symbol']=='.rodata.str1.8' and output_bytes(game.elf,r['resolved_address'],2)==b'0\0')
    address=game.maps['.data.cv_secbright'][0]+source['offset'];old=source['resolved_address'];alternatives=[]
    for section in game.elf.sections:
        if section[1]==8 or not section[2]&2:continue
        content=game.elf.bytes(section);position=content.find(b'0\0')
        while position>=0:
            if section[3]+position!=old and (section[3]+position)%8==old%8:alternatives.append(section[3]+position)
            position=content.find(b'0\0',position+1)
    if not alternatives:raise SystemExit('An alternate identical aligned zero literal is required')
    raw=bytearray(game.elf.data);section=next(s for s in game.elf.sections if s[1]!=8 and s[3]<=address< s[3]+s[5])
    struct.pack_into('<I',raw,section[4]+address-section[3],alternatives[0])
    reject('linked_pointer_to_identical_other_zero_literal',raw,'.ELF',lambda mutated:clone(elf=mutated).text(CALLER))
    native=Elf32(folder/'CHECKPLANE.ELF');build=json.loads((folder/'ee-build-report.json').read_text())
    symbols=linked_symbols(native)
    for title,obj_name,name,field in (
        ('executed_helper_register',selected_variant,selected_variant+'_'+NAME,'register'),
        ('executed_helper_symbol_size',selected_variant,selected_variant+'_'+NAME,'size'),
        ('executed_dependency_register','dependency','new_visplane','register'),
        ('executed_wrapper_register','memops','__wrap_memcpy','register')):
        raw=bytearray(native.data);symbol=symbols[name]
        if field=='register':
            section=native.sections[symbol['section']];at=section[4]+symbol['value']-section[3]
            struct.pack_into('<I',raw,at,struct.unpack_from('<I',raw,at)[0]^0x10000)
        else:
            sites=[]
            for section in native.sections:
                if section[1]!=2:continue
                strings=native.bytes(native.sections[section[6]])
                for at in range(section[4],section[4]+section[5],section[9]):
                    offset=struct.unpack_from('<I',raw,at)[0]
                    if strings[offset:].split(b'\0',1)[0].decode()==name:sites.append(at+8)
            if len(sites)!=1:raise SystemExit('One executed symbol-size mutation required')
            struct.pack_into('<I',raw,sites[0],symbol['size']-4)
        obj=Elf32(folder/(obj_name+'.o'))
        reject(title,raw,'.ELF',lambda mutated,o=obj,n=name:Binder(o,mutated,folder/'CHECKPLANE.map',build['link_command']).text(n))
    return results


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',type=Path,required=True);parser.add_argument('--fixture-dir',type=Path,required=True)
    for version in ('baseline','candidate'):
        for kind in ('engine','elf','map'):parser.add_argument('--'+version+'-'+kind,type=Path,required=True)
        parser.add_argument('--'+version+'-linker-relas',type=Path)
    parser.add_argument('--static-only',action='store_true')
    parser.add_argument('--negative-controls',action='store_true')
    parser.add_argument('--candidate-variant',choices=('production','reference'),default='production',
                        help='Hardware NOHW fallback must retain the executed original reference body')
    for kind in ('engine','elf','map','linker-relas'):parser.add_argument('--historical-'+kind,type=Path)
    parser.add_argument('--context-label',default='Frozen 602-source reference and only the R_CheckPlane scan overlay')
    args=parser.parse_args();out=args.out.resolve();out.mkdir(parents=True,exist_ok=True);folder=args.fixture_dir.resolve()
    left=build_binder(args.baseline_engine.resolve(),args.baseline_elf.resolve(),args.baseline_map.resolve(),args.baseline_linker_relas)
    right=build_binder(args.candidate_engine.resolve(),args.candidate_elf.resolve(),args.candidate_map.resolve(),args.candidate_linker_relas)
    if args.candidate_variant=='reference' and not all((args.historical_engine,args.historical_elf,args.historical_map,args.historical_linker_relas)):
        raise SystemExit('Original-scan retention requires the complete real historical engine/ELF/map/linker replay')
    manifest=json.loads((folder/'source-hashes.json').read_text())
    for name,digest in manifest['files'].items():
        if sha(folder/name)!=digest:raise SystemExit('Frozen actual fixture source differs '+name)
    for name,digest in manifest['frozen_headers'].items():
        if sha(folder/'frozen-src'/name)!=digest:raise SystemExit('Frozen actual fixture header differs '+name)
    report={'caller_comparison_context':args.context_label,'source_manifest_sha256':sha(folder/'source-hashes.json'),
            'reference':{},'candidate':{},'timed_ELF':{},'selected_candidate_variant':args.candidate_variant,
            'new_scan_cost_improvement_applies_to_candidate':args.candidate_variant=='production'}
    fixture_build=json.loads((folder/'ee-build-report.json').read_text())
    if sha(folder/'CHECKPLANE.ELF')!=fixture_build['ELF_sha256']:raise SystemExit('Compiled exact timed ELF differs')
    native=Elf32(folder/'CHECKPLANE.ELF')
    for role,variant,binder in (('reference','reference',left),('candidate',args.candidate_variant,right)):
        fixture=Elf32(folder/(variant+'.o'));name=variant+'_'+NAME
        match=fixture_match(fixture,name,binder.obj,NAME,{'new_visplane':DEPENDENCY})
        bound=Binder(fixture,native,folder/'CHECKPLANE.map',fixture_build['link_command']).text(name)
        report[role].update(helper_match=match,game_helper=binder.text(NAME))
        report['timed_ELF'][name]=bound
    if callers(left.obj)!=callers(right.obj) or set(callers(left.obj))!={CALLER}:
        raise SystemExit('Complete actual helper call graph/sites differ')
    for name in (CALLER,DEPENDENCY,'__wrap_memcpy','__wrap_memset'):
        if code(left.obj,name)!=code(right.obj,name) or frame(left.obj,name)!=frame(right.obj,name):
            raise SystemExit('Complete actual caller/dependency/wrapper code+ordered RELAs/frame differs '+name)
        l=left.text(name);r=right.text(name)
        if canonical_data(l)!=canonical_data(r):raise SystemExit('Complete actual data/literal/capacity changed '+name)
        report['reference'][name]=l;report['candidate'][name]=r
    if args.candidate_variant=='reference':
        historical=build_binder(args.historical_engine.resolve(),args.historical_elf.resolve(),args.historical_map.resolve(),args.historical_linker_relas)
        current_for_history=build_binder(args.candidate_engine.resolve(),args.candidate_elf.resolve(),args.candidate_map.resolve(),args.candidate_linker_relas)
        witness={'scope':'Complete original Hardware helper/caller/dependency/wrapper retention against its real frozen baseline; no capacity normalization',
                 'functions':{}}
        if callers(historical.obj)!=callers(right.obj):raise SystemExit('Historical original caller graph differs')
        for name in (NAME,CALLER,DEPENDENCY,'__wrap_memcpy','__wrap_memset'):
            if code(historical.obj,name)!=code(right.obj,name) or frame(historical.obj,name)!=frame(right.obj,name):
                raise SystemExit('Complete historical original code/RELAs/frame differs '+name)
            before=historical.text(name);after=current_for_history.text(name)
            if canonical_data(before)!=canonical_data(after):raise SystemExit('Historical original data/literal/capacity differs '+name)
            witness['functions'][name]={'complete_original_code_ordered_RELAs_frame_data_and_capacities_equal':True,
                                       'historical':before,'shipping':after}
        report['historical_original_retention']=witness
    dependency=Elf32(folder/'dependency.o');source=json.loads((folder/'dependency.json').read_text())
    if source['original_engine_sha256']!=hashlib.sha256(left.obj.data).hexdigest():
        # Current-header controls may have a different engine hash; their entire
        # dependency is still checked against the original extraction below.
        original=Elf32(Path(source['original_engine']))
        if sha(Path(source['original_engine']))!=source['original_engine_sha256'] or code(original,DEPENDENCY)!=code(left.obj,DEPENDENCY):
            raise SystemExit('Exact original dependency extraction differs from caller reference')
    if code(dependency,'new_visplane')['words']!=code(left.obj,DEPENDENCY)['words'] or frame(dependency,'new_visplane')!=frame(left.obj,DEPENDENCY):
        raise SystemExit('Complete timed dependency instructions/frame differ')
    expected=[(r['offset'],r['kind'],r['fixture_symbol'],r['fixture_addend']) for r in source['ordered_relocations']]
    if simple_records(dependency,'new_visplane')!=expected:
        raise SystemExit('Complete exact timed dependency ordered RELAs differ')
    original_records=[(r['offset'],r['kind'],r['exact_original_symbol'],r['exact_original_addend']) for r in source['ordered_relocations']]
    if simple_records(left.obj,DEPENDENCY)!=original_records:
        raise SystemExit('Complete exact original dependency ordered RELAs differ from extraction')
    report['timed_ELF']['new_visplane']=Binder(dependency,native,folder/'CHECKPLANE.map',fixture_build['link_command']).text('new_visplane')
    driver=Binder(Elf32(folder/'driver.o'),native,folder/'CHECKPLANE.map',fixture_build['link_command'])
    for name in ('visplanes','viewwidth','freetail','freehead','next11_ztracemin'):driver.named_data(name)
    report['timed_dependency_complete_driver_data']=list(driver.data.values())
    memops=Elf32(folder/'memops.o');mem=Binder(memops,native,folder/'CHECKPLANE.map',fixture_build['link_command'])
    for name in ('__wrap_memcpy','__wrap_memset'):
        fixture_match(memops,name,left.obj,name)
        report['timed_ELF'][name]=mem.text(name)
    if not args.static_only:report['exact_staged_execution_binding']=execution(folder,args.candidate_variant)
    report['whole_actual_caller_instruction_bytes']=code(left.obj,CALLER)['text_bytes']
    report['whole_actual_caller_ordered_RELA_count']=len(relocations(left.obj,CALLER))
    report['actual_call_sites']=callers(left.obj)
    if args.negative_controls:report['actual_binary_negative_controls']=binary_controls(out,right,folder,args.candidate_variant)
    report['checker_source_sha256']=sha(Path(__file__).resolve())
    (out/'code-report.json').write_text(json.dumps(report,indent=2)+'\n')
    print('Complete actual helpers, caller, wrappers, RELAs, data and resolved game/timed ELF bytes pass',flush=True)


if __name__=='__main__':main()
