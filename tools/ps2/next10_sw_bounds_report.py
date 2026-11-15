"""Fail closed over complete next10 native plane-bounds timing grids."""
import argparse
import hashlib
import json
from pathlib import Path
import re


def report(out,persist=True,quiet=False):
    raw_log=(out/'run.log').read_text()
    log=re.sub(r'^\[\s*\d+(?:\.\d+)?\]\s*', '', raw_log, flags=re.M)
    manifests=json.loads((out/'source-hashes.json').read_text())
    for name in ('functions.c','driver.c'):
        if hashlib.sha256((out/name).read_bytes()).hexdigest()!=manifests[name]:
            raise SystemExit('Frozen native fixture source changed '+name)
    selected=manifests['selected'][1:]
    if not selected:raise SystemExit('Native timing requires at least one candidate')
    driver=(out/'driver.c').read_text()
    alias_match=re.search(r'for\(alias=0;alias<([45]);alias\+\+\)',driver)
    if not alias_match:raise SystemExit('Unrecognized frozen native proof grid')
    aliases=int(alias_match[1])
    expected_cases=((21+17)*7*aliases+256+128)*len(selected)
    expected_compared_bytes=expected_cases*2688
    lengths=[-1,0,1,2,3,7,8,16,31,32,33,100,320,640]
    caches=['warm','cold'] if manifests.get('cold') else ['warm']
    expected={(variant,n,mode,cache) for variant in selected for n in lengths for mode in range(7) for cache in caches}
    found={}
    pattern=r'^BOUNDSBENCH variant=(\w+) n=(-?\d+) mode=(\d+)(?: cache=(warm|cold))? calls=(\d+) before=(\d+) after=(\d+)$'
    for line in log.splitlines():
        if not line.startswith('BOUNDSBENCH'):continue
        match=re.fullmatch(pattern,line)
        if not match:raise SystemExit('Malformed native timing row: '+line)
        variant,n,mode,cache,calls,before,after=match.groups()
        cache=cache or 'warm'
        key=(variant,int(n),int(mode),cache); calls,before,after=map(int,(calls,before,after))
        if key not in expected or key in found or calls!=(32 if cache=='cold' else 2048) or min(before,after)<=0:
            raise SystemExit('Unexpected/duplicate/invalid native timing row: '+line)
        found[key]={'variant':variant,'length':key[1],'mode':key[2],'cache':cache,'calls':calls,
                    'before':before,'after':after,'reduction_percent':100*(before-after)/before}
    if found.keys()!=expected:raise SystemExit('Incomplete timing grid')
    exact=re.findall(r'^BOUNDS EXACT cases=(\d+) compared_bytes=(\d+)$',log,re.M)
    if len(exact)!=1 or tuple(map(int,exact[0]))!=(expected_cases,expected_compared_bytes) or log.count('BOUNDS DONE')!=1 or 'BOUNDS FAIL' in log:
        raise SystemExit('Missing unique native exactness/completion gate')
    result={'ELF_sha256':hashlib.sha256((out/'BOUNDS.ELF').read_bytes()).hexdigest(),
            'functions_sha256':hashlib.sha256((out/'functions.o').read_bytes()).hexdigest(),
            'exactness':{'cases':int(exact[0][0]),'compared_bytes':int(exact[0][1])},'variants':{}}
    for variant in selected:
        rows=[found[key] for key in sorted(expected) if key[0]==variant]
        faster=sum(r['after']<r['before'] for r in rows)
        slower=sum(r['after']>r['before'] for r in rows)
        before=sum(r['before']/r['calls'] for r in rows);after=sum(r['after']/r['calls'] for r in rows)
        result['variants'][variant]={'rows':len(rows),'faster':faster,'equal':len(rows)-faster-slower,'slower':slower,
            'equally_weighted_summed_per_call_cost_reduction_percent':100*(before-after)/before,'timings':rows,'by_cache':{}}
        for cache in caches:
            subset=[r for r in rows if r['cache']==cache]
            faster_cache=sum(r['after']<r['before'] for r in subset)
            slower_cache=sum(r['after']>r['before'] for r in subset)
            before_cache=sum(r['before']/r['calls'] for r in subset);after_cache=sum(r['after']/r['calls'] for r in subset)
            result['variants'][variant]['by_cache'][cache]={'rows':len(subset),'faster':faster_cache,
                'equal':len(subset)-faster_cache-slower_cache,'slower':slower_cache,
                'equally_weighted_summed_per_call_cost_reduction_percent':100*(before_cache-after_cache)/before_cache}
        if not quiet:
            print('%s: faster=%d equal=%d slower=%d reduction=%.9f%%'%(variant,faster,len(rows)-faster-slower,slower,100*(before-after)/before))
    if persist:(out/'report.json').write_text(json.dumps(result,indent=2)+'\n')
    return result


def negative_controls(out):
    original=(out/'run.log').read_text();lines=original.splitlines(keepends=True)
    row=next(line for line in lines if 'BOUNDSBENCH variant=' in line)
    exact=next(line for line in lines if 'BOUNDS EXACT cases=' in line)
    mutations={
        'missing_row':original.replace(row,'',1),
        'duplicate_row':original+row,
        'malformed_row':original.replace(row,row.replace(' calls=',' callz='),1),
        'wrong_calls':original.replace(row,re.sub(r'calls=\d+','calls=1',row),1),
        'unexpected_variant':original.replace(row,re.sub(r'variant=\w+','variant=absent',row),1),
        'unexpected_mode':original.replace(row,re.sub(r'mode=\d+','mode=7',row),1),
        'zero_timing':original.replace(row,re.sub(r'before=\d+','before=0',row),1),
        'missing_exact':original.replace(exact,'',1),
        'duplicate_exact':original+exact,
        'wrong_exact_count':original.replace(exact,re.sub(r'cases=\d+','cases=1',exact),1),
        'wrong_exact_bytes':original.replace(exact,re.sub(r'compared_bytes=\d+','compared_bytes=1',exact),1),
        'missing_completion':original.replace('BOUNDS DONE','BOUNDS MISSING'),
        'duplicate_completion':original+'BOUNDS DONE\n',
        'reported_failure':original+'BOUNDS FAIL injected\n',
    }
    controls=[]
    for title,log in mutations.items():
        folder=out/'parser-negatives'/title;folder.mkdir(parents=True,exist_ok=True)
        for name in ('source-hashes.json','functions.c','driver.c','BOUNDS.ELF','functions.o'):
            path=folder/name
            if not path.exists():path.symlink_to(out/name)
        (folder/'run.log').write_text(log)
        try:report(folder)
        except SystemExit as error:
            controls.append({'mutation':title,'rejected':True,'reason':str(error)})
        else:raise SystemExit('Native parser accepted injected mutation '+title)
    return controls


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True)
    p.add_argument('--negative-controls',action='store_true');a=p.parse_args();out=a.out.resolve()
    result=report(out)
    if a.negative_controls:
        result['parser_negative_controls']=negative_controls(out)
        (out/'report.json').write_text(json.dumps(result,indent=2)+'\n')
        print('All %d native parser controls rejected'%len(result['parser_negative_controls']))
