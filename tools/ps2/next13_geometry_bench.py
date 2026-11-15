#!/usr/bin/env python3
"""Prepare/run the existing actual HWR_ProcessPolygon native helper benchmark.

Default preparation invokes no emulator/compiler. --run or --run-case requires
the coordinator's sole emulator grant and uses next12_runtime -> opt_run ->
run_pcsx2 without configuration changes. This is a helper cost observation,
not a game FPS, physical-console cache or integrated graphics claim.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import sys

import next12_pb_scratch_codecheck as binding
from next12_runtime import run_verified
import next13_geometry_variant as variant

ROOT=variant.ROOT
TOOLS=ROOT/'tools/ps2'


def sha(path):
    with Path(path).open('rb') as handle:return hashlib.file_digest(handle,'sha256').hexdigest()


def prepare(out,pak,repetitions,frames):
    proof_paths={name:variant.WORK/path for name,path in {
        'host':'geometry-host-v3/report.json','public':'geometry-public-v1/report.json',
        'inventory':'geometry-inventory-v2/report.json','native_binding':'geometry-codecheck-v2/report.json'}.items()}
    proofs={name:json.loads(path.read_text()) for name,path in proof_paths.items()}
    if not all(str(value['status']).startswith('PASS') for value in proofs.values()):raise RuntimeError('Every frozen component proof must pass')
    builds={'baseline':ROOT/'build/nov15-next12/final/hardware','candidate':variant.WORK/'geometry-context-v2'}
    source_pins={'baseline':variant.validate(variant.WORK/'geometry-baseline-v1'),
                 'candidate':variant.validate(variant.WORK/'geometry-source-v2','v2')}
    linkers={'baseline':ROOT/'build/nov15-next12/final/pb-binding/final-linker-relas',
             'candidate':variant.WORK/'geometry-linker-relas-v2'}
    assets={p.name:sha(p) for p in sorted(pak.glob('*.PAK'))}
    assets['FINEACON.DAT']=sha(pak/'FINEACON.DAT')
    if not {'SRB2.PAK','ZONES.PAK','CHARS.PAK','MUSIC.PAK'}<=set(assets):raise RuntimeError('Complete stock packs required')
    tools=('next13_geometry_bench.py','next13_geometry_codecheck.py','next13_geometry_variant.py',
        'next12_runtime.py','next12_local_rng.py','next12_pb_scratch_codecheck.py','next11_sw_checkplane_codecheck.py',
        'next11_sw_linker_relas.py','next10_sw_bounds_codecheck.py','next7_render_fog_codecheck.py',
        'next8_sw_codecheck.py','next11_local_rng.py','next10_gs_run.py','opt_run.py','run_pcsx2.py')
    shared={str(path):sha(path) for path in proof_paths.values()}
    shared.update({str(TOOLS/name):sha(TOOLS/name) for name in tools})
    shared.update({str(pak/name):digest for name,digest in assets.items()})
    entries=[];diagnostic_bindings={}
    for tag,build in builds.items():
        positive=proofs['native_binding']['variants'][tag]
        if sha(build/'SRB2.ELF')!=positive['ELF_sha256'] or sha(build/'obj/engine_lto.o')!=positive['engine_sha256']:
            raise RuntimeError('Actual normal ELF/engine differs from complete native binding')
        diagnostic=variant.WORK/'geometry-local-rng-v2'/tag
        patch_path=diagnostic/'patch-report.json';patch=json.loads(patch_path.read_text());seed=patch['variants']['01b2d3f5']
        if not seed['assembled_template']['reused_without_recompilation']:raise RuntimeError('Checked existing scalar initializer template required')
        value=binding.binder(build,linkers[tag])
        copied=binding.diagnostic_copies(value,diagnostic,positive['complete_bound_text'])
        if copied['01b2d3f5']['sha256']!=seed['ELF_sha256']:raise RuntimeError('Exact diagnostic binding differs')
        diagnostic_bindings[tag]=copied
        files=dict(shared)
        for name in ('SRB2.ELF','obj/engine_lto.o','SRB2.map','build-report.json'):
            files[str(build/name)]=sha(build/name)
        files[str(patch_path)]=sha(patch_path);elf=Path(seed['ELF']);files[str(elf)]=sha(elf)
        template=seed['assembled_template']
        for key in ('assembly','object'):files[template[key]]=sha(Path(template[key]))
        snapshot=variant.WORK/'geometry-baseline-v1' if tag=='baseline' else build/'snapshot'
        for name,digest in source_pins[tag].items():
            path=snapshot/name
            files[str(path)]=sha(path)
            if files[str(path)]!=digest:raise RuntimeError('Actual frozen role source differs')
        name='geometry-'+tag+'-native-bench-map02'
        # Do not enable -hwpolyhash: that would intentionally route the real
        # public call through HWR_ProcessPolygonSlow instead of its common path.
        game_args=['-singletics','-keepspawn','-renderer','Hardware','-ps2prof','-hwstats','35',
                   '-hwbench2',str(repetitions),'-zquit',str(frames)]
        command=[sys.executable,'-B',str(TOOLS/'opt_run.py'),'--name',name,'--elf',str(elf),
                 '--pak',str(pak),'--out',str(out/'runs'),'--map','MAP02','--timeout','420','--',*game_args]
        expected=['-config','reference.cfg','-nolog','-noendtxt','-skipintro','-warp','MAP02',*game_args]
        entries.append({'role':tag,'name':name,'command':command,'expected_args':expected,
            'actual_normal_ELF':str(build/'SRB2.ELF'),'actual_normal_ELF_sha256':sha(build/'SRB2.ELF'),
            'diagnostic_ELF':str(elf),'diagnostic_ELF_sha256':sha(elf),'assets':assets,'input_sha256':files})
    return {'status':'READY; no emulator/compiler invoked by preparation','kind':__doc__,
        'source_revision':variant.REVISION,'seed':'0x01b2d3f5','map':'MAP02','repetitions':repetitions,'frames':frames,
        'actual_trigger':'HWR_StartBatching ++calls == 400; both k=3 and k=4, 1000 real public calls per pass',
        'proof_inputs':{name:{'path':str(path),'sha256':sha(path)} for name,path in proof_paths.items()},
        'diagnostic_copy_bindings':diagnostic_bindings,'entries':entries}


def analyze(entry,out,repetitions,frames):
    run=out/'runs'/entry['name'];runtime=out/'runtime'/entry['name']
    pre=json.loads((runtime/'run-pre.json').read_text());post=json.loads((runtime/'run-post.json').read_text())
    if pre['command']!=entry['command'] or post['command']!=entry['command'] or post['rc']!=0:
        raise RuntimeError('Official complete runtime command/return differs')
    if not all(post[key] for key in ('all_frozen_inputs_unchanged','all_non_UI_settings_unchanged','emulator_binary_unchanged')):
        raise RuntimeError('Complete runtime/configuration proof differs')
    if (run/'ps2args').read_text().splitlines()!=entry['expected_args']:raise RuntimeError('Exact staged argument stream differs')
    if sha(run/'SRB2.ELF')!=entry['diagnostic_ELF_sha256']:raise RuntimeError('Staged diagnostic ELF differs')
    for name,digest in entry['assets'].items():
        if sha(run/name)!=digest:raise RuntimeError('Staged asset differs')
    summary=json.loads((run/'summary.json').read_text());boot=(run/'boot.txt').read_text(errors='replace')
    if summary['rc'] or summary['errors'] or not summary['done'] or 'ZQUIT DONE' not in boot:
        raise RuntimeError('Exact native test did not complete')
    if 'HWE startup renderer=Hardware chosen=2 hardware=1' not in boot or 'HARDWARE -> SOFTWARE' in boot:
        raise RuntimeError('Native Hardware path absent')
    matches=re.findall(r'^HWBENCH2 HWR_ProcessPolygon n=(\d+): (\d+) cycles a polygon \(least of (\d+) passes; mean (\d+); (\d+) polygons collected\)$',boot,re.M)
    if len(matches)!=2 or [int(row[0]) for row in matches]!=[3,4]:raise RuntimeError('Both original native helper benchmark rows required')
    rows={int(n):{'minimum_cycles_per_polygon':int(minimum),'passes':int(passes),'mean_cycles_per_polygon':int(mean),
                 'polygons_collected':int(count)} for n,minimum,passes,mean,count in matches}
    if any(row['passes']!=repetitions or row['polygons_collected']!=1000 for row in rows.values()):
        raise RuntimeError('Actual helper workload/pass count differs')
    if not summary['zstat'] or summary['zstat'][-1]['levelframes']!=str(frames):raise RuntimeError('Requested native frame endpoint not reached')
    return {'role':entry['role'],'rows':rows,'runtime_pre_sha256':sha(runtime/'run-pre.json'),
        'runtime_post_sha256':sha(runtime/'run-post.json'),'boot_sha256':sha(run/'boot.txt'),
        'staged_ELF_sha256':sha(run/'SRB2.ELF'),'final_zstat':summary['zstat'][-1],
        'all_frozen_inputs_and_non_UI_settings_unchanged':True,'scope':'Existing helper benchmark only; not physical timing or game FPS'}


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=Path,default=variant.WORK/'geometry-bench-v2')
    p.add_argument('--pak',type=Path,default=ROOT/'build/pak')
    p.add_argument('--repetitions',type=int,default=64)
    p.add_argument('--frames',type=int,default=350)
    mode=p.add_mutually_exclusive_group();mode.add_argument('--run',action='store_true');mode.add_argument('--run-case',choices=('baseline','candidate'))
    mode.add_argument('--analyze-only',action='store_true')
    a=p.parse_args();out=a.out.resolve()
    if not out.is_relative_to(variant.WORK) or out==variant.WORK or a.repetitions<1 or a.frames<1:raise RuntimeError('Isolated next13 output and positive workload required')
    plan=prepare(out,a.pak.resolve(),a.repetitions,a.frames);out.mkdir(parents=True,exist_ok=True)
    snapshot=out/'tool-snapshot';snapshot.mkdir(exist_ok=True)
    runner=snapshot/Path(__file__).name
    if runner.exists():
        if sha(runner)!=sha(Path(__file__)):raise RuntimeError('Frozen runner differs; preserve stage')
    else:shutil.copyfile(Path(__file__),runner)
    readiness=out/'readiness.json'
    if readiness.exists():
        if json.loads(readiness.read_text())!=plan:raise RuntimeError('Frozen readiness changed; preserve stage and choose a fresh output')
    else:readiness.write_text(json.dumps(plan,indent=2)+'\n')
    if not (a.run or a.run_case or a.analyze_only):
        print(json.dumps({'status':plan['status'],'readiness':str(readiness),'native_bench_roles':len(plan['entries'])},indent=2));return
    for entry in plan['entries']:
        if a.run_case and entry['role']!=a.run_case:continue
        result_path=out/(entry['role']+'-result.json')
        if a.run or a.run_case:
            if result_path.exists() or (out/'runs'/entry['name']).exists():raise RuntimeError('Preserve prior actual runtime; choose a fresh output')
            for name,digest in entry['input_sha256'].items():
                if sha(name)!=digest:raise RuntimeError('Frozen runtime input changed')
            print('START '+entry['role'],flush=True)
            result=run_verified(entry['command'],out/'runtime'/entry['name'],[Path(name) for name in entry['input_sha256']])
            if result.returncode:raise RuntimeError('Official native wrapper failed: '+entry['role'])
        observed=analyze(entry,out,a.repetitions,a.frames)
        if result_path.exists():
            if json.loads(result_path.read_text())!=observed:raise RuntimeError('Preserve prior actual result; analysis differs')
        else:result_path.write_text(json.dumps(observed,indent=2)+'\n')
        print(json.dumps({'role':entry['role'],'rows':observed['rows']},indent=2),flush=True)
    results={tag:json.loads((out/(tag+'-result.json')).read_text()) for tag in ('baseline','candidate') if (out/(tag+'-result.json')).is_file()}
    if len(results)==2:
        report={'status':'COMPLETE two fresh actual native helper benchmark runs; no game FPS or physical timing claim',
            'readiness_sha256':sha(readiness),'results':results,'deltas':{}}
        for n in ('3','4'):
            report['deltas'][n]={key:results['candidate']['rows'][n][key]-results['baseline']['rows'][n][key]
                for key in ('minimum_cycles_per_polygon','mean_cycles_per_polygon')}
        path=out/'report.json'
        if path.exists() and json.loads(path.read_text())!=report:raise RuntimeError('Preserve prior complete native report')
        if not path.exists():path.write_text(json.dumps(report,indent=2)+'\n')
        print(json.dumps({'status':report['status'],'report':str(path),'deltas':report['deltas']},indent=2))


if __name__=='__main__':main()
