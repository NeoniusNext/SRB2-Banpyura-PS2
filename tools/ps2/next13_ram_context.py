#!/usr/bin/env python3
"""Build the implicit drawnode candidate in the accepted 205-input Hardware LTO graph.

Consumes one explicitly granted serial EE worker. Only hw_main.c is
recompiled; all other actual default-build IR inputs retain their exact bytes.
No emulator is invoked and no production source is edited.
"""
import argparse
import json
from pathlib import Path
import shutil

from next10_sw_bounds_context import archive_generated
from next12_pb_scratch_context import run, compare_lto_diagnostics
from next12_pb_scratch_finalbind import exact_build, exact_source, FINAL_ELF, sha
import next13_ram_variant as variant

ROOT=variant.ROOT
WORK=ROOT/'build/nov15-next13'


def validate(source,candidate=False):
    manifest=json.loads((variant.OUT/'source-manifest.json').read_text())
    key='candidate_source_sha256' if candidate else 'source_sha256'
    actual={p.relative_to(source).as_posix():sha(p) for p in (source/'src').rglob('*') if p.is_file()}
    if actual!=manifest[key]:raise RuntimeError('Exact602 implicit drawnode source differs')
    return actual


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build',type=Path,default=ROOT/'build/nov15-next12/final/hardware')
    p.add_argument('--baseline',type=Path,default=variant.BASE)
    p.add_argument('--candidate',type=Path,default=variant.OUT)
    p.add_argument('--optout',action='store_true')
    p.add_argument('--out',type=Path,default=WORK/'ram-context-v1')
    a=p.parse_args();build,base,source,out=(x.resolve() for x in (a.build,a.baseline,a.candidate,a.out))
    baseline_pins=validate(base);candidate_pins=validate(source,True)
    committed=exact_source(base)
    admission=exact_build(build,base,FINAL_ELF)
    if {name:row['sha256'] for name,row in committed.items()}!=baseline_pins:
        raise RuntimeError('Actual tested-source admission differs')
    # The published revision adds evidence only; bind all 602 source blobs too.
    published=exact_source(base,'353cc94103aeb38cef87d26151c779302135d0e9')
    if published!=committed:raise RuntimeError('Published and tested complete source differ')
    if out.exists() or not out.is_relative_to(WORK) or out==WORK:
        raise RuntimeError('Use a fresh isolated next13 context output')
    out.mkdir(parents=True);(out/'obj').mkdir();(out/'pinned-tools').mkdir()
    for name in ('next13_ram_context.py','next13_ram_variant.py','next12_pb_scratch_context.py',
                 'next12_pb_scratch_finalbind.py','next10_sw_bounds_context.py'):
        shutil.copyfile(ROOT/'tools/ps2'/name,out/'pinned-tools'/name)
    original=json.loads((build/'build-report.json').read_text())
    overlay=out/'snapshot';shutil.copytree(source/'src',overlay/'src')
    overlay_pins={p.relative_to(overlay).as_posix():sha(p) for p in (overlay/'src').rglob('*') if p.is_file()}
    if overlay_pins!=candidate_pins:raise RuntimeError('Actual compile overlay differs')
    inputs={name:digest for name,digest in original['input_sha256'].items() if not Path(name).is_relative_to(ROOT/'src')}
    generated=archive_generated(out,inputs,[build/'gen/config.h'])
    generated_paths={entry['original_path']:entry['archive_path'] for entry in generated['details']['entries']}
    generated_dirs={str(Path(old).parent):str(Path(new).parent) for old,new in generated_paths.items()}
    flags=[flag for flag in original['flags'] if flag not in ('-MMD','-MP')]
    prefix='-I'+str(ROOT/'src')
    flags=['-I'+str(overlay/'src')+flag[len(prefix):] if flag.startswith(prefix) else flag for flag in flags]
    flags=['-I'+generated_dirs.get(flag[2:],flag[2:]) if flag.startswith('-I') else flag for flag in flags]
    if not {'-O2','-G8','-flto','-Werror','-DPS2','-DPS2_PROFILE','-DHWRENDER'}<=set(flags):
        raise RuntimeError('Complete actual default Hardware flags required')
    if a.optout:flags+=['-DPS2_NOOPT_IMPLICIT_DN']
    compiler=original['link_command'][0];objects=[];canonical_ir={}
    for name,digest in admission['complete_actual_IR_objects'].items():
        old=Path(name);new=out/'obj'/old.name;shutil.copyfile(old,new)
        if sha(new)!=digest:raise RuntimeError('Copied actual IR differs')
        canonical_ir[name]=digest;objects.append(new)
    replacement=out/'obj/src__hardware__hw_main.o'
    compile_command=[compiler,*flags,'-MMD','-MF',out/'hw_main.d','-c',overlay/'src/hardware/hw_main.c','-o',replacement]
    stdout,stderr,compile_seconds=run(compile_command,out/'compile.log',120)
    if stdout or stderr:raise RuntimeError('Candidate compile emitted diagnostics')
    engine=out/'obj/engine_lto.o'
    lto_command=[compiler,'-r','-nostdlib','-flto=1','-flinker-output=nolto-rel','-O2','-ffunction-sections','-fdata-sections',
                 *[f for f in flags if f.startswith('-G') or f=='-mno-abicalls'],*objects,'-o',engine]
    stdout,stderr,lto_seconds=run(lto_command,out/'lto.log',600)
    diagnostics=compare_lto_diagnostics(build/'lto.log',stdout,stderr,canonical_ir,objects,original,out)
    if not diagnostics['all_normalized_diagnostics_identical']:
        raise RuntimeError('Actual complete LTO diagnostics differ; inspect raw streams')
    link_command=list(original['link_command'])
    sites=[i for i,arg in enumerate(link_command) if arg.endswith('/engine_lto.o')]
    if len(sites)!=1:raise RuntimeError('One exact engine link input required')
    link_command[sites[0]]=str(engine);link_command[link_command.index('-o')+1]=str(out/'SRB2.ELF')
    sites=[i for i,arg in enumerate(link_command) if arg.startswith('-Wl,-Map=')]
    if len(sites)!=1:raise RuntimeError('One exact linker map required')
    link_command[sites[0]]='-Wl,-Map='+str(out/'SRB2.map')
    stdout,stderr,link_seconds=run(link_command,out/'link.log',120)
    if (build/'link.log').read_text() or stdout or stderr:raise RuntimeError('Candidate or accepted link diagnostics differ')
    validate(base);validate(source,True)
    if exact_build(build,base,FINAL_ELF)!=admission:raise RuntimeError('Accepted actual context changed during build')
    if {p.relative_to(overlay).as_posix():sha(p) for p in (overlay/'src').rglob('*') if p.is_file()}!=candidate_pins:
        raise RuntimeError('Actual compile overlay changed')
    changed=[Path(old).name for old,new in zip(canonical_ir,objects) if canonical_ir[old]!=sha(new)]
    if changed!=['src__hardware__hw_main.o']:raise RuntimeError('Another complete IR input differs')
    actual_inputs={}
    for name,digest in original['input_sha256'].items():
        path=Path(name)
        actual=overlay/path.relative_to(ROOT) if path.is_relative_to(ROOT/'src') else Path(generated_paths.get(name,name))
        actual_inputs[str(actual)]=sha(actual)
        if actual_inputs[str(actual)]!=candidate_pins.get(str(path.relative_to(ROOT)),digest) if path.is_relative_to(ROOT/'src') else actual_inputs[str(actual)]!=digest:
            raise RuntimeError('Actual build dependency differs: '+str(actual))
    context={'status':'COMPLETE actual default205-input Hardware candidate; target analysis pending',
        'source_revision':'d4fbc7c319f19edc70eda220c3323d0350504c98','published_revision':'353cc94103aeb38cef87d26151c779302135d0e9',
        'complete_published_and_tested_source_identical':True,'narrow_optout':a.optout,'baseline_origin':admission,
        'complete_baseline_source_sha256':baseline_pins,'complete_source_overlay_sha256':candidate_pins,
        'canonical_IR_objects':canonical_ir,'complete_actual_IR_objects':{str(p):sha(p) for p in objects},
        'changed_actual_IR_objects':changed,'all_other_complete_IR_inputs_identical':True,
        'only_recompiled_unit':'src/hardware/hw_main.c','generated_input_archive':generated,
        'compile_command':list(map(str,compile_command)),'lto_command':list(map(str,lto_command)),
        'link_command':link_command,'compile_seconds':compile_seconds,'lto_seconds':lto_seconds,'link_seconds':link_seconds,
        'lto_diagnostics':diagnostics,'engine_sha256':sha(engine),'ELF_sha256':sha(out/'SRB2.ELF'),
        'pinned_tools':{p.name:sha(p) for p in (out/'pinned-tools').iterdir()}}
    report=dict(original);report.update(flags=flags,compiled=1,link_command=link_command,input_sha256=actual_inputs,
        elf_sha256=context['ELF_sha256'],elf_bytes=(out/'SRB2.ELF').stat().st_size,ram_context_control=context)
    (out/'context-report.json').write_text(json.dumps(context,indent=2)+'\n')
    (out/'build-report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'status':context['status'],'out':str(out),'ELF_sha256':context['ELF_sha256'],
                     'changed_actual_IR_objects':changed,'compile_seconds':compile_seconds,'lto_seconds':lto_seconds,'link_seconds':link_seconds},indent=2))


if __name__=='__main__':main()
