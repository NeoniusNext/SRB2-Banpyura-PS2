"""Create diagnostic-only, layout-preserving fixed-local-RNG ELF copies.

Only M_RandomSeedFromOS's existing symbol extent changes. The stub calls the
unchanged actual M_RandomSeed(UINT32), preserving the complete SFC32 algorithm.
No production source or input object/ELF/map is written.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
from next7_render_fog_codecheck import Elf32
from next8_sw_codecheck import linked_symbols
from next10_sw_bounds_codecheck import bind,code,contributions,frame,output_bytes,relocations,symbol_tables

ROOT=Path(__file__).resolve().parents[2]
INITIALIZER='M_RandomSeedFromOS'
SEEDER='M_RandomSeed'
STUB='Next10_LocalRngStub'


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def writes(raw):
    """Decode every actual instruction in these scalar leaf/initializer bodies."""
    result=set();effects=[]
    for at in range(0,len(raw),4):
        word=struct.unpack_from('<I',raw,at)[0];op=word>>26;rt=(word>>16)&31;rd=(word>>11)&31;fun=word&63
        if op==0:
            if fun in (0,2,3,4,6,7,10,11,32,33,34,35,36,37,38,39,42,43,56,58,59,60,62,63):result.add(rd)
            elif fun==8:pass
            elif fun==9:result.add(rd)
            else:effects.append((at,hex(word),'unhandled SPECIAL/HI-LO effect'))
        elif op==3:result.add(31)
        elif op in (8,9,10,11,12,13,14,15,24,25,26,27,32,33,34,35,36,37,38,39,55):result.add(rt)
        elif op in (2,4,5,6,7,40,41,42,43,44,45,46,63):pass
        else:effects.append((at,hex(word),'unhandled COP/FPU/MMI effect'))
    result.discard(0)
    return result,effects


def stub_words(seed,target=0):
    return [0x27bdffe0,0xffbf0018,0x3c040000|(seed>>16),0x34840000|(seed&65535),
            0x0c000000|((target>>2)&0x03ffffff),0,0xdfbf0018,0x24020001,0x03e00008,0x27bd0020]


def audit_template(source,obj,seed):
    parsed=Elf32(obj);raw=parsed.bytes(parsed.sections[parsed.index['.text.'+STUB]])
    expected=struct.pack('<10I',*stub_words(seed))
    records=relocations(parsed,STUB)
    if raw!=expected or len(records)!=1 or (records[0]['offset'],records[0]['kind'],records[0]['symbol']['name'],records[0]['addend'])!=(16,4,SEEDER,0):
        raise SystemExit('Assembled stub differs from complete audited scalar template/RELA')
    if frame(parsed,STUB)['stack_frame_bytes']!=32:raise SystemExit('Stub frame record differs')
    return {'assembly':str(source),'assembly_sha256':sha(source),'object':str(obj),'object_sha256':sha(obj),
            'complete_text_bytes':len(raw),'only_RELA':{'offset':16,'kind':4,'target':SEEDER,'explicit_addend':0},
            'exact_frame_record':frame(parsed,STUB)}


def assemble(out,seed):
    import build as B
    source=out/('stub-%08x.S'%seed);obj=source.with_suffix('.o')
    source.write_text('''\t.set noreorder
\t.set noat
\t.set nomacro
\t.section .text.Next10_LocalRngStub,"ax",@progbits
\t.align 3
\t.globl Next10_LocalRngStub
\t.type Next10_LocalRngStub,@function
\t.ent Next10_LocalRngStub
Next10_LocalRngStub:
\t.frame $sp,32,$ra
\t.mask 0x80000000,-8
\taddiu $sp,$sp,-32
\tsd $ra,24($sp)
\tlui $a0,0x%04x
\tori $a0,$a0,0x%04x
\tjal M_RandomSeed
\tnop
\tld $ra,24($sp)
\taddiu $v0,$zero,1
\tjr $ra
\taddiu $sp,$sp,32
\t.end Next10_LocalRngStub
\t.size Next10_LocalRngStub,.-Next10_LocalRngStub
'''%(seed>>16,seed&65535))
    command=[str(B.CC),'-G8','-mno-abicalls','-c',str(source),'-o',str(obj)]
    result=subprocess.run(command,cwd=ROOT,capture_output=True,text=True)
    (out/('stub-%08x-build.log'%seed)).write_text(' '.join(command)+'\n'+result.stdout+result.stderr)
    if result.returncode or result.stdout or result.stderr:raise SystemExit('Diagnostic stub assembler failed')
    return audit_template(source,obj,seed)


def verify_patch(original,patched,start,size,payload):
    if len(original)!=len(patched):raise SystemExit('Diagnostic patch changed ELF length')
    if original[:start]!=patched[:start] or original[start+size:]!=patched[start+size:]:
        raise SystemExit('Diagnostic patch changed bytes outside original initializer symbol extent')
    if patched[start:start+len(payload)]!=payload or patched[start+len(payload):start+size]!=bytes(size-len(payload)):
        raise SystemExit('Diagnostic stub or complete NOP fill differs')


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for kind in ('engine','elf','map','out'):p.add_argument('--'+kind,type=Path,required=True)
    p.add_argument('--seeds',default='0x01b2d3f5,0x9e3779b9')
    p.add_argument('--assemble',action='store_true',help='consume one granted EE worker to independently assemble each template')
    p.add_argument('--template-dir',type=Path,help='reuse and fully check previously assembled same-seed templates; no EE compiler')
    p.add_argument('--audit-only',action='store_true',help='verify existing diagnostic copies without writing ELF bytes or compiling')
    p.add_argument('--negative-controls',action='store_true')
    a=p.parse_args();out=a.out.resolve()
    if a.assemble and a.template_dir:raise SystemExit('Choose compilation or existing assembled templates')
    if a.audit_only and a.assemble:raise SystemExit('Read-only audit cannot compile a stub')
    if not out.is_relative_to(ROOT/'build'):raise SystemExit('Diagnostic outputs must be under build/')
    out.mkdir(parents=True,exist_ok=True)
    paths={kind:getattr(a,kind).resolve() for kind in ('engine','elf','map')};inputs={kind:sha(path) for kind,path in paths.items()}
    engine=Elf32(paths['engine']);executable=Elf32(paths['elf']);syms=linked_symbols(executable);maps=contributions(paths['map'])
    original_bindings={name:bind(engine,executable,paths['map'],name) for name in (INITIALIZER,SEEDER)}
    init=syms[INITIALIZER];seeder=syms[SEEDER]
    if init['size']<40 or init['size']%4 or frame(engine,INITIALIZER)['stack_frame_bytes']!=32 or frame(engine,SEEDER)['stack_frame_bytes']!=0:
        raise SystemExit('Unexpected actual initializer extent or initializer/seeder ABI')
    header=executable.sections[init['section']];start=header[4]+init['value']-header[3]
    own_init=engine.bytes(engine.sections[engine.index['.text.'+INITIALIZER]])
    own_seed=engine.bytes(engine.sections[engine.index['.text.'+SEEDER]])
    seed_writes,seed_effects=writes(own_seed);init_writes,init_effects=writes(own_init)
    if seed_effects or init_effects or not seed_writes<=init_writes or seed_writes&set(range(16,24)) or seed_writes&{28,29,30,31}:
        raise SystemExit('Actual seeder has effects outside original initializer IPA clobber set')
    if any(r['kind']==4 for r in relocations(engine,SEEDER)):
        raise SystemExit('Actual seeder is not a leaf')
    calls=[];state_names={r['symbol']['name'] for r in relocations(engine,SEEDER)};graph=set()
    tables=symbol_tables(engine)
    for section in engine.sections:
        name=engine.names[section[7]]
        if section[1]!=4 or not name.startswith('.text.'):continue
        for at in range(section[4],section[4]+section[5],section[9]):
            offset,info,addend=struct.unpack_from('<IIi',engine.data,at);target=tables[section[6]][info>>8]['name']
            if target in (INITIALIZER,'.text.'+INITIALIZER):
                caller=name[6:];address=maps[name][0]+offset
                word=struct.unpack('<I',output_bytes(executable,address,4))[0]
                if info&255!=4 or addend!=0 or word!=stub_words(0,init['value'])[4]:raise SystemExit('Actual initializer callsite does not resolve exactly')
                calls.append({'caller':caller,'offset':offset,'address':address,'instruction':hex(word)})
            if target in state_names:graph.add(name[6:])
    if len(calls)!=1 or calls[0]['caller']!='D_SRB2Main':raise SystemExit('Expected exactly one actual startup initializer call')
    graph.discard(INITIALIZER);graph.add(SEEDER);graph_pins={}
    for name in sorted(graph):
        address,size=maps['.text.'+name]
        graph_pins[name]={'address':address,'complete_owned_text_bytes':size,
                          'original_ELF_sha256':hashlib.sha256(output_bytes(executable,address,size)).hexdigest()}
    seeds=[int(value,0) for value in a.seeds.split(',')]
    if len(seeds)!=len(set(seeds)) or any(not 0<=seed<=0xffffffff for seed in seeds):raise SystemExit('Seeds must be unique UINT32 values')
    result={'diagnostic_only':True,'mode':'readonly-audit' if a.audit_only else 'diagnostic-generation',
            'checker_sha256':sha(Path(__file__)),'production_inputs_unchanged':True,'original_inputs':{kind:{'path':str(paths[kind]),'sha256':inputs[kind]} for kind in paths},
            'original_initializer_and_actual_seeder_bindings':original_bindings,'actual_initializer_calls':calls,
            'IPA_clobber_proof':{'actual_seeder_GPR_writes':sorted(seed_writes),'original_initializer_own_GPR_writes':sorted(init_writes),
                'seeder_writes_subset_of_original_initializer':True,'no_FPU_HI_LO_GP_or_callee_saved_effects':True},
            'unchanged_actual_SFC32_state_consumers':graph_pins,'variants':{}}
    for seed in seeds:
        assembled=assemble(out,seed) if a.assemble else None
        if a.template_dir:
            template=a.template_dir.resolve()/('stub-%08x.S'%seed)
            assembled=audit_template(template,template.with_suffix('.o'),seed)
            assembled['reused_without_recompilation']=True
        if ((init['value']+20)^seeder['value'])&0xf0000000:raise SystemExit('Actual JAL target is outside caller region')
        payload=struct.pack('<10I',*stub_words(seed,seeder['value']))
        stub_writes,stub_effects=writes(payload)
        if stub_effects or not (stub_writes|seed_writes)<=init_writes:raise SystemExit('Stub exceeds actual original IPA GPR clobbers')
        modified=bytearray(executable.data);modified[start:start+init['size']]=payload+bytes(init['size']-len(payload))
        verify_patch(executable.data,modified,start,init['size'],payload)
        path=out/('DIAGNOSTIC-LOCAL-RNG-%08x.ELF'%seed)
        if a.audit_only:
            modified=bytearray(path.read_bytes());verify_patch(executable.data,modified,start,init['size'],payload)
        else:path.write_bytes(modified)
        parsed=Elf32(path)
        if linked_symbols(parsed)!=syms:raise SystemExit('Patched ELF symbols/addresses/sizes changed')
        for name,pin in graph_pins.items():
            if hashlib.sha256(output_bytes(parsed,pin['address'],pin['complete_owned_text_bytes'])).hexdigest()!=pin['original_ELF_sha256']:
                raise SystemExit('Actual SFC32 seeding/generator graph changed '+name)
        row={'seed_uint32':seed,'seed_hex':'0x%08x'%seed,'ELF':str(path),'ELF_sha256':sha(path),
             'initializer_address':init['value'],'patched_file_offset':start,'owned_symbol_bytes':init['size'],
             'stub_bytes':len(payload),'NOP_fill_bytes':init['size']-len(payload),'stub_words_hex':['%08x'%word for word in stub_words(seed,seeder['value'])],
             'actual_M_RandomSeed_address':seeder['value'],'frame_bytes':32,'saved_RA_offset':24,'outgoing_arg_area_bytes':16,
             'return_value':1,'all_other_ELF_bytes_identical':True,'all_symbol_sizes_addresses_identical':True,
             'all_actual_SFC32_graph_bytes_unchanged':True,'assembled_template':assembled}
        if a.negative_controls:
            negatives=[]
            for title in ('outside_symbol','seed_immediate','JAL_target','NOP_fill','ELF_size',
                          'misaligned_frame','RA_slot','false_return','unrestored_SP','callee_saved_write'):
                bad=bytearray(modified)
                if title=='outside_symbol':bad[start-1]^=1
                elif title=='seed_immediate':bad[start+8]^=1
                elif title=='JAL_target':bad[start+16]^=1
                elif title=='NOP_fill':bad[start+40]^=1
                elif title=='ELF_size':bad.append(0)
                elif title=='misaligned_frame':bad[start]^=1
                elif title=='RA_slot':bad[start+4]^=8
                elif title=='false_return':bad[start+28]^=1
                elif title=='unrestored_SP':bad[start+36]^=16
                else:bad[start+30]^=18
                try:verify_patch(executable.data,bad,start,init['size'],payload)
                except SystemExit as error:negatives.append({'mutation':title,'rejected':True,'reason':str(error),
                    'actual_mutated_ELF_byte_buffer_sha256':hashlib.sha256(bad).hexdigest(),'mutated_bytes':len(bad)})
                else:raise SystemExit('Patch auditor accepted mutation '+title)
            row['negative_controls']=negatives
        result['variants']['%08x'%seed]=row
    if inputs!={kind:sha(path) for kind,path in paths.items()}:raise SystemExit('Original shipping inputs changed')
    (out/('audit-report.json' if a.audit_only else 'patch-report.json')).write_text(json.dumps(result,indent=2)+'\n')
    for key,row in result['variants'].items():print(key,row['ELF'],row['ELF_sha256'])


if __name__=='__main__':main()
