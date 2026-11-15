#!/usr/bin/env python3
"""Run the granted clean canonical Hardware build and exact RELA companion.

Uses the official build.py with --jobs 1 and no source/config/flag overrides.
Requires a granted EE worker. No emulator, Git write or source mutation.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

from audio_storage_next7_finalbind import Object
from next12_pb_scratch_objectcheck import pdr
from next13_flats_context import lto_options
from next11_sw_linker_relas import allocated
from next7_render_fog_codecheck import Elf32
from next13_combined_context import linked_symbols
from next13_geometry_combined_bind import ROOT,WORK,SELECTED,sources

REVISION='5481bbbdc5aa3167c046836f3e51e4dbb31ad807'


def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def git(*args):return subprocess.check_output(['git',*args],cwd=ROOT,text=True).strip()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=Path,default=WORK/'final/hardware')
    p.add_argument('--source',type=Path,default=WORK/'final/source')
    p.add_argument('--admission',type=Path,default=WORK/'final-source-commit-v1/report.json')
    a=p.parse_args();out,source,admission_path=(x.resolve() for x in (a.out,a.source,a.admission))
    parent=out.parent;freeze_dir=parent/'hardware-canonical-inputs-v1'
    if out.exists() or freeze_dir.exists() or not out.is_relative_to(WORK):raise RuntimeError('Fresh canonical Hardware output and input snapshot required')
    admission=json.loads(admission_path.read_text());manifest=json.loads((source/'manifest.json').read_text())
    expected=manifest['source_sha256']
    if len(expected)!=602 or sources(ROOT)!=expected or sources(source)!=expected:
        raise RuntimeError('Actual/snapshotted complete canonical602 source differs')
    if admission['source_sha256']!=expected or admission['canonical_source_commit']['revision']!=REVISION or manifest['revision']!=REVISION:
        raise RuntimeError('Canonical committed source admission differs')
    if git('rev-parse','HEAD')!=REVISION or git('status','--porcelain','--','src'):
        raise RuntimeError('Canonical current HEAD/source cleanliness differs')
    if os.environ.get('SRB2_PS2_HW')!='1' or Path(os.environ.get('SRB2_PS2_OUT','')).resolve()!=out:
        raise RuntimeError('Use only the requested Hardware mode/output environment')
    overrides={k:v for k,v in os.environ.items() if k.startswith('SRB2_PS2_') and k not in ('SRB2_PS2_HW','SRB2_PS2_OUT')}
    if overrides:raise RuntimeError('Unexpected experiment build environment: '+str(overrides))
    baseline=ROOT/'build/nov15-next12/final/hardware';original=json.loads((baseline/'build-report.json').read_text())
    toolnames=('build.py','sources.txt','sources_hw.txt','opt_units.txt','next13_final_hardware_build.py','next11_sw_linker_relas.py',
        'next13_flats_context.py','next12_pb_scratch_objectcheck.py','audio_storage_next7_finalbind.py')
    toolpins={name:sha(ROOT/'tools/ps2'/name) for name in toolnames}
    for name,row in admission['official_build_tools_unchanged'].items():
        if toolpins[name]!=row['sha256']:raise RuntimeError('Official default build tool changed')
    freeze_dir.mkdir(parents=True);(freeze_dir/'tools').mkdir()
    for name in toolnames:shutil.copyfile(ROOT/'tools/ps2'/name,freeze_dir/'tools'/name)
    info={'revision':REVISION,'short_revision':git('rev-parse','--short','HEAD'),
          'branch':git('rev-parse','--abbrev-ref','HEAD'),'subject':git('log','-1','--format=%s')}
    config=(ROOT/'src/config.h.in').read_text()
    for old,new in {'${SRB2_COMP_REVISION}':info['short_revision'],'${SRB2_COMP_BRANCH}':info['branch'],
                    '${SRB2_COMP_NOTE}':info['subject'].replace('"',"'"),'${CMAKE_BUILD_TYPE}':'Release'}.items():config=config.replace(old,new)
    config=config.replace('#cmakedefine SRB2_COMP_UNCOMMITTED','/* clean */').replace('#cmakedefine01 SRB2_COMP_OPTIMIZED','#define SRB2_COMP_OPTIMIZED 1')
    (freeze_dir/'expected-actual-commit-config.h').write_text(config)
    command=[sys.executable,'-B','tools/ps2/build.py','--jobs','1']
    freeze={'status':'before official clean full Hardware build','canonical_source_admission':str(admission_path),
        'canonical_source_admission_sha256':sha(admission_path),'source_manifest_sha256':sha(source/'manifest.json'),
        'all602_actual_source_sha256':expected,'actual_source_commit':info,'official_build_tools_sha256':toolpins,
        'actual_requested_environment':{'SRB2_PS2_HW':'1','SRB2_PS2_OUT':str(out)},
        'setup_script_sha256':sha('/workspace/.onboarding/environment.sh'),'official_command':command,
        'compiler_path':original['link_command'][0],'compiler_sha256':sha(original['link_command'][0]),
        'expected_actual_commit_config_sha256':sha(freeze_dir/'expected-actual-commit-config.h'),
        'fresh_output_did_not_exist':True,'no_feature_optout_tuning_profiler_or_config_override':True}
    (freeze_dir/'input-freeze.json').write_text(json.dumps(freeze,indent=2)+'\n')
    start=time.monotonic();result=subprocess.run(command,cwd=ROOT,capture_output=True,text=True)
    orchestration=parent/'hardware-orchestration.log';orchestration.write_text(result.stdout+result.stderr)
    print(result.stdout+result.stderr,flush=True)
    if result.returncode:raise RuntimeError('Official canonical Hardware build failed; preserve complete orchestration')
    seconds=time.monotonic()-start
    build_report_path=out/'build-report.json';built=json.loads(build_report_path.read_text())
    if built['compiled']!=203 or len(built['sources'])!=203 or built['sources']!=original['sources'] or not built['hardware'] or built['diagnostic_build']:
        raise RuntimeError('Complete fresh canonical203-C Hardware compilation required')
    if (out/'gen/config.h').read_text()!=config:raise RuntimeError('Actual generated config differs from admitted source-commit metadata')
    if sources(ROOT)!=expected or sources(source)!=expected or git('rev-parse','HEAD')!=REVISION:
        raise RuntimeError('Canonical source/HEAD changed during official compilation')
    for name,digest in toolpins.items():
        if sha(ROOT/'tools/ps2'/name)!=digest:raise RuntimeError('Official tool changed during build')
    if sha(out/'SRB2.ELF')!=built['elf_sha256']:raise RuntimeError('Official ELF/report pin differs')
    paths=[out/'obj'/unit.replace('/','__').replace('.c','.o') for unit in built['sources']]
    paths += [out/'obj'/(name+'_vsm.o') for name in ('ps2_hw_vu1','ps2_hw_vu1_spr')]
    if len(paths)!=205 or not all(path.is_file() for path in paths):raise RuntimeError('Exactly205 complete actual target objects required')
    options={}
    for unit in built['sources']:
        name=unit.replace('/','__').replace('.c','.o');before=lto_options(baseline/'obj'/name);after=lto_options(out/'obj'/name)
        if before!=after:raise RuntimeError('Canonical full unit LTO option bytes differ: '+unit)
        options[unit]={'baseline_object':str(baseline/'obj'/name),'canonical_object':str(out/'obj'/name),
            'whole_option_bytes_identical':True,'complete_option_bytes_hex':after.hex(),'option_sha256':hashlib.sha256(after).hexdigest()}
    (out/'actual-all-unit-options.json').write_text(json.dumps(options,indent=2)+'\n')
    input_checks={}
    old_external={n:d for n,d in original['input_sha256'].items() if not Path(n).is_relative_to(ROOT/'src') and not (Path(n).name=='config.h' and Path(n).parent.name=='gen')}
    for name,digest in built['input_sha256'].items():
        path=Path(name)
        if sha(path)!=digest:raise RuntimeError('An actual canonical dependency changed')
        if path.is_relative_to(ROOT/'src'):
            if expected[path.relative_to(ROOT).as_posix()]!=digest:raise RuntimeError('Actual source dependency differs from602 source admission')
            kind='exact committed source'
        elif path==out/'gen/config.h':kind='actual new commit generated config'
        else:
            if old_external.get(name)!=digest:raise RuntimeError('Canonical external SDK dependency differs from accepted baseline')
            kind='exact accepted external SDK input'
        input_checks[name]={'sha256':digest,'kind':kind}
    if sha(build_report_path)!=sha(out/'build-report.json'):raise RuntimeError('Official report changed')
    # Keep this granted worker through the companion link, using the unchanged
    # official retained-RELA tool; no --emit-relocs byte/address inference.
    relay=[sys.executable,'-B',str(ROOT/'tools/ps2/next11_sw_linker_relas.py'),'--build',str(out),'--out',str(out/'linker-relas')]
    linked=subprocess.run(relay,cwd=ROOT,capture_output=True,text=True)
    (parent/'hardware-linker-relas-orchestration.log').write_text(linked.stdout+linked.stderr)
    print(linked.stdout+linked.stderr,flush=True)
    if linked.returncode:raise RuntimeError('Canonical retained-RELA companion failed')
    sections={'baseline':allocated(Elf32(baseline/'SRB2.ELF')),'canonical':allocated(Elf32(out/'SRB2.ELF'))}
    symbols={'baseline':linked_symbols(baseline/'SRB2.ELF'),'canonical':linked_symbols(out/'SRB2.ELF')}
    procedures={'baseline':pdr(Object(baseline/'obj/engine_lto.o')),'canonical':pdr(Object(out/'obj/engine_lto.o'))}
    changes={name:{tag:procedures[tag]['records'].get(name) for tag in procedures}
        for name in sorted(set(procedures['baseline']['records'])|set(procedures['canonical']['records']))
        if procedures['baseline']['records'].get(name,{}).get('raw_words')!=procedures['canonical']['records'].get(name,{}).get('raw_words')}
    for tag,data in procedures.items():(out/(tag+'-complete-pdr.json')).write_text(json.dumps(data,indent=2)+'\n')
    end_delta=symbols['canonical']['_end']['address']-symbols['baseline']['_end']['address']
    footprint={'allocated_sections':sections,'symbols':symbols,'actual_end_delta_bytes':end_delta,
        'actual_text_delta_bytes':sections['canonical']['.text']['bytes']-sections['baseline']['.text']['bytes'],
        'all_frame_differences':changes,'baseline_frame_records':len(procedures['baseline']['records']),
        'canonical_frame_records':len(procedures['canonical']['records']),
        'actual_new_commit_metadata_config_sha256':sha(out/'gen/config.h'),'static_end_nonincreasing':end_delta<=0}
    (out/'static-footprint.json').write_text(json.dumps(footprint,indent=2)+'\n')
    canonical={'status':'PASS clean canonical Hardware build/static admission; runtime/binding gates pending' if end_delta<=0 else 'REJECTED canonical static end growth; preserve actual build and stop',
        'source_commit':REVISION,'all602_source_sha256':expected,'source_manifest_sha256':sha(source/'manifest.json'),
        'source_admission_sha256':sha(admission_path),'actual_source_commit_metadata':info,'config_sha256':sha(out/'gen/config.h'),
        'official_command':command,'official_build_report_sha256':sha(build_report_path),'orchestration_sha256':sha(orchestration),
        'all203_C_freshly_compiled':True,'complete_actual205_IR_objects':{str(p):sha(p) for p in paths},
        'all203_whole_LTO_option_sections_match_original_default':True,'actual_all_unit_options_sha256':sha(out/'actual-all-unit-options.json'),
        'actual_complete_input_dependency_checks':input_checks,'baseline_build':str(baseline),'elapsed_build_seconds':seconds,
        'static_footprint_sha256':sha(out/'static-footprint.json'),'actual_end_delta_bytes':end_delta,
        'retained_RELA_report_sha256':sha(out/'linker-relas/linker-rela-report.json'),
        'ELF_sha256':sha(out/'SRB2.ELF'),'engine_sha256':sha(out/'obj/engine_lto.o'),'no_emulator_invoked':True,
        'all_source_and_official_build_tools_unchanged_during_build':True}
    (out/'canonical-build-admission.json').write_text(json.dumps(canonical,indent=2)+'\n')
    print(json.dumps({'status':canonical['status'],'ELF_sha256':canonical['ELF_sha256'],'compiled_C':built['compiled'],
        'actual_IR_inputs':len(paths),'whole_default_unit_options_equal':len(options),'actual_end_delta_bytes':end_delta,
        'actual_text_delta_bytes':footprint['actual_text_delta_bytes'],'frame_differences':len(changes),
        'build_seconds':seconds,'source_commit':REVISION},indent=2),flush=True)
    if end_delta>0:raise SystemExit(2)


if __name__=='__main__':main()
