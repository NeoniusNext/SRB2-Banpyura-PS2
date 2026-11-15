#!/usr/bin/env python3
"""Host geometry and exact-source drawnode/material replay for the late cull."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

from next12_cull_variant import BASE, OUT, ROOT, HELPER, QUERY


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def between(text, start, stop):
    a = text.index(start)
    return text[a:text.index(stop, a)]


def function(text, name):
    # Source-owned functions in this proof have one definition, apart from the
    # PS2/non-PS2 GetTexture alternatives. Select the first definition (PS2).
    pattern = re.compile(r"^[\w \t*]+\b" + re.escape(name) + r"\([^;]*?\)\s*\{", re.M)
    match = pattern.search(text)
    if not match:
        raise RuntimeError("Missing exact function " + name)
    start = match.start()
    brace = text.index("{", match.start())
    depth = 1
    i = brace + 1
    state = "code"
    while depth:
        c = text[i]
        n = text[i:i + 2]
        if state == "line":
            if c == "\n":
                state = "code"
        elif state == "comment":
            if n == "*/":
                state = "code"
                i += 1
        elif state in ('"', "'"):
            if c == "\\":
                i += 1
            elif c == state:
                state = "code"
        elif n == "//":
            state = "line"
            i += 1
        elif n == "/*":
            state = "comment"
            i += 1
        elif c in ('"', "'"):
            state = c
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        i += 1
    return text[start:i] + "\n"


def static(body, name):
    return re.sub(r"^(?:GLMapTexture_t \*|void |boolean |UINT32 )" + name,
                  lambda m: "static " + m.group(0), body, count=1)


COMMON = r'''
#ifndef NEXT12_CULL_COMMON
#define NEXT12_CULL_COMMON
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
typedef uint8_t UINT8; typedef uint16_t UINT16; typedef uint32_t UINT32;
typedef int16_t INT16; typedef int32_t INT32; typedef int32_t fixed_t;
typedef bool boolean; typedef uint32_t FUINT; typedef uint32_t FBITFIELD;
typedef float FLOAT;
typedef union { UINT32 rgba; struct { UINT8 red,green,blue,alpha; } s; } RGBA_t;
#define FRACUNIT 65536
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
#define MAXN 64
#define MAXT 8
#define MAXF 3
typedef struct { int id; } levelflat_t;
typedef struct { int id; } extrasubsector_t;
typedef struct { int id; } sector_t;
typedef struct { int id; unsigned flags; } polyobj_t;
typedef struct { int id; UINT32 rgba,fadergba; UINT8 fadestart,fadeend; UINT32 flags; } extracolormap_t;
typedef struct { int marker; } FTransform;
typedef struct { int marker; } gl_vissprite_t;
typedef struct { int width,height,transparency; } texture_t;
typedef struct { int value; } cv_t;
typedef struct { int kind,id,texture,light,cmap; FBITFIELD flags; fixed_t height; UINT8 alpha; FOutVector v[4]; } node_input_t;
typedef struct { int metadata,source_has255,width,height,downloaded,data; } texture_input_t;
typedef struct {
 unsigned n,frames; UINT32 fr_off; int valid,wire,shader,palette,cube,renderwalls,record,secbright;
 float matrix[MAXF][4][4]; texture_input_t tex[MAXT]; node_input_t node[MAXN];
 unsigned metadata_fault; int evict_each_frame,reload_frame;
} fixture_t;
typedef struct {
 int kind,id,texture,shader; FBITFIELD flags; FSurfaceInfo surf;
} draw_event_t;
typedef struct {
 UINT32 flags,format,width,height,regen_kind,regen_id,h255,twin,variant;
 float scaleX,scaleY;
} material_t;
typedef struct {
 UINT32 sorted[MAXF][MAXN], sorted_count[MAXF];
 draw_event_t event[MAXF*MAXN]; unsigned nevent;
 FSurfaceInfo wall_surf[MAXF][MAXN]; unsigned wall_count[MAXF];
 material_t material[2*MAXT]; UINT32 texsig,light_ids,lighting_calls,cube_calls,record_hash,record_count;
 unsigned selections,uploads,upload_bytes,variant_allocations,tag_calls,transforms,guard_calls,guard_culls;
 unsigned list_counts[3];
} result_t;
int oracle_visible(const float matrix[4][4], const FOutVector *v, unsigned n);
void base_run(const fixture_t *,result_t *); void cur_run(const fixture_t *,result_t *);
unsigned cur_guard(const float[4][4],int,UINT32,int,FBITFIELD,const FOutVector *,float[5][4]);
int cur_query_case(const fixture_t *);
#endif
'''

CONTEXT = r'''
#define PS2 1
#define PS2_PROFILE 1
#define PU_LEVEL 1
#define PU_HWRCACHE_UNLOCKED 2
#define POF_SPLAT 1
#define CMF_FOG 1
#define M_Memcpy memcpy
#define SHADER_NONE -1
#define SHADER_WALL 1
#define SHADER_FOG 2
#define HWP_SPAN_BEGIN(x) ((void)0)
#define HWP_SPAN_END(x,y) ((void)0)
#define PS_START_TIMING(x) ((void)0)
#define PS_STOP_TIMING(x) ((void)0)
#define HWFR_NOCULL 1u
#define FR_MARGIN 1.0f
#define FR_GATE() (frv.on && !(hwr_fr_off & HWFR_NOCULL))
static struct { fr_plane_t p[5]; int on; } frv;
static hwr_fbox_t hwr_fbox[5]; static int hwr_fbox_on;
static ps2cull_t fixture_cs;
static UINT32 hwr_fr_off,hwr_geo_off,validcount,hwr_lt_epoch;
static INT32 hl_view; static UINT32 hl_epoch,hl_id;
static boolean Cubeapply,gl_shadersavailable,r_renderwalls,currently_batching,hwr_grec_on;
static cv_t cv_glshaders,cv_secbright;
static int fixture_wire,fixture_palette,fixture_frame,fixture_shader;
static fixed_t viewz;
static sector_t *gl_frontsector;
static FTransform atransform;
static struct { struct { int i; } value; } ps_numdrawnodes;
static struct { UINT32 nd_pl,nd_plc,nd_rip,nd_wl,nd_wlc; } gc;
static result_t *res; static const fixture_t *input;
static GLMapTexture_t maps[MAXT]; static texture_t texdata[MAXT];
static GLMapTexture_t *gl_textures;
static texture_t *texture_ptrs[MAXT]; static texture_t **textures=texture_ptrs; static size_t gl_numtextures;
static int numtextures,textureformat; static boolean gl_maptexturesloaded;
static UINT32 hwr_texsig;
static size_t allocedwalls;
static GLMipmap_t *selected_texture,*current_texture;
static unsigned char pixels[MAXT][65536];
static levelflat_t flats[MAXN]; static extrasubsector_t xsubs[MAXN];
static sector_t sectors[MAXN]; static polyobj_t polys[MAXN];
static extracolormap_t cmaps[MAXN];
static UINT32 lut_ids[MAXN+1];
static unsigned ps2hwp_now(void) { return 0; }
static int HWR_IsWireframeMode(void) { return fixture_wire; }
static int HWR_UseShader(void) { return cv_glshaders.value && gl_shadersavailable; }
static int HWR_ShouldUsePaletteRendering(void) { return fixture_palette; }
static UINT32 HWR_GetLightTableID(extracolormap_t *c) {
 unsigned k=c?(unsigned)c->id+1u:0u;
 if(k>MAXN) abort();
 if(!lut_ids[k]) lut_ids[k]=++res->light_ids;
 return lut_ids[k];
}
static void V_CubeApply(UINT8 *r,UINT8 *g,UINT8 *b) {
 res->cube_calls++; if(Cubeapply) { *r^=3;*g^=5;*b^=7; }
}
static void I_Error(const char *fmt,...) __attribute__((noreturn));
static void I_Error(const char *fmt,...) { va_list a;va_start(a,fmt);vfprintf(stderr,fmt,a);va_end(a);abort(); }
static void Z_Realloc(void *old,size_t bytes,int tag,void *owner) {
 void *p=realloc(old,bytes);(void)tag;if(!p&&bytes)abort();*(void **)owner=p;
}
static void Z_ChangeTag(void *p,int tag) { (void)p;(void)tag;res->tag_calls++; }
static const ps2cull_t *PS2HWD_CullSetup(void) { return &fixture_cs; }
static void HWR_FrArc(void) { }
static void fixture_settex(GLMipmap_t *m) {
 res->selections++; selected_texture=m;if(!m)return;
 if(!m->downloaded) {
  res->uploads++;res->upload_bytes+=(unsigned)m->width*m->height;
  if(m->format==GL_TEXFMT_P_8) {
   if(!m->ps2_h255) m->ps2_h255=input->tex[m->regen_id].source_has255?2:1;
   if((m->flags&TF_CHROMAKEYED)&&m->ps2_h255==2&&!(m->flags&TF_TRANSPARENT)) {m->flags|=TF_TRANSPARENT;hwr_texsig++;}
  }
  m->downloaded=(UINT32)m->regen_id+1u;
 }
}
static void fixture_settransform(FTransform *t);
static struct { void (*pfnSetTexture)(GLMipmap_t *); void (*pfnSetTransform)(FTransform *); } HWD={fixture_settex,fixture_settransform};
static void HWR_GCRecTex(GLMipmap_t *m) { (void)m;abort(); }
static void PS2HWD_TouchTexture(GLMipmap_t *m) { (void)m; }
static void HWR_ProcessPolygon(FSurfaceInfo *s,FOutVector *v,FUINT n,FBITFIELD flags,int shader,boolean horizon);
static GLMapTexture_t *HWR_GetTexture(INT32,boolean);
static void HWR_GetLevelFlat(levelflat_t *f,boolean chroma) { HWR_GetTexture(f->id%MAXT,chroma); }
static void HWR_RenderPlane(void *,extrasubsector_t *,boolean,fixed_t,FBITFIELD,INT32,levelflat_t *,sector_t *,UINT8,extracolormap_t *);
static void HWR_RenderPolyObjectPlane(polyobj_t *,boolean,fixed_t,FBITFIELD,INT32,levelflat_t *,sector_t *,UINT8,extracolormap_t *);
static void HWR_GCRecTWall(FOutVector *,FSurfaceInfo *,INT32,FBITFIELD,boolean,INT32,extracolormap_t *);
'''

BOTTOM = r'''
static UINT32 hash_bytes(UINT32 h,const void *v,size_t n) {const unsigned char*p=v;while(n--)h=(h^*p++)*16777619u;return h;}
static void fixture_settransform(FTransform *t) {
 (void)t;res->transforms++;unsigned n=(unsigned)(numwalls+numplanes+numpolyplanes);
 res->sorted_count[fixture_frame]=n;
 for(unsigned i=0;i<n;i++) {
  gl_drawnode_t *q=&sortnode[sortindex[i]];
  unsigned kind=q->plane?1u:q->polyplane?2u:3u;
  unsigned id=q->plane?(unsigned)q->plane->xsub->id:q->polyplane?(unsigned)q->polyplane->polysector->id:(unsigned)q->wall->Surf.PolyFlags;
  res->sorted[fixture_frame][i]=kind*10000u+id;
 }
}
static void event(int kind,int id,FSurfaceInfo *s,FBITFIELD flags,int shader) {
 if(res->nevent>=MAXF*MAXN)abort();
 draw_event_t *e=&res->event[res->nevent++];
 e->kind=kind;e->id=id;e->texture=(flags&PF_NoTexture)?-1:selected_texture?selected_texture->regen_id:-2;
 e->shader=shader;e->flags=flags;e->surf=*s;
}
static void HWR_ProcessPolygon(FSurfaceInfo *s,FOutVector *v,FUINT n,FBITFIELD flags,int shader,boolean horizon) {
 (void)horizon;fixture_shader=shader;
 if(oracle_visible(input->matrix[fixture_frame],v,n))event(3,(int)s->PolyFlags,s,flags,shader);
}
static void HWR_RenderPlane(void *unused,extrasubsector_t*x,boolean c,fixed_t h,FBITFIELD flags,INT32 light,levelflat_t*f,sector_t*sec,UINT8 a,extracolormap_t*cm) {
 (void)unused;(void)c;(void)h;(void)f;(void)sec;FSurfaceInfo s={0};s.PolyFlags=(FUINT)x->id;HWR_Lighting(&s,light,cm);s.PolyColor.s.alpha=a;event(1,x->id,&s,flags,HWR_UseShader()?0:SHADER_NONE);
}
static void HWR_RenderPolyObjectPlane(polyobj_t*p,boolean c,fixed_t h,FBITFIELD flags,INT32 light,levelflat_t*f,sector_t*sec,UINT8 a,extracolormap_t*cm) {
 (void)c;(void)h;(void)f;(void)sec;FSurfaceInfo s={0};s.PolyFlags=(FUINT)p->id;HWR_Lighting(&s,light,cm);s.PolyColor.s.alpha=a;event(2,p->id,&s,flags,HWR_UseShader()?0:SHADER_NONE);
}
static void HWR_GCRecTWall(FOutVector*v,FSurfaceInfo*s,INT32 tex,FBITFIELD flags,boolean fog,INT32 light,extracolormap_t*cm) {
 res->record_count++;res->record_hash=hash_bytes(res->record_hash,v,4*sizeof*v);
 res->record_hash=hash_bytes(res->record_hash,s,sizeof*s);res->record_hash=hash_bytes(res->record_hash,&tex,sizeof tex);
 res->record_hash=hash_bytes(res->record_hash,&flags,sizeof flags);res->record_hash=hash_bytes(res->record_hash,&fog,sizeof fog);
 res->record_hash=hash_bytes(res->record_hash,&light,sizeof light);int id=cm?cm->id:-1;res->record_hash=hash_bytes(res->record_hash,&id,sizeof id);
}
static void init_textures(const fixture_t*f) {
 for(unsigned i=0;i<MAXT;i++) {
  texture_input_t t=f->tex[i];int w=t.width?t.width:32,h=t.height?t.height:32;
  memset(&maps[i],0,sizeof maps[i]);texdata[i]=(texture_t){w,h,0};textures[i]=&texdata[i];
  GLMipmap_t*m=&maps[i].mipmap;GLMipmap_t*v=calloc(1,sizeof*v);if(!v)abort();res->variant_allocations++;
  m->nextcolormap=v;m->format=v->format=GL_TEXFMT_P_8;m->width=v->width=(UINT16)w;m->height=v->height=(UINT16)h;
  m->flags=TF_WRAPXY;v->flags=TF_WRAPXY|TF_CHROMAKEYED;m->regen_kind=v->regen_kind=1;m->regen_id=v->regen_id=(INT32)i;
  m->ps2_h255=v->ps2_h255=t.metadata==1?1:t.metadata==2?2:0;v->ps2_twin=m;
  if(t.metadata==3)m->flags|=TF_TRANSPARENT;
  if(t.metadata==4)v->flags|=TF_TRANSPARENT;
  m->downloaded=v->downloaded=(UINT32)t.downloaded;
  memset(pixels[i],t.source_has255?255:7,sizeof pixels[i]);if(t.data)m->data=v->data=pixels[i];
  maps[i].scaleX=1.0f/(w*FRACUNIT);maps[i].scaleY=1.0f/(h*FRACUNIT);
 }
 gl_textures=maps;gl_numtextures=MAXT;numtextures=MAXT;textureformat=GL_TEXFMT_P_8;gl_maptexturesloaded=true;
 switch(f->metadata_fault) {
  case 1:maps[0].mipmap.width++;break;case 2:maps[0].mipmap.flags|=TF_TRANSPARENT;break;
  case 3:maps[0].mipmap.nextcolormap->ps2_h255=0;break;case 4:maps[0].mipmap.nextcolormap->ps2_twin=NULL;break;
  case 5:maps[0].scaleX=0;break;case 6:textureformat=GL_TEXFMT_RGBA;break;
  case 7:texdata[0].transparency=1;break;case 8:maps[0].mipmap.regen_id=1;break;
  case 9:maps[0].mipmap.nextcolormap->flags|=TF_TRANSPARENT;break;
  case 10:gl_maptexturesloaded=false;break;case 11:numtextures--;break;
  case 12:maps[0].mipmap.nextcolormap->format=GL_TEXFMT_RGBA;break;
  case 13:maps[0].mipmap.ps2_h255=0;break;case 14:maps[0].mipmap.nextcolormap->regen_kind=2;break;
 }
}
static void free_textures(void) {
 for(unsigned i=0;i<MAXT;i++) {GLMipmap_t*v=maps[i].mipmap.nextcolormap;while(v){GLMipmap_t*n=v->nextcolormap;free(v);v=n;}maps[i].mipmap.nextcolormap=NULL;}
}
static void snapshot_materials(void) {
 for(unsigned i=0;i<MAXT;i++)for(unsigned j=0;j<2;j++) {
  GLMipmap_t*m=j?maps[i].mipmap.nextcolormap:&maps[i].mipmap;material_t*r=&res->material[2*i+j];
  if(!m)continue;
  r->flags=m->flags;r->format=(UINT32)m->format;r->width=m->width;r->height=m->height;r->regen_kind=m->regen_kind;r->regen_id=(UINT32)m->regen_id;r->h255=m->ps2_h255;
  r->twin=m->ps2_twin?1u:0u;r->variant=m->nextcolormap?1u:0u;r->scaleX=maps[i].scaleX;r->scaleY=maps[i].scaleY;
 }
}
void TAG_run(const fixture_t*f,result_t*r) {
 memset(r,0,sizeof*r);res=r;input=f;res->record_hash=2166136261u;memset(lut_ids,0,sizeof lut_ids);
 hwr_texsig=5;hwr_fr_off=f->fr_off;hwr_geo_off=0;hwr_lt_epoch=7;hl_view=-1;hl_epoch=0;hl_id=0;
 fixture_wire=f->wire;fixture_palette=f->palette;Cubeapply=f->cube;cv_glshaders.value=f->shader;gl_shadersavailable=true;
 cv_secbright.value=f->secbright;r_renderwalls=f->renderwalls;currently_batching=false;hwr_grec_on=f->record;
 fixture_shader=SHADER_NONE;selected_texture=current_texture=NULL;viewz=0;memset(&gc,0,sizeof gc);
 init_textures(f);
 for(unsigned frame=0;frame<f->frames;frame++) {
  fixture_frame=(int)frame;validcount=frame+1u;fixture_cs.valid=f->valid;memcpy(fixture_cs.r,f->matrix[frame],sizeof fixture_cs.r);HWR_FrViewSetup();
  if(f->evict_each_frame&&frame)for(unsigned t=0;t<MAXT;t++){maps[t].mipmap.downloaded=0;maps[t].mipmap.nextcolormap->downloaded=0;}
  if(f->reload_frame==(int)frame+1) {free_textures();init_textures(f);for(unsigned t=0;t<MAXT;t++){maps[t].mipmap.ps2_h255=0;maps[t].mipmap.nextcolormap->ps2_h255=0;}}
  drawcount=0;numwalls=numplanes=numpolyplanes=0;
  for(unsigned i=0;i<f->n;i++) {
   const node_input_t*n=&f->node[i];cmaps[i]=(extracolormap_t){(int)i,0x20406080u+(UINT32)i,0xff102030u,2,27};
   extracolormap_t*cm=n->cmap?&cmaps[i]:NULL;flats[i].id=n->texture;xsubs[i].id=polys[i].id=n->id;polys[i].flags=POF_SPLAT;sectors[i].id=n->id;
   if(n->kind==1)HWR_AddTransparentFloor(&flats[i],&xsubs[i],false,n->height,n->light,n->alpha,&sectors[i],n->flags,false,true,cm);
   else if(n->kind==2)HWR_AddTransparentPolyobjectFloor(&flats[i],&polys[i],false,n->height,n->light,n->alpha,&sectors[i],n->flags,cm);
   else {FSurfaceInfo s={0};s.PolyFlags=(FUINT)n->id;s.PolyColor.rgba=0xaabbccddu;s.PolyColor.s.alpha=n->alpha;HWR_AddTransparentWall((FOutVector*)n->v,&s,n->texture,n->flags,false,n->light,cm);}
  }
  unsigned collectedwalls=(unsigned)numwalls;
  if(numwalls+numplanes+numpolyplanes)HWR_CreateDrawNodes();
  res->wall_count[frame]=collectedwalls;for(unsigned wi=0;wi<collectedwalls;wi++)res->wall_surf[frame][wi]=wallinfo[wi].Surf;
 }
 snapshot_materials();res->texsig=hwr_texsig;res->list_counts[0]=(unsigned)numwalls;res->list_counts[1]=(unsigned)numplanes;res->list_counts[2]=(unsigned)numpolyplanes;
 free_textures();free(wallinfo);wallinfo=NULL;free(planeinfo);planeinfo=NULL;free(polyplaneinfo);polyplaneinfo=NULL;
}
'''


def prepare(work, negative=0):
    work.mkdir(parents=True, exist_ok=True)
    defs = (BASE / "src/hardware/hw_defs.h").read_text()
    data = (BASE / "src/hardware/hw_data.h").read_text()
    fx = (BASE / "src/ps2/hw/ps2_hw_fx2.h").read_text()
    front = (BASE / "src/hardware/hw_front.inc").read_text()
    common = COMMON
    before = "typedef struct { int kind,id,texture,light,cmap;"
    actual = between(defs, "// Transformed vector, as passed to HWR API", "#ifdef GL_SHADERS")
    actual += between(defs, "enum EPolyFlags", "enum ESurfFlags")
    actual += between(defs, "enum ETextureFlags", "struct FTextureInfo")
    actual += between(defs, "struct FLightInfo", "#define GL_DEFAULTMIX")
    actual += between(data, "typedef enum GLTextureFormat_e", "// Patch information for the hardware renderer.")
    actual += between(fx, "typedef struct\n{\n\tfloat r[4][4]", "extern const ps2cull_t")
    actual += "typedef struct { float a,b,c,d; } fr_plane_t;\n"
    actual += "typedef struct { INT32 a,b,c,nd;UINT32 ox,oy,oz; } hwr_fbox_t;\n"
    common = common.replace(before, actual + before)
    (work / "common.h").write_text(common)
    pins = {}
    for tag, source in (("base", BASE), ("cur", OUT)):
        main = (source / "src/hardware/hw_main.c").read_text()
        cache = (source / "src/hardware/hw_cache.c").read_text()
        batch = (source / "src/hardware/hw_batching.c").read_text()
        layout = between(main, "typedef struct\n{\n\tFOutVector    wallVerts[4];", "static int CompareDrawNodes(")
        # The source declares one RenderWall prototype in this layout slice.
        layout = layout.replace("void HWR_RenderWall(", "static void HWR_RenderWall(")
        layout = layout.replace("void HWR_AddTransparentFloor(", "static void HWR_AddTransparentFloor(")
        layout = layout.replace("void HWR_AddTransparentPolyobjectFloor(", "static void HWR_AddTransparentPolyobjectFloor(")
        lighting = static(function(main, "HWR_Lighting"), "HWR_Lighting").replace("HWR_Lighting(", "HWR_LightingBody(", 1)
        lighting += "static void HWR_Lighting(FSurfaceInfo *s,INT32 l,extracolormap_t *c) {res->lighting_calls++;HWR_LightingBody(s,l,c);}\n"
        funcs = [function(front, "HWR_FrViewSetup"), lighting]
        funcs += [static(function(batch, "HWR_SetCurrentTexture"), "HWR_SetCurrentTexture"), static(function(cache, "HWR_GetTexture"), "HWR_GetTexture")]
        if tag == "cur":
            funcs += [static(function(cache, "HWR_PS2_CullTextureStable"), "HWR_PS2_CullTextureStable")]
            helper = function(main, "HWR_FrWallCullPlane")
            if negative == 2:
                helper = helper.replace("d > FR_MARGIN", "d > -1000.0f")
            if negative == 3:
                helper = helper.replace("return 0;\n\tfor (k = 0; k < 5; k++)", "return 1;\n\tfor (k = 0; k < 5; k++)", 1)
            funcs += [helper]
        addwall = static(function(main, "HWR_AddTransparentWall"), "HWR_AddTransparentWall")
        if tag == "cur" and negative == 4:
            addwall = addwall.replace("\t// Force realloc if buffer has been freed", "\tif (HWR_FrWallCullPlane(wallVerts,blend)) { drawcount++; return; }\n\t// Force realloc if buffer has been freed", 1)
        funcs += [addwall, static(function(main, "HWR_RenderWall"), "HWR_RenderWall"), function(main, "CompareDrawNodes"), function(main, "CompareDrawNodePlanes")]
        create = function(main, "HWR_CreateDrawNodes")
        if tag == "cur" and negative == 1:
            create = create.replace("cullplane && HWR_PS2_CullTextureStable(w->texnum)", "cullplane != 0")
        if tag == "cur" and negative == 5:
            create = create.replace("HWR_Lighting(&w->Surf, w->lightlevel, w->wallcolormap);", "((void)0);")
        funcs += [create]
        body = "#include \"common.h\"\n" + CONTEXT + layout + "\n".join(funcs) + BOTTOM.replace("TAG_", tag + "_")
        body = re.sub(r"(?m)^gl_drawnode_t \*sortnode;", "static gl_drawnode_t *sortnode;", body)
        body = re.sub(r"(?m)^size_t \*sortindex;", "static size_t *sortindex;", body)
        if tag == "cur":
            body += r'''
unsigned cur_guard(const float matrix[4][4],int valid,UINT32 off,int wire,FBITFIELD flags,const FOutVector*v,float planes[5][4]) {
 fixture_cs.valid=valid;memcpy(fixture_cs.r,matrix,sizeof fixture_cs.r);hwr_fr_off=off;fixture_wire=wire;HWR_FrViewSetup();
 if(planes)memcpy(planes,frv.p,sizeof frv.p);
 return HWR_FrWallCullPlane(v,flags);
}
int cur_query_case(const fixture_t*f) {result_t r={0};res=&r;input=f;init_textures(f);int yes=HWR_PS2_CullTextureStable(0);free_textures();return yes;}
'''
        path = work / (tag + "-production.c")
        path.write_text(body)
        pins[tag] = {"path": str(path), "sha256": sha(path), "extracted_functions": [re.search(r"\b(HWR_\w+|CompareDraw\w+)\(", x).group(1) for x in funcs]}
    (work / "extraction.json").write_text(json.dumps({"pins": pins, "negative": negative,
        "scope": "Exact production control/sort/lighting/GetTexture/query/guard source; HWD/zone/light-table/plane sinks are instrumented host models, not target performance evidence."}, indent=2) + "\n")


def run(command, log, expected=0):
    r = subprocess.run([str(x) for x in command], text=True, capture_output=True)
    log.write_text(r.stdout + r.stderr)
    if r.returncode == expected:
        return r
    raise RuntimeError("Unexpected rc=" + str(r.returncode) + ": " + str(log) + "\n" + (r.stdout + r.stderr)[-5000:])


def build_run(work, sanitize=False, negative=0):
    prepare(work, negative)
    flags = ["-std=gnu17", "-O2", "-ffp-contract=off", "-fwrapv", "-DPS2_PROFILE", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-parameter", "-Wno-missing-field-initializers", "-Wno-sign-compare"]
    if sanitize:
        flags += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
    binary = work / ("test-sanitize" if sanitize else "test")
    run(["gcc", *flags, "-I" + str(work), work / "base-production.c", work / "cur-production.c", ROOT / "tools/ps2/next12_cull_hosttest.c", "-o", binary, "-lm"], work / "build.log")
    r = run([binary], work / "test.log", expected=1 if negative else 0)
    return {"negative": negative, "sanitize": sanitize, "rc": r.returncode, "binary_sha256": sha(binary), "output": (r.stdout + r.stderr).strip(), "log": str(work / "test.log")}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, default=ROOT / "build/nov15-next12/cull/host")
    ap.add_argument("--skip-negative", action="store_true")
    args = ap.parse_args()
    work = args.out.resolve()
    rows = [build_run(work / "positive"), build_run(work / "sanitize", sanitize=True)]
    if not args.skip_negative:
        rows += [build_run(work / ("negative-" + str(n)), negative=n) for n in range(1, 6)]
    report = {"status": "host_source_replay_only", "rows": rows,
              "target_or_emulator_used": False,
              "source_freeze": str(ROOT / "build/nov15-next12/cull/source-freeze.json"),
              "proof_tools": {str(p.relative_to(ROOT)): sha(p) for p in (Path(__file__), ROOT / "tools/ps2/next12_cull_hosttest.c", ROOT / "tools/ps2/next12_cull_variant.py")}}
    (work / "host-report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
