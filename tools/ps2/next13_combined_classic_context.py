#!/usr/bin/env python3
"""Replace driver IR with the classic filler in the frozen all-up probe.

One granted serial EE worker; no emulator or production edit. Three variant IR
objects are reused only after every recorded actual dependency is checked
against the new source/config bytes. The other 199 C and two VU inputs retain
their accepted bytes with their own dependency/source checks. This remains an
isolated static exposure result, not shipping validation.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import shutil

from next13_combined_context import ROOT,WORK,REVISION,PUBLISHED,OVERLAYS,UNITS,copy_pin,sources,linked_symbols
from next10_sw_bounds_context import archive_generated
from next12_pb_scratch_context import run
from next12_pb_scratch_finalbind import exact_build,exact_source,FINAL_ELF,sha
from next13_flats_context import check_diagnostics,lto_options
from next11_sw_linker_relas import verify,allocated
from next7_render_fog_codecheck import Elf32
from audio_storage_next7_finalbind import Object
from next12_pb_scratch_objectcheck import pdr

PRIMARY='src/ps2/hw/ps2_hwd.c'
CACHE=PRIMARY
OVERLAY='src/ps2/hw/ps2_hw_tex.inc'
CUT_ROOT=WORK/'texture-fill3-classic/measured-source'
CUT_SHA='091e790814cf00002765b633f0bf12aa6d875880eefec59685f6ba5fb02401ea'


def dependencies(path):
    # Canonical build uses -MP. Read only the complete first logical target
    # rule, after joining its continuations, rather than the later phony rules.
    line=path.read_text().replace('\\\n',' ').splitlines()[0]
    target,body=line.split(':',1)
    return target.strip(),sorted(set(Path(word).resolve() for word in shlex.split(body)))


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build',type=Path,default=ROOT/'build/nov15-next12/final/hardware')
    p.add_argument('--baseline',type=Path,default=ROOT/'build/nov15-next12/final/source')
    p.add_argument('--previous',type=Path,default=WORK/'combined-v2')
    p.add_argument('--out',type=Path,default=WORK/'combined-classic-v1')
    a=p.parse_args();build,base,previous,out=(x.resolve() for x in (a.build,a.baseline,a.previous,a.out))
    if out.exists() or not out.is_relative_to(WORK):raise RuntimeError('Fresh isolated all-up destination required')
    baseline={n:r['sha256'] for n,r in exact_source(base).items()}
    if baseline!={n:r['sha256'] for n,r in exact_source(base,PUBLISHED).items()}:raise RuntimeError('Tested/published source differs')
    admission=exact_build(build,base,FINAL_ELF);original=json.loads((build/'build-report.json').read_text())
    old=json.loads((previous/'context-report.json').read_text());old_freeze=json.loads((previous/'input-freeze.json').read_text())
    if sha(previous/'input-freeze.json')!=old['input_freeze_sha256'] or sha(previous/'SRB2.ELF')!=old['ELF_sha256']:
        raise RuntimeError('Original combined-v2 evidence differs')
    if old['baseline_origin']!=admission or sources(previous/'snapshot')!=old['complete_source_overlay_sha256']:
        raise RuntimeError('Original actual all-up source/baseline differs')
    for path,digest in old['complete_actual_IR_objects'].items():
        if sha(Path(path))!=digest:raise RuntimeError('Original combined-v2 actual IR changed')
    root_before=sources(ROOT)
    if root_before!=baseline:raise RuntimeError('Production source must remain the accepted baseline')
    cut=sources(CUT_ROOT);manifest=json.loads((CUT_ROOT.parent/'candidate-source-freeze.json').read_text())
    if cut!=manifest['source_sha256'] or manifest['source_file_count']!=602 or cut[OVERLAY]!=CUT_SHA:
        raise RuntimeError('New complete classic texture source freeze differs')
    if sorted(n for n in baseline if cut[n]!=baseline[n])!=[OVERLAY]:raise RuntimeError('Classic texture source changes another path')
    if old['complete_source_overlay_sha256']['src/hardware/hw_cache.c']!='869c84455d07bbf55af24d0f0fc48914809c5261e68c2a72d8baebce9551a857':
        raise RuntimeError('Stable flat-cut v2 source required')
    combined=dict(old['complete_source_overlay_sha256']);combined[OVERLAY]=CUT_SHA
    if sorted(n for n in baseline if baseline[n]!=combined[n])!=sorted(OVERLAYS):raise RuntimeError('Expected exactly four baseline overlays')
    out.mkdir(parents=True);(out/'obj').mkdir();snapshot=out/'snapshot';shutil.copytree(previous/'snapshot/src',snapshot/'src')
    shutil.copyfile(CUT_ROOT/OVERLAY,snapshot/OVERLAY)
    if sources(snapshot)!=combined:raise RuntimeError('New all-up602 snapshot differs')
    (snapshot/'manifest.json').write_text(json.dumps({'revision':REVISION,'published_revision':PUBLISHED,
        'source_sha256':combined,'changed':sorted(OVERLAYS),'stage':'classic filler v3 all-up exposure only','shipping_result':False},indent=2)+'\n')
    external={n:d for n,d in original['input_sha256'].items() if not Path(n).is_relative_to(ROOT/'src')}
    generated=archive_generated(out,external,[build/'gen/config.h'])
    gen_paths={r['original_path']:r['archive_path'] for r in generated['details']['entries']}
    provenance={previous:old}
    cursor=previous
    while True:
        previous_freeze=json.loads((cursor/'input-freeze.json').read_text())
        if 'previous_combined_context' not in previous_freeze:break
        parent=Path(previous_freeze['previous_combined_context']).resolve()
        if parent in provenance:raise RuntimeError('Cyclic IR provenance')
        parent_report=json.loads((parent/'context-report.json').read_text())
        if sha(parent/'context-report.json')!=previous_freeze['previous_context_sha256']:
            raise RuntimeError('Recorded parent IR provenance differs')
        if sources(parent/'snapshot')!=parent_report['complete_source_overlay_sha256']:
            raise RuntimeError('Parent compilation snapshot differs')
        provenance[parent]=parent_report;cursor=parent
    gen_old_to_new={}
    for info in provenance.values():
        for row in info['generated_input_archive']['details']['entries']:
            gen_old_to_new[str(Path(row['archive_path']).resolve())]=Path(gen_paths[row['original_path']]).resolve()
    external_pins={}
    for name,digest in external.items():
        actual=Path(gen_paths.get(name,name))
        if sha(actual)!=digest:raise RuntimeError('Existing external/config bytes differ')
        external_pins[name]=copy_pin(actual,out/'frozen-external-inputs')
    external_expected={str(Path(n).resolve()):d for n,d in external.items()}
    for info in provenance.values():
        external_expected.update({str(Path(r['archive_path']).resolve()):r['archive_sha256'] for r in info['generated_input_archive']['details']['entries']})
    external_expected.update({str(Path(r['archive_path']).resolve()):r['archive_sha256'] for r in generated['details']['entries']})
    def mapped_dependency(path):
        for origin,info in provenance.items():
            if path.is_relative_to(origin/'snapshot/src'):
                relative=path.relative_to(origin/'snapshot').as_posix();expected=info['complete_source_overlay_sha256'][relative]
                return snapshot/relative,expected
        if path.is_relative_to(ROOT/'src'):
            relative=path.relative_to(ROOT).as_posix();return snapshot/relative,baseline[relative]
        if path.is_relative_to(base/'src'):
            relative=path.relative_to(base).as_posix();return snapshot/relative,baseline[relative]
        if str(path) in gen_old_to_new:return gen_old_to_new[str(path)],external_expected[str(path)]
        if str(path) in gen_paths:return Path(gen_paths[str(path)]),external_expected[str(path)]
        if str(path) not in external_expected:raise RuntimeError('Actual dependency was absent from canonical external pins: '+str(path))
        return path,external_expected[str(path)]
    variant_units=set(UNITS)-{CACHE};dependency_checks={}
    for unit in original['sources']:
        if unit==CACHE:continue
        origin=previous/(Path(unit).stem+'.d') if unit in variant_units else build/'obj'/unit.replace('/','__').replace('.c','.d')
        if not origin.is_file():raise RuntimeError('Missing actual per-unit dependency file: '+str(origin))
        target,paths=dependencies(origin);rows=[]
        for path in paths:
            new,expected=mapped_dependency(path)
            if sha(path)!=expected or sha(new)!=expected:raise RuntimeError('Reused IR actual dependency differs in the new complete source/config: '+str(path))
            rows.append({'actual_previous_path':str(path),'new_source_or_config_path':str(new),
                'sha256':expected,'both_file_contents_byte_identical':True})
        dependency_checks[unit]={'origin':'combined-v2 variant IR (actual original compile paths retained)' if unit in variant_units else 'accepted baseline IR',
            'actual_dependency_file':str(origin),'actual_dependency_file_sha256':sha(origin),'original_object_target':target,
            'actual_dependency_count':len(rows),'complete_dependency_rows':rows}
        # Preserve a correct dependency record for the reused physical IR:
        # it deliberately retains the actual old compilation paths.
        if unit in variant_units:shutil.copyfile(origin,out/(Path(unit).stem+'.d'))
    vu_checks={}
    for name in ('ps2_hw_vu1','ps2_hw_vu1_spr'):
        relative='src/ps2/hw/vu1/'+name+'.vsm';digest=baseline[relative]
        if sha(base/relative)!=digest or sha(snapshot/relative)!=digest:raise RuntimeError('Reused VU source bytes differ')
        vu_checks[name]={'canonical_source':str(base/relative),'new_source':str(snapshot/relative),'sha256':digest,
            'actual_VU_IR_byte_identity_checked_in_205_input_list':True}
    rules=previous/'snapshot/tools/ps2/opt_units.txt';rules_target=snapshot/'tools/ps2/opt_units.txt'
    rules_target.parent.mkdir(parents=True);shutil.copyfile(rules,rules_target)
    if sha(rules)!=sha(ROOT/'tools/ps2/opt_units.txt'):raise RuntimeError('Existing unit policy differs')
    objects=[];canonical_ir=admission['complete_actual_IR_objects'];old_by_name={Path(n).name:(n,d) for n,d in old['complete_actual_IR_objects'].items()}
    input_origins={}
    for canonical,digest in canonical_ir.items():
        name=Path(canonical).name;prior,prior_digest=old_by_name[name];new=out/'obj'/name
        if name!=CACHE.replace('/','__').replace('.c','.o') and name not in {u.replace('/','__').replace('.c','.o') for u in variant_units} and prior_digest!=digest:
            raise RuntimeError('An unchanged one of201 IR inputs differs from accepted baseline')
        shutil.copyfile(prior,new)
        if sha(new)!=prior_digest:raise RuntimeError('Copied original205 graph differs')
        objects.append(new);input_origins[str(new)]={'canonical_IR':canonical,'canonical_sha256':digest,
            'actual_copied_IR':prior,'actual_copied_sha256':prior_digest,'freshly_compiled_in_this_stage':False}
    # Existing per-unit options are checked for all four, including the three
    # reused physical objects; driver retains its actual -O3 input exactly.
    options={}
    for unit in UNITS:
        name=unit.replace('/','__').replace('.c','.o');before=lto_options(build/'obj'/name);copied=lto_options(out/'obj'/name)
        if copied!=before:raise RuntimeError('Reused whole LTO options differ: '+unit)
        options[unit]={'actual_existing_override':['-O3'] if unit.endswith('ps2_hwd.c') else [],
            'complete_option_bytes_hex':before.hex(),'whole_options_equal_canonical':True}
    tools={name:copy_pin(ROOT/'tools/ps2'/name,out/'pinned-tools') for name in ('next13_combined_classic_context.py',
        'next13_combined_context.py','next13_flats_context.py','next12_pb_scratch_context.py','next12_pb_scratch_finalbind.py',
        'next12_pb_scratch_objectcheck.py','next10_sw_bounds_context.py','next11_sw_linker_relas.py')}
    metadata={str(path):copy_pin(path,out/'origin-metadata') for path in (previous/'context-report.json',previous/'input-freeze.json',
        previous/'actual-unit-options.json',previous/'static-footprint.json',CUT_ROOT.parent/'candidate-source-freeze.json',CUT_ROOT.parent/'ownership.json')}
    linkpins={}
    for name,row in old_freeze['link_inputs_and_compiler'].items():
        if sha(Path(row['original']))!=row['sha256']:raise RuntimeError('Original selected linker/compiler input changed')
        linkpins[name]=copy_pin(Path(row['original']),out/'frozen-link-inputs')
    freeze={'stage':'before any target tool in combined-classic-v1','source_revision':REVISION,'published_revision':PUBLISHED,
        'canonical_actual_build':admission,'previous_combined_context':str(previous),'previous_context_sha256':sha(previous/'context-report.json'),
        'all602_baseline_source_sha256':baseline,'all602_combined_source_sha256':combined,
        'actual205_IR_input_origins_before_replacement':input_origins,'all_reused202_C_IR_actual_dependency_checks':dependency_checks,
        'unchanged_two_VU_source_checks':vu_checks,'three_variant_IR_reused_after_full_dependency_byte_check':sorted(variant_units),
        'all_other201_IR_canonical_byte_identical':True,'generated_input_archive':generated,'external_inputs':external_pins,
        'link_inputs_and_compiler':linkpins,'origin_metadata':metadata,'tools':tools,'source_ROOT_before_sha256':root_before}
    (out/'input-freeze.json').write_text(json.dumps(freeze,indent=2)+'\n')
    flags=[f for f in original['flags'] if f not in ('-MMD','-MP')];prefix='-I'+str(ROOT/'src')
    flags=['-I'+str(snapshot/'src')+f[len(prefix):] if f.startswith(prefix) else f for f in flags]
    directories={str(Path(k).parent):str(Path(v).parent) for k,v in gen_paths.items()}
    flags=['-I'+directories.get(f[2:],f[2:]) if f.startswith('-I') else f for f in flags]
    if not {'-O2','-G8','-flto','-Werror','-DPS2','-DPS2_PROFILE','-DHWRENDER'}<=set(flags):raise RuntimeError('Canonical default flags required')
    compiler=original['link_command'][0];replacement=out/'obj'/CACHE.replace('/','__').replace('.c','.o')
    include_at=next(i for i,f in enumerate(flags) if f.startswith('-I'))
    driver_flags=flags[:include_at]+['-O3']+flags[include_at:]
    command=[compiler,*driver_flags,'-MMD','-MF',out/'ps2_hwd.d','-c',snapshot/CACHE,'-o',replacement]
    stdout,stderr,compile_seconds=run(command,out/'ps2_hwd-compile.log',120)
    if stdout or stderr:raise RuntimeError('New driver compile diagnostics differ')
    if lto_options(replacement)!=bytes.fromhex(options[CACHE]['complete_option_bytes_hex']):raise RuntimeError('New driver complete LTO option bytes differ')
    options[CACHE]['freshly_compiled_in_this_stage']=True
    (out/'actual-unit-options.json').write_text(json.dumps(options,indent=2)+'\n')
    input_origins[str(replacement)]['freshly_compiled_in_this_stage']=True
    expected_dep={str((snapshot/n).resolve()):d for n,d in combined.items()}
    expected_dep.update(external_expected)
    _,cache_deps=dependencies(out/'ps2_hwd.d')
    for path in cache_deps:
        if expected_dep.get(str(path))!=sha(path):raise RuntimeError('Actual new driver dependency was not frozen before target run')
    engine=out/'obj/engine_lto.o';lto=[compiler,'-r','-nostdlib','-flto=1','-flinker-output=nolto-rel','-O2',
        '-ffunction-sections','-fdata-sections',*[f for f in flags if f.startswith('-G') or f=='-mno-abicalls'],*objects,'-o',engine]
    stdout,stderr,lto_seconds=run(lto,out/'lto.log',600);diag=check_diagnostics(build,out,stdout,stderr,canonical_ir,objects,original)
    link=list(original['link_command']);eng=[i for i,x in enumerate(link) if x.endswith('/engine_lto.o')];maps=[i for i,x in enumerate(link) if x.startswith('-Wl,-Map=')]
    if len(eng)!=1 or len(maps)!=1:raise RuntimeError('One actual engine/map position required')
    link[eng[0]]=str(engine);link[link.index('-o')+1]=str(out/'SRB2.ELF');link[maps[0]]='-Wl,-Map='+str(out/'SRB2.map')
    stdout,stderr,link_seconds=run(link,out/'link.log',120)
    if (build/'link.log').read_text() or stdout or stderr:raise RuntimeError('Normal all-up link diagnostics differ')
    companion=out/'linker-relas';companion.mkdir();emitted=list(link);emitted[emitted.index('-o')+1]=str(companion/'LINKER-RELAS.ELF')
    emitted[maps[0]]='-Wl,-Map='+str(companion/'LINKER-RELAS.map');emitted.append('-Wl,--emit-relocs')
    stdout,stderr,reloc_seconds=run(emitted,companion/'link.log',120)
    if stdout or stderr:raise RuntimeError('Retained-RELA link diagnostics differ')
    checked=verify(Elf32(out/'SRB2.ELF'),Elf32(companion/'LINKER-RELAS.ELF'))
    checked.update(original_ELF=str(out/'SRB2.ELF'),original_ELF_sha256=sha(out/'SRB2.ELF'),original_engine=str(engine),
        original_engine_sha256=sha(engine),original_map=str(out/'SRB2.map'),original_map_sha256=sha(out/'SRB2.map'),
        diagnostic_ELF_sha256=sha(companion/'LINKER-RELAS.ELF'),exact_link_command=list(map(str,emitted)),
        all_pinned_linker_inputs_unchanged={str(engine):sha(engine),**{n:r['sha256'] for n,r in linkpins.items()}})
    (companion/'linker-rela-report.json').write_text(json.dumps(checked,indent=2)+'\n')
    changed=[Path(old_path).name for old_path,new in zip(canonical_ir,objects) if canonical_ir[old_path]!=sha(new)]
    if sorted(changed)!=sorted(unit.replace('/','__').replace('.c','.o') for unit in UNITS):raise RuntimeError('Actual205 graph differs beyond original four overlays')
    for unit in variant_units:
        name=unit.replace('/','__').replace('.c','.o')
        if sha(out/'obj'/name)!=old_by_name[name][1]:raise RuntimeError('A reused variant IR changed during link')
    if sources(snapshot)!=combined or sources(ROOT)!=root_before or sources(CUT_ROOT)!=cut:raise RuntimeError('Frozen source or production root changed')
    if exact_build(build,base,FINAL_ELF)!=admission or sha(previous/'context-report.json')!=freeze['previous_context_sha256']:
        raise RuntimeError('Original accepted/combined context changed')
    for group in (external_pins,linkpins,metadata,tools):
        for row in group.values():
            if sha(Path(row['original']))!=row['sha256'] or sha(Path(row['snapshot']))!=row['sha256']:raise RuntimeError('An input changed during build')
    actual_inputs={}
    for name,digest in original['input_sha256'].items():
        path=Path(name);actual=snapshot/path.relative_to(ROOT) if path.is_relative_to(ROOT/'src') else Path(gen_paths.get(name,name))
        expected=combined[path.relative_to(ROOT).as_posix()] if path.is_relative_to(ROOT/'src') else digest
        if sha(actual)!=expected:raise RuntimeError('New complete context dependency differs')
        actual_inputs[str(actual)]=expected
    sections={'baseline':allocated(Elf32(build/'SRB2.ELF')),'candidate':allocated(Elf32(out/'SRB2.ELF'))}
    syms={'baseline':linked_symbols(build/'SRB2.ELF'),'candidate':linked_symbols(out/'SRB2.ELF')}
    procs={'baseline':pdr(Object(build/'obj/engine_lto.o')),'candidate':pdr(Object(engine))}
    differences={name:{tag:procs[tag]['records'].get(name) for tag in procs} for name in sorted(set(procs['baseline']['records'])|set(procs['candidate']['records']))
        if procs['baseline']['records'].get(name,{}).get('raw_words')!=procs['candidate']['records'].get(name,{}).get('raw_words')}
    for tag,data in procs.items():(out/(tag+'-complete-pdr.json')).write_text(json.dumps(data,indent=2)+'\n')
    end_delta=syms['candidate']['_end']['address']-syms['baseline']['_end']['address']
    footprint={'allocated_sections':sections,'symbols':syms,'actual_end_delta_bytes':end_delta,
        'actual_text_delta_bytes':sections['candidate']['.text']['bytes']-sections['baseline']['.text']['bytes'],
        'all_frame_differences':differences,'baseline_frame_records':len(procs['baseline']['records']),
        'candidate_frame_records':len(procs['candidate']['records']),'measured_from_actual_combined_ELF_not_component_sum':True,'shipping_admission':False}
    (out/'static-footprint.json').write_text(json.dumps(footprint,indent=2)+'\n')
    ctx={'status':'COMPLETE isolated classic filler v3 all-up Hardware exposure; NOT validated shipping result',
        'source_revision':REVISION,'published_revision':PUBLISHED,'baseline_origin':admission,
        'complete_baseline_source_sha256':baseline,'complete_source_overlay_sha256':combined,'project_source_differences':sorted(OVERLAYS),
        'canonical_IR_objects':canonical_ir,'complete_actual_IR_objects':{str(x):sha(x) for x in objects},'changed_actual_IR_objects':changed,
        'only_recompiled_units':[CACHE],'reused_variant_IR_units':sorted(variant_units),'all_other201_complete_IR_inputs_identical':True,
        'all202_reused_C_IR_actual_dependencies_byte_identical_to_new_snapshot':True,'actual_dependency_checks_sha256':sha(out/'input-freeze.json'),
        'whole_LTO_option_section_bytes_identical_all_four_units':True,'actual_unit_options_sha256':sha(out/'actual-unit-options.json'),
        'preserved_per_unit_policy':{u:options[u]['actual_existing_override'] for u in UNITS},'generated_input_archive':generated,
        'compile_commands':{CACHE:list(map(str,command))},'compile_seconds':{CACHE:compile_seconds},'lto_command':list(map(str,lto)),
        'link_command':link,'emit_relocs_command':list(map(str,emitted)),'lto_seconds':lto_seconds,'link_seconds':link_seconds,
        'reloc_link_seconds':reloc_seconds,'lto_diagnostics':diag,'input_freeze_sha256':sha(out/'input-freeze.json'),
        'static_footprint_sha256':sha(out/'static-footprint.json'),'actual_end_delta_bytes':end_delta,
        'static_fit_observation':'_end grew; report and stop, no admission' if end_delta>0 else '_end did not grow; static exposure only; other gates remain',
        'production_source_ROOT_unchanged':True,'no_emulator_invoked':True,'engine_sha256':sha(engine),'ELF_sha256':sha(out/'SRB2.ELF')}
    result=dict(original);result.update(flags=flags,compiled=1,link_command=link,input_sha256=actual_inputs,elf_sha256=ctx['ELF_sha256'],
        elf_bytes=(out/'SRB2.ELF').stat().st_size,combined_context_control=ctx)
    (out/'context-report.json').write_text(json.dumps(ctx,indent=2)+'\n');(out/'build-report.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({'status':ctx['status'],'out':str(out),'ELF_sha256':ctx['ELF_sha256'],'actual_end_delta_bytes':end_delta,
        'actual_text_delta_bytes':footprint['actual_text_delta_bytes'],'frame_differences':len(differences),
        'newly_compiled_units':[CACHE],'reused_variant_IR_units':sorted(variant_units),'LTO_options_equal_all_four':True,
        'lto_seconds':lto_seconds,'link_seconds':link_seconds,'reloc_link_seconds':reloc_seconds},indent=2))


if __name__=='__main__':main()
