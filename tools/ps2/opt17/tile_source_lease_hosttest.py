#!/usr/bin/env python3
"""Actual engine source release/Update with real zone pressure; GS selection stubbed."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

base = Path(__file__).resolve().parents[3]
ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--source-root', type=Path, default=base)
ap.add_argument('--out', type=Path, required=True)
ap.add_argument('--negative', action='store_true', help='omit engine lease-aware release guard')
ap.add_argument('--sanitize', action='store_true')
a = ap.parse_args()
source, work = a.source_root.resolve(), a.out.resolve()
work.mkdir(parents=True, exist_ok=True)
def function(text,name):
 m=re.search(r'^(?:static )?(?:inline )?\w+[ *]+'+re.escape(name)+r'\([^\n]*\)\n\{',text,re.M);start=text.index('{',m.start());depth=0
 for i in range(start,len(text)):
  depth+=(text[i]=='{')-(text[i]=='}')
  if not depth:return text[m.start():i+1]+'\n'
 raise RuntimeError(name)
hdr=(source/'src/hardware/hw_data.h').read_text();start=hdr.index('#if defined(PS2) && defined(PS2_PROFILE)\n#define HWR_PS2_CPU_TILED_SOURCE');end=hdr.index('#if defined(PS2) && defined(PS2_PROFILE) && !defined(PS2_NOOPT_PATCH_PIXELS)',start);macro=hdr[start:end]
driver=(source/'src/ps2/hw/ps2_hwd.c').read_text();engine=(source/'src/hardware/hw_cache.c').read_text();tile=(source/'src/ps2/hw/ps2_hw_tile.inc').read_text()
patch_lease = bool(re.search(r'^#define HWR_PATCH_UNLOCKED\(m\) HWR_RELEASE_MIPMAP_DATA', engine, re.M)) and 'static u32 tile_cpu_source_bytes(' in tile
patch_code = ''
if patch_lease:
 marker=engine.index('INT32 ps2hwt_patchtag')
 pstart=engine.rfind('#ifdef PS2_PROFILE',0,marker);pend=engine.index('#endif',marker)+len('#endif')
 patch_macro=engine[pstart:pend]+'\n'
 if a.negative:
  original='HWR_RELEASE_MIPMAP_DATA((m), ps2hwt_patchtag)'
  assert original in patch_macro
  patch_macro=patch_macro.replace(original,'Z_ChangeTag((m)->data, ps2hwt_patchtag)')
 enum_start=hdr.index('typedef enum GLTextureFormat_e');enum_end=hdr.index('} GLTextureFormat_t;',enum_start)+len('} GLTextureFormat_t;')
 packed_start=hdr.index('#define HWR_PS2_PACKED_PATCHES');packed_end=hdr.index('static inline boolean HWR_PS2_IsPackedPatch',packed_start)
 patch_code=hdr[enum_start:enum_end]+'\n'+hdr[packed_start:packed_end]+function(hdr,'HWR_PS2_IsPackedPatch')+function(tile,'tile_cpu_source_bytes')+patch_macro
elif a.negative:
 macro=macro.replace('if (!((m_)->ps2_cpu & HWR_PS2_CPU_TILED_SOURCE))','if (1)')
code='#include <stdint.h>\ntypedef uint8_t UINT8; static int netgame; int PS2Lua_InCall(void) { return 0; } void PS2Spill_Reset(void) {}\n#define max(a,b) ((a)>(b)?(a):(b))\n#define ZONE_HOST_SOURCE "'+str(source/'src/z_zone.c')+'"\n#define ZONE_MEM_SOURCE "'+str(source/'src/ps2/ps2_mem.c')+'"\n#define main zone_main_unused\n#include "'+str(base/'tools/ps2/zone_hosttest.c')+'"\n#undef main\n'
code+='''typedef uint8_t u8; typedef uint32_t u32; typedef uint8_t UINT8; typedef uint16_t UINT16; typedef unsigned lumpnum_t;
typedef struct { void *data; UINT32 downloaded; u8 ps2_cpu,format,regen_kind; UINT16 width,height,ps2_uw,ps2_uh; } GLMipmap_t;
typedef struct { GLMipmap_t *mipmap; } GLPatch_t;
typedef struct { void *hardware; } patch_t;
typedef struct { GLMipmap_t *owner; void *tile_source; UINT32 tile_source_bytes; u8 tiled; } texrec_t;
static GLMipmap_t mip; static GLPatch_t patch_hw; static patch_t patch; static int leasing;
#define LUMPERROR ((lumpnum_t)-1)
static patch_t *HWR_GetCachedGLPatch(lumpnum_t l) { (void)l; return &patch; }
static GLPatch_t *Patch_AllocateHardwarePatch(patch_t *p) { (void)p; return &patch_hw; }
static void HWR_CacheFadeMask(GLMipmap_t *m, lumpnum_t l) { (void)m; (void)l; check(0,"unexpected fade reload"); }
static void HWR_CacheRawFlat(GLMipmap_t *m, lumpnum_t l) { (void)m; (void)l; check(0,"unexpected raw reload"); }
static void HWR_SetCurrentTexture(GLMipmap_t *m) { (void)m; }
static void HWR_MakePatch(patch_t *p,GLPatch_t *g,GLMipmap_t *m,boolean b) { (void)p; (void)g; (void)m; (void)b; check(0,"unexpected canonical patch regeneration"); }
'''+macro+patch_code+function(engine,'HWR_PS2_LockData')+function(engine,'HWR_PS2_UnlockData')+function(tile,'tile_release_source')+'''
static void driver_set_texture(GLMipmap_t *m)
{
    if (leasing) { HWR_PS2_LockData(m->data); m->ps2_cpu |= HWR_PS2_CPU_TILED_SOURCE; }
    m->downloaded = 1;
}
static struct { void (*pfnSetTexture)(GLMipmap_t *); } HWD = {driver_set_texture};
'''+function(engine,'HWR_GetFadeMask')+function(engine,'HWR_GetRawFlat')+(function(engine,'HWR_LoadPatchMipmap')+function(engine,'HWR_UnlockCachedPatch') if patch_lease else '')+'''
static texrec_t update_record;
static struct { int up; texrec_t *rec; } H = {1,&update_record};
static texrec_t *rec_of(GLMipmap_t *m) { return m->downloaded ? &update_record : NULL; }
static void ov_flush_all(void) {}
static void dma_fence(void) {}
static void tex_drop(int index, int evict) { (void)index; (void)evict; tile_release_source(&update_record); mip.downloaded=0; }
static void settex_now(GLMipmap_t *m) { driver_set_texture(m); update_record.owner=m; update_record.tile_source=m->data; update_record.tile_source_bytes=4096; update_record.tiled=1; }
'''+function(driver,'hw_UpdateTexture')+('''
static void ap88_zone_cases(void)
{
    for(int packed=0;packed<=1;packed++) for(leasing=0;leasing<=1;leasing++)
    {
        arena_reset(8u<<20,0,0,0); Z_Init(); memset(&mip,0,sizeof mip);
        patch_hw.mipmap=&mip; patch.hardware=&patch_hw;
        mip.width=1024; mip.height=512; mip.format=GL_TEXFMT_AP_88; mip.regen_kind=0;
        if(packed) { mip.ps2_cpu=HWR_PS2_CPU_PATCH; mip.ps2_uw=513; mip.ps2_uh=259; }
        const size_t pixels=packed?(size_t)513*259:(size_t)1024*512;
        const size_t bytes=packed?pixels+(pixels+7)/8:pixels*2;
        check(tile_cpu_source_bytes(&mip)==bytes,"AP88 actual raw/packed CPU byte accounting");
        mip.data=Z_TryMallocAlign(bytes,PU_HWRCACHE,&mip.data,6); check(mip.data!=NULL,"AP88 actual byte allocation");
        for(size_t i=0;i<bytes;i++) ((unsigned char *)mip.data)[i]=(unsigned char)(i*37u+packed*91u);
        HWR_LoadPatchMipmap(&patch,&mip); HWR_UnlockCachedPatch(&patch_hw);
        check(mip.downloaded==1,"AP88 actual load did not call admission");
        // LRU patch releases retain their ordinary frame policy; age twice before pressure.
        Z_NextFrame(); Z_NextFrame();
        void *pressure=NULL; size_t request=Z_ArenaFree()+bytes/2;
        pressure=Z_TryMallocAlign(request,PU_STATIC,&pressure,6);
        if(!leasing)
        {
            check(pressure!=NULL && mip.data==NULL,"nonleased AP88 failed ordinary LRU pressure purge");
            Z_Free(pressure);
        }
        else
        {
            check(pressure==NULL && mip.data!=NULL,"AP88 actual patch caller retagged retained source");
            for(size_t i=0;i<bytes;i++) check(((unsigned char *)mip.data)[i]==(unsigned char)(i*37u+packed*91u),"leased AP88 raw/packed bytes changed under pressure");
            update_record.owner=&mip; update_record.tile_source=mip.data; update_record.tile_source_bytes=(UINT32)bytes; update_record.tiled=1;
            memset(mip.data,0xA3,bytes); hw_UpdateTexture(&mip);
            update_record.tile_source_bytes=tile_cpu_source_bytes(&mip);
            HWR_UnlockCachedPatch(&patch_hw); Z_NextFrame(); Z_NextFrame();
            request=Z_ArenaFree()+bytes/2; pressure=Z_TryMallocAlign(request,PU_STATIC,&pressure,6);
            check(pressure==NULL && mip.data!=NULL && ((unsigned char *)mip.data)[bytes-1]==0xA3,"AP88 Update or caller unlock released the fresh source lease");
            tile_release_source(&update_record);
            check(!(mip.ps2_cpu&HWR_PS2_CPU_TILED_SOURCE) && (!packed || (mip.ps2_cpu&HWR_PS2_CPU_PATCH)),"AP88 delete corrupted representation flags");
            HWR_UnlockCachedPatch(&patch_hw); Z_NextFrame(); Z_NextFrame();
            pressure=Z_TryMallocAlign(request,PU_STATIC,&pressure,6);
            check(pressure!=NULL && mip.data==NULL,"deleted AP88 source remained pinned"); Z_Free(pressure);
        }
        consistent();
        printf("LEASE AP88 packed=%d leasing=%d bytes=%zu actual_load_unlock=PASS pressure=PASS update_delete=%s\\n",packed,leasing,bytes,leasing?"PASS":"not_leased");
    }
}
''' if patch_lease else '')+'''
int main(void)
{
    for (int kind = 0; kind < 3; kind++) for (leasing = 0; leasing <= 1; leasing++)
    {
        const size_t bytes = kind == 0 ? 640u*400u : kind == 1 ? 1024u*1024u : 1024u*512u*4u;
        arena_reset(8u<<20,0,0,0); Z_Init(); memset(&mip,0,sizeof mip); patch_hw.mipmap=&mip;
        mip.data = Z_TryMallocAlign(bytes,PU_HWRCACHE,&mip.data,6); check(mip.data!=NULL,"allocate actual zone source"); memset(mip.data,0x5B,bytes);
        if (kind == 0) HWR_GetFadeMask(1);
        else if (kind == 1) HWR_GetRawFlat(2);
        else { driver_set_texture(&mip); HWR_RELEASE_MIPMAP_DATA(&mip,PU_HWRMODELTEXTURE_UNLOCKED); }
        Z_NextFrame(); Z_FlushCache();
        check((mip.data!=NULL)==leasing,"actual engine post-SetTexture release overrode source lease");
        if (leasing)
        {
            check(((unsigned char *)mip.data)[bytes-1]==0x5B,"leased source corrupted under real zone purge");
            void *pressure=NULL; size_t request=Z_ArenaFree()+bytes/2;
            pressure=Z_TryMallocAlign(request,PU_STATIC,&pressure,6);
            check(pressure==NULL && mip.data!=NULL,"allocation pressure purged leased source");
            texrec_t r={&mip,mip.data,(UINT32)bytes,1}; tile_release_source(&r);
            check(!(mip.ps2_cpu&HWR_PS2_CPU_TILED_SOURCE),"delete left lease flag set");
            Z_FlushCache(); check(mip.data==NULL,"deleted source lease leaked zone memory");
        }
        consistent();
    }
    arena_reset(1u<<20,0,0,0); Z_Init(); memset(&mip,0,sizeof mip); leasing=1;
    mip.data=Z_TryMallocAlign(4096,PU_HWRCACHE,&mip.data,6); void *old=mip.data;
    driver_set_texture(&mip); update_record.owner=&mip; update_record.tile_source=mip.data; update_record.tile_source_bytes=4096; update_record.tiled=1;
    Z_Free(mip.data); check(mip.data==NULL,"explicit source free did not clear owner");
    mip.data=Z_TryMallocAlign(4096,PU_HWRCACHE,&mip.data,6); check(mip.data==old,"fresh source did not exercise same-address reuse");
    memset(mip.data,0xA3,4096); HWR_PS2_UnlockData(mip.data); hw_UpdateTexture(&mip);
    HWR_RELEASE_MIPMAP_DATA(&mip,PU_HWRMODELTEXTURE_UNLOCKED); Z_FlushCache();
    check(mip.data==old && ((unsigned char *)mip.data)[4095]==0xA3,"Update released the fresh same-address source generation");
    tile_release_source(&update_record); Z_FlushCache(); check(mip.data==NULL,"updated source lease leaked"); consistent();
    ''' + ('ap88_zone_cases();' if patch_lease else '') + '''
    printf("LEASE ZONE COMPLETE checks=%u actual_fade_raw_model_release_and_purge=PASS update_same_address=PASS\\n",checks); return 0;
}
'''
p=work/'test.c';p.write_text(code);cmd=['gcc','-std=gnu17','-O2','-g','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-Wno-unused-parameter','-Wno-clobbered','-I'+str(source/'src'),'-I'+str(source/'src/ps2'),str(p),'-o',str(work/'test')]
if a.sanitize:cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie']
built=subprocess.run(cmd,text=True,capture_output=True);(work/'build.log').write_text(built.stdout+built.stderr)
if built.returncode:print(built.stdout+built.stderr);raise SystemExit(2)
p=subprocess.run([str(work/'test')],text=True,capture_output=True);(work/'test.log').write_text(p.stdout+p.stderr);print(p.returncode,p.stdout+p.stderr)
(work/'result.json').write_text(json.dumps({'returncode':p.returncode,'negative':a.negative,'sanitize':a.sanitize,'ap88_patch_cases':patch_lease,'negative_target':'HWR_PATCH_UNLOCKED' if patch_lease else 'HWR_RELEASE_MIPMAP_DATA','fixture_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'actual_sources':{str(f.relative_to(source)):hashlib.sha256(f.read_bytes()).hexdigest() for f in [source/'src/z_zone.c',source/'src/ps2/ps2_mem.c',source/'src/hardware/hw_cache.c',source/'src/hardware/hw_data.h',source/'src/ps2/hw/ps2_hw_tile.inc']}},indent=2)+'\n')
output=p.stdout+p.stderr
matched=(p.returncode==0 and 'LEASE ZONE COMPLETE' in output) if not a.negative else (p.returncode==1 and ('AP88 actual patch caller retagged retained source' if patch_lease else 'actual engine post-SetTexture release overrode source lease') in output)
matched=matched and 'runtime error:' not in output and 'ERROR: AddressSanitizer' not in output
raise SystemExit(int(not matched))
