#!/usr/bin/env python3
"""Read-only canonical Software189-unit/input/default-option admission."""
import hashlib,json
from pathlib import Path
from next13_flats_context import lto_options
ROOT=Path(__file__).resolve().parents[2]
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def main():
    old=ROOT/'build/nov15-next12/final/software';new=ROOT/'build/nov15-next13/final/software'
    a=json.loads((old/'build-report.json').read_text());b=json.loads((new/'build-report.json').read_text())
    assert a['sources']==b['sources'] and len(b['sources'])==b['compiled']==189 and not b['hardware'] and not b['diagnostic_build']
    assert [s.replace(str(old),str(new)) for s in a['flags']]==b['flags']
    assert [s.replace(str(old),str(new)) for s in a['link_command']]==b['link_command']
    assert a['compiler']==b['compiler'] and sha(new/'SRB2.ELF')==b['elf_sha256']
    assert (new/'gen/config.h').read_bytes()==(ROOT/'build/nov15-next13/final/hardware/gen/config.h').read_bytes()
    source=json.loads((ROOT/'build/nov15-next13/final/source/manifest.json').read_text());assert len(source['source_sha256'])==602
    inputs={};changed=[]
    for n,d in b['input_sha256'].items():
        assert sha(n)==d
        before=n.replace(str(new),str(old));x=a['input_sha256'].get(before)
        if x!=d:
            assert n==str(new/'gen/config.h');changed.append({'baseline_path':before,'baseline_sha256':x,'canonical_path':n,'canonical_sha256':d})
        inputs[n]={'sha256':d,'baseline_sha256':x,'complete_bytes_equal':x==d}
    assert len(inputs)==466 and len(changed)==1 and len(a['input_sha256'])==len(inputs)
    units={};ir_diffs=[]
    for unit in b['sources']:
        n=unit.replace('/','__').replace('.c','.o');p,q=old/'obj'/n,new/'obj'/n
        x,y=lto_options(p),lto_options(q);assert x==y
        units[unit]={'actual_IR':str(q),'actual_IR_sha256':sha(q),'baseline_IR_sha256':sha(p),'complete_LTO_option_bytes_hex':y.hex(),'option_sha256':hashlib.sha256(y).hexdigest(),'whole_option_bytes_identical':True}
        if sha(p)!=sha(q):ir_diffs.append(unit)
    logs={}
    for n in ['build.log','lto.log','link.log']:
        x,y=(old/n).read_text(),(new/n).read_text()
        assert x==y if n!='link.log' else x.replace(str(old/'obj/engine_lto.o'),str(new/'obj/engine_lto.o'))==y
        logs[n]={'baseline_sha256':sha(old/n),'canonical_sha256':sha(new/n),'complete_raw_bytes_identical':x==y,'baseline_raw':x,'canonical_raw':y,'only_explicit_authoritative_engine_path_binding':n=='link.log'}
    assert (new/'build.log').read_text().count(' warning: ')==3 and b['compile_diagnostics']==2 and not b['link_diagnostics']
    out=ROOT/'build/nov15-next13/final/software-input-admission-v2';out.mkdir(parents=True,exist_ok=False)
    report={'status':'PASS read-only canonical Software default189C,466actualinputs,fulloptions/rawdiagnostics admission; native outcomes separate',
        'source_commit':source['revision'],'source_manifest_sha256':sha(ROOT/'build/nov15-next13/final/source/manifest.json'),
        'actual_ELF_sha256':sha(new/'SRB2.ELF'),'actual_engine_sha256':sha(new/'obj/engine_lto.o'),'official_build_report_sha256':sha(new/'build-report.json'),
        'orchestration_sha256':sha(new.parent/'software-orchestration.log'),'all189_actual_IR_and_whole_LTO_options':units,
        'complete_IR_units_with_different_bytes':ir_diffs,'complete466_actual_input_checks':inputs,'only_actual_input_bytes_different':changed,
        'generated_config_scope':'Actual local committed5481 metadata config exactly equals canonical Hardware config; complete config bytes deliberately not claimed equal to accepted d4 baseline.',
        'all_raw_build_LTO_link_diagnostics':logs,'warnings_scope':'Exactly3 preexisting source warnings in2 unchanged source units; complete raw build and59-LTRANS warning/note logs exactly baseline. Raw linker retains the existing ABICALL warning with only authoritative engine-path substitution; both full raw texts retained, no warning text normalized away.',
        'default_flags_and_link_order_equal_after_only_explicit_build_directory_projection':True,'no_compiler_linker_emulator_invoked_here':True,'tool_sha256':sha(__file__)}
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n');print(str(out/'report.json'),sha(out/'report.json'),len(ir_diffs))
if __name__=='__main__':main()
