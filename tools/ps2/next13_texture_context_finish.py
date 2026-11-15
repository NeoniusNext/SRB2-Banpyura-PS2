#!/usr/bin/env python3
"""Link the completed immutable LTO attempt after recording its exact partition count."""
import argparse
import json
from pathlib import Path
import re
import shlex
import shutil

from next12_lighttable_context import run, sha
from next13_texture_variant import ROOT, PATH, rewrite
from next11_sw_linker_relas import verify
from next7_render_fog_codecheck import Elf32


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--build',type=Path,default=ROOT/'build/nov15-next12/final/hardware')
    p.add_argument('--snapshot',type=Path,default=ROOT/'build/nov15-next12/final/source')
    p.add_argument('--candidate',type=Path,default=ROOT/'build/nov15-next13/texture-first/measured-source')
    a=p.parse_args();out=a.out.resolve();build=a.build.resolve();snapshot=a.snapshot.resolve();candidate=a.candidate.resolve()
    if (out/'SRB2.ELF').exists() or (out/'build-report.json').exists():raise SystemExit('Completed context is immutable')
    original=json.loads((build/'build-report.json').read_text())
    raw=(out/'canonical-lto.log').read_text();actual=(out/'lto-stderr.txt').read_text()
    counts=re.findall(r'using serial compilation of (\d+) LTRANS jobs',raw)
    other=re.findall(r'using serial compilation of (\d+) LTRANS jobs',actual)
    if len(counts)!=1 or len(other)!=1:raise SystemExit('One explicit actual partition count in each raw diagnostic required')
    diag=json.loads((out/'lto-diagnostics.json').read_text())
    expected=raw.replace('using serial compilation of '+counts[0]+' LTRANS jobs','using serial compilation of '+other[0]+' LTRANS jobs')
    for entry in diag['byte_identical_input_paths']:
        expected=expected.replace(entry['canonical_path'],entry['copied_path'])
    if expected!=actual or (out/'lto-stdout.txt').read_text():raise SystemExit('A diagnostic changed beyond recorded partition count and identical input spelling')
    commands={name:shlex.split((out/name).read_text().splitlines()[0]) for name in ('ps2_hwd-compile.log','lto.log')}
    compile_command=commands['ps2_hwd-compile.log'];lto=commands['lto.log']
    if compile_command[-2]!='-o' or Path(compile_command[-1]).name!='src__ps2__hw__ps2_hwd.o':raise SystemExit('Exact changed unit required')
    if (out/'ps2_hwd-compile.log').read_text().splitlines()[1:]:raise SystemExit('Actual changed-unit compiler diagnostics')
    flags=compile_command[1:compile_command.index('-MMD')]
    objects=[Path(s) for s in lto if s.endswith('.o') and s!=str(out/'obj/engine_lto.o')]
    if len(objects)!=205:raise SystemExit('Full203-C plus2-VU graph required')
    canonical_ir={};changed=[]
    for obj in objects:
        old=build/'obj'/obj.name;canonical_ir[str(old)]=sha(old)
        if sha(old)!=sha(obj):changed.append(obj.name)
    if changed!=['src__ps2__hw__ps2_hwd.o']:raise SystemExit('Only changed hwd IR permitted '+str(changed))
    source_manifest=json.loads((snapshot/'manifest.json').read_text())
    before={str(p.relative_to(snapshot)):sha(p) for p in sorted((snapshot/'src').rglob('*')) if p.is_file()}
    after={str(p.relative_to(candidate)):sha(p) for p in sorted((candidate/'src').rglob('*')) if p.is_file()}
    if len(before)!=602 or before!=source_manifest['source_sha256'] or set(before)!=set(after):raise SystemExit('Full source freeze differs')
    differences=[name for name in before if before[name]!=after[name]]
    if differences!=['src/'+str(PATH)] or (candidate/'src'/PATH).read_text()!=rewrite((snapshot/'src'/PATH).read_text()):raise SystemExit('Only score selection source permitted')
    generated=json.loads((out/'generated-input-archive.json').read_text());generated_paths={e['original_path']:e['archive_path'] for e in generated['entries']}
    actual_inputs={}
    for name,digest in original['input_sha256'].items():
        path=Path(name)
        if path.is_relative_to(ROOT/'src'):
            old=snapshot/'src'/path.relative_to(ROOT/'src');actual=candidate/'src'/path.relative_to(ROOT/'src')
            if sha(old)!=digest:raise SystemExit('Baseline frozen dependency differs '+name)
        else:
            actual=Path(generated_paths.get(name,name))
            if sha(actual)!=digest:raise SystemExit('Pinned generated/SDK dependency differs '+name)
        actual_inputs[str(actual)]=sha(actual)
    frozen={str(p):sha(p) for p in (build/'build-report.json',build/'obj/engine_lto.o',build/'SRB2.ELF',build/'SRB2.map',out/'obj/engine_lto.o')}
    if sha(build/'SRB2.ELF')!=original['elf_sha256']:raise SystemExit('Actual canonical ELF/report mismatch')
    link=list(original['link_command']);positions=[i for i,s in enumerate(link) if s.endswith('/engine_lto.o')]
    if len(positions)!=1:raise SystemExit('One exact merged-engine input required')
    link[positions[0]]=str(out/'obj/engine_lto.o');link[link.index('-o')+1]=str(out/'SRB2.ELF')
    positions=[i for i,s in enumerate(link) if s.startswith('-Wl,-Map=')]
    if len(positions)!=1:raise SystemExit('One exact final map required')
    link[positions[0]]='-Wl,-Map='+str(out/'SRB2.map')
    stdout,stderr,seconds=run(link,out/'link.log',120)
    diagnostics='\n'.join(s for s in (stdout+stderr).splitlines() if 'linking abicalls files with non-abicalls files' not in s).strip()
    if diagnostics:raise SystemExit('New final link diagnostic')
    companion=out/'linker-relas';companion.mkdir()
    emitted=list(link);emitted[emitted.index('-o')+1]=str(companion/'LINKER-RELAS.ELF')
    emitted[positions[0]]='-Wl,-Map='+str(companion/'LINKER-RELAS.map');emitted.append('-Wl,--emit-relocs')
    stdout,stderr,reloc_seconds=run(emitted,companion/'link.log',120)
    diagnostics='\n'.join(s for s in (stdout+stderr).splitlines() if 'linking abicalls files with non-abicalls files' not in s).strip()
    if diagnostics:raise SystemExit('New retained-RELA link diagnostic')
    checked=verify(Elf32(out/'SRB2.ELF'),Elf32(companion/'LINKER-RELAS.ELF'))
    checked.update(original_ELF=str(out/'SRB2.ELF'),original_ELF_sha256=sha(out/'SRB2.ELF'),
       original_engine=str(out/'obj/engine_lto.o'),original_engine_sha256=sha(out/'obj/engine_lto.o'),
       original_map=str(out/'SRB2.map'),original_map_sha256=sha(out/'SRB2.map'),
       diagnostic_ELF_sha256=sha(companion/'LINKER-RELAS.ELF'),exact_link_command=emitted)
    (companion/'linker-rela-report.json').write_text(json.dumps(checked,indent=2)+'\n')
    for name,digest in {**frozen,**canonical_ir,**actual_inputs}.items():
        if sha(Path(name))!=digest:raise SystemExit('Actual immutable input changed '+name)
    shutil.copyfile(Path(__file__),out/'pinned-tools'/Path(__file__).name)
    context={'canonical_build':str(build),'canonical_artifacts':frozen,'canonical_IR_objects':canonical_ir,
      'complete_actual_IR_objects':{str(p):sha(p) for p in objects},'only_recompiled_units':['src/ps2/hw/ps2_hwd.c'],
      'project_source_differences':differences,'complete_source_overlay_sha256':after,'generated_input_archive':generated,
      'compile_command':compile_command,'lto_command':lto,'link_command':link,'emit_relocs_command':emitted,
      'all_other204_complete_IR_inputs_identical':True,'all_actual_compiler_diagnostics_except_explicit_partition_count_identical':True,
      'LTRANS_partition_count':{'baseline':int(counts[0]),'candidate':int(other[0]),'full_unfiltered_raw_streams_preserved':True},
      'source_revision':source_manifest['revision'],'source_manifest_sha256':sha(snapshot/'manifest.json'),
      'pinned_tools_sha256':{p.name:sha(p) for p in (out/'pinned-tools').iterdir()},
      'link_seconds':seconds,'reloc_link_seconds':reloc_seconds,'engine_sha256':sha(out/'obj/engine_lto.o'),'ELF_sha256':sha(out/'SRB2.ELF')}
    report=dict(original);report.update(flags=flags,compiled=1,link_command=link,input_sha256=actual_inputs,
       elf_sha256=context['ELF_sha256'],elf_bytes=(out/'SRB2.ELF').stat().st_size,texture_context_control=context)
    (out/'context-report.json').write_text(json.dumps(context,indent=2)+'\n')
    (out/'build-report.json').write_text(json.dumps(report,indent=2)+'\n')
    print('Linked complete actual default candidate; exact LTRANS count difference recorded.')


if __name__=='__main__':main()
