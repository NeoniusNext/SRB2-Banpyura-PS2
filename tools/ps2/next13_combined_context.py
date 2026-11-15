#!/usr/bin/env python3
"""Build a four-candidate all-up static exposure probe in actual Hardware LTO.

Requires one explicitly granted serial EE worker. This is not admitted shipping
source: drawnode and texture candidates still have separate pending gates.
No production file, padding, reserve, capacity, flag policy or emulator changes.
"""
import argparse
import difflib
import hashlib
import json
from pathlib import Path
import re
import shutil

from audio_storage_next7_finalbind import Object
from next10_sw_bounds_context import archive_generated
from next12_pb_scratch_context import run
from next12_pb_scratch_finalbind import exact_build,exact_source,FINAL_ELF,sha
from next12_pb_scratch_objectcheck import pdr
from next11_sw_linker_relas import verify,allocated
from next7_render_fog_codecheck import Elf32
from next10_sw_bounds_codecheck import symbol_tables
from next13_flats_context import check_diagnostics,lto_options

ROOT=Path(__file__).resolve().parents[2]
WORK=ROOT/'build/nov15-next13'
REVISION='d4fbc7c319f19edc70eda220c3323d0350504c98'
PUBLISHED='353cc94103aeb38cef87d26151c779302135d0e9'
OVERLAYS={
    'src/hardware/hw_pbatch.inc':('geometry-source-v2','9d025beaed5c982180f8d8fbb94bcc692e132f753f3100b4b1bdff59c1f51aee'),
    'src/hardware/hw_main.c':('ram-source-v2','25ab400933e147ae202648629b088999f7f2b3d84a70a839aeed471cffcdd3a6'),
    'src/hardware/hw_cache.c':('flats-source-v1','35fab374191af39ba02f7ce2b5c375c25007286496a40bfa241fa829754862d1'),
    'src/ps2/hw/ps2_hw_tex.inc':('texture-fill2/measured-source','03cad69496e29bfdd629d2e8b2ba2fee21e9ac867ca590401f2ea57b2afad0af'),
}
UNITS=('src/hardware/hw_main.c','src/hardware/hw_batching.c','src/hardware/hw_cache.c','src/ps2/hw/ps2_hwd.c')


def sources(path):
    return {p.relative_to(path).as_posix():sha(p) for p in sorted((path/'src').rglob('*')) if p.is_file()}


def linked_symbols(path):
    e=Elf32(path)
    return {s['name']:{'address':s['value'],'bytes':s['size']} for table in symbol_tables(e).values()
            for s in table if s['name'] in ('_end','_gp')}


def copy_pin(path,out):
    """Keep bytes with their original path and hash before any target tool runs."""
    path=Path(path).resolve();group=hashlib.sha256(str(path).encode()).hexdigest()[:16]
    target=out/group/path.name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(path,target)
    if sha(path)!=sha(target):raise RuntimeError('Input copy differs: '+str(path))
    return {'original':str(path),'snapshot':str(target),'sha256':sha(target),'bytes':target.stat().st_size}


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build',type=Path,default=ROOT/'build/nov15-next12/final/hardware')
    p.add_argument('--baseline',type=Path,default=ROOT/'build/nov15-next12/final/source')
    p.add_argument('--out',type=Path,default=WORK/'combined-v1')
    a=p.parse_args();build,base,out=(x.resolve() for x in (a.build,a.baseline,a.out))
    if out.exists() or not out.is_relative_to(WORK) or out==WORK:raise RuntimeError('Fresh isolated next13 probe required')
    committed=exact_source(base);published=exact_source(base,PUBLISHED)
    baseline={n:r['sha256'] for n,r in committed.items()}
    if committed!=published or len(baseline)!=602:raise RuntimeError('Actual tested/published602source differs')
    admission=exact_build(build,base,FINAL_ELF);original=json.loads((build/'build-report.json').read_text())
    root_before=sources(ROOT)
    if root_before!=baseline:raise RuntimeError('This exposure probe expects the production source to remain accepted baseline')
    overlay_pins=dict(baseline);origins={}
    for target,(directory,digest) in OVERLAYS.items():
        origin=WORK/directory;all_sources=sources(origin)
        changed=sorted(n for n in baseline if baseline[n]!=all_sources.get(n))
        if set(all_sources)!=set(baseline) or changed!=[target] or all_sources[target]!=digest:
            raise RuntimeError('Complete isolated origin differs: '+str(origin))
        overlay_pins[target]=digest
        origins[target]={'root':str(origin),'target_sha256':digest,'complete_source_sha256':all_sources,
                         'acceptance_scope':'Isolated candidate input only; not admission of retention'}
    out.mkdir(parents=True);(out/'obj').mkdir();snapshot=out/'snapshot';shutil.copytree(base/'src',snapshot/'src')
    for target,(directory,_) in OVERLAYS.items():shutil.copyfile(WORK/directory/target,snapshot/target)
    if sources(snapshot)!=overlay_pins:raise RuntimeError('Combined complete602snapshot differs')
    differences=sorted(n for n in baseline if baseline[n]!=overlay_pins[n])
    if differences!=sorted(OVERLAYS):raise RuntimeError('Combined source scope differs')
    (snapshot/'manifest.json').write_text(json.dumps({'revision':REVISION,'published_revision':PUBLISHED,
        'stage':'all-up-static-exposure-only','source_sha256':overlay_pins,'changed':differences,
        'production_src_edited':False,'shipping_result':False},indent=2)+'\n')
    patch=''.join(''.join(difflib.unified_diff((base/name).read_text().splitlines(True),(snapshot/name).read_text().splitlines(True),
        fromfile='a/'+name,tofile='b/'+name)) for name in differences)
    (out/'combined-candidate.patch').write_text(patch)
    rules_path=ROOT/'tools/ps2/opt_units.txt';rules_raw=rules_path.read_bytes()
    rules_file=out/'snapshot/tools/ps2/opt_units.txt';rules_file.parent.mkdir(parents=True);rules_file.write_bytes(rules_raw)
    import subprocess
    if rules_raw!=subprocess.check_output(['git','show',PUBLISHED+':tools/ps2/opt_units.txt'],cwd=ROOT):
        raise RuntimeError('Per-unit rules are not the exact existing published policy')
    rules={}
    for line in rules_raw.decode().splitlines():
        fields=line.split('#',1)[0].split()
        if len(fields)>1:rules[fields[0]]=fields[1:]
    if {unit:rules.get(unit,[]) for unit in UNITS}!={**{unit:[] for unit in UNITS[:-1]},UNITS[-1]:['-O3']}:
        raise RuntimeError('Exactly existing O2/O2/O2/O3 policy required')
    pinned=out/'pinned-tools';pinned.mkdir()
    toolnames=('next13_combined_context.py','next13_flats_context.py','next12_pb_scratch_context.py',
        'next12_pb_scratch_finalbind.py','next12_pb_scratch_objectcheck.py','next10_sw_bounds_context.py',
        'next11_sw_linker_relas.py','next7_render_fog_codecheck.py','next10_sw_bounds_codecheck.py',
        'audio_storage_next7_finalbind.py')
    toolpins={name:copy_pin(ROOT/'tools/ps2'/name,pinned) for name in toolnames}
    metadata={}
    for directory,name in (('geometry-source-v2','manifest.json'),('ram-source-v2','source-manifest.json'),
                           ('flats-source-v1','source-manifest.json'),('texture-fill2','candidate-source-freeze.json'),
                           ('texture-fill2','ownership.json')):
        path=WORK/directory/name;metadata[str(path)]=copy_pin(path,out/'origin-metadata')
    external={n:d for n,d in original['input_sha256'].items() if not Path(n).is_relative_to(ROOT/'src')}
    generated=archive_generated(out,external,[build/'gen/config.h'])
    generated_paths={r['original_path']:r['archive_path'] for r in generated['details']['entries']}
    external_pins={}
    for name,digest in external.items():
        actual=Path(generated_paths.get(name,name))
        if sha(actual)!=digest:raise RuntimeError('Canonical external/generated input changed: '+name)
        external_pins[name]=copy_pin(actual,out/'frozen-external-inputs')
    link_paths={Path(name) for name in re.findall(r'(/[^\s()]+\.(?:a|o))',(build/'SRB2.map').read_text())
                if Path(name).is_file() and not name.endswith('/engine_lto.o')}
    link_paths.add(Path(original['link_command'][0]))
    link_input_pins={str(path):copy_pin(path,out/'frozen-link-inputs') for path in sorted(link_paths)}
    canonical_ir={};objects=[]
    for name,digest in admission['complete_actual_IR_objects'].items():
        old=Path(name);new=out/'obj'/old.name;shutil.copyfile(old,new)
        if sha(new)!=digest:raise RuntimeError('Copied whole actual IR differs')
        canonical_ir[name]=digest;objects.append(new)
    # Complete freeze precedes the first compiler/target command.
    freeze={'stage':'before target compilation','source_revision':REVISION,'published_revision':PUBLISHED,
        'canonical_actual_build':admission,'all602_baseline_source_sha256':baseline,
        'all602_combined_source_sha256':overlay_pins,'overlay_origins':origins,'changed_source_paths':differences,
        'canonical205_IR_objects':canonical_ir,'copied205_IR_objects_before_replacement':{str(v):sha(v) for v in objects},
        'generated_input_archive':generated,'external_inputs':external_pins,'link_inputs_and_compiler':link_input_pins,
        'origin_metadata':metadata,'tools':toolpins,'per_unit_rules_sha256':sha(rules_file),'source_ROOT_before_sha256':root_before}
    (out/'input-freeze.json').write_text(json.dumps(freeze,indent=2)+'\n')
    flags=[f for f in original['flags'] if f not in ('-MMD','-MP')]
    src_prefix='-I'+str(ROOT/'src')
    flags=['-I'+str(snapshot/'src')+f[len(src_prefix):] if f.startswith(src_prefix) else f for f in flags]
    directories={str(Path(old).parent):str(Path(new).parent) for old,new in generated_paths.items()}
    flags=['-I'+directories.get(f[2:],f[2:]) if f.startswith('-I') else f for f in flags]
    if not {'-O2','-G8','-flto','-Werror','-DPS2','-DPS2_PROFILE','-DHWRENDER'}<=set(flags):
        raise RuntimeError('Exact existing global Hardware flags required')
    compiler=original['link_command'][0];commands={};seconds={};options={}
    include_at=next(i for i,f in enumerate(flags) if f.startswith('-I'))
    for unit in UNITS:
        replacement=out/'obj'/unit.replace('/','__').replace('.c','.o')
        unit_flags=flags[:include_at]+rules.get(unit,[])+flags[include_at:]
        command=[compiler,*unit_flags,'-MMD','-MF',out/(Path(unit).stem+'.d'),'-c',snapshot/unit,'-o',replacement]
        stdout,stderr,elapsed=run(command,out/(Path(unit).stem+'-compile.log'),120)
        if stdout or stderr:raise RuntimeError('Combined changed-unit compile diagnostics: '+unit)
        before=lto_options(build/'obj'/replacement.name);after=lto_options(replacement)
        if before!=after:raise RuntimeError('Whole .gnu.lto_.opts differs for '+unit)
        commands[unit]=list(map(str,command));seconds[unit]=elapsed
        options[unit]={'actual_existing_override':rules.get(unit,[]),'canonical_IR':str(build/'obj'/replacement.name),
            'candidate_IR':str(replacement),'complete_option_section_bytes_hex':before.hex(),
            'whole_option_section_bytes_identical':True,'option_bytes_sha256':hashlib.sha256(before).hexdigest()}
    (out/'actual-unit-options.json').write_text(json.dumps(options,indent=2)+'\n')
    engine=out/'obj/engine_lto.o'
    lto=[compiler,'-r','-nostdlib','-flto=1','-flinker-output=nolto-rel','-O2','-ffunction-sections','-fdata-sections',
        *[f for f in flags if f.startswith('-G') or f=='-mno-abicalls'],*objects,'-o',engine]
    stdout,stderr,lto_seconds=run(lto,out/'lto.log',600)
    diagnostics=check_diagnostics(build,out,stdout,stderr,canonical_ir,objects,original)
    link=list(original['link_command']);engine_sites=[i for i,v in enumerate(link) if v.endswith('/engine_lto.o')]
    map_sites=[i for i,v in enumerate(link) if v.startswith('-Wl,-Map=')]
    if len(engine_sites)!=1 or len(map_sites)!=1:raise RuntimeError('One actual engine/map position required')
    link[engine_sites[0]]=str(engine);link[link.index('-o')+1]=str(out/'SRB2.ELF');link[map_sites[0]]='-Wl,-Map='+str(out/'SRB2.map')
    stdout,stderr,link_seconds=run(link,out/'link.log',120)
    if (build/'link.log').read_text() or stdout or stderr:raise RuntimeError('Normal combined link diagnostics differ')
    companion=out/'linker-relas';companion.mkdir();emitted=list(link)
    emitted[emitted.index('-o')+1]=str(companion/'LINKER-RELAS.ELF');emitted[map_sites[0]]='-Wl,-Map='+str(companion/'LINKER-RELAS.map');emitted.append('-Wl,--emit-relocs')
    stdout,stderr,reloc_seconds=run(emitted,companion/'link.log',120)
    if stdout or stderr:raise RuntimeError('Combined retained-RELA link diagnostics differ')
    checked=verify(Elf32(out/'SRB2.ELF'),Elf32(companion/'LINKER-RELAS.ELF'))
    checked.update(original_ELF=str(out/'SRB2.ELF'),original_ELF_sha256=sha(out/'SRB2.ELF'),
        original_engine=str(engine),original_engine_sha256=sha(engine),original_map=str(out/'SRB2.map'),
        original_map_sha256=sha(out/'SRB2.map'),diagnostic_ELF_sha256=sha(companion/'LINKER-RELAS.ELF'),
        exact_link_command=list(map(str,emitted)),all_pinned_linker_inputs_unchanged={str(engine):sha(engine),
            **{n:r['sha256'] for n,r in link_input_pins.items()}})
    (companion/'linker-rela-report.json').write_text(json.dumps(checked,indent=2)+'\n')
    changed_ir=[Path(old).name for old,new in zip(canonical_ir,objects) if canonical_ir[old]!=sha(new)]
    expected_ir=sorted(unit.replace('/','__').replace('.c','.o') for unit in UNITS)
    if sorted(changed_ir)!=expected_ir:raise RuntimeError('Combined graph changed IR beyond four primary units')
    if sources(snapshot)!=overlay_pins or sources(ROOT)!=root_before:raise RuntimeError('Combined/production source changed during exposure build')
    for target,(directory,_) in OVERLAYS.items():
        if sources(WORK/directory)!=origins[target]['complete_source_sha256']:raise RuntimeError('A frozen isolated origin changed')
    if exact_build(build,base,FINAL_ELF)!=admission:raise RuntimeError('Accepted whole default baseline changed')
    for records in (external_pins,link_input_pins,metadata,toolpins):
        for record in records.values():
            if sha(Path(record['original']))!=record['sha256'] or sha(Path(record['snapshot']))!=record['sha256']:
                raise RuntimeError('An archived exact input changed during build')
    if sha(rules_path)!=sha(rules_file):raise RuntimeError('Existing per-unit flag policy changed')
    actual_inputs={}
    for name,digest in original['input_sha256'].items():
        path=Path(name);actual=snapshot/path.relative_to(ROOT) if path.is_relative_to(ROOT/'src') else Path(generated_paths.get(name,name))
        expected=overlay_pins[path.relative_to(ROOT).as_posix()] if path.is_relative_to(ROOT/'src') else digest
        if sha(actual)!=expected:raise RuntimeError('Actual compiled dependency differs')
        actual_inputs[str(actual)]=expected
    section_costs={'baseline':allocated(Elf32(build/'SRB2.ELF')),'candidate':allocated(Elf32(out/'SRB2.ELF'))}
    symbol_costs={'baseline':linked_symbols(build/'SRB2.ELF'),'candidate':linked_symbols(out/'SRB2.ELF')}
    delta_end=symbol_costs['candidate']['_end']['address']-symbol_costs['baseline']['_end']['address']
    old_obj,new_obj=Object(build/'obj/engine_lto.o'),Object(engine);procedures={'baseline':pdr(old_obj),'candidate':pdr(new_obj)}
    frame_differences={name:{tag:procedures[tag]['records'].get(name) for tag in procedures}
        for name in sorted(set(procedures['baseline']['records'])|set(procedures['candidate']['records']))
        if procedures['baseline']['records'].get(name,{}).get('raw_words')!=procedures['candidate']['records'].get(name,{}).get('raw_words')}
    for tag,data in procedures.items():(out/(tag+'-complete-pdr.json')).write_text(json.dumps(data,indent=2)+'\n')
    footprint={'allocated_sections':section_costs,'symbols':symbol_costs,'actual_end_delta_bytes':delta_end,
        'actual_text_delta_bytes':section_costs['candidate']['.text']['bytes']-section_costs['baseline']['.text']['bytes'],
        'all_frame_differences':frame_differences,'baseline_frame_records':len(procedures['baseline']['records']),
        'candidate_frame_records':len(procedures['candidate']['records']),
        'measured_from_actual_combined_ELF_not_component_sum':True,'shipping_admission':False}
    (out/'static-footprint.json').write_text(json.dumps(footprint,indent=2)+'\n')
    context={'status':'COMPLETE isolated four-candidate actual Hardware exposure probe; NOT validated shipping result',
        'source_revision':REVISION,'published_revision':PUBLISHED,'baseline_origin':admission,
        'complete_baseline_source_sha256':baseline,'complete_source_overlay_sha256':overlay_pins,
        'project_source_differences':differences,'overlay_origins':origins,'canonical_IR_objects':canonical_ir,
        'complete_actual_IR_objects':{str(path):sha(path) for path in objects},'changed_actual_IR_objects':changed_ir,
        'only_recompiled_units':list(UNITS),'all_other201_complete_IR_inputs_identical':True,
        'whole_LTO_option_section_bytes_identical_all_four_units':True,'actual_unit_options_sha256':sha(out/'actual-unit-options.json'),
        'preserved_per_unit_policy':{unit:rules.get(unit,[]) for unit in UNITS},'generated_input_archive':generated,
        'compile_commands':commands,'compile_seconds':seconds,'lto_command':list(map(str,lto)),'link_command':link,
        'emit_relocs_command':list(map(str,emitted)),'lto_seconds':lto_seconds,'link_seconds':link_seconds,
        'reloc_link_seconds':reloc_seconds,'lto_diagnostics':diagnostics,'input_freeze_sha256':sha(out/'input-freeze.json'),
        'static_footprint_sha256':sha(out/'static-footprint.json'),'actual_end_delta_bytes':delta_end,
        'static_fit_observation':'_end grew; exposure recorded, no fit/retention admission' if delta_end>0 else '_end did not grow; static observation only, pending semantic/runtime/quality gates',
        'production_source_ROOT_unchanged':True,'no_emulator_invoked':True,'engine_sha256':sha(engine),'ELF_sha256':sha(out/'SRB2.ELF')}
    result=dict(original);result.update(flags=flags,compiled=4,link_command=link,input_sha256=actual_inputs,
        elf_sha256=context['ELF_sha256'],elf_bytes=(out/'SRB2.ELF').stat().st_size,combined_context_control=context)
    (out/'context-report.json').write_text(json.dumps(context,indent=2)+'\n');(out/'build-report.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({'status':context['status'],'out':str(out),'ELF_sha256':context['ELF_sha256'],
        'changed_actual_IR_objects':changed_ir,'all_four_whole_LTO_options_equal':True,'actual_end_delta_bytes':delta_end,
        'actual_text_delta_bytes':footprint['actual_text_delta_bytes'],'frame_differences':len(frame_differences),
        'compile_seconds':seconds,'lto_seconds':lto_seconds,'link_seconds':link_seconds,'reloc_link_seconds':reloc_seconds},indent=2))


if __name__=='__main__':main()
