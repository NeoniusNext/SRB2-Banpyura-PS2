#!/usr/bin/env python3
"""Run the exact conversion witness through the locked standard 32 MiB runner."""
import argparse,hashlib,json,os,re,statistics,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'tools/ps2'))
from next12_runtime import run_verified
from next10_gs_run import critical_settings
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build',type=Path,required=True);p.add_argument('--out',type=Path,required=True);p.add_argument('--pak',type=Path,default=ROOT/'build/pak');a=p.parse_args()
 build,out,pak=a.build.resolve(),a.out.resolve(),a.pak.resolve();profile=Path(os.environ.get('SRB2_PCSX2_ROOT','/workspace/.onboarding/pcsx2'));configs=list(profile.glob('slot*/**/PCSX2.ini'))
 if os.environ.get('SRB2_PCSX2_SLOTS')!='1' or not configs or any(critical_settings(path).get('EmuCore',{}).get('EnableExtraMemory')!='false' for path in configs):raise RuntimeError('Unchanged standard32MiB single-slot profile required')
 if out.exists():raise RuntimeError('Choose fresh output directory')
 out.mkdir(parents=True);elf=build/'TILE_I64F.ELF';report=build/'ee-build-report.json';meta=json.loads(report.read_text());proof=meta['source_proof']
 inputs=[elf,report,build/'source-proof.json',Path(proof['functions']),Path(proof['fixture']),Path(__file__),Path(__file__).with_name('tile_i64f_hosttest.py'),*sorted(pak.glob('*.PAK')),pak/'FINEACON.DAT']
 command=[sys.executable,'-B',str(ROOT/'tools/ps2/opt_run.py'),'--name','paired','--elf',str(elf),'--ram','32','--pak',str(pak),'--out',str(out/'runs'),'--timeout','180','--until','TI COMPLETE']
 r=run_verified(command,out/'runtime',inputs);bootfile=out/'runs/paired/boot.txt';boot=bootfile.read_text(errors='replace') if bootfile.exists() else '';complete=re.search(r'TI COMPLETE checks=(\d+) failures=(\d+) direct_mismatches=(\d+)',boot)
 rows=[{'rep':int(m[1]),'which':int(m[2]),'calls':int(m[3]),'ticks':int(m[4]),'checksum':m[5]} for m in re.finditer(r'TI BENCH rep=(\d+) which=(\d+) calls=(\d+) ticks=(\d+) checksum=(\d+)',boot)]
 labels=('reference_soft64','nearest_signed32','exact_only_signed32','direct_signed32_diagnostic');timings={labels[w]:statistics.median(x['ticks'] for x in rows if x['which']==w) for w in range(4)} if len(rows)==12 else {}
 equal=len({x['checksum'] for x in rows if x['which'] in (0,1,2)})==1
 passed=r.returncode==0 and complete and int(complete[2])==0 and len(rows)==12 and equal
 final={'passed':bool(passed),'scope':'Actual EE conversion bits/Count, standard32MiB PCSX2; no GS or whole-game performance claim. Direct32 is diagnostic, not required to match.','checks':int(complete[1]) if complete else None,'failures':int(complete[2]) if complete else None,'direct32_mismatches':int(complete[3]) if complete else None,'paired_candidate_checksums_equal':equal,'benchmarks':rows,'median_Count':timings,'source_proof':proof,'elf_sha256':hashlib.sha256(elf.read_bytes()).hexdigest(),'run_post':json.loads((out/'runtime/run-post.json').read_text()),'boot':boot}
 (out/'results.json').write_text(json.dumps(final,indent=2)+'\n');print(json.dumps({k:final[k] for k in ('passed','checks','failures','direct32_mismatches','median_Count')},indent=2));return int(not passed)
if __name__=='__main__':raise SystemExit(main())
