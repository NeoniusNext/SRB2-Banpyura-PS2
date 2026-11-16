#!/usr/bin/env python3
"""Independently check disjoint queued victim/wide GS pixels and low24 depth."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--run',type=Path,required=True)
    p.add_argument('--negative',choices=['victim','wide'])
    a=p.parse_args();run=a.run.resolve()
    native=json.loads((run/'native-report.json').read_text())
    host=Path(native['private_host_stage']);pairs=[]
    for reference in sorted(host.glob('tn-pressure-l*-v0-320x200.rgba')):
        candidate=reference.with_name(reference.name.replace('-v0-','-v1-'))
        az,bz=reference.with_suffix('.z32'),candidate.with_suffix('.z32')
        x,y,z0,z1=[path.read_bytes() for path in [reference,candidate,az,bz]]
        if any(len(data)!=64000*4 for data in [x,y,z0,z1]):
            raise RuntimeError('Missing complete RGBA or raw depth frame')
        diff=[0,0];visible=[0,0];written_z=[0,0];maximum=0;alpha=0;badz=0
        for i in range(64000):
            at=i*4;side=(i%320)>=160
            delta=max(abs(x[at+c]-y[at+c]) for c in range(4))
            diff[side]+=delta!=0;visible[side]+=x[at:at+3]!=bytes([0x1d,0x2b,0x35])
            maximum=max(maximum,delta);alpha+=x[at+3]!=y[at+3]
            raw_z0,raw_z1=struct.unpack_from('<I',z0,at)[0],struct.unpack_from('<I',z1,at)[0]
            written_z[side]+=(raw_z0&0xffffff)!=0
            badz+=((raw_z0^raw_z1)&0xffffff)!=0
        pairs.append(dict(name=reference.name.split('-v0-')[0],compared=64000,
            different_victim_RGBA=diff[0],different_wide_RGBA=diff[1],max_channel_error=maximum,
            different_alpha=alpha,victim_visible=visible[0],wide_visible=visible[1],wrong_Zlow24=badz,
            victim_nonzero_Zlow24=written_z[0],wide_nonzero_Zlow24=written_z[1],
            hashes={path.name:hashlib.sha256(data).hexdigest() for path,data in zip([reference,candidate,az,bz],[x,y,z0,z1])}))
    log=(run/'native.txt').read_text()
    packed=re.findall(r'H0 (PASS|FAIL) sp_packed_(victim|sky) .*actual_STQ_RGBA_XY_Zlow24_wrong=(\d+) input_changed=(\d+) qwords=(\d+)',log)
    prefix=re.findall(r'TN PRESSURE_OWNER .*variant=1 borrowed=(\d+) block=(\d+) capacity=(\d+) evictions=(\d+) .*used=(\d+) fixture_fence_before=(\d+)',log)
    returned=re.findall(r'H0 PASS sp_end_dome_return .*variant=1 permission=0 failed=0 borrowed=0 ordinary_capacity=448 used=64',log)
    overwrite=re.findall(r'TN PRESSURE_OVERWRITE .*variant=1 ordinary=(\d+) blocks=(\d+) borrowed_after=(\d+) used=(\d+) .*fixture_fence_before=(\d+)',log)
    cpu=re.findall(r'H0 PASS sp_CPU_pixels_retained ',log)
    count=1 if a.negative else 2
    complete=len(pairs)==count and len(packed)==count*4 and len(prefix)==len(returned)==len(overwrite)==count and len(cpu)==count*2
    words=all(mark=='PASS' and int(bad)==int(changed)==0 and int(qwords)==9 for mark,_,bad,changed,qwords in packed)
    admitted=all((int(borrow),int(cap),int(evict),int(used),int(fence))==(64,128,1,128,0) for borrow,_,cap,evict,used,fence in prefix)
    reused=all((int(blocks),int(borrow),int(used),int(fence))==(448,0,512,0) for _,blocks,borrow,used,fence in overwrite)
    reused&=all(int(owner[1])+64==int(ordinary[0])+448 for owner,ordinary in zip(prefix,overwrite))
    nonempty=all(row['victim_visible']>3000 and row['wide_visible']>3000
        and row['victim_nonzero_Zlow24']>3000 and row['wide_nonzero_Zlow24']>3000 for row in pairs)
    rgba=all(row['different_victim_RGBA']==row['different_wide_RGBA']==0 for row in pairs)
    depth=all(row['wrong_Zlow24']==0 for row in pairs)
    profile=all(native.get(k) for k in ['completed','profile_32MiB','single_emulator_slot','all_frozen_inputs_unchanged','all_non_UI_settings_unchanged','emulator_binary_unchanged','check_totals_consistent'])
    pixel_failures=[row for row in native['failures'] if row.startswith('H0 FAIL sp_full_RGBA_parity ')]
    additional_failures=[row for row in native['failures'] if row not in pixel_failures]
    matched=complete and words and admitted and reused and nonempty and depth and profile
    sensitivity=None
    if a.negative:
        own='different_'+a.negative+'_RGBA';other='different_'+('wide' if a.negative=='victim' else 'victim')+'_RGBA'
        sensitivity=all(row[own]>0 and row[other]==0 for row in pairs) and len(pixel_failures)==count
        matched&=sensitivity and not additional_failures
    else:matched&=rgba and native['failure_count']==0
    report=dict(scope=__doc__,native_report=str(run/'native-report.json'),negative=a.negative,
        complete_pairs=complete,actual_pressure_eviction=admitted,actual_return_and_ordinary_reuse=reused,
        original_packed_words_exact=words,CPU_sources_retained=len(cpu)==count*2,disjoint_readers_visible=nonempty,
        RGBA_exact=rgba,Zlow24_exact=depth,tolerance=0,physical_PS2_tested=False,
        profile_and_bindings_unchanged=profile,matched_expectation=bool(matched),pairs=pairs,
        strict_native_failure_count=native['failure_count'],negative_pixel_sensitivity_confirmed=sensitivity,
        additional_native_failures=additional_failures,
        checker_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    (run/'pressure-report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:report[k] for k in ['negative','complete_pairs','actual_pressure_eviction','RGBA_exact','Zlow24_exact','original_packed_words_exact','matched_expectation']},indent=2))
    return int(not matched)


if __name__=='__main__':raise SystemExit(main())
