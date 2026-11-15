#!/usr/bin/env python3
"""One changed IR unit, complete actual default203-C+2-VU LTO/link context."""
import argparse
import json
from pathlib import Path
import shutil

from next10_sw_bounds_context import archive_generated
from next12_lighttable_context import compare_lto_diagnostics, run, sha
from next13_texture_variant import ROOT, PATH, rewrite
from audio_storage_next7_finalbind import Object


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build',type=Path,default=ROOT/'build/nov15-next12/final/hardware')
    p.add_argument('--snapshot',type=Path,default=ROOT/'build/nov15-next12/final/source')
    p.add_argument('--candidate',type=Path,default=ROOT/'build/nov15-next13/texture-first/measured-source')
    p.add_argument('--out',type=Path,required=True)
    a=p.parse_args();build=a.build.resolve();snapshot=a.snapshot.resolve();candidate=a.candidate.resolve();out=a.out.resolve()
    if out.exists():raise SystemExit('Fresh actual-context destination required')
    out.mkdir(parents=True);(out/'obj').mkdir()
    pinned=out/'pinned-tools';pinned.mkdir()
    for name in ('next13_texture_context.py','next12_lighttable_context.py','next10_sw_bounds_context.py','next13_texture_variant.py'):
        shutil.copyfile(ROOT/'tools/ps2'/name,pinned/name)
    manifest=json.loads((snapshot/'manifest.json').read_text())
    sources={str(p.relative_to(snapshot)):sha(p) for p in sorted((snapshot/'src').rglob('*')) if p.is_file()}
    overlay={str(p.relative_to(candidate)):sha(p) for p in sorted((candidate/'src').rglob('*')) if p.is_file()}
    if len(sources)!=602 or sources!=manifest['source_sha256'] or set(sources)!=set(overlay):
        raise SystemExit('Complete actual602 snapshot mismatch')
    differences=[name for name in sources if sources[name]!=overlay[name]]
    if differences!=['src/'+str(PATH)] or (candidate/'src'/PATH).read_text()!=rewrite((snapshot/'src'/PATH).read_text()):
        raise SystemExit('Not exactly isolated dec_level selection')
    report_path=build/'build-report.json';original=json.loads(report_path.read_text())
    if not original['hardware'] or len(original['sources'])!=203:raise SystemExit('Complete actual default Hardware required')
    frozen={str(p):sha(p) for p in (report_path,build/'obj/engine_lto.o',build/'SRB2.ELF',build/'SRB2.map')}
    if frozen[str(build/'SRB2.ELF')]!=original['elf_sha256']:raise SystemExit('Actual baseline ELF/report mismatch')
    inputs={}
    for name,digest in original['input_sha256'].items():
        path=Path(name)
        if path.is_relative_to(ROOT/'src'):
            expected=snapshot/'src'/path.relative_to(ROOT/'src')
            if sha(expected)!=digest:raise SystemExit('Frozen actual source input mismatch '+name)
            frozen[str(expected)]=digest
        else:inputs[name]=digest
    generated=archive_generated(out,inputs,[build/'gen/config.h'])
    generated_paths={entry['original_path']:entry['archive_path'] for entry in generated['details']['entries']}
    for name,digest in inputs.items():
        if name not in generated_paths and sha(Path(name))!=digest:raise SystemExit('Actual external dependency changed '+name)
    flags=[f for f in original['flags'] if f not in ('-MMD','-MP')]
    prefix='-I'+str(ROOT/'src')
    flags=['-I'+str(candidate/'src')+f[len(prefix):] if f.startswith(prefix) else f for f in flags]
    directories={str(Path(old).parent):str(Path(new).parent) for old,new in generated_paths.items()}
    flags=['-I'+directories.get(f[2:],f[2:]) if f.startswith('-I') else f for f in flags]
    if '-flto' not in flags or '-O2' not in flags or '-G8' not in flags:raise SystemExit('Actual default flags required')
    compiler=original['link_command'][0];objects=[];canonical_ir={}
    for unit in original['sources']:
        name=unit.replace('/','__').replace('.c','.o');old=build/'obj'/name;new=out/'obj'/name
        canonical_ir[str(old)]=sha(old);shutil.copyfile(old,new);objects.append(new)
    unit='src/ps2/hw/ps2_hwd.c';replacement=out/'obj'/unit.replace('/','__').replace('.c','.o')
    units=candidate/'tools/ps2/opt_units.txt'
    unit_flags={}
    for line in units.read_text().splitlines():
        parts=line.split('#',1)[0].split()
        if len(parts)>1:unit_flags[parts[0]]=parts[1:]
    if unit_flags.get(unit)!=['-O3']:raise SystemExit('Exact existing default driver unit flags required')
    command=[compiler,*flags,*unit_flags[unit],'-MMD','-MF',out/'ps2_hwd.d','-c',candidate/unit,'-o',replacement]
    stdout,stderr,compile_seconds=run(command,out/'ps2_hwd-compile.log',120)
    if stdout or stderr:raise SystemExit('Changed unit produced compile diagnostics')
    def options(path):
        obj=Object(path);sections=[s for s in obj.sections if s.startswith('.gnu.lto_.opts')]
        if len(sections)!=1:raise SystemExit('One actual LTO input option section required')
        return obj.content(obj.sections[sections[0]])
    before_options=options(build/'obj'/replacement.name);after_options=options(replacement)
    if before_options!=after_options:raise SystemExit('Actual complete changed-unit LTO options differ from canonical IR')
    (out/'actual-unit-options.json').write_text(json.dumps({'baseline_IR':str(build/'obj'/replacement.name),
        'candidate_IR':str(replacement),'all_original_option_section_bytes_identical':True,
        'option_bytes_hex':before_options.hex(),'opt_units_path':str(units),'opt_units_sha256':sha(units),
        'actual_default_unit_flags':unit_flags[unit]},indent=2)+'\n')
    for name in ('ps2_hw_vu1','ps2_hw_vu1_spr'):
        old=build/'obj'/(name+'_vsm.o');new=out/'obj'/old.name
        canonical_ir[str(old)]=sha(old);shutil.copyfile(old,new);objects.append(new)
    engine=out/'obj/engine_lto.o'
    lto=[compiler,'-r','-nostdlib','-flto=1','-flinker-output=nolto-rel','-O2','-ffunction-sections','-fdata-sections',
         *[f for f in flags if f.startswith('-G') or f=='-mno-abicalls'],*objects,'-o',engine]
    stdout,stderr,lto_seconds=run(lto,out/'lto.log',600)
    diag=compare_lto_diagnostics(build/'lto.log',stdout,stderr,canonical_ir,objects,original,out)
    if not diag['all_normalized_diagnostics_identical']:raise SystemExit('Complete actual LTO diagnostics changed')
    link=list(original['link_command']);sites=[i for i,s in enumerate(link) if s.endswith('/engine_lto.o')]
    if len(sites)!=1:raise SystemExit('Exactly one actual merged engine input required')
    link[sites[0]]=str(engine);link[link.index('-o')+1]=str(out/'SRB2.ELF')
    sites=[i for i,s in enumerate(link) if s.startswith('-Wl,-Map=')]
    if len(sites)!=1:raise SystemExit('Exactly one actual link map required')
    link[sites[0]]='-Wl,-Map='+str(out/'SRB2.map')
    stdout,stderr,link_seconds=run(link,out/'link.log',120)
    diagnostics='\n'.join(line for line in (stdout+stderr).splitlines() if 'linking abicalls files with non-abicalls files' not in line).strip()
    if diagnostics:raise SystemExit('Actual final link has new diagnostics')
    emitted=list(link);emitted.insert(1,'-Wl,--emit-relocs');emitted[emitted.index('-o')+1]=str(out/'LINKER-RELAS.ELF')
    emitted[sites[0]+1]='-Wl,-Map='+str(out/'LINKER-RELAS.map')
    stdout,stderr,reloc_seconds=run(emitted,out/'linker-relas.log',120)
    diagnostics='\n'.join(line for line in (stdout+stderr).splitlines() if 'linking abicalls files with non-abicalls files' not in line).strip()
    if diagnostics:raise SystemExit('Actual emit-relocs link has new diagnostics')
    for name,digest in {**frozen,**canonical_ir}.items():
        if sha(Path(name))!=digest:raise SystemExit('Immutable canonical input changed '+name)
    if overlay!={str(p.relative_to(candidate)):sha(p) for p in sorted((candidate/'src').rglob('*')) if p.is_file()}:
        raise SystemExit('Actual isolated source changed during build')
    actual_inputs={}
    for name in original['input_sha256']:
        path=Path(name)
        actual=str(candidate/'src'/path.relative_to(ROOT/'src')) if path.is_relative_to(ROOT/'src') else generated_paths.get(name,name)
        actual_inputs[actual]=sha(Path(actual))
    context={'canonical_build':str(build),'canonical_artifacts':frozen,'canonical_IR_objects':canonical_ir,
      'complete_actual_IR_objects':{str(path):sha(path) for path in objects},'only_recompiled_units':[unit],
      'project_source_differences':differences,'complete_source_overlay_sha256':overlay,'generated_input_archive':generated,
      'compile_command':list(map(str,command)),'lto_command':list(map(str,lto)),'link_command':link,'emit_relocs_command':emitted,
      'compile_seconds':compile_seconds,'lto_seconds':lto_seconds,'link_seconds':link_seconds,'reloc_link_seconds':reloc_seconds,
      'actual_unit_options_sha256':sha(out/'actual-unit-options.json'),'preserved_default_unit_flags':unit_flags[unit],
      'all_other204_complete_IR_inputs_identical':True,'all_normalized_LTO_diagnostics_identical':True,
      'source_revision':manifest['revision'],'source_manifest_sha256':sha(snapshot/'manifest.json'),
      'pinned_tools_sha256':{p.name:sha(p) for p in pinned.iterdir()},'lto_diagnostics':diag,
      'engine_sha256':sha(engine),'ELF_sha256':sha(out/'SRB2.ELF'),'emit_relocs_ELF_sha256':sha(out/'LINKER-RELAS.ELF')}
    report=dict(original);report.update(flags=flags,compiled=1,link_command=link,input_sha256=actual_inputs,
      elf_sha256=context['ELF_sha256'],elf_bytes=(out/'SRB2.ELF').stat().st_size,texture_context_control=context)
    (out/'build-report.json').write_text(json.dumps(report,indent=2)+'\n')
    (out/'context-report.json').write_text(json.dumps(context,indent=2)+'\n')
    print('Built complete actual one-unit dec_level context '+str(out),flush=True)


if __name__=='__main__':main()
