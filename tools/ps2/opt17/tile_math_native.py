"""Run the paired actual-EE tile-math witness with immutable standard settings."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import statistics
import sys
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'tools/ps2'))
from next12_runtime import run_verified
from next10_gs_run import critical_settings

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build',type=Path,required=True);p.add_argument('--out',type=Path,required=True);p.add_argument('--pak',type=Path,default=ROOT/'build/pak');a=p.parse_args()
 build,out,pak=a.build.resolve(),a.out.resolve(),a.pak.resolve()
 profile=Path(os.environ.get('SRB2_PCSX2_ROOT','/workspace/.onboarding/pcsx2'))
 configs=list(profile.glob('slot*/**/PCSX2.ini'))
 if os.environ.get('SRB2_PCSX2_SLOTS')!='1' or not configs or any(critical_settings(path).get('EmuCore',{}).get('EnableExtraMemory')!='false' for path in configs):raise RuntimeError('Unchanged standard32MiB single-slot profile required')
 if out.exists():raise RuntimeError('Choose a fresh runtime destination')
 out.mkdir(parents=True)
 elf=build/'TILE_MATH.ELF';report=build/'ee-build-report.json';meta=json.loads(report.read_text())
 inputs=[elf,report,build/'source-proof.json',build/'base.c',build/'fast.c',build/'common.h',Path(__file__),Path(__file__).with_name('tile_math_hosttest.py'),Path(__file__).with_name('tile_math_hosttest.c'),*sorted(pak.glob('*.PAK')),pak/'FINEACON.DAT']
 proof=meta['source_proof'];inputs += [Path(proof['base']),Path(proof['candidate'])]
 command=[sys.executable,'-B',str(ROOT/'tools/ps2/opt_run.py'),'--name','paired','--elf',str(elf),'--ram','32','--pak',str(pak),'--out',str(out/'runs'),'--timeout','180','--until','TM COMPLETE']
 result=run_verified(command,out/'runtime',inputs)
 run=out/'runs/paired';boot=(run/'boot.txt').read_text(errors='replace') if (run/'boot.txt').exists() else ''
 complete=re.search(r'TM COMPLETE checks=(\d+) failures=(\d+)',boot)
 rows=[]
 for line in boot.splitlines():
  match=re.match(r'TM BENCH (origin|classify) rep=(\d+) which=(\d+) calls=(\d+)(?: bbox_pixel_upper=(\d+))? ticks=(\d+) checksum=(\d+)',line)
  if match:rows.append({'kind':match[1],'rep':int(match[2]),'which':int(match[3]),'calls':int(match[4]),'ticks':int(match[6]),'checksum':match[7]})
 ratios={}
 for kind in ('origin','classify'):
  ratios[kind]={'base_median_Count':statistics.median(x['ticks'] for x in rows if x['kind']==kind and x['which']==0),'candidate_median_Count':statistics.median(x['ticks'] for x in rows if x['kind']==kind and x['which']==1)} if len([x for x in rows if x['kind']==kind])==6 else None
  if ratios[kind]:ratios[kind]['base_over_candidate']=ratios[kind]['base_median_Count']/ratios[kind]['candidate_median_Count']
 checksums=all(len(set(x['checksum'] for x in rows if x['kind']==kind))==1 for kind in ('origin','classify'))
 passed=result.returncode==0 and complete and int(complete[2])==0 and len(rows)==12 and checksums
 final={'scope':'Actual PS2 EE code/Count in standard32MiB PCSX2. Successful-draw sinks replace GS calls. No rendered pixels or whole-game timing; not physical PS2.','passed':bool(passed),'wrapper_rc':result.returncode,'checks':int(complete[1]) if complete else None,'failures':int(complete[2]) if complete else None,'paired_checksums_equal':checksums,'benchmarks':rows,'ratios':ratios,'source_proof':proof,'ELF_sha256':hashlib.sha256(elf.read_bytes()).hexdigest(),'run_post':json.loads((out/'runtime/run-post.json').read_text()),'boot':boot}
 (out/'results.json').write_text(json.dumps(final,indent=2)+'\n');print(json.dumps({k:final[k] for k in ('passed','checks','failures','ratios')},indent=2));return int(not passed)
if __name__=='__main__':raise SystemExit(main())
