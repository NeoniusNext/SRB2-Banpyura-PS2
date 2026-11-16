"""Strict differential source tests and CPU-only timing of tile math; no emulator."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import os

ROOT=Path(__file__).resolve().parents[3]
COMMON=r'''
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <string.h>
#ifdef TILE_MATH_EE
#include <tamtypes.h>
#else
typedef uint32_t u32;typedef uint64_t u64;
#endif
typedef struct{float x,y,z,w,a[6];}cv_t;
typedef struct{float sb,tb;int x,y,ripple;}piece_t;
typedef struct{int x0,x1,y0,y1,ox,oy;}run_t;
typedef struct{run_t*runs;size_t capacity,count;u64 hash;unsigned failed;}output_t;
typedef struct{int width,height,sw,sh,sizew,sizeh,wrapx,wrapy,region;float kx,ky,ox,oy;cv_t tri[3];piece_t piece;}input_t;
int base_origin(float,int,int);int fast_origin(float,int,int);
void base_classify(const input_t*,output_t*);void fast_classify(const input_t*,output_t*);
'''
STUBS=r'''
#define TILE_SIZE 64
#define TILE_REGION_CORE 32
typedef struct{int x0,x1,y0,ox,oy;}tile_run_t;
typedef struct{int sw,sh,w,h,region;}texrec_t;
static struct{int vw,vh;u32 sciss_x0,sciss_x1,sciss_y0,sciss_y1,serial;texrec_t rec[1];}H;
static struct{float tsx,tsy,kx,ky,ox,oy;u64 clamp;u32 serial;int wrapx,wrapy;texrec_t*r;}P;
static u32 tile_run_peak_bytes;
static output_t*O;
static float hw_minf(float a,float b){return a<b?a:b;}
static float hw_maxf(float a,float b){return a>b?a:b;}
static void tile_stencil_begin(int a,int b,int c,int d){(void)a;(void)b;(void)c;(void)d;}
static void tile_stencil_end(void){}
static void hw_failure(const char*s){(void)s;O->failed++;}
static int tile_draw_run(const cv_t t[3],const piece_t*p,const texrec_t*s,const tile_run_t*r,int y1)
{
 (void)t;(void)p;(void)s;run_t v={r->x0,r->x1,r->y0,y1,r->ox,r->oy};
 if(O->runs){if(O->count>=O->capacity){O->failed++;return 0;}O->runs[O->count]=v;}
 const unsigned char*b=(const unsigned char*)&v;for(size_t i=0;i<sizeof v;i++){O->hash^=b[i];O->hash*=1099511628211ull;}O->count++;return 1;
}
'''
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def prepare(base,candidate,work):
 work.mkdir(parents=True,exist_ok=True)
 spec=importlib.util.spec_from_file_location('helpers',Path(__file__).with_name('highres_state_hosttest.py'));h=importlib.util.module_from_spec(spec);spec.loader.exec_module(h)
 (work/'common.h').write_text(COMMON)
 for tag,source in [('base',base),('fast',candidate)]:
  text=source.read_text();code='#include "common.h"\n'+STUBS+h.function(text,'tile_region_origin')+h.function(text,'tile_emit_triangle')
  code+=f'\n__attribute__((noinline)) int {tag}_origin(float v,int s,int w){{return tile_region_origin(v,s,w);}}\n'
  code+=f'''__attribute__((noinline)) void {tag}_classify(const input_t*i,output_t*out)
{{
 O=out;H.vw=i->width;H.vh=i->height;H.sciss_x0=H.sciss_y0=0;H.sciss_x1=i->width-1;H.sciss_y1=i->height-1;
 H.rec[0]=(texrec_t){{i->sw,i->sh,i->sizew,i->sizeh,i->region}};
 P.kx=i->kx;P.ky=i->ky;P.ox=i->ox;P.oy=i->oy;P.wrapx=i->wrapx;P.wrapy=i->wrapy;P.r=&H.rec[0];
 tile_emit_triangle(i->tri,&i->piece,0);
}}
'''
  (work/(tag+'.c')).write_text(code)
 (work/'source-proof.json').write_text(json.dumps({'base':str(base),'base_sha256':sha(base),'candidate':str(candidate),'candidate_sha256':sha(candidate),'scope':'Actual region helper and entire triangle classification function; successful draw/stencil sinks replaced, geometry outputs compared strictly.'},indent=2)+'\n')
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--base',type=Path,required=True);p.add_argument('--candidate',type=Path,required=True);p.add_argument('--out',type=Path,required=True);p.add_argument('--sanitize',action='store_true');p.add_argument('--ee-only',action='store_true');a=p.parse_args()
 work=a.out.resolve();prepare(a.base.resolve(),a.candidate.resolve(),work)
 if a.ee_only:
  os.environ['SRB2_PS2_OUT']=str(work/'sdkcfg');sys.path.insert(0,str(ROOT/'tools/ps2'));import build as B
  B.gen_config();flags=[f for f in B.CFLAGS if f not in ('-flto','-MMD','-MP')]+['-fno-lto','-ffp-contract=off','-DTILE_MATH_EE','-I'+str(work),'-Werror','-Wno-unused-variable','-Wno-unused-function','-Wno-unused-parameter']
  objects=[];commands=[]
  for source in (work/'base.c',work/'fast.c',Path(__file__).with_suffix('.c')):
   obj=work/(source.stem+'.o');cmd=[str(B.CC),*flags,*B.INCS,'-c',str(source),'-o',str(obj)];commands.append(cmd);r=subprocess.run(cmd,capture_output=True,text=True,env=B.ENV);(work/(source.stem+'-ee-build.log')).write_text(r.stdout+r.stderr)
   if r.returncode or r.stdout or r.stderr:raise RuntimeError(r.stdout+r.stderr)
   objects.append(obj)
  elf=work/'TILE_MATH.ELF';cmd=[str(B.CC),*B.LDFLAGS,*map(str,objects),'-o',str(elf),'-ldebug','-lpatches','-lm'];commands.append(cmd);r=subprocess.run(cmd,capture_output=True,text=True,env=B.ENV);(work/'ee-link.log').write_text(r.stdout+r.stderr)
  if r.returncode:raise RuntimeError(r.stdout+r.stderr)
  (work/'ee-build-report.json').write_text(json.dumps({'elf':str(elf),'elf_sha256':sha(elf),'fixture_sha256':sha(Path(__file__).with_suffix('.c')),'source_proof':json.loads((work/'source-proof.json').read_text()),'commands':commands,'scope':'Separate non-LTO actual math/classification source; no GS draws or whole-game timing.'},indent=2)+'\n');print(str(elf));return 0
 flags=['-std=gnu17','-O2','-g','-ffp-contract=off','-fwrapv','-Wall','-Wextra','-Werror','-Wno-unused-variable','-Wno-unused-function','-Wno-unused-parameter']
 if a.sanitize:flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie']
 cmd=['cc',*flags,'-I'+str(work),work/'base.c',work/'fast.c',Path(__file__).with_suffix('.c'),'-o',work/'test','-lm'];r=subprocess.run(list(map(str,cmd)),capture_output=True,text=True);(work/'build.log').write_text(r.stdout+r.stderr)
 if r.returncode:raise RuntimeError(r.stdout+r.stderr)
 r=subprocess.run([str(work/'test')],capture_output=True,text=True);(work/'test.log').write_text(r.stdout+r.stderr);passed=r.returncode==0 and 'failures=0' in r.stdout and 'runtime error:' not in r.stderr
 (work/'results.json').write_text(json.dumps({'passed':passed,'returncode':r.returncode,'sanitize':a.sanitize,'source_proof':json.loads((work/'source-proof.json').read_text()),'output':r.stdout,'stderr':r.stderr,'native':False},indent=2)+'\n');print(r.stdout+r.stderr);return int(not passed)
if __name__=='__main__':raise SystemExit(main())
