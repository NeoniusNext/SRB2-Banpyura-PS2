"""Build two checked light-table opt-out IR replacements in a complete canonical graph.

Consumes one granted EE compiler worker, serial LTO; never runs an emulator.
Source/header and generated-input snapshots are immutable for the whole build.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import time

from next10_sw_bounds_context import archive_generated

ROOT=Path(__file__).resolve().parents[2]


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command,log,timeout):
    command=list(map(str,command));start=time.monotonic()
    result=subprocess.run(command,cwd=ROOT,capture_output=True,text=True,timeout=timeout)
    log.write_text(' '.join(command)+'\n'+result.stdout+result.stderr)
    if result.returncode:raise SystemExit('Context build failed '+str(log))
    return result.stdout,result.stderr,time.monotonic()-start


def compare_lto_diagnostics(canonical_log,stdout,stderr,canonical_ir,objects,original,out):
    """Normalize only checked equal-object path spellings, retaining raw streams."""
    raw=canonical_log.read_text();diagnostics=raw;invocation={'removed':False}
    for key in ('lighttable_context_control','checkplane_context_control','slope_pool_context_control','bounds_context_control'):
        context=original.get(key,{})
        if 'lto_command' not in context:continue
        line=' '.join(map(str,context['lto_command']))+'\n'
        if diagnostics.startswith(line):
            diagnostics=diagnostics[len(line):]
            invocation={'removed':True,'exact_structured_report_field':key+'.lto_command',
                        'recorded_invocation_sha256':hashlib.sha256(line.encode()).hexdigest()}
            break
    if len(canonical_ir)!=len(objects):raise SystemExit('Complete LTO input list lengths differ')
    identities={};paths={};equal=[];changed=[]
    for (old,digest),new in zip(canonical_ir.items(),objects):
        old=Path(old);new=Path(new)
        if sha(old)!=digest:raise SystemExit('Canonical LTO input changed before diagnostic comparison')
        copied=sha(new)
        if copied!=digest:
            changed.append({'canonical_path':str(old),'copied_path':str(new),
                            'canonical_sha256':digest,'copied_sha256':copied,'path_normalized':False})
            continue
        identity='<byte-identical-LTO-input:'+old.name+':'+digest+'>'
        entry={'canonical_path':str(old),'copied_path':str(new),'sha256':digest,'identity':identity}
        equal.append(entry);identities.setdefault(old.name,[]).append(entry)
        for path in (old,new):
            paths[str(path)]=identity
            if path.is_relative_to(ROOT):paths[str(path.relative_to(ROOT))]=identity
    # Copied canonical builds may retain an earlier equal input's path in their
    # raw log. Its actual bytes must still match the unique checked input.
    aliases=[]
    for spelling in sorted(set(re.findall(r'(?<![\w./-])(/[^\s:]+\.o|[\w./-]+\.o)(?=: warning:)',diagnostics+'\n'+stderr))):
        path=Path(spelling);actual=path if path.is_absolute() else ROOT/path
        candidates=identities.get(path.name,[])
        if len(candidates)!=1 or not actual.is_file():continue
        entry=candidates[0]
        if sha(actual)!=entry['sha256']:continue
        paths[spelling]=entry['identity'];aliases.append({'diagnostic_spelling':spelling,
            'actual_path':str(actual),'sha256':entry['sha256'],'identity':entry['identity']})
    def normalize(value):
        for spelling,identity in sorted(paths.items(),key=lambda item:len(item[0]),reverse=True):
            value=re.sub(r'(?<![\w./-])'+re.escape(spelling)+r'(?=[:\s\'"\)]|$)',
                         lambda match:identity,value)
        return value.replace(str(ROOT)+'/', '')
    left,right=normalize(diagnostics),normalize(stderr)
    report={'canonical_raw_log':str(canonical_log),'canonical_raw_log_sha256':sha(canonical_log),
            'canonical_diagnostic_stream_sha256':hashlib.sha256(diagnostics.encode()).hexdigest(),
            'control_stdout_sha256':hashlib.sha256(stdout.encode()).hexdigest(),
            'control_stderr_sha256':hashlib.sha256(stderr.encode()).hexdigest(),
            'canonical_normalized_sha256':hashlib.sha256(left.encode()).hexdigest(),
            'control_normalized_sha256':hashlib.sha256(right.encode()).hexdigest(),
            'exact_recorded_invocation_metadata':invocation,'byte_identical_input_paths':equal,
            'changed_inputs_never_path_normalized':changed,'checked_diagnostic_aliases':aliases,
            'all_normalized_diagnostics_identical':not stdout and left==right,
            'rule':'Only exact byte-identical input path spellings and the existing exact repository prefix normalize; no diagnostic text or warning is filtered'}
    (out/'canonical-lto.log').write_text(raw);(out/'lto-stdout.txt').write_text(stdout)
    (out/'lto-stderr.txt').write_text(stderr)
    (out/'lto-diagnostics.json').write_text(json.dumps(report,indent=2)+'\n')
    return report


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build',type=Path,required=True)
    parser.add_argument('--snapshot',type=Path,required=True,help='Frozen canonical tree containing src/')
    parser.add_argument('--out',type=Path,required=True)
    group=parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--optout',action='store_true',help='Restore the original RGBA table path while the tested PB overlay stays active')
    group.add_argument('--original-table-source',type=Path,help='Exact original five table files in the same final generated-header/PB-active graph')
    args=parser.parse_args();build=args.build.resolve();snapshot=args.snapshot.resolve();out=args.out.resolve()
    if out.exists() or not out.is_relative_to(ROOT/'build'):
        raise SystemExit('A fresh context destination under build/ is required')
    out.mkdir(parents=True);(out/'obj').mkdir()
    pinned=out/'pinned-tools';pinned.mkdir()
    for path in (Path(__file__).resolve(),ROOT/'tools/ps2/next10_sw_bounds_context.py'):
        shutil.copyfile(path,pinned/path.name)
    report_path=build/'build-report.json';original=json.loads(report_path.read_text())
    if not original['hardware'] or len(original['sources'])!=203:
        raise SystemExit('Complete actual default203-C plus2-VU Hardware graph required')
    source_manifest=json.loads((snapshot/'manifest.json').read_text())
    source_hashes={str(p.relative_to(snapshot)):sha(p) for p in sorted((snapshot/'src').rglob('*')) if p.is_file()}
    if len(source_hashes)!=602 or source_hashes!=source_manifest['source_sha256']:
        raise SystemExit('Complete602 published source snapshot differs')
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
    legacy_files=('src/hardware/hw_cache.c','src/ps2/hw/ps2_hw_pal.inc','src/ps2/hw/ps2_hwd.c','src/ps2/hw/ps2_hwd.h','src/ps2/hw/ps2_hwd_null.c')
    if args.original_table_source:
        baseline=json.loads((ROOT/'tools/ps2/next12_baseline.json').read_text())['source_sha256']
        for name in legacy_files:
            path=args.original_table_source.resolve()/name
            if sha(path)!=baseline[name]:raise SystemExit('Only the exact original source85 table files may be restored')
            shutil.copyfile(path,overlay/name)
    complete_overlay={str(path.relative_to(overlay)):sha(path) for path in sorted((overlay/'src').rglob('*')) if path.is_file()}
    project_differences=[]
    for relative,digest in complete_overlay.items():
        if sha(snapshot/relative)!=digest:project_differences.append(relative)
    if sorted(project_differences)!=(sorted(legacy_files) if args.original_table_source else []):
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
    flags+=['-Werror']+(['-DPS2_NOOPT_LIGHTTABLE_INDEXED'] if args.optout else [])
    compiler=original['link_command'][0]
    objects=[];canonical_ir={}
    for source in original['sources']:
        name=source.replace('/','__').replace('.c','.o');old=build/'obj'/name;new=out/'obj'/name
        canonical_ir[str(old)]=sha(old);shutil.copyfile(old,new);objects.append(new)
    compile_commands=[];compile_seconds=0
    for unit in ('src/hardware/hw_cache.c','src/ps2/hw/ps2_hwd.c'):
        name=unit.replace('/','__').replace('.c','.o');replacement=out/'obj'/name
        command=[compiler,*flags,'-MMD','-MF',out/(Path(unit).stem+'.d'),'-c',overlay/unit,'-o',replacement]
        stdout,stderr,elapsed=run(command,out/(Path(unit).stem+'-compile.log'),120)
        if stdout or stderr:raise SystemExit('Opt-out context compile diagnostics differ')
        compile_commands.append(list(map(str,command)));compile_seconds+=elapsed
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
             'only_recompiled_units':['src/hardware/hw_cache.c','src/ps2/hw/ps2_hwd.c'],'project_source_differences':project_differences,
             'narrow_optout':args.optout,'original_five_source_overlay':bool(args.original_table_source),'complete_source_overlay_sha256':complete_overlay,
             'generated_input_archive':generated,'compile_commands':compile_commands,
             'lto_command':list(map(str,lto_command)),'link_command':command,
             'compile_seconds':compile_seconds,'lto_seconds':lto_seconds,'link_seconds':link_seconds,
             'all_other_complete_IR_inputs_identical':True,'all_normalized_LTO_diagnostics_identical':True,
             'source_revision':source_manifest['revision'],'source_manifest_sha256':sha(snapshot/'manifest.json'),
             'pinned_tools_sha256':{p.name:sha(p) for p in pinned.iterdir()},
             'lto_diagnostics':lto_diagnostics,'lto_diagnostics_report_sha256':sha(out/'lto-diagnostics.json'),
             'engine_sha256':sha(engine),'ELF_sha256':sha(out/'SRB2.ELF')}
    report=dict(original);report.update(flags=flags,compiled=2,link_command=command,input_sha256=actual_inputs,
        elf_sha256=context['ELF_sha256'],elf_bytes=(out/'SRB2.ELF').stat().st_size,lighttable_context_control=context)
    (out/'build-report.json').write_text(json.dumps(report,indent=2)+'\n')
    (out/'context-report.json').write_text(json.dumps(context,indent=2)+'\n')
    print('Built complete actual two-unit light-table opt-out context '+str(out),flush=True)


if __name__=='__main__':main()
