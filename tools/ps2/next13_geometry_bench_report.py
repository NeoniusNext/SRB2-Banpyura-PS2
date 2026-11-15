#!/usr/bin/env python3
"""Compare all recorded native bench telemetry without discarding timing rows.

Non-time resource/count streams have explicit comparisons. Every complete
timing, cadence, maximum, scene and audio row remains in the report, including
differences. These no-hwpolyhash cost runs are not an image/quality matrix.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil

import next13_geometry_variant as variant


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def rows(text,prefix):return [line for line in text.splitlines() if line.startswith(prefix)]


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--bench',type=Path,default=variant.WORK/'geometry-bench-v2')
    p.add_argument('--out',type=Path,default=variant.WORK/'geometry-bench-audit-v1')
    a=p.parse_args();bench,out=a.bench.resolve(),a.out.resolve()
    if out.exists() or not out.is_relative_to(variant.WORK):raise RuntimeError('Fresh next13 telemetry audit required')
    measured=json.loads((bench/'report.json').read_text());ready=json.loads((bench/'readiness.json').read_text())
    texts={};records={};streams={}
    for entry in ready['entries']:
        tag=entry['role'];run=bench/'runs'/entry['name'];runtime=bench/'runtime'/entry['name']
        text=(run/'boot.txt').read_text(errors='replace');texts[tag]=text
        post=json.loads((runtime/'run-post.json').read_text());pre=json.loads((runtime/'run-pre.json').read_text())
        if pre['command']!=entry['command'] or post['command']!=entry['command'] or post['rc']:
            raise RuntimeError('Actual runtime command/return differs')
        if not all(post[key] for key in ('all_frozen_inputs_unchanged','all_non_UI_settings_unchanged','emulator_binary_unchanged')):
            raise RuntimeError('Actual complete runtime freeze failed')
        for path,digest in entry['input_sha256'].items():
            if sha(Path(path))!=digest:raise RuntimeError('An original frozen runtime input changed')
        complete=[line for line in text.splitlines() if re.match(r'^(?:HW|PROF|ASTAT|ZSTAT|\[z)',line)]
        records[tag]={'boot_path':str(run/'boot.txt'),'boot_sha256':sha(run/'boot.txt'),
            'runtime_pre_sha256':sha(runtime/'run-pre.json'),'runtime_post_sha256':sha(runtime/'run-post.json'),
            'complete_ordered_telemetry_rows':complete,
            'complete_cadence_maximum_and_scene_rows':[line for line in complete if line.startswith(('HWPROF0 ','HWFH ','HWSPIKE ','HW flip ','HW state ','ASTAT '))],
            'canonical_polygon_hash_rows':rows(text,'HWPH '),'loader_hash_rows':rows(text,'LHASH ')}
        streams[tag]={
            'resident_texture_rows':rows(text,'HWWS '),'light_table_rows':rows(text,'HWLT '),
            'hardware_memory_rows':rows(text,'HW mem '),'scene_rows':rows(text,'HW state '),
            'PB_collection_rows':rows(text,'HWPROF50 collection:'),
            'driver_geometry_upload_policy_tails':[line.split(' | polys=',1)[1] for line in rows(text,'HWPROF win=')],
            'PB_cached_geometry_count_rows':[line.split(' | cycles ',1)[0] for line in rows(text,'HWPROF70 ')],
            'texture_data_cache_rows':rows(text,'HWPROF46 '),
            'texture_regeneration_rows_except_explicit_cycle_field':[re.sub(r' in \d+ cycles',' in <recorded-cycle-field> cycles',line) for line in rows(text,'HWREGEN ')]}
    equality={name:bool(streams['baseline'][name]) and streams['baseline'][name]==streams['candidate'][name]
              for name in streams['baseline']}
    if not all(equality.values()):raise RuntimeError('An explicit recorded count/resource/texture/scene stream differs')
    old,new=measured['results']['baseline']['final_zstat'],measured['results']['candidate']['final_zstat']
    exclusions={'cycles','t_ms'}
    non_time={key:{'baseline':value,'candidate':new.get(key),'equal':value==new.get(key)}
              for key,value in old.items() if key not in exclusions}
    if set(old)!=set(new) or not all(row['equal'] for row in non_time.values()):raise RuntimeError('Recorded final non-time resources differ')
    pairs={}
    for prefix in ('HWPROF0 ','HWFH ','HWSPIKE ','HW flip ','HW state ','ASTAT '):
        left,right=(rows(texts[tag],prefix) for tag in ('baseline','candidate'))
        pairs[prefix]={'baseline':left,'candidate':right,'complete_rows_equal':left==right,
            'all_rows_and_differences_retained':True}
    report={'status':'PASS explicit recorded resource/count/texture/scene comparisons; complete timing/cadence/maxima differences retained',
        'scope':'Two fresh no-hwpolyhash helper-cost runs. No canonical polygon/loader hashes or images were requested, so this does not establish an integrated quality matrix or game FPS.',
        'checker_sha256':sha(Path(__file__)),'native_bench_report_sha256':sha(bench/'report.json'),
        'readiness_sha256':sha(bench/'readiness.json'),'helper_costs':measured,
        'actual_recorded_streams':streams,'explicit_streams_equal':equality,'final_non_time_resources':non_time,
        'full_final_timing_fields':{key:{'baseline':old[key],'candidate':new[key],'delta':int(new[key])-int(old[key])} for key in sorted(exclusions)},
        'full_cadence_maximum_scene_and_audio_comparisons':pairs,'complete_records':records,
        'missing_quality_evidence_explicit':{'HWPH_rows':0,'LHASH_rows':0,'requested_images':0}}
    if any(records[tag]['canonical_polygon_hash_rows'] or records[tag]['loader_hash_rows'] for tag in records):
        raise RuntimeError('Unexpected canonical hash instrumentation; revisit exact benchmark path')
    out.mkdir(parents=True);shutil.copyfile(Path(__file__),out/Path(__file__).name)
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'status':report['status'],'report':str(out/'report.json'),
        'explicit_streams_equal':equality,'stream_rows':{name:len(value) for name,value in streams['baseline'].items()},
        'non_time_resource_fields_equal':len(non_time),'final_timing_fields':report['full_final_timing_fields']},indent=2))


if __name__=='__main__':main()
