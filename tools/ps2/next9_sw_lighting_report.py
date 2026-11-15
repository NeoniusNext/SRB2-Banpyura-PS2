"""Require complete lighting experiment rows and summarize measured regressions."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',required=True,type=Path)
    a=p.parse_args(); out=a.out.resolve()
    text=(out/'run.log').read_text()
    manifest=json.loads((out/'source-hashes.json').read_text())
    selected=manifest.get('selected',list(manifest['variants']))
    names=[n for n in selected if n!='reference']
    frozen_driver=(out/'driver.c').read_text()
    declaration=re.search(r'static const int lengths\[\]\s*=\s*\{([^}]+)\}',frozen_driver)
    if declaration is None:raise SystemExit('Frozen native length grid missing')
    lengths=[int(value.strip()) for value in declaration[1].split(',')]
    if not lengths or len(lengths)!=len(set(lengths)):raise SystemExit('Invalid frozen native length grid')
    rows=[]
    pattern=r'LIGHTBENCH variant=(\w+) n=(-?\d+) mode=(\d+) calls=(\d+) before=(\d+) after=(\d+)'
    for m in re.finditer(pattern,text):
        name=m[1]; n,mode,calls,before,after=map(int,m.groups()[1:])
        rows.append({'variant':name,'n':n,'mode':mode,'calls':calls,'before':before,'after':after,
                     'reduction_percent':100*(before-after)/before})
    expected={(name,n,mode) for name in names for n in lengths for mode in range(15)}
    actual=Counter((r['variant'],r['n'],r['mode']) for r in rows)
    if set(actual)!=expected or any(count!=1 for count in actual.values()) or any(r['calls']!=2048 for r in rows):
        raise SystemExit('Missing, duplicate, unexpected or inconsistent timing rows')
    if 'LIGHT DONE' not in text or 'LIGHT FAIL' in text or 'LIGHT TARGET FAIL' in text or not re.search(r'LIGHT EXACT cases=\d+ compared_bytes=\d+ hash=[0-9a-f]{16}',text):
        raise SystemExit('Missing completion/equality marker or observed failure')
    result={'source_inputs':manifest,'timed_elf_sha256':hashlib.sha256((out/'LIGHT.ELF').read_bytes()).hexdigest(),
            'functions_object_sha256':hashlib.sha256((out/'functions.o').read_bytes()).hexdigest(),
            'scope':'Exploration only until complete candidate default-LTO and resolved final ELF binding passes',
            'target_equality':re.search(r'LIGHT EXACT[^\n]+',text)[0],
            'rows':rows,'variants':{}}
    for name in names:
        values=[r for r in rows if r['variant']==name]
        totals={key:sum(r[key] for r in values) for key in ('before','after')}
        result['variants'][name]={
            'rows':len(values),'faster':sum(r['after']<r['before'] for r in values),
            'equal':sum(r['after']==r['before'] for r in values),'slower':sum(r['after']>r['before'] for r in values),
            'summed_reduction_percent':100*(totals['before']-totals['after'])/totals['before'],
            'worst_row':min(values,key=lambda r:r['reduction_percent']),
            'short_reversed_rows':[r for r in values if r['n']<=3],
            'fully_negative_rows':[r for r in values if r['mode'] in (0,1,2)],
            'fully_upper_clamped_rows':[r for r in values if r['mode'] in (9,10)]}
    (out/'report.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({name:{k:v for k,v in data.items() if not isinstance(v,list)} for name,data in result['variants'].items()},indent=2))


if __name__=='__main__': main()
