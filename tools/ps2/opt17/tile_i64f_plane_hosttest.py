#!/usr/bin/env python3
"""Compare complete actual tile_plane_uv outputs after the exact cast change."""
import argparse,hashlib,json,re,subprocess
from pathlib import Path

def function(text,name):
 m=re.search(r'^static (?:float|void) '+re.escape(name)+r'\([^{}]*?\)\s*\{',text,re.M)
 if not m:raise RuntimeError('Missing function '+name)
 start=text.index('{',m.start());depth=0
 for at in range(start,len(text)):
  if text[at]=='{':depth+=1
  elif text[at]=='}':
   depth-=1
   if not depth:return text[m.start():at+1]+'\n'
 raise RuntimeError('Unclosed function '+name)
def norm_body(s):return re.sub(r'\s+','',re.sub(r'//[^\n]*','',s[s.index('{'):]))
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--base',type=Path,required=True);p.add_argument('--candidate',type=Path,required=True);p.add_argument('--out',type=Path,required=True);p.add_argument('--sanitize',action='store_true');a=p.parse_args()
 out=a.out.resolve();out.mkdir(parents=True,exist_ok=True);base=a.base.read_text();candidate=a.candidate.read_text();f=Path(__file__).with_name('tile_i64f_functions.c');native=f.read_text()
 # The deployed helper body must exactly retain the arithmetic tested natively.
 direct=re.search(r'__attribute__\(\(noinline\)\) float nearest_cast\([^{}]*\)',native)
 nearest=function(native.replace('__attribute__((noinline)) float nearest_cast','static float nearest_cast'),'nearest_cast')
 helper=function(candidate,'tile_i64_to_float');assert norm_body(helper)==norm_body(nearest)
 old=function(base,'tile_plane_uv');new=function(candidate,'tile_plane_uv');assert new.replace('tile_i64_to_float(n1)','(float)n1').replace('tile_i64_to_float(n2)','(float)n2')==old
 anchor='typedef struct\n{\n\tint x[3], y[3];';start=base.index(anchor);end=base.index('} tile_plane_t;',start)+len('} tile_plane_t;');decl=base[start:end];assert decl in candidate
 (out/'common.h').write_text('#include <stdint.h>\n#include <math.h>\n'+decl+'\nvoid base_plane(const tile_plane_t*,int64_t,int64_t,float*);void fast_plane(const tile_plane_t*,int64_t,int64_t,float*);\n')
 for tag,text in [('base',base),('fast',candidate)]:
  code='#include "common.h"\n'+(helper if tag=='fast' else '')+function(text,'tile_plane_uv')
  code+=f'\nvoid {tag}_plane(const tile_plane_t*p,int64_t n1,int64_t n2,float*out){{tile_plane_uv(p,n1,n2,&out[0],&out[1],&out[2],&out[3],&out[4],&out[5]);}}\n';(out/(tag+'.c')).write_text(code)
 fixture=Path(__file__).with_suffix('.c');flags=['-std=gnu17','-O2','-g','-fno-lto','-ffp-contract=off','-Wall','-Wextra','-Werror']
 if a.sanitize:flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie']
 cmd=['cc',*flags,'-I'+str(out),str(out/'base.c'),str(out/'fast.c'),str(fixture),'-o',str(out/'test'),'-lm'];r=subprocess.run(cmd,capture_output=True,text=True);(out/'build.log').write_text(r.stdout+r.stderr)
 if r.returncode:raise RuntimeError(r.stdout+r.stderr)
 r=subprocess.run([str(out/'test')],capture_output=True,text=True);(out/'test.log').write_text(r.stdout+r.stderr);passed=r.returncode==0 and 'failures=0' in r.stdout and not r.stderr
 proof={'base':str(a.base.resolve()),'base_sha256':sha(a.base),'candidate':str(a.candidate.resolve()),'candidate_sha256':sha(a.candidate),'native_functions_sha256':sha(f),'helper_body_matches_native_test_tokens':True,'all_other_UV_function_bytes_unchanged':True,'helper_body_token_sha256':hashlib.sha256(norm_body(helper).encode()).hexdigest()}
 (out/'results.json').write_text(json.dumps({'passed':passed,'source_proof':proof,'sanitize':a.sanitize,'returncode':r.returncode,'output':r.stdout,'stderr':r.stderr},indent=2)+'\n');print(r.stdout+r.stderr);return int(not passed)
if __name__=='__main__':raise SystemExit(main())
