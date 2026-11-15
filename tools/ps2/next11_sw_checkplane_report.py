"""Strictly parse the complete recycled-plane native cost matrix."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import re

LENGTHS=(-1,0,1,2,3,7,8,16,31,32,33,100,320,640)
ROW=re.compile(r'CHECKPLANEBENCH variant=(\w+) n=(-?\d+) mode=(\d+) ffloor=(\d+) cache=(warm|cold) calls=(\d+) before=(\d+) after=(\d+)')


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def parse(text,build):
    exact=re.findall(r'CHECKPLANE EXACT cases=(\d+) compared_bytes=(\d+)',text)
    expected=(str(build['expected_native_cases']),str(build['expected_native_cases']*build['native_snapshot_bytes']))
    if exact!=[expected] or text.count('CHECKPLANE DONE')!=1 or 'CHECKPLANE FAIL' in text:
        raise ValueError('Missing/invalid complete native exactness or completion')
    if re.findall(r'CHECKPLANE TIMED allocation_or_free_stubs=(\d+)',text)!=['0']:
        raise ValueError('Timed allocator/free hook was executed or record is missing')
    lines=re.findall(r'CHECKPLANEBENCH[^\r\n]*',text)
    seen={};rows=[]
    for line in lines:
        match=ROW.fullmatch(line)
        if not match:raise ValueError('Malformed native timing row')
        variant,n,mode,ffloor,cache,calls,before,after=match.groups()
        row={'variant':variant,'n':int(n),'mode':int(mode),'ffloor':int(ffloor),'cache':cache,
             'calls':int(calls),'before':int(before),'after':int(after)}
        key=(variant,row['n'],row['mode'],row['ffloor'],cache)
        if key in seen or row['calls']!=(2048 if cache=='warm' else 32) or min(row['before'],row['after'])<=0:
            raise ValueError('Duplicate/invalid timing row')
        seen[key]=row;rows.append(row)
    wanted={('production',n,mode,ffloor,cache) for n in LENGTHS for mode in range(7)
            for ffloor in range(2) for cache in ('warm','cold')}
    if set(seen)!=wanted:raise ValueError('Incomplete/extra native timing matrix')
    results={}
    for cache in ('warm','cold','combined'):
        selected=[r for r in rows if cache=='combined' or r['cache']==cache]
        before=sum(r['before']/r['calls'] for r in selected)
        after=sum(r['after']/r['calls'] for r in selected)
        results[cache]={'rows':len(selected),'faster':sum(r['after']<r['before'] for r in selected),
                        'equal':sum(r['after']==r['before'] for r in selected),
                        'slower':sum(r['after']>r['before'] for r in selected),
                        'equally_weighted_summed_per_call_cost_reduction_percent':100*(before-after)/before}
    return {'exactness':{'cases':int(exact[0][0]),'complete_state_bytes':int(exact[0][1]),
                        'timed_allocation_or_free_hooks':0},'summary':results,'timings':rows,
            'scope':'Selected recycled-plane scan/helper costs with unchanged actual dependency; includes identical re-prime/dispatch overhead'}


def report(out,persist=True):
    build=json.loads((out/'ee-build-report.json').read_text())
    result=parse((out/'run.log').read_text(),build)
    result.update(ELF_sha256=sha(out/'CHECKPLANE.ELF'),
        helper_objects_sha256={name:sha(out/(name+'.o')) for name in ('reference','production','dependency','memops')},
        source_manifest_sha256=sha(out/'source-hashes.json'),run_log_sha256=sha(out/'run.log'))
    if persist:(out/'report.json').write_text(json.dumps(result,indent=2)+'\n')
    return result


def negative_controls(out):
    text=(out/'run.log').read_text();build=json.loads((out/'ee-build-report.json').read_text())
    line=ROW.search(text).group(0);mutations={
        'missing_row':text.replace(line+'\n','',1),'duplicate_row':text+'\n'+line,
        'bad_calls':text.replace('calls=2048','calls=2047',1),
        'bad_mode':text.replace('mode=0','mode=9',1),
        'bad_variant':text.replace('variant=production','variant=other',1),
        'zero_before':text.replace(line,ROW.sub(lambda m:m.group(0).replace('before='+m.group(7),'before=0'),line),1),
        'zero_after':text.replace(line,ROW.sub(lambda m:m.group(0).replace('after='+m.group(8),'after=0'),line),1),
        'missing_exact':text.replace('CHECKPLANE EXACT','CHECKPLANE OMITTED',1),
        'wrong_cases':text.replace('cases='+str(build['expected_native_cases']),'cases=1',1),
        'wrong_bytes':text.replace('compared_bytes=','compared_bytes=1',1),
        'missing_done':text.replace('CHECKPLANE DONE','CHECKPLANE OMITTED',1),
        'duplicate_done':text+'\nCHECKPLANE DONE',
        'executed_hook':text.replace('allocation_or_free_stubs=0','allocation_or_free_stubs=1',1),
        'failure_marker':text+'\nCHECKPLANE FAIL deliberate control'}
    result=[]
    for name,changed in mutations.items():
        if changed==text:raise ValueError('Missing parser control '+name)
        try:parse(changed,build)
        except (ValueError,KeyError):result.append({'mutation':name,'rejected':True})
        else:raise ValueError('Parser accepted actual log mutation '+name)
    return result


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args();result=report(args.out.resolve());result['parser_negative_controls']=negative_controls(args.out.resolve())
    (args.out/'report.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result['summary'],indent=2))
