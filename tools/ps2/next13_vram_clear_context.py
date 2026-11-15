#!/usr/bin/env python3
"""Compile the compact clear in the complete actual accepted O3-hwd graph.

Uses one explicitly granted serial EE worker. Replaces exactly one of205 actual
IR inputs, preserves the existing per-unit -O3 and complete LTO option bytes,
links normally, and proves exact allocated-byte equality of retained-RELA links.
No production source is changed and no emulator is launched.
"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys

from audio_storage_next7_finalbind import Object
from next10_sw_bounds_context import archive_generated
from next12_pb_scratch_context import compare_lto_diagnostics, run
from next12_pb_scratch_finalbind import exact_source, exact_build, FINAL_ELF, sha
from next11_sw_linker_relas import verify
from next7_render_fog_codecheck import Elf32
from next13_vram_clear_hosttest import COMPACT_CANDIDATE, section, REVISION

ROOT=Path(__file__).resolve().parents[2]
WORK=ROOT/'build/nov15-next13/vram-clear/stages'
GS='src/ps2/hw/ps2_hw_gs.inc'


def source_pins(source):
    return {p.relative_to(source).as_posix():sha(p) for p in sorted((source/'src').rglob('*')) if p.is_file()}


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build',type=Path,default=ROOT/'build/nov15-next12/final/hardware')
    ap.add_argument('--baseline',type=Path,default=ROOT/'build/nov15-next12/final/source')
    ap.add_argument('--candidate',type=Path,default=WORK/'compact-v2/candidate')
    ap.add_argument('--out',type=Path,required=True)
    a=ap.parse_args();build,base,source,out=(p.resolve() for p in (a.build,a.baseline,a.candidate,a.out))
    if out.exists() or not out.is_relative_to(WORK):raise SystemExit('Fresh isolated clear stage required')
    committed=exact_source(base);published=exact_source(base,REVISION)
    if committed!=published:raise SystemExit('Published and actual accepted602 source differ')
    baseline_pins={p:v['sha256'] for p,v in committed.items()};candidate_pins=source_pins(source)
    if len(candidate_pins)!=602 or set(candidate_pins)!=set(baseline_pins):raise SystemExit('Complete602 candidate required')
    changed=[p for p in baseline_pins if baseline_pins[p]!=candidate_pins[p]]
    before=(base/GS).read_text();old=section(before,'static void gs_fill_all(','// ---- bring-up ----')
    if changed!=[GS] or (source/GS).read_text()!=before.replace(old,COMPACT_CANDIDATE,1):
        raise SystemExit('Candidate is not exactly compact-v2 clear function')
    admission=exact_build(build,base,FINAL_ELF)
    out.mkdir(parents=True);(out/'obj').mkdir();(out/'pinned-tools').mkdir()
    for name in ('next13_vram_clear_context.py','next13_vram_clear_hosttest.py','next12_pb_scratch_context.py',
                 'next12_pb_scratch_finalbind.py','next10_sw_bounds_context.py','next11_sw_linker_relas.py',
                 'next7_render_fog_codecheck.py','next10_sw_bounds_codecheck.py','audio_storage_next7_finalbind.py'):
        shutil.copy2(ROOT/'tools/ps2'/name,out/'pinned-tools'/name)
    overlay=out/'snapshot';shutil.copytree(source/'src',overlay/'src')
    (overlay/'tools/ps2').mkdir(parents=True)
    units=overlay/'tools/ps2/opt_units.txt'
    units.write_bytes(subprocess.check_output(['git','show',REVISION+':tools/ps2/opt_units.txt'],cwd=ROOT))
    if source_pins(overlay)!=candidate_pins:raise SystemExit('Compile overlay differs')
    original=json.loads((build/'build-report.json').read_text())
    external={p:h for p,h in original['input_sha256'].items() if not Path(p).is_relative_to(ROOT/'src')}
    generated=archive_generated(out,external,[build/'gen/config.h'])
    generated_paths={r['original_path']:r['archive_path'] for r in generated['details']['entries']}
    generated_dirs={str(Path(old).parent):str(Path(new).parent) for old,new in generated_paths.items()}
    for p,h in external.items():
        if p not in generated_paths and sha(Path(p))!=h:raise SystemExit('Actual external input differs: '+p)
    flags=[f for f in original['flags'] if f not in ('-MMD','-MP')]
    prefix='-I'+str(ROOT/'src')
    flags=['-I'+str(overlay/'src')+f[len(prefix):] if f.startswith(prefix) else f for f in flags]
    flags=['-I'+generated_dirs.get(f[2:],f[2:]) if f.startswith('-I') else f for f in flags]
    if not {'-O2','-G8','-flto','-Werror','-DPS2','-DPS2_PROFILE','-DHWRENDER'}<=set(flags):
        raise SystemExit('Full actual default flags required')
    unit='src/ps2/hw/ps2_hwd.c';unit_flags={}
    for line in units.read_text().splitlines():
        parts=line.split('#',1)[0].split()
        if len(parts)>1:unit_flags[parts[0]]=parts[1:]
    if unit_flags.get(unit)!=['-O3']:raise SystemExit('Existing default driver -O3 required')
    compiler=original['link_command'][0];objects=[];canonical_ir=admission['complete_actual_IR_objects']
    for name,digest in canonical_ir.items():
        old=Path(name);new=out/'obj'/old.name;shutil.copy2(old,new)
        if sha(new)!=digest:raise SystemExit('Copied actual IR differs')
        objects.append(new)
    replacement=out/'obj/src__ps2__hw__ps2_hwd.o'
    command=[compiler,*flags,*unit_flags[unit],'-MMD','-MF',out/'ps2_hwd.d','-c',overlay/unit,'-o',replacement]
    print('Compiling actual O3 hwd unit',flush=True)
    stdout,stderr,compile_seconds=run(command,out/'compile.log',180)
    if stdout or stderr:raise SystemExit('Actual clear unit emitted diagnostics')
    def options(path):
        obj=Object(path);names=[s for s in obj.sections if s.startswith('.gnu.lto_.opts')]
        if len(names)!=1:raise SystemExit('One .gnu.lto_.opts section required')
        return obj.content(obj.sections[names[0]])
    before_opts=options(build/'obj'/replacement.name);after_opts=options(replacement)
    if before_opts!=after_opts or b'-O3' not in before_opts:raise SystemExit('Actual canonical O3 LTO option bytes differ')
    option_report={'baseline_IR':str(build/'obj'/replacement.name),'candidate_IR':str(replacement),
        'all_option_section_bytes_identical':True,'option_bytes_hex':before_opts.hex(),
        'decoded_options':before_opts.decode(errors='replace'),'actual_default_unit_flags':unit_flags[unit],
        'opt_units_sha256':sha(units)}
    (out/'actual-unit-options.json').write_text(json.dumps(option_report,indent=2)+'\n')
    engine=out/'obj/engine_lto.o'
    lto=[compiler,'-r','-nostdlib','-flto=1','-flinker-output=nolto-rel','-O2','-ffunction-sections','-fdata-sections',
         *[f for f in flags if f.startswith('-G') or f=='-mno-abicalls'],*objects,'-o',engine]
    print('Merging all205 actual IR inputs serially',flush=True)
    stdout,stderr,lto_seconds=run(lto,out/'lto.log',600)
    diag=compare_lto_diagnostics(build/'lto.log',stdout,stderr,canonical_ir,objects,original,out)
    if not diag['all_normalized_diagnostics_identical']:raise SystemExit('Complete actual LTO diagnostics differ')
    link=list(original['link_command']);sites=[i for i,p in enumerate(link) if p.endswith('/engine_lto.o')]
    if len(sites)!=1:raise SystemExit('One exact engine input required')
    link[sites[0]]=str(engine);link[link.index('-o')+1]=str(out/'SRB2.ELF')
    maps=[i for i,p in enumerate(link) if p.startswith('-Wl,-Map=')]
    if len(maps)!=1:raise SystemExit('One exact map output required')
    link[maps[0]]='-Wl,-Map='+str(out/'SRB2.map')
    print('Linking complete actual clear candidate',flush=True)
    stdout,stderr,link_seconds=run(link,out/'link.log',180)
    if stdout or stderr or (build/'link.log').read_text():raise SystemExit('Normal link diagnostics differ')
    actual_inputs={}
    for name,digest in original['input_sha256'].items():
        p=Path(name);actual=overlay/p.relative_to(ROOT) if p.is_relative_to(ROOT/'src') else Path(generated_paths.get(name,name))
        actual_inputs[str(actual)]=sha(actual)
        expected=candidate_pins[str(p.relative_to(ROOT))] if p.is_relative_to(ROOT/'src') else digest
        if actual_inputs[str(actual)]!=expected:raise SystemExit('Actual compile dependency differs')
    changed_ir=[Path(old).name for old,new in zip(canonical_ir,objects) if canonical_ir[old]!=sha(new)]
    if changed_ir!=[replacement.name]:raise SystemExit('Exactly one changed actual IR input required')
    context={'status':'Complete actual O3-hwd clear candidate; native validation pending','source_revision':REVISION,
        'baseline_origin':admission,'complete_baseline_source_sha256':baseline_pins,'complete_source_overlay_sha256':candidate_pins,
        'project_source_differences':changed,'canonical_IR_objects':canonical_ir,'complete_actual_IR_objects':{str(p):sha(p) for p in objects},
        'changed_actual_IR_objects':changed_ir,'all_other204_complete_IR_inputs_identical':True,'generated_input_archive':generated,
        'actual_unit_options_sha256':sha(out/'actual-unit-options.json'),'preserved_default_unit_flags':unit_flags[unit],
        'compile_command':list(map(str,command)),'lto_command':list(map(str,lto)),'link_command':link,
        'compile_seconds':compile_seconds,'lto_seconds':lto_seconds,'link_seconds':link_seconds,'lto_diagnostics':diag,
        'engine_sha256':sha(engine),'ELF_sha256':sha(out/'SRB2.ELF'),
        'pinned_tools_sha256':{p.name:sha(p) for p in (out/'pinned-tools').iterdir()}}
    report=dict(original);report.update(flags=flags,compiled=1,link_command=link,input_sha256=actual_inputs,
        elf_sha256=context['ELF_sha256'],elf_bytes=(out/'SRB2.ELF').stat().st_size,clear_context_control=context)
    (out/'build-report.json').write_text(json.dumps(report,indent=2)+'\n')
    for name,actual_build in (('candidate',out),('baseline',build)):
        print('Retaining exact '+name+' whole-link RELAs',flush=True)
        relas=out/(name+'-linker-relas')
        result=subprocess.run([sys.executable,'-B',str(ROOT/'tools/ps2/next11_sw_linker_relas.py'),
            '--build',str(actual_build),'--out',str(relas)],cwd=ROOT,capture_output=True,text=True)
        (out/(name+'-linker-relas-driver.log')).write_text(result.stdout+result.stderr)
        if result.returncode:raise SystemExit('Exact retained-RELA link failed: '+name)
        verify(Elf32(actual_build/'SRB2.ELF'),Elf32(relas/'LINKER-RELAS.ELF'))
        context[name+'_linker_relas_report_sha256']=sha(relas/'linker-rela-report.json')
    if exact_build(build,base,FINAL_ELF)!=admission or source_pins(overlay)!=candidate_pins:
        raise SystemExit('Frozen actual graph/source changed during build')
    context['status']='PASS complete actual O3-hwd205-input normal and exact retained-RELA links; native validation pending'
    (out/'context-report.json').write_text(json.dumps(context,indent=2)+'\n')
    print(json.dumps({'status':context['status'],'out':str(out),'ELF_sha256':context['ELF_sha256'],
                     'changed_IR':changed_ir,'compile_seconds':compile_seconds,'lto_seconds':lto_seconds,
                     'link_seconds':link_seconds},indent=2),flush=True)


if __name__=='__main__':main()
