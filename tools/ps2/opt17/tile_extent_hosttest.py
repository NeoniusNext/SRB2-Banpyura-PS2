#!/usr/bin/env python3
"""Prove 64/128 tile conversion and loader layout against a frozen V7 reference.

The checked-in C fixture contains only the required old conversion functions.
The candidate, alpha helper and packed accessor are extracted from --source-root.
No candidate source is edited and no emulator is started.
"""
from pathlib import Path
import argparse, hashlib, importlib.util, json, re, subprocess
ROOT=Path(__file__).resolve().parents[3]
HERE=Path(__file__).resolve().parent
FROZEN_V7_TILE_SHA256='47e41c5147b7a0a202e88fa410dea26b579adcfddd8f63745e6e1a74938a5d8d'
FROZEN_REFERENCE_SHA256='3003eb7c4bd4767de189f46044f151ae061608c8a35cdb0a923ba96e0ce99df0'
FROZEN_CACHE_GET_SHA256='eb9379f2ed5f6915f3a66bd697a255adb20ee64223b01a5a3d87d6a5fe203db1'

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source-root',type=Path,default=ROOT,help='Candidate source tree (default: current repository).')
    ap.add_argument('--out',type=Path,required=True)
    ap.add_argument('--sanitize',action='store_true')
    ap.add_argument('--negative',action='store_true',help='Corrupt candidate 128 columns only; 64 regressions must remain exact.')
    a=ap.parse_args()
    source=a.source_root.resolve();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
    candidate=source/'src/ps2/hw/ps2_hw_tile.inc';fixture=HERE/'tile_extent_hosttest.c'
    frozen_text=fixture.read_text()
    b=frozen_text.split('/* TILE_EXTENT_FROZEN_V7_BEGIN */\n',1)[1].split('/* TILE_EXTENT_FROZEN_V7_END */',1)[0]
    if hashlib.sha256(b.encode()).hexdigest()!=FROZEN_REFERENCE_SHA256:raise RuntimeError('Frozen V7 conversion reference hash mismatch')
    spec=importlib.util.spec_from_file_location('extract',ROOT/'tools/ps2/opt17/highres_state_hosttest.py');extract=importlib.util.module_from_spec(spec);spec.loader.exec_module(extract)
    fn=extract.function;c=candidate.read_text();hdr=(source/'src/hardware/hw_data.h').read_text();tex_text=(candidate.parent/'ps2_hw_tex.inc').read_text()
    if not re.search(r'^#define TILE_SIZE 64$',c,re.M) or not re.search(r'^#define TILE_MAX_SIZE 128$',c,re.M):raise RuntimeError('Candidate must use fixed64 / maximum128 extent policy')
    if hashlib.sha256(fn(c,'tile_cache_get').encode()).hexdigest()!=FROZEN_CACHE_GET_SHA256:raise RuntimeError('Fixed64-block cache allocator changed')
    start=hdr.index('typedef enum GLTextureFormat_e');end=hdr.index('} GLTextureFormat_t;',start)+len('} GLTextureFormat_t;')
    common='''#include <stdint.h>
    #include <stddef.h>
    #include <string.h>
    typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;
    typedef uint8_t UINT8;typedef uint16_t UINT16;typedef uint32_t UINT32;typedef int boolean;
    #define NOREC (-1)
    #define TILE_SIZE 64
    #define TILE_MAX_SIZE 128
    #define PSM_T8 19
    #define PSM_CT32 0
    #define PSM_CT16S 10
    #define TF_CHROMAKEYED 1
    '''+hdr[start:end]+'''
    typedef struct {int width,height,format,flags,packed,regen_kind;u8 ps2_cpu;u16 ps2_uw,ps2_uh;const u8*data;}GLMipmap_t;
    typedef struct {GLMipmap_t*owner;int w,h,screen,psm;const u32*tile_palette;void*tile_source;
     int blk,nblk,tiled,uw,vh,sw,sh,tw,th,tbw,region,npot_x,npot_y,wrapx,wrapy;float sxs,sys;}texrec_t;
    typedef struct {GLMipmap_t m;texrec_t r;const u8*data;int x,y,wrapx,wrapy,region,fbw,fb32;u32 pal[256];}input_t;
    void reference_fill(const input_t*,u8*);void frozen_fill(const input_t*,u8*);void candidate_fill(const input_t*,u8*);
    u32 candidate_plan(const input_t*,u32*,u32*);int candidate_extent(const input_t*);int layout_probe(const input_t*);
    '''
    (out/'common.h').write_text(common)
    packed_prefix='#define HWR_PS2_PACKED_PATCHES 1\n#define HWR_PS2_CPU_PATCH 1u\n'
    packed=packed_prefix+fn(hdr,'HWR_PS2_IsPackedPatch')+fn(hdr,'HWR_PS2_ReadAP88')
    packed_frozen=packed_prefix+fn(b,'HWR_PS2_IsPackedPatch')+fn(b,'HWR_PS2_ReadAP88')
    state='''static struct {int fbw,fb32;u32 pal[256];void*scr_data[8];texrec_t rec[1];u32 frame_no,pal_gen;struct{u32 uploads,upload_bytes;}st;}H;
    static u32 a8_to_gs(u32 a){return(a+1u)/2u;}
    '''
    state=state.replace('static u32 a8_to_gs(u32 a){return(a+1u)/2u;}\n',fn(tex_text,'a8_to_gs'))
    state_frozen=state.replace(fn(tex_text,'a8_to_gs'),fn(b,'a8_to_gs'))
    def ctx(text):
     start=text.index('typedef struct\n{',text.index('static int tile_coord('));end=text.index('} tile_fill_t;',start)+len('} tile_fill_t;');return text[start:end]
    def init():return 'tile_fill_t v;memset(&v,0,sizeof v);v.source=i->r;v.source.owner=(GLMipmap_t*)&i->m;v.data=i->data;v.x=i->x;v.y=i->y;v.wrapx=i->wrapx;v.wrapy=i->wrapy;v.region=i->region;H.fbw=i->fbw;H.fb32=i->fb32;memcpy(H.pal,i->pal,sizeof H.pal);'
    def wrap(name,extent,prepare):return '\nvoid '+name+'(const input_t*i,u8*out){'+init()+'int side='+extent+';'+prepare+'for(u32 y=0;y<(u32)side;y++)tile_fill(&v,y,out+(size_t)y*side*4);}\n'
    # Frozen V7 scalar conversion is retained; only its source-coordinate calculation is independently widened.
    row=fn(b,'tile_fill');at=row.index('\tif (c->run_count)');brace=row.index('{',at);depth=1;last=brace+1
    while depth:
     depth+=(row[last]=='{')-(row[last]=='}');last+=1
    row=row[:at]+row[last:]
    row=row.replace('const GLMipmap_t *m = r->owner;','const GLMipmap_t *m = r->owner;\n const int side=c->region&&r->psm==PSM_T8&&!r->screen?128:64;',1)
    row=row.replace('TILE_SIZE','side')
    row=row.replace('const int sx = c->sx[x];','''int dx=x-1;
            if(c->region){dx=(x-(c->x&(side-1))+2*side+side/4)&(side-1);dx-=side/4;}
            const int sx=tile_coord(c->x+dx,r->w,c->wrapx);''',1)
    refctx='typedef struct{texrec_t source;const u8*data;int x,y,wrapx,wrapy,region;}tile_fill_t;\n'
    reference='#include "common.h"\n'+state_frozen+packed_frozen+fn(b,'tile_coord')+refctx+row+wrap('reference_fill','i->region&&i->r.psm==PSM_T8&&!i->r.screen?128:64','')
    (out/'reference.c').write_text(reference)
    frozen='#include "common.h"\n'+state_frozen+packed_frozen+fn(b,'tile_coord')+ctx(b)+'\n'+fn(b,'tile_fill_columns')+fn(b,'tile_fill')+wrap('frozen_fill','64','tile_fill_columns(&v);')
    (out/'frozen.c').write_text(frozen)
    prep=fn(c,'tile_fill_columns')
    if a.negative:
     anchor='c->sx[x] = tile_coord(c->x + dx,'
     if anchor not in prep:raise RuntimeError('Candidate128 negative anchor missing')
     prep=prep.replace(anchor,'c->sx[x] = tile_coord(c->x + dx + (extent == 128),',1)
    fast='#include "common.h"\n'+state+packed+fn(c,'tile_general_extent')+fn(c,'tile_coord')+ctx(c)+'\n'+prep+fn(c,'tile_fill')+wrap('candidate_fill','i->region?tile_general_extent(&v.source):64','tile_fill_columns(&v);')
    fast+='''
    int candidate_extent(const input_t*i){return i->region?tile_general_extent(&i->r):64;}
    u32 candidate_plan(const input_t*i,u32*columns,u32*plan){'''+init()+'''
     tile_fill_columns(&v);int side=i->region?tile_general_extent(&v.source):64;
     for(int x=0;x<side;x++)columns[x]=(u32)v.sx[x];
     for(u32 r=0;r<v.run_count;r++){plan[r*4]=v.runs[r].sx;plan[r*4+1]=v.runs[r].dst;plan[r*4+2]=v.runs[r].count;plan[r*4+3]=v.runs[r].same;}
     return v.run_count;
    }
    '''
    # Actual loader/cache-key code with only allocator and synchronous upload boundaries stubbed.
    cache_start=c.index('static struct',c.index('static int tile_cache_rec'));cache_end=c.index('} tile_cache;',cache_start)+len('} tile_cache;')
    fast+=c[cache_start:cache_end]+'''
    typedef struct{const u8*p;u8 busy;}dcent_t;
    static unsigned locks,unlocks,image_calls,image_w,image_h,image_bpp;
    static u8 image[16384];
    static dcent_t*dc_find(u32 k,int w,int h,int l){(void)k;(void)w;(void)h;(void)l;return NULL;}
    static u32 dc_key(const GLMipmap_t*m){(void)m;return 0;}
    static void HWR_PS2_RegenerateMipmap(GLMipmap_t*m){(void)m;}
    static void HWR_PS2_LockData(void*p){(void)p;locks++;}
    static void HWR_PS2_UnlockData(void*p){(void)p;unlocks++;}
    static void dma_fence(void){}
    static int tile_cache_get(void){H.rec[0].blk=77;H.rec[0].nblk=64;return 0;}
    static int log2_u32(u32 v){int n=0;while((1u<<n)<v)n++;return n;}
    static void upload_bands(u32 blk,u32 tbw,u32 psm,u32 w,u32 h,u32 bpp,void(*fill)(void*,u32,u8*),void*arg){
     (void)blk;(void)tbw;(void)psm;image_calls++;image_w=w;image_h=h;image_bpp=bpp;
     if((size_t)w*h*bpp>sizeof image)__builtin_trap();
     for(u32 y=0;y<h;y++)fill(arg,y,image+(size_t)y*w*bpp);
    }
    '''+fn(c,'tile_source_unlock')+fn(c,'tile_load_impl')+'''
    int layout_probe(const input_t*i){
     texrec_t r=i->r,out;int good=1;r.owner=(GLMipmap_t*)&i->m;
     H.fbw=i->fbw;H.fb32=i->fb32;H.scr_data[0]=(void*)i->data;H.frame_no++;H.pal_gen++;memcpy(H.pal,i->pal,sizeof H.pal);
     tile_cache.valid=0;H.st.uploads=H.st.upload_bytes=0;locks=unlocks=image_calls=0;
     for(int step=0;step<5;step++){
      int region=step==1||step==2,side=region?tile_general_extent(&r):64;
      unsigned expected_calls=step==0?1:step==1?2:3;
      if(step==2)expected_calls=2;
      if(step==4)expected_calls=3;
      if(!tile_load_impl(&r,i->x,i->y,i->wrapx,i->wrapy,region,&out))return 0;
      int layout=region|(side==128?2:0);
      if(out.w!=side||out.h!=side||out.tw!=log2_u32(side)||out.th!=log2_u32(side)||out.tbw!=(r.psm==PSM_T8?2:1)||out.nblk!=64||tile_cache.region!=layout)good=0;
      if(image_w!=(unsigned)side||image_h!=(unsigned)side||image_calls!=expected_calls)good=0;
     }
     unsigned bpp=r.psm==PSM_T8?1:r.screen&&!H.fb32?2:4;
     int general=tile_general_extent(&r);
     if(H.st.upload_bytes!=(u32)(2*64*64+general*general)*bpp||H.st.uploads!=3||image_bpp!=bpp||locks!=unlocks)good=0;
     return good;
    }
    '''
    (out/'candidate.c').write_text(fast)
    fixture=HERE/'tile_extent_hosttest.c'
    command=['cc','-std=gnu17','-O2','-g','-fwrapv','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-I'+str(out),str(out/'reference.c'),str(out/'frozen.c'),str(out/'candidate.c'),str(fixture),'-o',str(out/'test')]
    if a.sanitize:command[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie']
    r=subprocess.run(command,text=True,capture_output=True);(out/'build.log').write_text(r.stdout+r.stderr)
    if r.returncode:print(r.stdout+r.stderr);raise SystemExit(2)
    r=subprocess.run([str(out/'test')],text=True,capture_output=True);(out/'test.log').write_text(r.stdout+r.stderr);print(r.stdout+r.stderr)
    totals=re.search(r'DYN COMPLETE checks=(\d+) cases64=(\d+) cases128=(\d+) failures=(\d+) regression64_failures=(\d+) compared_bytes=(\d+).*loader_checks=(\d+)',r.stdout)
    exact_counts=totals is not None and tuple(int(totals[i]) for i in [1,2,3,6,7])==(12672,9856,2816,346030080,198)
    passed=r.returncode==0 and exact_counts and int(totals[4])==0 and int(totals[5])==0 and not r.stderr
    matched=(r.returncode==1 and exact_counts and int(totals[4])>0 and int(totals[5])==0 and 'DYN FAIL' in r.stdout and not r.stderr) if a.negative else passed
    sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
    report={'candidate_sha256':hashlib.sha256(c.encode()).hexdigest(),'candidate':str(candidate),'source_root':str(source),
     'runner_sha256':sha(Path(__file__)),'fixture_sha256':sha(fixture),'extraction_helper_sha256':sha(HERE/'highres_state_hosttest.py'),
     'frozen_v7_tile_provenance_sha256':FROZEN_V7_TILE_SHA256,'frozen_public_reference':str(fixture),
     'frozen_reference_sha256':FROZEN_REFERENCE_SHA256,'frozen_cache_get_sha256':FROZEN_CACHE_GET_SHA256,
     'actual_packed_header_sha256':hashlib.sha256(hdr.encode()).hexdigest(),'actual_alpha_helper_source_sha256':hashlib.sha256(tex_text.encode()).hexdigest(),
     'sanitized':a.sanitize,'negative':a.negative,'passed':passed,'matched_expectation':matched,'returncode':r.returncode,
     'checks':int(totals[1]) if totals else None,'regression64_cases':int(totals[2]) if totals else None,
     'extent128_cases':int(totals[3]) if totals else None,'compared_bytes':int(totals[6]) if totals else None,
     'loader_checks':int(totals[7]) if totals else None,'output':r.stdout,'stderr':r.stderr,'emulator_used':False,
     'physical_cache_function_unchanged':True,'candidate_edited':False,
     'reference':'Minimal frozen V7 converter and packed accessor; independent per-pixel128 roll; exact actual V7 row-run regression for64.',
     'scope':['actual candidate tile_coord/fill context/columns/fill/load_impl/source_unlock extracted verbatim except explicit negative',
     'real packed-AP88 header accessor; ALPHA8 and indexed-RGBA palette history; CT32/CT16 screens',
     '198 actual loader layout/cache-hit/byte-count/lock-balance checks; allocator and upload boundaries stubbed',
     'candidate-only128 negative preserves all9856 frozen64 regressions; native geometry is a separate gate']}
    
    (out/'result.json').write_text(json.dumps(report,indent=2)+'\n');return int(not matched)


if __name__=='__main__':
    raise SystemExit(main())
