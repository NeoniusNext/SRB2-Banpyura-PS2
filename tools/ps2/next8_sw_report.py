"""Report every affine-NPOT EE timing row, including cold/transparent regressions."""
import argparse
import hashlib
import json
from pathlib import Path
import re
from next8_sw_hosttest import NAMES


def summarize(rows):
    before=sum(r['before'] for r in rows);after=sum(r['after'] for r in rows)
    return {'rows':len(rows),'before':before,'after':after,'reduction_pct':100*(before-after)/before,
            'faster':sum(r['after']<r['before'] for r in rows),'equal':sum(r['after']==r['before'] for r in rows),
            'slower':sum(r['after']>r['before'] for r in rows),'minimum_pct':min(r['reduction_pct'] for r in rows),
            'maximum_pct':max(r['reduction_pct'] for r in rows)}


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',required=True,type=Path)
    p.add_argument('--modes',type=int,default=6)
    p.add_argument('--selected',nargs='+',choices=NAMES)
    a=p.parse_args();work=a.out.resolve();source=(work/'run.log').read_text(errors='replace')
    rows=[]
    pattern=r'AFFINEBENCH drawer=(\w+) n=(\d+) mode=(\d+) calls=(\d+) before=(\d+) after=(\d+)'
    for match in re.finditer(pattern,source):
        name,length,mode,calls,before,after=match.groups()
        calls,before,after=int(calls),int(before),int(after)
        rows.append({'drawer':name,'length':int(length),'mode':int(mode),'calls':calls,'before':before,'after':after,
                     'before_per_call':before/calls,'after_per_call':after/calls,'reduction_pct':100*(before-after)/before})
    lengths=(0,1,2,3,7,8,16,31,32,33,100,320)
    expected={(n,length,mode) for n in NAMES for length in lengths for mode in range(a.modes)}
    actual={(r['drawer'],r['length'],r['mode']) for r in rows}
    if len(rows)!=len(expected) or actual!=expected or 'AFFINE DONE' not in source or 'AFFINE FAIL' in source:
        raise SystemExit('Incomplete benchmark/row equality failure')
    if any(r['calls']!=2048 or r['before']<=0 or r['after']<=0 for r in rows): raise SystemExit('Invalid timing counters')
    runner=(work/'runner.log').read_text()
    if 'pcsx2 exit code 0 (2 = timeout)' not in runner: raise SystemExit('Runner did not exit zero')
    choices=[('all',rows),('zero_pixels',[r for r in rows if r['length']==0]),
             ('one_to_31_pixels',[r for r in rows if 1<=r['length']<=31]),
             ('32_or_more_pixels',[r for r in rows if r['length']>=32])]
    choices.extend((name,[r for r in rows if r['drawer']==name]) for name in NAMES)
    if a.selected: choices.append(('selected',[r for r in rows if r['drawer'] in a.selected]))
    choices.extend(('mode_'+str(mode),[r for r in rows if r['mode']==mode]) for mode in range(a.modes))
    report={'groups':{name:summarize(selected) for name,selected in choices},'rows':rows,
            'elf_sha256':hashlib.sha256((work/'AFFINE.ELF').read_bytes()).hexdigest(),
            'functions_object_sha256':hashlib.sha256((work/'functions.o').read_bytes()).hexdigest(),
            'source_hashes':json.loads((work/'source-hashes.json').read_text()),
            'all_rows_check_complete_framebuffer_canaries_and_cursor_before_timing':True,
            'selected_functions':a.selected,
            'timing':'2048 calls, best of five paired repetitions, alternating order',
            'interrupts_masked_between_timestamps_with_prior_state_restored':(work/'driver.c').exists() and 'DIntr()' in (work/'driver.c').read_text(),
            'modes':{'0':'bounded positive','1':'bounded negative','2':'fallback negative','3':'fallback positive',
                     '4':'bounded fully transparent splat/sprite','5':'bounded fully opaque splat/sprite'},
            'scope':'Selected complete EE kernels under PCSX2; requires complete final-LTO instruction/RELA identity; no whole-game FPS/physical PS2 claim'}
    (work/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report['groups'],indent=2))
    print('Slower rows: '+json.dumps([r for r in rows if r['after']>r['before']],indent=2))


if __name__=='__main__': main()
