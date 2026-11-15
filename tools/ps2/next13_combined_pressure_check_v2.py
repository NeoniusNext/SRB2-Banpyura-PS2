#!/usr/bin/env python3
"""Freeze/replay NEW canonical pressure accounting; never execute a game.

Old next13_combined_pressure_check and its rejected run remain immutable.
Existing quality/runtime/request witnesses are reused byte-for-byte. New report
blocks provide direct physical counters and reset tag membership per snapshot.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import re
import shutil

import next13_combined_pressure_check as old
from next7_render_fog_codecheck import Elf32
from next8_sw_codecheck import linked_symbols

ROOT = old.ROOT
sha = old.sha
POLICY = ROOT / 'build/nov15-next13/final-startup-resource-policy-v2.json'
POLICY_SHA = '6562f09bed475dcd17c64cc320f5fce9e958388ccc76682dc051e0e6ffeda21f'
CFG = 'fpscap "35"\nfullscreen "Off"\nshowfps "No"\nshowping "Off"\nrollingdemos "Off"\ncon_hudlines "0"\n'
TAG = re.compile(r'^ps2_mem: tag\s+(\d+)\s+(\w+)\s+(\d+) blocks\s+(\d+) B \(payload\s+(\d+) B\)$')
ARENA = re.compile(r'^ps2_mem: arena (\d+) B  used (\d+) B \(peak (\d+), since start (\d+)\)  free (\d+) B in (\d+) blocks, largest (\d+) B, fragmentation (\d+)%$')
COUNTER = re.compile(r'^ps2_mem: allocs (\d+) frees (\d+) failed (\d+) evicted (\d+) blocks/(\d+) B, free blocks scanned (\d+)$')
END = re.compile(r'^ps2_mem: C heap blocks taken from the arena \(PS2-79\): now (\d+) B in (\d+) blocks, peak (\d+) B, (\d+) in all, refused (\d+), left by other threads (\d+)$')


def reports(boot):
    result, current = [], None
    for i, line in enumerate(boot.splitlines(), 1):
        start = re.fullmatch(r'ps2_mem: frame (\d+)', line)
        if start:
            if current is not None:
                raise ValueError('Incomplete preceding report block')
            current = {'frame': int(start[1]), 'first_line': i, 'raw': [], 'tags': {}}
        if current is None:
            continue
        current['raw'].append(line)
        if len((line + '\n').encode()) >= 1024:
            raise ValueError('Report line reaches I_OutputMsg dynamic-long-line boundary')
        if m := ARENA.fullmatch(line):
            if 'arena' in current:
                raise ValueError('Duplicate arena row')
            current['arena'] = dict(zip(('arena','used','peak','gpeak','free','freeblocks','largest','fragmentation'), map(int,m.groups())))
        if m := TAG.fullmatch(line):
            n = int(m[1])
            if n in current['tags']:
                raise ValueError('Duplicate tag within report')
            current['tags'][n] = dict(zip(('name','blocks','allocated','payload'), (m[2], *map(int,m.groups()[2:]))))
        if m := COUNTER.fullmatch(line):
            if 'counters' in current:
                raise ValueError('Duplicate counters row')
            current['counters'] = dict(zip(('allocs','frees','failed','evicted','evictedbytes','scanned'),map(int,m.groups())))
        if m := END.fullmatch(line):
            if 'arena' not in current or 'counters' not in current or not current['tags']:
                raise ValueError('Incomplete direct memory report')
            if sum(t['allocated'] for t in current['tags'].values()) != current['arena']['used']:
                raise ValueError('Reported tag physical extents do not sum to used')
            current['spill'] = dict(zip(('now','blocks','peak','total','refused','other_threads'),map(int,m.groups())))
            current['last_line'] = i
            result.append(current)
            current = None
    if current is not None:
        raise ValueError('Truncated final report')
    return result


def direct(boot, zstat):
    snapshots = reports(boot)
    checkpoints = []
    for i, line in enumerate(boot.splitlines(),1):
        if line.startswith('[zck] '):
            checkpoints.append({'line':i, 'name':line.split()[1], 'raw':line,
                'fields':{k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',line)}})
    errors = []
    before = [r for r in checkpoints if r['name']=='level-free-before']
    after = [r for r in checkpoints if r['name']=='level-free-after']
    precache = [r for r in checkpoints if r['name']=='precache']
    if len(before)!=1 or len(after)!=1 or len(precache)!=1 or not before[0]['line']<after[0]['line']<precache[0]['line']:
        raise ValueError('Exact ordered startup checkpoints absent')
    if before[0]['fields'].get('evicted')!=0 or after[0]['fields'].get('cache')!=0 or after[0]['fields'].get('evicted')!=31:
        errors.append('Startup0→31/cache0 checkpoint gate differs')
    if any(r['fields'].get('evicted')!=31 for r in checkpoints if r['line']>=after[0]['line']):
        errors.append('Loader checkpoint eviction count changed after forced flush')
    initial = [r for r in snapshots if r['first_line']<before[0]['line']]
    if not initial or any(r['counters']['evicted']!=0 or r['counters']['evictedbytes']!=0 for r in initial):
        errors.append('Direct initial count/byte zero anchor absent')
    world = [r for r in snapshots if r['first_line']>precache[0]['line']]
    # One report at each frame boundary plus the existing final report at350.
    if [r['frame'] for r in world] != [*range(1,351),350]:
        errors.append('Exact consecutive1..350 plus existing final350 report sequence absent')
    if not world:
        raise ValueError('Direct first-world report absent')
    anchor = {k:world[0]['counters'][k] for k in ('evicted','evictedbytes')}
    if anchor['evicted']!=31:
        errors.append('Direct first-world noted-event count differs from31')
    increments = []
    for row in world:
        inc = {k:row['counters'][k]-anchor[k] for k in anchor}
        increments.append({'frame':row['frame'],'line':row['first_line'],**inc})
        if any(inc.values()):
            errors.append('Direct world eviction count/physical-byte increment is nonzero at frame'+str(row['frame']))
    final_inc = {k:zstat[k]-anchor[k] for k in anchor}
    if any(final_inc.values()):
        errors.append('Final ZSTAT world count/physical-byte increment is nonzero')
    if world[-2]['tags']!=world[-1]['tags']:
        errors.append('Duplicate final350 tag snapshots differ')
    for key in ('arena','used','peak','gpeak','free','largest'):
        if world[-1]['arena'][key]!=zstat[key]:
            errors.append('Final direct arena/ZSTAT snapshot differs:'+key)
    return {'errors':errors,'all_complete_reports':snapshots,'all_checkpoints':checkpoints,'world_reports':world,
        'startup_direct_initial_reports':initial,'direct_first_world_anchor':anchor,'world_direct_increments':increments,
        'final_direct_increment':final_inc,'final_tags':world[-1]['tags'],
        'max_report_line_bytes_including_newline':max(len((line+'\n').encode()) for row in snapshots for line in row['raw']),
        'scope':'Checkpoint has count/cache only; first-postprecache report directly samples bytes after first world frame. No immediate postflush byte sample or victim identities inferred.'}


def tag_deltas(x,y,saving):
    errors, delta = [], {}
    for n in sorted(set(x)|set(y)):
        a,b = x.get(n,{}),y.get(n,{})
        d = {k:b.get(k,0)-a.get(k,0) for k in ('blocks','allocated','payload')}
        delta[str(n)] = d
        if a.get('name')!=b.get('name') or d['blocks'] or d['payload']!=(-saving if n==1 else 0):
            errors.append('Exact logical tag residency/payload gate differs:'+str(n))
    if 1 not in x or 1 not in y:
        errors.append('STATIC tag flat witness absent')
    return errors,delta


def paired_reports(a,b,saving):
    errors,rows = [],[]
    x,y = a['world_reports'],b['world_reports']
    if len(x)!=len(y):
        errors.append('Direct paired report counts differ')
    for left,right in zip(x,y):
        if left['frame']!=right['frame']:
            errors.append('Direct paired frames differ')
        e,tags = tag_deltas(left['tags'],right['tags'],saving)
        errors.extend('frame'+str(left['frame'])+':'+s for s in e)
        delta = {k:right['arena'][k]-v for k,v in left['arena'].items()}
        if delta['arena'] or any(delta[k]>0 for k in ('used','peak','gpeak')) or any(delta[k]<0 for k in ('free','largest')):
            errors.append('Per-frame physical memory gate worsened at frame'+str(left['frame']))
        rows.append({'frame':left['frame'],'all_arena_deltas':delta,'all_tag_deltas':tags})
    return {'errors':errors,'all_paired_report_deltas':rows,
        'startup_physical_freed_extent_bytes_delta':b['direct_first_world_anchor']['evictedbytes']-a['direct_first_world_anchor']['evictedbytes'],
        'startup_byte_gate':'No sign/equality gate; cumulative freed physical extents are recorded, not used as a memory/quality ordering'}


def parser_controls():
    def block(frame,payload,allocated,evicted=31,byte=48):
        return '\n'.join([f'ps2_mem: frame {frame}',f'ps2_mem: arena 1000 B  used {allocated} B (peak {allocated}, since start {allocated})  free {1000-allocated} B in 1 blocks, largest {1000-allocated} B, fragmentation 0%',
            f'ps2_mem: tag   1 PU_STATIC 1 blocks {allocated} B (payload {payload} B)',
            f'ps2_mem: allocs 1 frees 0 failed 0 evicted {evicted} blocks/{byte} B, free blocks scanned 1',
            'ps2_mem: C heap blocks taken from the arena (PS2-79): now 0 B in 0 blocks, peak 0 B, 0 in all, refused 0, left by other threads 0'])+'\n'
    initial=block(0,1,32,0,0)
    prefix=initial+'[zck] level-free-before cache=48 evicted=0\n[zck] level-free-after cache=0 evicted=31\n[zck] precache cache=0 evicted=31\n'
    a=prefix+''.join(block(f,100,128) for f in [*range(1,351),350])
    z={'arena':1000,'used':128,'peak':128,'gpeak':128,'free':872,'largest':872,'evicted':31,'evictedbytes':48}
    good=direct(a,z)
    if good['errors']:
        raise RuntimeError('Host synthetic direct positive fails')
    checks=[]
    mutations={
      'world count':a.replace('evicted 31 blocks/48 B','evicted 32 blocks/48 B',1),
      'world bytes':a.replace('evicted 31 blocks/48 B','evicted 31 blocks/64 B',1),
      'cache after flush':a.replace('level-free-after cache=0','level-free-after cache=16'),
      'missing frame':a.replace('ps2_mem: frame 17\n','ps2_mem: frame 18\n',1),
      'truncated report':a.rsplit('ps2_mem: C heap blocks taken',1)[0],
      'duplicate tag':a.replace('ps2_mem: allocs 1','ps2_mem: tag   1 PU_STATIC 1 blocks 128 B (payload 100 B)\nps2_mem: allocs 1',1),
      'long output line':a.replace('ps2_mem: allocs 1','X'*1024+'\nps2_mem: allocs 1',1)}
    for name,bad in mutations.items():
        try: rejected=bool(direct(bad,z)['errors'])
        except ValueError: rejected=True
        if not rejected: raise RuntimeError('Direct counter/parser mutation escaped:'+name)
        checks.append(name)
    for key in ('evicted','evictedbytes'):
        bad=dict(z);bad[key]+=1
        if not direct(a,bad)['errors']:raise RuntimeError('Final counter mutation escaped')
        checks.append('final '+key)
    # Absent tags in a later block must not survive from an earlier block.
    parsed=reports(block(1,100,128)+block(2,0,32).replace('tag   1 PU_STATIC','tag  49 PU_CACHE'))
    if 1 in parsed[-1]['tags'] or 49 in parsed[0]['tags']:raise RuntimeError('Report tag reset parser failed')
    checks.append('no stale tags across blocks')
    left=copy.deepcopy(good);right=copy.deepcopy(good)
    for row in right['world_reports']:row['tags'][1]['payload']-=8
    if paired_reports(left,right,8)['errors']:raise RuntimeError('Synthetic paired logical payload oracle fails')
    right['world_reports'][20]['tags'][1]['payload']+=1
    if not paired_reports(left,right,8)['errors']:raise RuntimeError('Per-frame payload mutation escaped')
    checks.append('per-frame payload')
    return {'scope':'Host checker fixtures only; no native event/capacity claim','executed_controls':checks}


def freeze(args):
    if sha(args.resource_policy)!=POLICY_SHA:
        raise RuntimeError('Select exact NEW v2 master policy')
    plan=json.loads(args.readiness.read_text());policy=json.loads(args.resource_policy.read_text())
    cases={c['id']:c for c in plan['cases']}
    if plan['config_addition']!='con_hudlines "0"' or plan.get('new_protocol_engine_observer')!=['-zreport','1']:
        raise RuntimeError('Common new direct observer/HUD control absent')
    for name,role in ((args.baseline_id,'baseline'),(args.candidate_id,'candidate')):
        c=cases[name]
        if c['role']!=role or c['mode']!='default' or c['expected_ps2args'].count('-zreport')!=1 or c['expected_ps2args'][c['expected_ps2args'].index('-zreport')+1]!='1':
            raise RuntimeError('Exact fresh default role/direct observer args absent')
        if Path(c['run']).exists() or Path(c['runtime']).exists():raise RuntimeError('Pre-execution fresh runtime required')
    if args.expected_flat_count!=plan['expected_flat_count_prediction_until_native_requests'] or args.expected_flat_count not in (4740,4743):
        raise RuntimeError('Frozen scene full-N prediction differs')
    for name,digest in plan['immutable_input_sha256'].items():
        if sha(name)!=digest:raise RuntimeError('Readiness input changed:'+name)
    out=args.out.resolve();out.mkdir(parents=True,exist_ok=False)
    helpers=[Path(__file__),Path(old.__file__),ROOT/'tools/ps2/next12_final_hardware_pressure_check.py',ROOT/'tools/ps2/next11_smoke_check.py',ROOT/'tools/ps2/next11_integration_check.py',ROOT/'tools/ps2/next10_gs_run.py',*args.input]
    pinned=out/'pinned-tools';pinned.mkdir()
    for path in helpers:
        if path.suffix=='.py':shutil.copyfile(path,pinned/path.name)
    manifest={'status':'FROZEN NEW v2 before both canonical native executions; no compiler/emulator',
        'readiness':str(args.readiness.resolve()),'readiness_sha256':sha(args.readiness),
        'resource_policy':str(args.resource_policy.resolve()),'resource_policy_sha256':sha(args.resource_policy),
        'baseline_id':args.baseline_id,'candidate_id':args.candidate_id,'expected_flat_count':args.expected_flat_count,
        'required_flat_payload_reduction':8*args.expected_flat_count,'checker_sha256':sha(__file__),
        'helper_sha256':{str(p.resolve()):sha(p) for p in helpers},'rules':policy,
        'actual_quality_positive_and_nine_mutations':old.self_test(),'direct_counter_parser_controls':parser_controls(),
        'old_strict_rejection_retained':{'path':str(ROOT/'build/nov15-next13/ram-combined-pressure-map02-v1/report.json'),'sha256':sha(ROOT/'build/nov15-next13/ram-combined-pressure-map02-v1/report.json')}}
    (out/'policy-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(json.dumps({'policy_manifest':str(out/'policy-manifest.json'),'sha256':sha(out/'policy-manifest.json'),'checker_sha256':sha(__file__)},indent=2))


def analyze(args):
    manifest=json.loads(args.policy_manifest.read_text())
    if sha(__file__)!=manifest['checker_sha256']:raise RuntimeError('Frozen checker changed')
    for name,digest in manifest['helper_sha256'].items():
        if sha(name)!=digest:raise RuntimeError('Frozen helper/report changed:'+name)
    readiness=Path(manifest['readiness']);resource=Path(manifest['resource_policy'])
    if sha(readiness)!=manifest['readiness_sha256'] or sha(resource)!=manifest['resource_policy_sha256']:raise RuntimeError('Frozen policy/readiness changed')
    plan=json.loads(readiness.read_text())
    for name,digest in plan['immutable_input_sha256'].items():
        if sha(name)!=digest:raise RuntimeError('Frozen actual input changed:'+name)
    cases={c['id']:c for c in plan['cases']};saving=manifest['required_flat_payload_reduction']
    a,b=[old.actual(cases[manifest[k]],plan,manifest['expected_flat_count']) for k in ('baseline_id','candidate_id')]
    for item in (a,b):
        boot=(Path(item['case']['run'])/'boot.txt').read_text()
        item['direct_memory']=direct(boot,item['ZSTAT']);item['errors']+=item['direct_memory']['errors']
        item['all_tags']=item['direct_memory']['final_tags']
        if (Path(item['case']['run'])/'.srb2/reference.cfg').read_text()!=CFG:item['errors'].append('Exact common93-byte staged pressure config differs')
    quality=old.quality(a,b);errors=a['errors']+b['errors']+quality['errors']
    paired=paired_reports(a['direct_memory'],b['direct_memory'],saving);errors+=paired['errors']
    oldz,newz=a['ZSTAT'],b['ZSTAT'];delta={k:newz[k]-v for k,v in oldz.items() if k in newz}
    allowed={'ram','class','map','levelframes','frames','cycles','arena','used','peak','gpeak','free','largest','evicted','evictedbytes','failed','libcfree','flushes','libcpeak','stackused','spillpeak','spillnow','t_ms'}
    if set(oldz)!=set(newz) or set(oldz)!=allowed:errors.append('Complete predeclared ZSTAT field set differs')
    for k in manifest['rules']['exact_ZSTAT_fields']:
        if delta.get(k)!=0:errors.append('Required exact final resource field differs:'+k)
    for k in ('libcpeak','failed','spillpeak','spillnow'):
        if delta.get(k,1)>0:errors.append('Final resource worsened:'+k)
    for k in ('libcfree','free','largest'):
        if delta.get(k,-1)<0:errors.append('Final available capacity decreased:'+k)
    for k in ('used','peak','gpeak'):
        if delta.get(k,0)>=0:errors.append('Final allocated reduction absent:'+k)
    e,tags=tag_deltas(a['all_tags'],b['all_tags'],saving);errors+=e
    if len(a['actual_constructor_pairs'])!=len(b['actual_constructor_pairs']):errors.append('Constructor request-pair count changed')
    layouts={role:{n:linked_symbols(Elf32(Path(r['build'])/'SRB2.ELF'))[n]['value'] for n in ('_end','_gp')} for role,r in plan['admissions'].items()}
    if layouts['baseline']!=layouts['candidate']:errors.append('Actual final whole-ELF _end/_gp differs')
    result={'status':'pass' if not errors else 'reject','errors':errors,'policy_manifest_sha256':sha(args.policy_manifest),
        'resource_policy_sha256':manifest['resource_policy_sha256'],'baseline':a,'candidate':b,'quality':quality,
        'all_resource_deltas':delta,'all_tag_deltas':tags,'all_paired_direct_reports':paired,'actual_layouts':layouts,
        'required_flat_payload_saving':saving,'actual_allocated_byte_reduction':-delta['used'],
        'allocated_residual_after_flat_payload':-delta['used']-saving,'spillnow_residual_after_flat_payload':delta['spillnow']+saving,
        'residual_scope':'Unattributed measured physical extent residual; no cause,31identities, or peak additivity inferred',
        'observer_scope':'Matched existing-zreport1/HUD0 pressure observer only; report frame3296 B and level-exit localpeak reset. Normal shipping/default images separate.',
        'timing_scope':'All raw timings retained; no FPS claim','old_result_remains_rejected':manifest['old_strict_rejection_retained'],
        'no_compiler_or_emulator_invoked':True}
    out=args.out.resolve();out.mkdir(parents=True,exist_ok=False);(out/'report.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({k:result[k] for k in ('status','errors','all_resource_deltas','required_flat_payload_saving','actual_allocated_byte_reduction','allocated_residual_after_flat_payload')},indent=2))


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--freeze',action='store_true');p.add_argument('--self-test',action='store_true')
    for name in ('readiness','resource-policy','policy-manifest','out'):p.add_argument('--'+name,type=Path)
    p.add_argument('--input',type=Path,action='append',default=[])
    p.add_argument('--baseline-id',default='baseline-default');p.add_argument('--candidate-id',default='candidate-default')
    p.add_argument('--expected-flat-count',type=int,default=4740);a=p.parse_args()
    if a.self_test:print(json.dumps({'quality':old.self_test(),'direct':parser_controls()},indent=2))
    elif a.freeze:freeze(a)
    else:analyze(a)

if __name__=='__main__':main()
