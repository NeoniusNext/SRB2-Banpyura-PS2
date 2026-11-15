#!/usr/bin/env python3
"""Aggregate four independently replayed canonical pressure pairs, read-only."""
import argparse
import hashlib
import json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
WORK=ROOT/'build/nov15-next13'

def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,default=WORK/'final/ram-pressure-matrix-v3');a=p.parse_args()
    inputs={};pairs={};paths=set();timings=[]
    for seed in ('01b2d3f5','9e3779b9'):
        for scene in ('map02','fog-stereo'):
            d=WORK/'final-hardware-pressure-v3-check'/seed/scene
            owner=d/'report-v3/report.json';independent=d/'ram-independent-v3/report.json'
            assert owner.read_bytes()==independent.read_bytes()
            x=json.loads(independent.read_text());assert x['status']=='pass' and not x['errors']
            manifest=d/'pressure-admission-v3/policy-manifest.json';m=json.loads(manifest.read_text());assert x['policy_manifest_sha256']==sha(manifest)
            readiness=Path(m['readiness']);plan=json.loads(readiness.read_text());assert sha(readiness)==m['readiness_sha256']
            assert m['resource_policy_sha256']=='6562f09bed475dcd17c64cc320f5fce9e958388ccc76682dc051e0e6ffeda21f'
            assert m['checker_sha256']=='d83b33c19dc8a36b72f2de7c890a9cb5f6b66118bde0862d35a7d7d0fa1b0469'
            for n,digest in {**m['helper_sha256'],**plan['immutable_input_sha256']}.items():
                if n in inputs:assert inputs[n]==digest
                else:assert sha(n)==digest;inputs[n]=digest
            summaries={}
            for role in ('baseline','candidate'):
                r=x[role];dm=r['direct_memory'];case=r['case']
                assert dm['errors']==[] and len(dm['all_complete_reports'])==352 and len(dm['world_reports'])==351
                assert all(row['evicted']==0 and row['evictedbytes']==0 for row in dm['world_direct_increments'])
                assert dm['final_direct_increment']=={'evicted':0,'evictedbytes':0}
                for n,digest in r['actual_result_hashes'].items():assert sha(n)==digest
                for key in ('run','runtime'):
                    path=case[key];assert path not in paths;paths.add(path)
                    assert Path(path).is_relative_to(WORK/'final-hardware-pressure-v3')
                summaries[role]={'ZSTAT':r['ZSTAT'],'first_world_direct_counter_anchor':dm['direct_first_world_anchor'],
                    'complete_reports':352,'world_reports_including_final_duplicate':351,'max_actual_report_line_bytes':dm['max_report_line_bytes_including_newline'],
                    'all_ordered_stream_row_counts':{n:len(v) for n,v in r['ordered_streams'].items()},
                    'all_tag_residency_payload_gates_pass':True,'actual_full_N_constructor_pairs':r['actual_constructor_pairs']}
                timings.append({'seed':seed,'scene':scene,'role':role,'cycles':r['ZSTAT']['cycles'],'elapsed_ms':r['ZSTAT']['t_ms']})
            pairs[seed+'/'+scene]={'owner_report':str(owner),'independent_report':str(independent),'complete_serialized_reports_equal':True,
                'report_sha256':sha(independent),'policy_manifest':str(manifest),'policy_manifest_sha256':sha(manifest),
                'readiness':str(readiness),'readiness_sha256':sha(readiness),'baseline':summaries['baseline'],'candidate':summaries['candidate'],
                'all_resource_deltas':x['all_resource_deltas'],'required_flat_payload_saving':x['required_flat_payload_saving'],
                'actual_zone_allocated_reduction':x['actual_allocated_byte_reduction'],'unattributed_allocated_residual':x['allocated_residual_after_flat_payload'],
                'unattributed_spillnow_residual':x['spillnow_residual_after_flat_payload'],'all_exact_full_quality_streams_pass':True,
                'startup_physical_freed_extent_bytes_delta':x['all_paired_direct_reports']['startup_physical_freed_extent_bytes_delta']}
    assert len(paths)==16
    # Both fresh seeds reproduce the same resources within each scene; timings
    # remain observations and are deliberately not normalized or averaged.
    for scene in ('map02','fog-stereo'):
        x,y=(pairs[seed+'/'+scene] for seed in ('01b2d3f5','9e3779b9'))
        for k in x['all_resource_deltas']:
            if k not in ('cycles','t_ms'):assert x['all_resource_deltas'][k]==y['all_resource_deltas'][k]
    old=WORK/'ram-combined-pressure-map02-v1/report.json';assert sha(old)=='59082888f896d8a26b5e510018017b993987db395bf4b9a7a4c9982399a5fd7d' and json.loads(old.read_text())['status']=='reject'
    report={'status':'PASS four independently replayed canonical pressure pairs / eight fresh native runs; image acceptance separate',
        'actual_canonical_source_commit':'5481bbbdc5aa3167c046836f3e51e4dbb31ad807',
        'actual_canonical_Hardware_ELF_sha256':'7e4764fb609a1b3665936c0594ad4358c4bf1891545f6d4c48da13f5a0e7a9af',
        'pairs':pairs,'verified_frozen_input_sha256':inputs,'all8_actual_native_result_files_rehashed':True,
        'actual_world_frames':2800,'complete_direct_reports':2816,'world_direct_snapshots_including8_final_duplicates':2808,
        'all16_run_runtime_directories_distinct_and_in_new_v3_root':True,
        'resource_scope':'Per-frame/final source-bound payload reductions and actual nonworsening resources pass. Startup physical freed extents are recorded without sign/equality gate; no victim identity or allocated residual cause inferred. Libc savings measured, not assumed additive.',
        'observer_scope':'Common existing-zreport1 stdout plus HUD0 pressure protocol only. Actual reporting3296-byte frame/localpeak reset are scoped separately from shipping/default full-image tests.',
        'all_raw_timing_observations':timings,'timing_scope':'Single deterministic protocol observations; no CPU/FPS performance claim.',
        'old_strict_rejection_retained':{'path':str(old),'sha256':sha(old)},
        'old_v2_partial_first_baseline_not_reused_or_relabelled':True,
        'proof_authorities':{str(n):sha(n) for n in [WORK/'final/ram-binding-v1/report.json',WORK/'review/ram-sdk-final-v1/report.json',WORK/'final/ram-observer-binding-v2/report.json',WORK/'review/canonical-pressure-readiness-v3/report.json',WORK/'ram-pressure-parser-controls-v3/report.json']},
        'tool_sha256':sha(__file__),'no_compiler_linker_emulator_invoked_by_this_aggregation':True}
    out=a.out.resolve();out.mkdir(parents=True,exist_ok=False);(out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'status':report['status'],'report':str(out/'report.json'),'sha256':sha(out/'report.json'),'verified_shared_inputs':len(inputs)},indent=2))
if __name__=='__main__':main()
