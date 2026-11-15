#!/usr/bin/env python3
"""Complete source row fillers against byte/formula oracle, guards and corruptions."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from next13_texture_fill3_variant import ROOT,PATH,rewrite,OLD_IDX,NEW_IDX,OLD_IA,NEW_IA
from next13_texture_hosttest import section


def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()


MUTATIONS={
 'step':('sx += step','sx += step + 1'),
 'centre':('u32 sx = step >> 1;','u32 sx = 0;'),
 'row':('src_row(&c, row)','src_row(&c, 0)'),
 'alpha':('a8_to_gs(t[1])','(1u ^ a8_to_gs(t[1]))'),
 'output':('while (--remaining)','while (remaining--)'),
 'stale-context':('const fillctx_t c = *(const fillctx_t *)ctx;','static fillctx_t c; static int once; if (!once) { c = *(const fillctx_t *)ctx; once = 1; }'),
}


def prepare(work,base,current,mutation):
 work.mkdir(parents=True)
 before=(base/PATH).read_text();after=(current/PATH).read_text()
 if after!=rewrite(before) or after.replace(NEW_IDX,OLD_IDX).replace(NEW_IA,OLD_IA)!=before:raise SystemExit('Exact isolated two-filler source required')
 context=section(before,'typedef struct\n{\n\tconst u8 *src;','// 8-bit index data')
 alpha=section(before,'static inline u32 a8_to_gs(','static inline u32 pal_hash(')
 common='#ifndef NEXT13_FILL_COMMON_H\n#define NEXT13_FILL_COMMON_H\n#include <stdint.h>\n#include <stddef.h>\n#include <string.h>\ntypedef uint8_t u8; typedef uint32_t u32;\n'+context+alpha
 for tag,text in [('base',before),('cur',after)]:
  code=''
  for name,end in [('fill_idx','// Gather the low byte'),('fill_ia88','// why the last tex_upload')]:
   body=section(text,'static void '+name+'(',end)
   if tag=='cur' and mutation:
    old,new=MUTATIONS[mutation]
    if old in body:body=body.replace(old,new)
    elif mutation not in ('alpha',):raise SystemExit('Missing actual mutation '+mutation)
    if mutation=='row':body=body.replace('{\n','{\n\t(void)row;\n',1)
   body=body.replace('static void '+name+'(','void '+tag+'_'+name+'(',1)
   code+=body
   common+='void '+tag+'_'+name+'(void *,u32,u8 *);\n'
  (work/(tag+'.c')).write_text('#include "common.h"\n'+code)
 (work/'common.h').write_text(common+'#endif\n')
 return {'baseline_texture_sha256':sha(base/PATH),'candidate_texture_sha256':sha(current/PATH),
   'generated_sha256':{p.name:sha(p) for p in sorted(work.iterdir())}}


def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--base-src',type=Path,default=ROOT/'build/nov15-next12/final/source/src')
 p.add_argument('--candidate-src',type=Path,default=ROOT/'build/nov15-next13/texture-fill3-classic/overlay-src')
 p.add_argument('--out',type=Path,required=True);p.add_argument('--sanitize',action='store_true');p.add_argument('--negative-controls',action='store_true')
 a=p.parse_args()
 if a.out.exists():raise SystemExit('Fresh immutable filler host output required')
 a.out.mkdir(parents=True);reports={}
 configs=[('ordinary',False,None)]+([('sanitize',True,None)] if a.sanitize else [])
 if a.negative_controls:configs += [('negative-'+m,False,m) for m in MUTATIONS]
 pinned=a.out/'pinned-run-inputs';pinned.mkdir()
 import shutil
 for name in ('next13_texture_fill3_hosttest.py','next13_texture_fill_hosttest.c','next13_texture_fill3_variant.py'):
  shutil.copyfile(ROOT/'tools/ps2'/name,pinned/name)
 for name,sanitize,mutation in configs:
  work=a.out/name;inputs=prepare(work,a.base_src.resolve(),a.candidate_src.resolve(),mutation)
  flags=['-std=gnu17','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable']
  if sanitize:flags+=['-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie']
  command=['gcc',*flags,'-I'+str(work),work/'base.c',work/'cur.c',pinned/'next13_texture_fill_hosttest.c','-o',work/'test']
  result=subprocess.run([str(x) for x in command],capture_output=True,text=True)
  (work/'build.log').write_text(result.stdout+result.stderr)
  if result.returncode:raise SystemExit('Host filler compile failed '+str(work)+'\n'+result.stderr)
  result=subprocess.run([str(work/'test')],capture_output=True,text=True)
  output=result.stdout+result.stderr;(work/'run.log').write_text(output)
  if result.returncode!=(1 if mutation else 0) or ('FILL NEXT13 FAIL' not in output if mutation else 'FILL NEXT13 PASS' not in output):raise SystemExit('Host filler run unexpected '+str(work)+'\n'+output)
  reports[name]={**inputs,'command':list(map(str,command)),'stdout':output,'binary_sha256':sha(work/'test'),'expected_failure':bool(mutation)}
  print(name+': '+output.strip().splitlines()[-1],flush=True)
 reports['fixture_sha256']=sha(pinned/'next13_texture_fill_hosttest.c')
 (a.out/'host-report.json').write_text(json.dumps(reports,indent=2)+'\n')


if __name__=='__main__':main()
