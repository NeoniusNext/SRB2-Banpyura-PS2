"""Audit the frozen bounded Z24-high-byte helper through a host GS packet boundary.

No emulator is started. The helper is read verbatim; negative controls mutate
only a temporary compiler input. All source files remain untouched.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
C = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;typedef uint64_t u64;
typedef union{u64 d[2];u32 w[4];}qw_t;
typedef struct{u64 test,alpha,clamp,tex0,tex1,zbuf,frame,scissor,fogcol,rgbaq;int use_tex,use_fog;}gsstate_t;
#define PSM_CT32 0
#define ZPSM_32 0
#define PRIM_SPRITE 6
#define GIF_REGS_XYZ 5
#define RV_RGBAQ 512
#define GSR_FOG 0x0a
#define HWGIF_TAG(n,e,p,pr,f,r)((u64)(n)|((u64)(e)<<15)|((u64)(p)<<46)|((u64)(pr)<<47)|((u64)(f)<<58)|((u64)(r)<<60))
static struct{int fbh,fbw,vw,vh,target;u32 z_addr,sciss_x0,sciss_y0,sciss_x1,sciss_y1;struct{u32 valid;}gsr;}H;
static struct{int n;}OV;
static int tile_stencil_active;static u32 tile_stencil_zor,tile_stencil_rectangles,tile_stencil_mask_sprites;
static gsstate_t current,states[8192];static qw_t packets[8192*3],fog;
static u32 z[640*544],low[640*544],original[640*544];static unsigned count,done,flushes;static unsigned long long checks;
#define CHECK(x)do{checks++;if(!(x)){fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void ov_flush(void){CHECK(OV.n);OV.n=0;flushes++;}
static u64 scissor_reg(int a,int b,int c,int d){return(u64)a|((u64)c<<16)|((u64)b<<32)|((u64)d<<48);}
static u64 scissor_current(void){return scissor_reg(H.sciss_x0,H.sciss_y0,H.sciss_x1,H.sciss_y1);}
static u64 rgbaq_of(u32 c){return(u64)c|((u64)0x3f800000u<<32);}
static u64 frame_reg(int target,u32 mask){return(u64)target|((u64)mask<<32);}
static u64 zbuf_reg(int mask){return(u64)(H.z_addr/8192u)|((u64)(tile_stencil_active?0:1)<<24)|((u64)mask<<32);}
static void state_flush(const gsstate_t*s){current=*s;H.gsr.valid|=RV_RGBAQ;}
static qw_t*pk_alloc(u32 n){CHECK(n==3&&count<8192);states[count]=current;return &packets[count++*3];}
static qw_t*ad_alloc(u32 n){CHECK(n==1);return &fog;}
static void ad_set(qw_t*p,u64 v,u32 r){p->d[0]=v;p->d[1]=r;}
/* ACTUAL_HELPER */
static void drain(void){
 int rows=(H.fbh+31)&~31;
 for(;done<count;done++){
  qw_t*p=&packets[done*3];gsstate_t*s=&states[done];
  CHECK(p[0].d[0]==HWGIF_TAG(2,1,1,PRIM_SPRITE,0,1)&&p[0].d[1]==GIF_REGS_XYZ);
  CHECK(s->frame==((u64)(H.z_addr/8192u)|((u64)(H.fbw/64)<<16)|(0xffffffull<<32)));
  CHECK(s->zbuf==((u64)(H.z_addr/8192u)|(1ull<<32))&&s->test==((1ull<<16)|(1ull<<17)));
  CHECK(s->scissor==scissor_reg(0,0,H.fbw-1,rows-1)&&!s->use_tex&&!s->use_fog);
  int x0=(int)p[1].w[0]/16-2048,y0=(int)p[1].w[1]/16-2048,x1=(int)p[2].w[0]/16-2048,y1=(int)p[2].w[1]/16-2048;
  CHECK(x0>=0&&x1<=H.fbw&&y0>=0&&y1<=rows&&x0<x1&&y0<y1);
  for(int y=y0;y<y1;y++)for(int x=x0;x<x1;x++){
   int at=(y^16)*H.fbw+(x^32);CHECK(at>=0&&at<H.fbw*rows);z[at]=(z[at]&0xffffffu)|((u32)s->rgbaq&0xff000000u);
  }
 }
 CHECK(tile_stencil_mask_sprites==count);
}
static void expected(int x0,int y0,int x1,int y1,int pass,int ended){
 int rows=(H.fbh+31)&~31,a=tile_stencil_region.clear_x0,b=tile_stencil_region.clear_y0,c=tile_stencil_region.clear_x1,d=tile_stencil_region.clear_y1;
 for(int y=0;y<rows;y++)for(int x=0;x<H.fbw;x++){
  int at=y*H.fbw+x;u32 high=original[at]>>24;
  if(x>=a&&x<c&&y>=b&&y<d)high=ended?0:4;
  if(pass&&x>=x0&&x<x1&&y>=y0&&y<y1)high=0;
  CHECK((z[at]&0xffffffu)==low[at]);CHECK((z[at]>>24)==high);
 }
 CHECK(H.sciss_x0==7&&H.sciss_y0==11&&H.sciss_x1==41&&H.sciss_y1==23);
}
static void one(int w,int h,int x0,int y0,int x1,int y1){
 H.fbw=H.vw=w;H.fbh=H.vh=h;H.z_addr=0x240000u;H.target=1;H.sciss_x0=7;H.sciss_y0=11;H.sciss_x1=41;H.sciss_y1=23;
 count=done=flushes=0;tile_stencil_rectangles=tile_stencil_mask_sprites=0;tile_stencil_active=0;tile_stencil_zor=0;tile_stencil_reset_region();
 int rows=(h+31)&~31;
 for(int i=0;i<w*rows;i++){low[i]=(u32)(i*7919u)&0xffffffu;z[i]=original[i]=low[i]|((u32)(0x70+(i&15))<<24);}
 OV.n=1;tile_stencil_begin(x0,y0,x1,y1);CHECK(count==1&&tile_stencil_active&&tile_stencil_zor==0);drain();
 CHECK(tile_stencil_region.clear_x0<=x0&&tile_stencil_region.clear_x1>=x1&&tile_stencil_region.clear_y0<=y0&&tile_stencil_region.clear_y1>=y1);
 CHECK(!(tile_stencil_region.clear_x0&63)&&!(tile_stencil_region.clear_x1&63)&&!(tile_stencil_region.clear_y0&31)&&!(tile_stencil_region.clear_y1&31));
 expected(x0,y0,x1,y1,0,0);
 tile_stencil_select(x0,y0,x1,y1);drain();expected(x0,y0,x1,y1,1,0);
 for(int y=y0;y<y1;y++)for(int x=x0;x<x1;x++)z[y*w+x]|=0x01000000u;
 OV.n=1;tile_stencil_before_pass(1);drain();CHECK(tile_stencil_zor==0x01000000u);expected(x0,y0,x1,y1,1,0);
 // Model source failure after selection: cleanup clears all touched high bytes, only inside the saved bounded pages.
 unsigned n=count;OV.n=1;tile_stencil_end();drain();CHECK(count==n+1&&!tile_stencil_active&&!tile_stencil_zor&&!tile_stencil_region.selected);
 expected(x0,y0,x1,y1,0,1);CHECK(current.frame==frame_reg(H.target,0)&&current.zbuf==zbuf_reg(1)&&current.scissor==scissor_current());
 CHECK(fog.d[0]==255ull<<56&&fog.d[1]==GSR_FOG&&!H.gsr.valid);
 n=count;tile_stencil_end();CHECK(count==n);
 int area=(tile_stencil_region.clear_x1-tile_stencil_region.clear_x0)*(tile_stencil_region.clear_y1-tile_stencil_region.clear_y0);
 printf("BOUNDED STENCIL %dx%d bbox=%d,%d,%d,%d endpoint_pixels=%d old=%d sprites=1 failure_cleanup=exact\n",w,h,x0,y0,x1,y1,area,w*rows);
}
int main(void){
 one(640,480,100,100,120,120);one(640,512,639,511,640,512);one(320,300,0,0,1,1);one(320,300,17,13,319,299);one(640,480,0,0,640,480);
 count=done=0;OV.n=0;tile_stencil_begin(5,7,5,9);CHECK(!count&&!tile_stencil_active);tile_stencil_end();CHECK(!count);
 printf("BOUNDED STENCIL COMPLETE checks=%llu\n",checks);return 0;
}
'''

def variants(source):
    bounded = ('tile_stencil_sprite(tile_stencil_region.clear_x0, tile_stencil_region.clear_y0,\n'
               '\t\ttile_stencil_region.clear_x1, tile_stencil_region.clear_y1);')
    fullscreen = 'tile_stencil_sprite(0, 0, H.fbw, (H.fbh + 31) & ~31);'
    assert source.count(bounded) == 2
    yield 'normal', source, True
    yield 'negative-fullscreen-begin', source.replace(bounded, fullscreen, 1), False
    last = source.rindex(bounded)
    yield 'negative-fullscreen-end', source[:last] + fullscreen + source[last + len(bounded):], False
    changed = source.replace('clear_x0 = x0 & ~63;', 'clear_x0 = x0;').replace('clear_y0 = y0 & ~31;', 'clear_y0 = y0;')
    yield 'negative-unrounded-origin', changed, False


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source-root', type=Path, default=ROOT)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--expect-helper-sha256')
    a = ap.parse_args()
    helper_path = a.source_root.resolve() / 'src/ps2/hw/ps2_hw_tile_stencil.inc'
    out = a.out.resolve(); out.mkdir(parents=True, exist_ok=True)
    original = helper_path.read_text()
    digest = hashlib.sha256(original.encode()).hexdigest()
    if a.expect_helper_sha256 and digest != a.expect_helper_sha256:
        raise RuntimeError('helper hash mismatch: ' + digest)
    rows = []
    for name, helper, expected in variants(original):
        with tempfile.TemporaryDirectory(prefix='nov17-bounded-stencil-') as tmp:
            binary = str(Path(tmp) / 'test')
            command = ['cc', '-std=gnu17', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                       '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie', '-x', 'c', '-', '-o', binary]
            built = subprocess.run(command, input=C.replace('/* ACTUAL_HELPER */', helper), text=True, capture_output=True)
            if built.returncode:
                raise RuntimeError(built.stdout + built.stderr)
            ran = subprocess.run([binary], text=True, capture_output=True)
            output = ran.stdout + ran.stderr
            passed = ran.returncode == 0 and 'BOUNDED STENCIL COMPLETE' in output and 'runtime error:' not in output
            observed_failure = re.search(r'^FAIL .+$', output, re.M)
            valid = passed if expected else ran.returncode == 1 and bool(observed_failure) and 'runtime error:' not in output
            log = out / ('bounded-stencil-' + name + '.log')
            log.write_text(output)
            rows.append({'variant': name, 'sanitizers': ['address', 'undefined'], 'expected_pass': expected,
                         'passed': passed, 'matched_expectation': valid, 'returncode': ran.returncode,
                         'failure': observed_failure.group(0) if observed_failure else None,
                         'tested_helper_sha256': hashlib.sha256(helper.encode()).hexdigest(), 'log': str(log)})
    normal_log = Path(rows[0]['log']).read_text()
    checks = int(re.search(r'COMPLETE checks=(\d+)', normal_log).group(1))
    report = {'helper': str(helper_path), 'helper_sha256': digest, 'fixture': str(Path(__file__).resolve()),
              'fixture_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'checks': checks, 'all_matched_expectation': all(row['matched_expectation'] for row in rows), 'results': rows,
              'compiler': subprocess.check_output(['cc', '--version'], text=True).splitlines()[0],
              'reproduce': 'python tools/ps2/opt17/tile_stencil_hosttest.py --out build/tile-stencil-host',
              'source_edits': False, 'emulator_used': False,
              'scope': ['verbatim frozen helper plus mocked GS packet/state services',
                        'page-rounded begin/end each emit exactly one CT32 sprite',
                        'XOR32/XOR16 aliases stay inside the same 64x32 pages',
                        'all low24 bits and every word outside the saved pages remain exact',
                        'end after a partial selected run clears the same saved pages and restores ordinary Z24/scissor',
                        'empty begin and repeated end are no-ops'],
              'coverage_precondition': 'Caller passes its conservative half-open bbox from original packed GS vertices, clipped to original scissor; all run rectangles lie inside it. Native coverage/parity rerun is owned by the parent.'}
    report_path = out / 'result.json'
    report_path.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'report': str(report_path), 'helper_sha256': digest, 'checks': checks,
                      'all_matched_expectation': report['all_matched_expectation']}, indent=2))
    return int(not report['all_matched_expectation'])

if __name__ == '__main__':
    raise SystemExit(main())
