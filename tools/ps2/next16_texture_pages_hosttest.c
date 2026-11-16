/* Production declarations and complete cache functions are inserted by the
 * runner. The service model checks stable identities and live zone owners. */
/* PREAMBLE */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <setjmp.h>
typedef uint8_t UINT8;typedef uint16_t UINT16;typedef uint32_t UINT32;typedef int32_t INT32;
typedef enum {false,true} boolean;
static void *test_calloc(size_t,size_t);
static void test_free(void *);
static void Z_GuardThrow(const char *);
__attribute__((noreturn)) static void I_Error(const char *,...);
static void backing_free(void *p){free(p);}
#define calloc test_calloc
#define free test_free
/* DECLARATIONS */

#define FRACUNIT 65536
enum {PU_HWRCACHE=10,PU_HWRCACHE_UNLOCKED=11,LEVELFLAT_NONE=0};
typedef struct {int type;INT32 texture_id;} levelflat_t;
typedef struct {int width,height,transparency;} texture_t;
static texture_t **textures;static INT32 *texturetranslation;static INT32 numtextures;
static int textureformat=GL_TEXFMT_P_8;
static boolean currently_batching=true;
static UINT32 hwr_texsig=1;
static GLMipmap_t *selected;
static UINT8 pixels[64];
static unsigned hash=2166136261u,events,checks,alloc_calls,fail_alloc,errors;
static size_t live_bytes,peak_bytes,live_blocks;
static unsigned expected_error;
static jmp_buf unwind;
static struct allocation {void *p;size_t bytes;void **owner;unsigned live;} allocations[40000];
static size_t allocation_count;
static struct held {GLMipmap_t *m;void *data;} held[32768];
static size_t held_count;
static void HWR_PS2_RegenerateMipmap(GLMipmap_t *);
static void *HWR_PS2_StealData(GLMipmap_t *,void **);
static void steal(GLMipmap_t *);

static void check(int yes,const char *what){checks++;if(!yes){fprintf(stderr,"Page proof failed: %s\n",what);exit(101);}}
static void word(UINT32 v){hash=(hash^v)*16777619u;}
static void event(UINT32 op,UINT32 a,UINT32 b){events++;word(op);word(a);word(b);}
static unsigned mip_id(const GLMipmap_t *m){return m?(unsigned)m->regen_id*4u+(unsigned)m->regen_kind+(m->ps2_twin?0x40000000u:0u):0;}
static int block_of(const void *p){for(size_t i=0;i<allocation_count;i++)if(allocations[i].p==p&&allocations[i].live)return(int)i;return-1;}
static void *allocate(size_t n,void **owner){
 void *p=malloc(n?n:1);check(p!=NULL,"host backing allocation");
 check(allocation_count<sizeof allocations/sizeof *allocations,"allocation bookkeeping capacity");
 memset(p,0,n?n:1);allocations[allocation_count++]=(struct allocation){p,n,owner,1};
 live_bytes+=n;live_blocks++;if(live_bytes>peak_bytes)peak_bytes=live_bytes;if(owner)*owner=p;return p;
}
static void release(void *p){
 if(!p)return;int b=block_of(p);check(b>=0,"known live allocation freed");
 if(allocations[b].owner){check(*allocations[b].owner==p,"zone owner names its allocation");*allocations[b].owner=NULL;}
 for(size_t i=0;i<allocation_count;i++)if(allocations[i].live&&allocations[i].owner){uintptr_t o=(uintptr_t)allocations[i].owner,a=(uintptr_t)p;check(o<a||o-a>=allocations[b].bytes,"metadata freed while a live zone owner names it");}
 allocations[b].live=0;live_bytes-=allocations[b].bytes;live_blocks--;backing_free(p);
}
static void *test_calloc(size_t n,size_t width){alloc_calls++;if(alloc_calls==fail_alloc)return NULL;check(!width||n<=SIZE_MAX/width,"allocation product is representable");return allocate(n*width,NULL);}
static void test_free(void *p){release(p);}
static void Z_Free(void *p){if(p)event(10,(unsigned)allocations[block_of(p)].bytes,0);release(p);}
static void Z_ChangeTag(void *p,int tag){if(p)check(block_of(p)>=0,"tag only live pixels");(void)tag;}
static void Z_SetUser(void *p,void **owner){int b=block_of(p);check(b>=0,"owner transfer names live pixels");allocations[b].owner=owner;*owner=p;event(11,(unsigned)allocations[b].bytes,0);}
static void *Z_Malloc(size_t bytes,int tag,void **owner){(void)tag;return allocate(bytes,owner);}
static void *Z_TryMallocAlign(size_t bytes,int tag,void **owner,size_t align){(void)tag;check(align==sizeof(void*),"pixel alignment retained");return allocate(bytes,owner);}
static const UINT8 *R_TryGetFlatForTexture(size_t n){event(12,(unsigned)n,0);return pixels;}
static void HWR_GCacheFlush(void){event(13,0,0);}
static void PS2TexC_Reset(void){event(14,0,0);}
static void Z_ClearRebuildCosts(void){event(15,0,0);}
static void Z_GuardThrow(const char *why){(void)why;errors++;check(expected_error,"expected guarded allocation failure");longjmp(unwind,1);}
__attribute__((noreturn)) static void I_Error(const char *why,...){(void)why;errors++;check(expected_error,"expected fatal allocation failure");longjmp(unwind,2);}
static void HWR_GenerateTexture(INT32 n,GLMapTexture_t *wall,GLMipmap_t *m){
 check(wall!=NULL,"generator receives wall scales record");
 check(wall->mipmap.regen_kind==1&&wall->mipmap.regen_id==n,"generator receives the exact texture's scales record");
 if(!m->data){UINT8 *p=Z_TryMallocAlign(64,PU_HWRCACHE,&m->data,sizeof(void*));for(unsigned i=0;i<64;i++)p[i]=(UINT8)(i*37u+(unsigned)n);}
 wall->scaleX=1.0f/(8*FRACUNIT);wall->scaleY=wall->scaleX;event(16,(unsigned)n,mip_id(m));
}
static void snapshot(const GLMipmap_t *m){
 event(17,mip_id(m),m->downloaded);word(m->format);word(m->flags);word(m->width);word(m->height);
 word((UINT32)m->regen_id);word(m->regen_kind);word(m->ps2_h255);word(m->ps2_planfr);word(m->ps2_pi);
 word(m->ps2_want);word(m->ps2_ihint);word(m->ps2_ihfr);word(m->ps2_imfr);word(m->ps2_full_fr);word(m->ps2_gcv);word(m->ps2_spv);word(m->ps2_vis);word(m->ps2_nup);word(m->ps2_drop);word(m->ps2_cost);word(m->ps2_uw);word(m->ps2_uh);
 word(m->ps2_twin?mip_id(m->ps2_twin):0);word(!!m->data);
 if(m->data){int b=block_of(m->data);check(b>=0,"snapshot has live pixels");for(size_t i=0;i<allocations[b].bytes;i++)word(((UINT8*)m->data)[i]);}
}
static void test_set(GLMipmap_t *m){HWR_PS2_RegenerateMipmap(m);m->downloaded=0x89abcdefu;snapshot(m);if(m->regen_kind==2&&m->ps2_twin&&m->data)steal(m);}
static void test_delete(GLMipmap_t *m){
 /* Zero records in an eager table have never been submitted or acquired
  * owners. Their real callback fences no owned work and clears zero. */
 if(!m->regen_kind&&!m->data&&!m->downloaded&&!m->nextcolormap)return;
 event(18,mip_id(m),0);snapshot(m);
 for(size_t i=0;i<held_count;i++)if(held[i].m==m&&held[i].data){event(19,mip_id(m),0);Z_Free(held[i].data);held[i].m=NULL;}
 m->downloaded=0;
}
static struct{void(*pfnDeleteTexture)(GLMipmap_t*);void(*pfnSetTexture)(GLMipmap_t*);}HWD={test_delete,test_set};
static void HWR_SetCurrentTexture(GLMipmap_t *m){selected=m;if(m)snapshot(m);else event(20,0,0);}
/* FUNCTIONS */
#undef calloc
#undef free

static void touch(unsigned n,boolean flat,boolean chroma){
 if(flat){levelflat_t f={1,(INT32)n};HWR_GetLevelFlat(&f,chroma);}
 else {GLMapTexture_t *wall=HWR_GetTexture((INT32)n,chroma);check(wall->scaleX==1.0f/(8*FRACUNIT),"wall scale is initialized");}
 check(selected!=NULL,"selected logical record");check(selected->regen_id==(INT32)n&&selected->regen_kind==(flat?2:1),"logical identity retained");
 test_set(selected);snapshot(selected);
}
static void metadata(GLMipmap_t *m){m->ps2_planfr=0xffffffffu;m->ps2_ihfr=0x87654321u;m->ps2_imfr=0xabcdef98u;m->ps2_full_fr=0x80000000u;m->ps2_gcv=0xfedcba98u;m->ps2_spv=0xffffffffu;m->ps2_pi=0xffffu;m->ps2_want=255;m->ps2_ihint=255;m->ps2_uw=0xffffu;m->ps2_uh=0x8001u;m->ps2_h255=2;m->ps2_vis=1;m->ps2_nup=255;m->ps2_drop=5;m->ps2_cost=255;}
static void steal(GLMipmap_t *m){check(held_count<sizeof held/sizeof *held,"held catalog");held[held_count].m=m;HWR_PS2_StealData(m,&held[held_count].data);held_count++;snapshot(m);}
static void drop(GLMipmap_t *m){if(m->data)Z_Free(m->data);m->downloaded=0;}
static void cleanup(void){HWR_FreeMapTextures();check(!gl_textures&&!gl_flats&&!gl_numtextures&&!gl_maptexturesloaded,"complete cache reset");check(!live_bytes&&!live_blocks,"all pixel and metadata owners released");}
static void fail_next_page(unsigned n,boolean flat){
#ifdef HWR_TEXTURE_PAGES
 unsigned errors_before=errors;fail_alloc=alloc_calls+1;expected_error=1;
 if(!setjmp(unwind))touch(n,flat,false);
 check(errors==errors_before+1,"page allocation failure was reached");fail_alloc=expected_error=0;
 check(gl_maptexturesloaded&&gl_numtextures==(size_t)numtextures,"page failure preserves prior live cache");
 touch(n,flat,false);
#else
 touch(n,flat,false);
#endif
}
int main(int argc,char **argv){
 check(argc>=3,"scenario and N required");const char *scenario=argv[1];size_t n=strtoul(argv[2],NULL,10);numtextures=(INT32)n;
 myargc=argc-2;myargv=argv+2;for(unsigned i=0;i<64;i++)pixels[i]=(UINT8)(i*13u);
 textures=malloc((n?n:1)*sizeof *textures);texturetranslation=malloc((n?n:1)*sizeof *texturetranslation);
 texture_t *values=malloc((n?n:1)*sizeof *values);check(textures&&texturetranslation&&values,"fixture texture catalogue");
 for(size_t i=0;i<n;i++){values[i]=(texture_t){8,8,0};textures[i]=&values[i];texturetranslation[i]=(INT32)i;}
 HWR_InitMapTextures();
 if(!strcmp(scenario,"fail-first")||!strcmp(scenario,"fail-second")){fail_alloc=!strcmp(scenario,"fail-first")?1u:2u;expected_error=1;if(!setjmp(unwind))HWR_LoadMapTextures(n);check(errors==1,"constructor allocation failure reached");
#ifdef PS2
 check(!gl_textures&&!gl_flats&&!gl_numtextures&&!gl_maptexturesloaded,"constructor failure resets partial cache");
#else
 /* Generic I_Error is fatal. Reclaim its unchanged partial state only in
  * this fixture after observing the longjmp boundary. */
 if(gl_textures)release(gl_textures);if(gl_flats)release(gl_flats);gl_numtextures=0;HWR_InitMapTextures();
#endif
 check(!live_bytes,"constructor failure releases successful allocation");fail_alloc=expected_error=0;}
 HWR_LoadMapTextures(n);check(gl_numtextures==n&&gl_maptexturesloaded,"full logical count retained");
 if(n){unsigned calls_before=alloc_calls;check(HWR_PS2_TexTransparent((INT32)n-1)==0,"unallocated page is not transparent");if(n>1)check(HWR_PS2_SideTexWord((INT32)n-1)==(UINT32)n-1,"unallocated page key has no transparency");check(alloc_calls==calls_before,"read-only flag lookup never allocates");}
 if(n&&strcmp(scenario,"unused")){
  unsigned first=0,last=(unsigned)n-1;
  touch(first,false,false);GLMipmap_t *wall=selected;metadata(wall);snapshot(wall);steal(wall);
  touch(first,false,true);GLMipmap_t *wall_twin=selected;check(wall_twin->ps2_twin==wall,"wall twin names stable original");
  touch(first,true,false);GLMipmap_t *flat=selected;metadata(flat);snapshot(flat);steal(flat);
  touch(first,true,true);GLMipmap_t *flat_twin=selected;check(flat_twin->ps2_twin==flat,"flat twin names stable original");
  if(!strcmp(scenario,"fail-variant")&&n>1){touch(1,false,false);fail_alloc=alloc_calls+1;expected_error=1;if(!setjmp(unwind))touch(1,false,true);check(errors==1,"child allocation failure reached");fail_alloc=expected_error=0;touch(1,false,true);}
  touch(last,false,false);touch(last,true,true);
  if(!strcmp(scenario,"full"))for(size_t i=0;i<n;i++){touch((unsigned)i,false,(i&1)!=0);touch((unsigned)i,true,(i&2)!=0);}
  else if(strcmp(scenario,"fail-page"))for(size_t i=15;i<n&&i<81;i+=16){touch((unsigned)i,false,true);touch((unsigned)i,true,false);}
  if(!strcmp(scenario,"fail-page")&&n>32){fail_next_page(32,false);fail_next_page(32,true);}
  touch(first,false,false);check(selected==wall,"wall page never moves");touch(first,false,true);check(selected==wall_twin,"wall child identity stable");
  touch(first,true,false);check(selected==flat,"flat page never moves");touch(first,true,true);check(selected==flat_twin,"flat child identity stable");
  drop(wall_twin);drop(flat_twin);HWR_PS2_RegenerateMipmap(wall_twin);HWR_PS2_RegenerateMipmap(flat_twin);snapshot(wall_twin);snapshot(flat_twin);test_set(flat_twin);
  check(HWR_PS2_TexTransparent(0)==0,"unmarked transparency");wall->flags|=TF_TRANSPARENT;check(HWR_PS2_TexTransparent(0)==1,"existing page transparency");
  if(n>1){texturetranslation[1]=0;check(HWR_PS2_SideTexWord(1)==0,"zero texture translation");texturetranslation[1]=(INT32)last;word(HWR_PS2_SideTexWord(1));}
 }
 size_t observed_bytes=live_bytes,observed_peak=peak_bytes;cleanup();
 HWR_LoadMapTextures(n+1);cleanup();
 printf("{\"hash\":%u,\"events\":%u,\"errors\":%u,\"checks\":%u,\"bytes\":%zu,\"peak\":%zu,\"allocations\":%u}\n",hash,events,errors,checks,observed_bytes,observed_peak,alloc_calls);
 free(values);free(textures);free(texturetranslation);return 0;
}
