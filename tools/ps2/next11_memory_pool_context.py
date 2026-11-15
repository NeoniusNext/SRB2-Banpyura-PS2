"""Build one checked slope-pool IR replacement in a complete canonical graph.

Consumes one granted EE compiler worker, serial LTO; never runs an emulator.
Source/header and generated-input snapshots are immutable for the whole build.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import time

from next10_sw_bounds_context import archive_generated
from next11_sw_checkplane_context import compare_lto_diagnostics

ROOT=Path(__file__).resolve().parents[2]


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command,log,timeout):
    command=list(map(str,command));start=time.monotonic()
    result=subprocess.run(command,cwd=ROOT,capture_output=True,text=True,timeout=timeout)
    log.write_text(' '.join(command)+'\n'+result.stdout+result.stderr)
    if result.returncode:raise SystemExit('Context build failed '+str(log))
    return result.stdout,result.stderr,time.monotonic()-start


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build',type=Path,required=True)
    parser.add_argument('--snapshot',type=Path,required=True,help='Frozen canonical tree containing src/')
    parser.add_argument('--out',type=Path,required=True)
    group=parser.add_mutually_exclusive_group()
    group.add_argument('--production-source',type=Path,help='Only p_slopes.c pool overlay; all headers stay frozen')
    group.add_argument('--optout',action='store_true',help='Use the snapshot plus the narrow scan opt-out')
    args=parser.parse_args();build=args.build.resolve();snapshot=args.snapshot.resolve();out=args.out.resolve()
    if out.exists() or not out.is_relative_to(ROOT/'build'):
        raise SystemExit('A fresh context destination under build/ is required')
    out.mkdir(parents=True);(out/'obj').mkdir()
    report_path=build/'build-report.json';original=json.loads(report_path.read_text())
    canonical_engine=build/'obj/engine_lto.o';canonical_elf=build/'SRB2.ELF'
    if sha(canonical_elf)!=original['elf_sha256']:
        raise SystemExit('Canonical exact ELF/report mismatch')
    canonical_hashes={str(path):sha(path) for path in (canonical_engine,canonical_elf,report_path,build/'SRB2.map')}
    inputs={};frozen={}
    for name,digest in original['input_sha256'].items():
        path=Path(name)
        if path.is_relative_to(ROOT/'src'):
            copied=snapshot/'src'/path.relative_to(ROOT/'src')
            if sha(copied)!=digest:raise SystemExit('Canonical source snapshot differs '+name)
            frozen[str(copied)]=digest
        else:inputs[name]=digest
    overlay=out/'snapshot'
    shutil.copytree(snapshot/'src',overlay/'src')
    if args.production_source:
        shutil.copyfile(args.production_source.resolve(),overlay/'src/p_slopes.c')
    complete_overlay={str(path.relative_to(overlay)):sha(path) for path in sorted((overlay/'src').rglob('*')) if path.is_file()}
    project_differences=[]
    for relative,digest in complete_overlay.items():
        if sha(snapshot/relative)!=digest:project_differences.append(relative)
    if project_differences!=(['src/p_slopes.c'] if args.production_source else []):
        raise SystemExit('Unexpected complete source overlay changes '+str(project_differences))
    generated=archive_generated(out,inputs,[build/'gen/config.h'])
    generated_paths={entry['original_path']:entry['archive_path'] for entry in generated['details']['entries']}
    for name,digest in inputs.items():
        if name not in generated_paths and sha(Path(name))!=digest:
            raise SystemExit('Canonical external dependency changed '+name)
    flags=[f for f in original['flags'] if f not in ('-MMD','-MP')]
    prefix='-I'+str(ROOT/'src')
    flags=['-I'+str(overlay/'src')+f[len(prefix):] if f.startswith(prefix) else f for f in flags]
    generated_directories={str(Path(old).parent):str(Path(new).parent) for old,new in generated_paths.items()}
    flags=['-I'+generated_directories.get(f[2:],f[2:]) if f.startswith('-I') else f for f in flags]
    if '-flto' not in flags:raise SystemExit('Expected complete default-LTO graph')
    flags+=['-Werror']+(['-DPS2_NOOPT_SLOPE_POOL_BYTES'] if args.optout else [])
    compiler=original['link_command'][0]
    objects=[];canonical_ir={}
    for source in original['sources']:
        name=source.replace('/','__').replace('.c','.o');old=build/'obj'/name;new=out/'obj'/name
        canonical_ir[str(old)]=sha(old);shutil.copyfile(old,new);objects.append(new)
    replacement=out/'obj/src__p_slopes.o'
    compile_command=[compiler,*flags,'-MMD','-MF',out/'p_slopes.d','-c',overlay/'src/p_slopes.c','-o',replacement]
    stdout,stderr,compile_seconds=run(compile_command,out/'compile.log',120)
    if stdout or stderr:raise SystemExit('Context compile diagnostics differ')
    if original['hardware']:
        for name in ('ps2_hw_vu1','ps2_hw_vu1_spr'):
            old=build/'obj'/(name+'_vsm.o');new=out/'obj'/old.name
            canonical_ir[str(old)]=sha(old);shutil.copyfile(old,new);objects.append(new)
    engine=out/'obj/engine_lto.o'
    lto_command=[compiler,'-r','-nostdlib','-flto=1','-flinker-output=nolto-rel','-O2',
                 '-ffunction-sections','-fdata-sections',*[f for f in flags if f.startswith('-G') or f=='-mno-abicalls'],
                 *objects,'-o',engine]
    stdout,stderr,lto_seconds=run(lto_command,out/'lto.log',600)
    lto_diagnostics=compare_lto_diagnostics(build/'lto.log',stdout,stderr,canonical_ir,objects,original,out)
    if not lto_diagnostics['all_normalized_diagnostics_identical']:
        raise SystemExit('Context complete LTO diagnostics differ from canonical')
    command=list(original['link_command']);sites=[i for i,a in enumerate(command) if a.endswith('/engine_lto.o')]
    if len(sites)!=1:raise SystemExit('One exact canonical engine link input required')
    command[sites[0]]=str(engine);command[command.index('-o')+1]=str(out/'SRB2.ELF')
    sites=[i for i,a in enumerate(command) if a.startswith('-Wl,-Map=')]
    if len(sites)!=1:raise SystemExit('One exact map output required')
    command[sites[0]]='-Wl,-Map='+str(out/'SRB2.map')
    stdout,stderr,link_seconds=run(command,out/'link.log',120)
    diagnostics='\n'.join(line for line in (stdout+stderr).splitlines()
                          if 'linking abicalls files with non-abicalls files' not in line).strip()
    if diagnostics:raise SystemExit('Context final link has new diagnostics')
    for name,digest in {**canonical_hashes,**canonical_ir,**frozen}.items():
        if sha(Path(name))!=digest:raise SystemExit('Canonical input changed during context build '+name)
    if complete_overlay!={str(path.relative_to(overlay)):sha(path) for path in sorted((overlay/'src').rglob('*')) if path.is_file()}:
        raise SystemExit('Actual immutable source overlay changed')
    for entry in generated['details']['entries']:
        if sha(Path(entry['archive_path']))!=entry['original_pinned_sha256']:
            raise SystemExit('Archived actual generated input changed')
    actual_inputs={}
    for name,digest in original['input_sha256'].items():
        path=Path(name)
        actual=str(overlay/'src'/path.relative_to(ROOT/'src')) if path.is_relative_to(ROOT/'src') else generated_paths.get(name,name)
        actual_inputs[actual]=sha(Path(actual))
    context={'canonical_build':str(build),'canonical_artifacts':canonical_hashes,'canonical_IR_objects':canonical_ir,
             'complete_actual_IR_objects':{str(path):sha(path) for path in objects},
             'only_recompiled_unit':'src/p_slopes.c','project_source_differences':project_differences,
             'narrow_optout':args.optout,'complete_source_overlay_sha256':complete_overlay,
             'generated_input_archive':generated,'compile_command':list(map(str,compile_command)),
             'lto_command':list(map(str,lto_command)),'link_command':command,
             'compile_seconds':compile_seconds,'lto_seconds':lto_seconds,'link_seconds':link_seconds,
             'all_other_complete_IR_inputs_identical':True,'all_normalized_LTO_diagnostics_identical':True,
             'lto_diagnostics':lto_diagnostics,'lto_diagnostics_report_sha256':sha(out/'lto-diagnostics.json'),
             'engine_sha256':sha(engine),'ELF_sha256':sha(out/'SRB2.ELF')}
    report=dict(original);report.update(flags=flags,compiled=1,link_command=command,input_sha256=actual_inputs,
        elf_sha256=context['ELF_sha256'],elf_bytes=(out/'SRB2.ELF').stat().st_size,slope_pool_context_control=context)
    (out/'build-report.json').write_text(json.dumps(report,indent=2)+'\n')
    (out/'context-report.json').write_text(json.dumps(context,indent=2)+'\n')
    print('Built complete actual one-unit context '+str(out),flush=True)


if __name__=='__main__':main()
