#!/usr/bin/env python3
"""Build only: standalone EE scan/fill probe from literal isolated source."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[2]
sys.path.insert(0,str(HERE));sys.path.insert(0,str(HERE.parent))
from packed_patch_hosttest import make_source
import build as B
import cook


def run(cmd,log):
    r=subprocess.run(list(map(str,cmd)),capture_output=True,text=True)
    log.write_text(' '.join(map(str,cmd))+'\n'+r.stdout+r.stderr)
    if r.returncode:raise RuntimeError(str(log)+'\n'+r.stderr[-5000:])


NATIVE=r'''
#include <kernel.h>
static u32 ticks(void){u32 v;__asm__ volatile("mfc0 %0,$9":"=r"(v)::"memory");return v;}
static volatile u32 timing_sink;
static u32 timed(GLMipmap_t*m,u32 cw,u32 ch,u32 dx,u32 dy,u32 w,u32 h,unsigned phase,unsigned iterations,u8*out){
 selection_t s=select_patch(m,cw,ch);s.c.src=m->data;s.c.sw=m->width;s.c.sh=m->height;s.c.dx=dx;s.c.dy=dy;s.c.W2=w;s.c.H2=h;
 selection_t(*volatile choose)(GLMipmap_t*,u32,u32)=select_patch;rowfill_t volatile fill=s.fill;
 u32 sum=0,start=ticks();
 for(unsigned i=0;i<iterations;i++){
  if(phase!=1){selection_t q=choose(m,cw,ch);sum+=(u32)(q.psm+q.clut+q.baked+q.holes);}
  if(phase!=0)for(u32 y=0;y<h;y++){size_t bytes=(size_t)w*(s.psm==PSM_T8?1u:4u);u8*row=out+(size_t)y*bytes;fill(&s.c,y,row);sum+=row[0]+row[bytes-1u];}
 }
 u32 elapsed=ticks()-start;timing_sink=sum;return elapsed;
}
int main(int argc,char**argv){
 (void)argc;(void)argv;
 for(unsigned i=0;i<256;i++)H.pal[i]=i|(i<<8)|(i<<16);
 for(unsigned test=0;test<sizeof input_data/sizeof*input_data;test++){
  patch_t*p=Patch_CreateFromDoomPatch((softwarepatch_t*)input_data[test]);GLMipmap_t packed={0};GLPatch_t g={0};
  HWR_MakePatch(p,&g,&packed,true);GLMipmap_t raw=packed;raw.data=NULL;MakeBlock(&raw);HWR_DrawPatchInCache(&raw,raw.width,raw.height,p->width,p->height,p);
  check(compare_ap88(&packed,raw.data),"native complete source decode");
  const size_t outcap=(size_t)packed.width*packed.height*4;
  u8*out=memalign(64,outcap*2+128);check(out!=NULL,"native contiguous upload output");u8*expected=out+outcap+64;
  printf("PATCH17 EXACT case=%u w=%u h=%u logical=%ux%u oldbytes=%u newbytes=%u\n",test,(unsigned)p->width,(unsigned)p->height,(unsigned)packed.width,(unsigned)packed.height,(unsigned)blocks[block_of(raw.data)].n,(unsigned)blocks[block_of(packed.data)].n);
  for(unsigned view=0;view<2;view++)for(unsigned cache=0;cache<2;cache++)for(unsigned phase=0;phase<3;phase++){
   u32 cw=view?packed.width:(u32)p->width,ch=view?packed.height:(u32)p->height;
   u32 dx=view?(packed.width>=4?2:0):0,dy=view?(packed.height>=4?2:0):0;
   u32 w=view?(u32)packed.width>>dx:(u32)p->width,h=view?(u32)packed.height>>dy:(u32)p->height;
   unsigned iterations=131072u/(cw*ch);if(!iterations)iterations=1;if(iterations>64)iterations=64;
   check_upload(&packed,&raw,cw,ch,dx,dy,w,h);
   if(!cache&&!phase){
    selection_t a=select_patch(&packed,cw,ch),b=select_patch(&raw,cw,ch);a.c.src=packed.data;b.c.src=raw.data;a.c.sw=b.c.sw=packed.width;a.c.sh=b.c.sh=packed.height;a.c.dx=b.c.dx=dx;a.c.dy=b.c.dy=dy;a.c.W2=b.c.W2=w;a.c.H2=b.c.H2=h;
    size_t rowbytes=(size_t)w*(a.psm==PSM_T8?1u:4u);for(u32 y=0;y<h;y++){a.fill(&a.c,y,out+(size_t)y*rowbytes);b.fill(&b.c,y,expected+(size_t)y*rowbytes);}
    check(!memcmp(out,expected,rowbytes*h),"native complete contiguous upload including unaligned row starts");printf("PATCH17 VIEWEXACT case=%u view=%u bytes=%u\n",test,view,(unsigned)(rowbytes*h));
   }
   for(unsigned rep=0;rep<3;rep++){
    u32 values[2];for(unsigned order=0;order<2;order++){
     unsigned variant=order^(rep&1u);if(cache){FlushCache(0);FlushCache(1);FlushCache(2);}
     values[variant]=timed(variant?&packed:&raw,cw,ch,dx,dy,w,h,phase,iterations,out);
    }
    printf("PATCH17 TIME case=%u view=%u cache=%u phase=%u iterations=%u rep=%u old=%u new=%u\n",test,view,cache,phase,iterations,rep,(unsigned)values[0],(unsigned)values[1]);
   }
  }
  free(out);Z_Free(raw.data);Z_Free(packed.data);Patch_Free(p);
 }
 printf("PATCH17 EE DONE sink=%u\n",(unsigned)timing_sink);return 0;
}
'''


def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--out',type=Path,required=True);ap.add_argument('--source-root',type=Path,default=ROOT/'build/nov17-sprite-cache/source');a=ap.parse_args()
    out=a.out.resolve();out.mkdir(parents=True,exist_ok=False);root=a.source_root.resolve()
    rows=json.loads((ROOT/'build/nov17-sprite-audit-v2/rows.json').read_text())['patches']
    chosen=[next(r for r in rows if r['pack']=='CHARS.PAK' and 32<=r['width']<=128 and 32<=r['height']<=128),next(r for r in rows if r['name']=='Sprites/Scenery/Deep Sea/CORLE0'),next(r for r in rows if r['name']=='Sprites/Scenery/Pristine Shores/PPALA0')]
    inputs=[];arrays=[]
    for k,r in enumerate(chosen):
        p=cook.PackReader(ROOT/'build/pak'/r['pack']);data=p.get(r['index']);assert not data.startswith(b'\x89SRPIC')
        arrays.append('static const unsigned char input_%u[] __attribute__((aligned(16)))={'%k+','.join(map(str,data))+'};\n')
        inputs.append({'name':r['name'],'pack':r['pack'],'sha256':hashlib.sha256(data).hexdigest(),'bytes':len(data)})
    arrays.append('static const unsigned char*input_data[]={'+','.join('input_%u'%k for k in range(len(chosen)))+'};\n')
    body=make_source(root);body=body[:body.rindex('int main(int argc,char**argv){')]
    body=body.replace('#include <limits.h>','#include <limits.h>\n#include <malloc.h>\n#include <tamtypes.h>')
    body=body.replace('typedef uint32_t UINT32;','typedef unsigned int UINT32;').replace('typedef int32_t INT32;','typedef int INT32;')
    body=body.replace('typedef enum {false,true} boolean;','#include <stdbool.h>\ntypedef bool boolean;')
    body=body.replace('typedef UINT8 u8;typedef UINT16 u16;typedef UINT32 u32;typedef UINT64 u64;','')
    body=body.replace('void *p=NULL;check(posix_memalign(&p,64,n?n:1)==0,"host backing allocation");','void *p=memalign(64,n?n:1);check(p!=NULL,"native backing allocation");')
    c=out/'native.c';c.write_text(body+'\n_Static_assert(sizeof(GLMipmap_t)==68&&sizeof(fillctx_t)==40&&sizeof(column_t)==12&&sizeof(post_t)==12,"actual timed EE ABI");\n'+''.join(arrays)+NATIVE)
    flags=[f for f in B.CFLAGS if f not in ('-MMD','-MP','-O2','-G0','-DNOHW','-std=gnu23')]+['-std=gnu17','-O3','-G8','-flto','-DHWRENDER','-DROTSPRITE','-Wno-unused-function','-Wno-unused-variable','-Wno-unused-parameter','-Wno-misleading-indentation','-Wno-clobbered']
    obj=out/'native.o';run([B.CC,*flags,*B.INCS,'-c',c,'-o',obj],out/'build.log')
    merged=out/'native-lto.o';run([B.CC,'-r','-nostdlib','-flto','-flinker-output=nolto-rel','-O3','-G8','-mno-abicalls','-ffunction-sections','-fdata-sections',obj,'-o',merged],out/'lto.log')
    elf=out/'PATCH17.ELF';run([B.CC,*B.LDFLAGS,merged,'-Wl,-Map='+str(out/'PATCH17.map'),'-o',elf,'-ldebug','-lpatches','-lm'],out/'link.log')
    report={'scope':'Standalone -O3 -G8 LTO source probe, scan/row fill CPU procedures only. Not shipping LTO binding, zone allocator timing, DMA/GS/frame timing, or physical PS2 measurement. Built only; execution requires coordinator grant.','inputs':inputs,'source_sha256':hashlib.sha256(c.read_bytes()).hexdigest(),'elf_sha256':hashlib.sha256(elf.read_bytes()).hexdigest(),'flags':flags}
    (out/'build-report.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))


if __name__=='__main__':main()
