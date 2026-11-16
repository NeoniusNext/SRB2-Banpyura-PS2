#!/usr/bin/env python3
"""Real engine flat creation/teardown and real PS2 Zone owner checks; no emulator."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent

def function(text, name):
    m = re.search(r'^(?:static )?(?:inline )?[\w *]+\b' + re.escape(name) + r'\([^\n]*\)\n\{', text, re.M)
    if not m:
        raise RuntimeError('missing actual function: ' + name)
    start = text.index('{', m.start())
    depth = 0
    for i in range(start, len(text)):
        depth += (text[i] == '{') - (text[i] == '}')
        if not depth:
            return text[m.start():i + 1] + '\n'
    raise RuntimeError(name)

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source-root', type=Path, default=ROOT)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--layout', choices=('pages','compact','dense'), default='pages')
    ap.add_argument('--sanitize', action='store_true')
    ap.add_argument('--expect-orphan', action='store_true', help='require the old cleanup to leave a live Zone user in retired metadata')
    ap.add_argument('--owner-reuse', action='store_true', help='hold retired child storage to expose the exact Z_CheckHeap user error without a host UAF')
    a = ap.parse_args()
    source, work = a.source_root.resolve(), a.out.resolve()
    work.mkdir(parents=True, exist_ok=True)
    engine = (source/'src/hardware/hw_cache.c').read_text()
    hdr = (source/'src/hardware/hw_data.h').read_text()
    enum = hdr[hdr.index('typedef enum GLTextureFormat_e'):hdr.index('} GLTextureFormat_t;')+len('} GLTextureFormat_t;')]
    mip = hdr[hdr.index('struct GLMipmap_s\n{'):hdr.index('typedef struct GLMipmap_s GLMipmap_t;')+len('typedef struct GLMipmap_s GLMipmap_t;')]
    maptype = hdr[hdr.index('struct GLMapTexture_s\n{'):hdr.index('typedef struct GLMapTexture_s GLMapTexture_t;')+len('typedef struct GLMapTexture_s GLMapTexture_t;')]
    ls = hdr.index('#if defined(PS2) && defined(PS2_PROFILE)\n#define HWR_PS2_CPU_TILED_SOURCE')
    le = hdr.index('#if defined(PS2) && defined(PS2_PROFILE) && !defined(PS2_NOOPT_PATCH_PIXELS)',ls)
    lease = hdr[ls:le]
    meta = engine[engine.index('#if defined(PS2) && defined(PS2_PROFILE) && !defined(PS2_NOOPT_TEXTURE_PAGES)'):engine.index('boolean gl_maptexturesloaded = false;')+len('boolean gl_maptexturesloaded = false;')]
    code = '#include <stdint.h>\n#include <strings.h>\ntypedef uint8_t UINT8; typedef uint16_t UINT16; static int netgame; int PS2Lua_InCall(void){return 0;} void PS2Spill_Reset(void){}\n#define max(a,b) ((a)>(b)?(a):(b))\n'
    code += '#define ZONE_HOST_SOURCE "'+str(source/'src/z_zone.c')+'"\n#define ZONE_MEM_SOURCE "'+str(source/'src/ps2/ps2_mem.c')+'"\n#define main zone_fixture_main_unused\n#include "'+str(ROOT/'tools/ps2/zone_hosttest.c')+'"\n#undef main\n'
    code += enum+'\n'+mip+'\n'+maptype+'\n'+lease+'\n'
    code += '''
#define TF_WRAPXY 3u
#define TF_CHROMAKEYED 4u
#define TF_TRANSPARENT 8u
#define LEVELFLAT_NONE 0
static int myargc; static char **myargv;
static int currently_batching;
static int texturetranslation[1];
typedef struct { int width,height; } texture_t;
static texture_t texture={64,64}, *textures[1]={&texture};
typedef struct { int type,texture_id; } levelflat_t;
static void HWR_GCacheFlush(void){}
static void PS2TexC_Reset(void){}
static void HWR_SetCurrentTexture(GLMipmap_t *m){(void)m;}
void HWR_PS2_RegenerateMipmap(GLMipmap_t *m);
'''+meta+'\n'+function(engine,'HWR_InitMapTextures')+'\n'
    code += '''
static UINT8 *flat_source; static void *driver_owned; static int steal_child;
static GLMipmap_t *retired_child; static int hold_retired;
static void *retired_storage;
static void metadata_free(void *p)
{
    if (hold_retired && p==retired_child) { retired_storage=p; memset(p,0,sizeof(GLMipmap_t)); return; }
    free(p);
}
static const UINT8 *R_TryGetFlatForTexture(size_t id){check(id==0,"flat recipe id");return flat_source;}
static void HWR_PS2_RegeneratePatch(GLMipmap_t *m){(void)m;check(0,"unexpected patch regeneration");}
static void HWR_GenerateTexture(INT32 id,GLMapTexture_t *t,GLMipmap_t *m){(void)id;(void)t;(void)m;check(0,"unexpected wall composition");}
'''+function(engine,'HWR_PS2_RegenerateMipmap')+function(engine,'HWR_PS2_StealData')+'''
static void driver_set(GLMipmap_t *m)
{
    HWR_PS2_RegenerateMipmap(m);check(m->data!=NULL,"actual flat recipe produced pixels");
    if(steal_child && m->ps2_twin) HWR_PS2_StealData(m,&driver_owned);
    m->downloaded=1;
}
static void driver_delete(GLMipmap_t *m)
{
    m->downloaded=0;
    if(!m->ps2_twin && driver_owned){Z_Free(driver_owned);driver_owned=NULL;}
}
static struct {void(*pfnSetTexture)(GLMipmap_t*);void(*pfnDeleteTexture)(GLMipmap_t*);} HWD={driver_set,driver_delete};
'''+function(engine,'MakeLevelFlatMipmap')+function(engine,'HWR_GetLevelFlat')+'\n'
    code += '#define free metadata_free\n'+function(engine,'DeleteTextureMipmap')+function(engine,'FreeMapTexture')+'\n'
    code += '#if defined(HWR_TEXTURE_PAGES) || (defined(PS2) && defined(PS2_PROFILE) && !defined(PS2_NOOPT_FLAT_METADATA))\n'+function(engine,'FreeFlatMipmap')+'#endif\n'
    code += function(engine,'HWR_FreeMapTextures')+'#undef free\n'+function(engine,'HWR_LoadMapTextures')+'\n'
    fixture = (HERE/'flat_child_lifetime_hosttest.c').read_text()
    code += fixture
    p = work/'test.c'; p.write_text(code)
    cmd = ['gcc','-std=gnu17','-O2','-g','-fwrapv','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-Wno-unused-parameter','-Wno-clobbered','-I'+str(source/'src'),'-I'+str(source/'src/ps2')]
    if a.layout != 'pages': cmd += ['-DPS2_NOOPT_TEXTURE_PAGES']
    if a.layout == 'dense': cmd += ['-DPS2_NOOPT_FLAT_METADATA']
    if a.sanitize: cmd += ['-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie']
    if a.expect_orphan: cmd += ['-DEXPECT_ORPHAN']
    if a.owner_reuse: cmd += ['-DOWNER_REUSE']
    cmd += [str(p),'-o',str(work/'test')]
    (work/'command.json').write_text(json.dumps(cmd,indent=2)+'\n')
    built = subprocess.run(cmd,text=True,capture_output=True)
    (work/'build.log').write_text(built.stdout+built.stderr)
    if built.returncode: print(built.stdout+built.stderr); return 2
    ran = subprocess.run([str(work/'test')],text=True,capture_output=True)
    output = ran.stdout+ran.stderr; (work/'test.log').write_text(output); print(output,end='')
    ok = ran.returncode==0 and 'FLAT LIFETIME COMPLETE' in output and 'runtime error:' not in output and 'ERROR: AddressSanitizer' not in output
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    (work/'result.json').write_text(json.dumps({'passed':ok,'returncode':ran.returncode,'layout':a.layout,'sanitize':a.sanitize,'expect_orphan':a.expect_orphan,'owner_reuse':a.owner_reuse,'source_sha256':{str(p.relative_to(source)):sha(p) for p in [source/'src/hardware/hw_cache.c',source/'src/hardware/hw_data.h',source/'src/z_zone.c',source/'src/ps2/ps2_mem.c']},'fixture_sha256':sha(HERE/'flat_child_lifetime_hosttest.c'),'runner_sha256':sha(Path(__file__)),'generated_source_sha256':sha(work/'test.c'),'output':output,'emulator_used':False},indent=2)+'\n')
    return int(not ok)

if __name__=='__main__':raise SystemExit(main())
