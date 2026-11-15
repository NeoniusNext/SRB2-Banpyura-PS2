/* Complete production types/functions are inserted by the runner. */
/* PREAMBLE */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <stdarg.h>
typedef uint8_t UINT8;typedef uint16_t UINT16;typedef uint32_t UINT32;typedef uint64_t UINT64;
typedef int16_t INT16;typedef int32_t INT32;typedef int64_t INT64;
typedef enum {false,true} boolean;typedef INT32 fixed_t;
#define ATTRPACK __attribute__((packed))
#define FRACBITS 16
#define FRACUNIT 65536
#define ROTANGLES 72
#define SHORT(v) (v)
#define LONG(v) (v)
#define M_Memcpy memcpy
#define max(a,b) ((a)>(b)?(a):(b))
#define min(a,b) ((a)<(b)?(a):(b))
#define WRITEUINT8(p,v) do{UINT8 _v=(v);memcpy(p,&_v,1);p=(UINT8*)p+1;}while(0)
#define WRITEUINT16(p,v) do{UINT16 _v=(v);memcpy(p,&_v,2);p=(UINT8*)p+2;}while(0)
#define WRITEUINT32(p,v) do{UINT32 _v=(v);memcpy(p,&_v,4);p=(UINT8*)p+4;}while(0)
#define TRANSPARENTPIXEL 255
#define HWR_PATCHES_CHROMAKEY_COLORINDEX 255
#define HWR_TRYPATCH_MIN (64u<<10)
#define FUNCMATH
#define FUNCINLINE
#define ATTRINLINE
#define PS2_OPT_MATH
enum{PU_STATIC=1,PU_PATCH=14,PU_PATCH_LOWPRIORITY=15,PU_PATCH_ROTATED=16,PU_PATCH_DATA=17,PU_SPRITE=18,PU_HUDGFX=19,PU_HWRPATCHINFO=21,PU_HWRPATCHCOLMIPMAP=22,PU_HWRCACHE=48,PU_HWRCACHE_UNLOCKED=102};
static unsigned checks,cases,fatal_calls,guard_calls,iterating,iter_calls,try_calls,malloc_calls,fail_try,guard_armed;
static unsigned long long compared_bytes;
static UINT32 hash=2166136261u;
static void check(int yes,const char *why){checks++;if(!yes){fprintf(stderr,"Patch rescue proof failed: %s\n",why);exit(101);}}
__attribute__((noreturn)) static void I_Error(const char *fmt,...){fatal_calls++;fprintf(stderr,"Unexpected fatal allocator/drawer: %s\n",fmt);exit(102);}
static void Z_GuardThrow(const char *why){(void)why;guard_calls++;check(0,"rescue must never throw a guard");}
static struct block{void *p;size_t n;void **owner;int tag;unsigned live;} blocks[16384];
static size_t block_high;
static int block_of(const void *p){for(size_t i=0;i<block_high;i++)if(blocks[i].live&&blocks[i].p==p)return(int)i;return-1;}
static void *allocate(size_t n,int tag,void **owner){
 check(!iterating,"allocation forbidden inside tag iterator");check(n<=0x7ffffff0u,"oversized request rejected before allocator");
 size_t b=0;while(b<block_high&&blocks[b].live)b++;check(b<sizeof blocks/sizeof *blocks,"block ledger capacity");if(b==block_high)block_high++;
 void *p=NULL;check(posix_memalign(&p,64,n?n:1)==0,"host backing allocation");memset(p,0,n?n:1);blocks[b]=(struct block){p,n,owner,tag,1};if(owner)*owner=p;return p;
}
static void *Z_Malloc(size_t n,int tag,void *owner){malloc_calls++;return allocate(n,tag,owner);}
static void *Z_Calloc(size_t n,int tag,void *owner){return Z_Malloc(n,tag,owner);}
static void Z_Free(void *p){
 if(!p)return;check(!iterating,"free forbidden inside tag iterator");int b=block_of(p);check(b>=0,"free only live zone blocks");
 if(blocks[b].owner){check(*blocks[b].owner==p,"owner names its source");*blocks[b].owner=NULL;}
 for(size_t i=0;i<block_high;i++)if(blocks[i].live&&blocks[i].owner){uintptr_t a=(uintptr_t)p,o=(uintptr_t)blocks[i].owner;check(o<a||o-a>=blocks[b].n,"metadata cannot die before pixel owners");}
 blocks[b].live=0;free(p);
}
static void *Z_Realloc(void *p,size_t n,int tag,void *owner){
 if(!p)return Z_Calloc(n,tag,owner);int b=block_of(p);check(b>=0&&!iterating,"realloc live source outside iteration");
 void *q=allocate(n,tag,NULL);memcpy(q,p,n<blocks[b].n?n:blocks[b].n);Z_Free(p);int k=block_of(q);blocks[k].owner=owner;if(owner)*(void**)owner=q;return q;
}
static void *Z_TryMallocAlign(size_t n,int tag,void *owner,int align){
 try_calls++;check(!iterating,"try allocation forbidden inside iterator");check(n<=0x7ffffff0u,"try allocator would fatally reject oversized request");check(align==(int)sizeof(void*),"original image alignment");
 if(fail_try)return NULL;return allocate(n,tag,owner);
}
static void Z_ChangeTag(void *p,int tag){int b=block_of(p);check(b>=0,"retag live source");blocks[b].tag=tag;}
static void Z_SetUser(void *p,void **owner){int b=block_of(p);check(b>=0,"set owner of live source");blocks[b].owner=owner;if(owner)*owner=p;}
static void Z_IterateTags(INT32 low,INT32 high,boolean(*callback)(void*)){
 check(!iterating,"tag search is not nested");iterating=1;iter_calls++;
 for(size_t i=0;i<block_high;i++)if(blocks[i].live&&blocks[i].tag>=low&&blocks[i].tag<=high){check(blocks[i].tag!=PU_PATCH_DATA,"raw patch data must not be cast to patch_t");check(!callback(blocks[i].p),"source search may not free a patch");}
 iterating=0;
}
/* TYPES */
static boolean ps2_slow_composite;
static unsigned ps2hwd_dbg_flags,ps2hwt_mkpatch_n,ps2hwt_mkpatch_cyc;
static unsigned ps2hwt_now(void){static unsigned ticks;return++ticks;}
static INT32 patchformat=GL_TEXFMT_AP_88;
static RGBA_t palette[256],*pMasterPalette=palette;
static RGBA_t *HWR_GetTexturePalette(void){return palette;}
static INT32 ASTTextureBlendingThreshold[2];
static UINT8 ASTBlendPaletteIndexes(UINT8 a,UINT8 b,enum patchalphastyle style,UINT8 alpha){(void)a;(void)b;(void)style;(void)alpha;I_Error("unreachable composite blend");}
static UINT32 ASTBlendTexturePixel(RGBA_t a,RGBA_t b,enum patchalphastyle style,UINT8 alpha){(void)a;(void)b;(void)style;(void)alpha;I_Error("unreachable composite blend");}
#define CONS_WARNING 1
#define CONS_Alert(...) ((void)0)
#define CONS_Printf(...) ((void)0)
static UINT8 NearestColor(UINT8 r,UINT8 g,UINT8 b){(void)g;(void)b;return r;}
static size_t gl_numtextures;
static GLMapTexture_t *gl_textures;
static GLMapTexture_t *HWR_MapTexture(size_t n,boolean create){(void)n;(void)create;I_Error("unreachable wall generation");}
static void HWR_GenerateTexture(INT32 n,GLMapTexture_t *t,GLMipmap_t *m){(void)n;(void)t;(void)m;I_Error("unreachable wall generation");}
static const UINT8 *R_TryGetFlatForTexture(size_t n){(void)n;I_Error("unreachable flat generation");}
static fixed_t rollcosang[ROTANGLES],rollsinang[ROTANGLES];
enum{VID_GL_LIBRARY_LOADED=1};
static struct{int glstate;}vid={VID_GL_LIBRARY_LOADED};
static void delete_texture(GLMipmap_t*m){if(m->data)check(block_of(m->data)>=0,"driver deletion precedes primary pixel free");m->downloaded=0;}
static struct{void(*pfnDeleteTexture)(GLMipmap_t*);}HWD={delete_texture};
void HWR_FreeTexture(patch_t*);
void HWR_FreeTextureColormaps(patch_t*);
void Patch_Free(patch_t*);
void *Picture_GetPatchPixel(patch_t*,pictureformat_t,INT32,INT32,pictureflags_t);
INT32 Picture_FormatBPP(pictureformat_t);
boolean Picture_IsPatchFormat(pictureformat_t);
boolean Picture_IsInternalPatchFormat(pictureformat_t);
boolean Picture_IsDoomPatchFormat(pictureformat_t);
boolean Picture_IsFlatFormat(pictureformat_t);
patch_t *Patch_CreateFromDoomPatch(softwarepatch_t*);
void Patch_CalcDataSizes(softwarepatch_t*,size_t*,size_t*);
void Patch_MakeColumns(softwarepatch_t*,size_t,INT16,UINT8*,column_t*,post_t*,boolean);
static void *Picture_Convert(pictureformat_t a,void *p,pictureformat_t b,size_t n,size_t *out,INT32 w,INT32 h,INT32 l,INT32 t,pictureflags_t flags);
/* FUNCTIONS */
static void *Picture_Convert(pictureformat_t a,void *p,pictureformat_t b,size_t n,size_t *out,INT32 w,INT32 h,INT32 l,INT32 t,pictureflags_t flags){(void)n;return Picture_PatchConvert(a,p,b,out,w,h,l,t,flags);}
static void digest(const void *p,size_t n){const UINT8*s=p;while(n--)hash=(hash^*s++)*16777619u;}
static void free_patch(patch_t *p){
 Patch_Free(p);
}
static GLPatch_t *hardware(patch_t *p){
 GLPatch_t*g=Z_Calloc(sizeof *g,PU_HWRPATCHINFO,&p->hardware);g->mipmap=Z_Calloc(sizeof *g->mipmap,PU_HWRPATCHINFO,&g->mipmap);return g;
}
static void pressure(void){
 for(size_t i=0;i<block_high;i++)if(blocks[i].live){
  if(blocks[i].tag==PU_HWRCACHE_UNLOCKED)Z_Free(blocks[i].p);
  else if(blocks[i].tag==PU_SPRITE&&Patch_IsEvictable(blocks[i].p))free_patch(blocks[i].p);
 }
}
static void one(patch_t *p,int tag,unsigned mapped,unsigned slow,unsigned chroma){
 Z_ChangeTag(p,tag);GLPatch_t *g=hardware(p);GLMipmap_t*m=g->mipmap;
 if(mapped){GLMipmap_t *first=calloc(1,sizeof *first);check(first!=NULL,"first mapped mipmap allocation");m->nextcolormap=first;first->nextcolormap=calloc(1,sizeof *first);check(first->nextcolormap!=NULL,"second mapped mipmap allocation");m=first->nextcolormap;m->colormap=Z_Calloc(sizeof *m->colormap,PU_HWRPATCHCOLMIPMAP,NULL);for(unsigned i=0;i<256;i++)m->colormap->data[i]=(UINT8)(mapped==1?255-i:(i*37u)^0xa5u);}
 ps2_slow_composite=(boolean)slow;HWR_MakePatch(p,g,m,true);if(chroma){m->flags|=TF_CHROMAKEYED;HWR_MakePatch(p,g,m,true);}
 check(m->data!=NULL,"reference made exact pixels");size_t bytes=(size_t)m->width*m->height*2;UINT8 *expected=malloc(bytes);memcpy(expected,m->data,bytes);digest(expected,bytes);
 m->ps2_planfr=0xabcdef98u;m->ps2_ihfr=0xffffffffu;m->ps2_pi=0xffffu;m->ps2_want=255;m->ps2_cost=255;m->downloaded=0x87654321u;
 GLMipmap_t before=*m;before.data=NULL;(void)before;
 Z_ChangeTag(m->data,PU_HWRCACHE_UNLOCKED);pressure();check(p->hardware==g&&!m->data,"purge clears image owner, preserves pinned canonical source");
 for(unsigned armed=0;armed<2;armed++){
  guard_armed=armed;fail_try=1;unsigned fatal_before=fatal_calls,guard_before=guard_calls,malloc_before=malloc_calls;
  HWR_PS2_RegenerateMipmap(m);check(!m->data,"failed try leaves existing missing-data state");check(fatal_calls==fatal_before&&guard_calls==guard_before,"failed rescue never throws or becomes fatal");check(malloc_calls==malloc_before,"rescue never takes mandatory allocation route");
 }
 fail_try=0;HWR_PS2_RegenerateMipmap(m);
#ifdef PS2_NOOPT_PATCH_RESCUE
 check(!m->data,"opt-out retains original missing patch");
#else
 check(m->data!=NULL,"purged patch rescued");check(!memcmp(expected,m->data,bytes),"complete before-purge AP88 bytes identical");compared_bytes+=bytes;
 GLMipmap_t after=*m;after.data=NULL;check(!memcmp(&before,&after,sizeof before),"rescue preserves every nonpixel metadata byte");
 unsigned tc=try_calls,ic=iter_calls;HWR_PS2_RegenerateMipmap(m);check(tc==try_calls&&ic==iter_calls,"present pixels skip all rescue work");
 Z_Free(m->data);HWR_PS2_RegenerateMipmap(m);check(m->data&&!memcmp(expected,m->data,bytes),"repeat purge regenerates identical bytes");
 compared_bytes+=bytes;
#endif
 free(expected);free_patch(p);cases++;
}
static void unsupported(void){
 GLMipmap_t m={0};unsigned tc=try_calls,mc=malloc_calls,ic=iter_calls;
 m.format=GL_TEXFMT_AP_88;m.width=m.height=65535;HWR_PS2_RegenerateMipmap(&m);check(!m.data&&tc==try_calls&&ic==iter_calls,"oversized request rejected without allocator or scan");
 m.width=m.height=0;HWR_PS2_RegenerateMipmap(&m);check(!m.data&&tc==try_calls&&ic==iter_calls,"zero size rejected");
 m.width=m.height=16;m.format=GL_TEXFMT_P_8;HWR_PS2_RegenerateMipmap(&m);check(!m.data&&tc==try_calls&&ic==iter_calls,"model and raw-flat recipes excluded");
 m.format=GL_TEXFMT_ALPHA_8;HWR_PS2_RegenerateMipmap(&m);check(!m.data&&tc==try_calls&&ic==iter_calls,"fade-mask recipe excluded");
 m.format=GL_TEXFMT_AP_88;HWR_PS2_RegenerateMipmap(&m);check(!m.data&&tc==try_calls,"no matching parent preserves prior skip");
 patch_t *fake=Patch_Create(16,16);GLPatch_t*g=hardware(fake);g->mipmap->format=GL_TEXFMT_AP_88;g->mipmap->width=g->mipmap->height=16;HWR_PS2_RegenerateMipmap(g->mipmap);check(!g->mipmap->data&&tc==try_calls,"placeholder has no real source columns");free_patch(fake);
 check(mc==malloc_calls-3,"unsupported recipes allocate no rescue memory");cases++;
}
static patch_t *flat_source(unsigned w,unsigned h,unsigned mode){
 UINT16 *flat=malloc((size_t)w*h*2);check(flat!=NULL,"flat fixture allocation");
 for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++){UINT8 index=(UINT8)(x*29u+y*67u);unsigned hole=mode==1||((x*3u+y*5u)%11u)<3u;flat[y*w+x]=hole?0:(UINT16)(0xff00u|index);}
 patch_t*p=Picture_PatchConvert(PICFMT_FLAT16,flat,PICFMT_PATCH,NULL,w,h,3,5,0);free(flat);return p;
}
int main(int argc,char **argv){
 for(unsigned i=0;i<256;i++)palette[i].rgba=i|(i<<8)|(i<<16)|0xff000000u;
 for(unsigned i=0;i<ROTANGLES;i++){rollcosang[i]=(fixed_t)(cos(6.283185307179586*i/ROTANGLES)*FRACUNIT);rollsinang[i]=(fixed_t)(sin(6.283185307179586*i/ROTANGLES)*FRACUNIT);}
 void *poison=Z_Malloc(1,PU_PATCH_DATA,NULL);*(UINT8*)poison=0;
 for(unsigned s=0;s<2;s++)for(unsigned c=0;c<2;c++)for(unsigned mapped=0;mapped<3;mapped++)for(unsigned tag=14;tag<=19;tag++)if(tag!=17)
  for(unsigned d=0;d<6;d++){const unsigned dims[][2]={{1,1},{7,9},{15,65},{17,63},{128,127},{257,129}};one(flat_source(dims[d][0],dims[d][1],0),(int)tag,mapped,s,c);}
 for(unsigned s=0;s<2;s++){
  patch_t*p=Patch_Create(7,9);p->columns=Z_Calloc(sizeof(column_t)*7,PU_PATCH_DATA,NULL);one(p,PU_HUDGFX,2,s,0);
 }
 for(unsigned angle=1;angle<ROTANGLES;angle++)for(unsigned flip=0;flip<2;flip++){
  patch_t*p=flat_source(17,19,0);rotsprite_t rot={ROTANGLES,NULL};rot.patches=Z_Calloc(ROTANGLES*2*sizeof(void*),PU_STATIC,NULL);
  RotatedPatch_DoRotation(&rot,p,(INT32)angle,8,9,(boolean)flip);unsigned idx=angle+(flip?ROTANGLES:0);patch_t*out=rot.patches[idx];check(out!=NULL,"actual rotation produces source patch");Z_SetUser(out,NULL);rot.patches[idx]=NULL;Z_Free(rot.patches);free_patch(p);one(out,PU_PATCH_ROTATED,angle%3,angle&1,0);
 }
 for(int a=1;a<argc;a++){
  FILE*f=fopen(argv[a],"rb");check(f!=NULL,"PNG oracle matrix opened");INT32 header[4];check(fread(header,sizeof header,1,f)==1,"PNG matrix header");size_t n=(size_t)header[0]*header[1];UINT16*flat=malloc(n*2);check(flat&&fread(flat,2,n,f)==n,"PNG matrix complete pixels");fclose(f);
  for(size_t i=0;i<n;i++)flat[i]=flat[i]<=255?(UINT16)(0xff00|flat[i]):0;
  for(unsigned slow=0;slow<2;slow++)for(unsigned mapped=0;mapped<3;mapped++){
   patch_t*p=Picture_PatchConvert(PICFMT_FLAT16,flat,PICFMT_PATCH,NULL,header[0],header[1],header[2],header[3],0);one(p,PU_HUDGFX,mapped,slow,0);
  }free(flat);
 }
 unsupported();Z_Free(poison);for(size_t i=0;i<block_high;i++)check(!blocks[i].live,"no source/metadata/image leaks");
 printf("{\"cases\":%u,\"checks\":%u,\"compared_bytes\":%llu,\"hash\":%u,\"try_calls\":%u,\"malloc_calls\":%u,\"fatal_calls\":%u,\"guard_calls\":%u}\n",cases,checks,compared_bytes,hash,try_calls,malloc_calls,fatal_calls,guard_calls);return 0;
}
