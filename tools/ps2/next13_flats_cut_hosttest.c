/* Service boundaries use a checked allocator, stable semantic pointer names,
 * and owner callbacks. Every production type and function is extracted. */
#define MAX_TEXTURES 8192
#define MAX_BLOCKS 512
enum { PU_HWRCACHE=10, PU_HWRCACHE_UNLOCKED=11, PU_HWRCACHE_LRU=12, LEVELFLAT_NONE=0 };
typedef struct { int type; INT32 texture_id; } levelflat_t;
typedef struct { int width,height; } texture_t;
static texture_t texture_values[MAX_TEXTURES], *textures[MAX_TEXTURES];
static INT32 texturetranslation[MAX_TEXTURES];
static UINT8 flat_pixels[4096];
static boolean currently_batching;
static GLMipmap_t *selected;
static unsigned trace_hash=2166136261u, events, errors, deletes, owner_calls;
static unsigned calloc_calls, fail_calloc, regen_null, fail_zone, steal_data;
static unsigned expected_error;
static unsigned callback_mode, callback_probes;
static void layout_callback_probe(void);
void HWR_InitMapTextures(void);
static size_t saved_wall_bytes, saved_flat_bytes, saved_count;
static int saved_compact;
static jmp_buf unwind;
static unsigned char arena[24*1024*1024] __attribute__((aligned(64)));
static size_t arena_used;
static struct { void *p; size_t bytes; void **owner; unsigned live, driver; } blocks[MAX_BLOCKS];
static unsigned nblocks;
static void check(int ok,const char *what){if(!ok){out("FAIL ");out(what);out("\n");quit(101);}}
static void hash_bytes(const void *p,size_t n){const unsigned char *s=p;while(n--)trace_hash=(trace_hash^*s++)*16777619u;}
static void event(unsigned op,unsigned a,unsigned b){events++;hash_bytes(&op,sizeof op);hash_bytes(&a,sizeof a);hash_bytes(&b,sizeof b);}
static GLMipmap_t *flat_at(size_t i) {
#if FLAT_CANDIDATE && defined(PS2) && defined(PS2_PROFILE) && !defined(PS2_NOOPT_FLAT_METADATA)
 return HWR_FlatMipmap(i);
#else
 return &gl_flats[i].mipmap;
#endif
}
static size_t flat_stride(void) {
#if FLAT_CANDIDATE && defined(PS2) && defined(PS2_PROFILE) && !defined(PS2_NOOPT_FLAT_METADATA)
 return sizeof(GLMapTexture_t)-gl_flats_cut;
#else
 return sizeof(GLMapTexture_t);
#endif
}
static unsigned mip_id(const GLMipmap_t *m) {
 uintptr_t p=(uintptr_t)m,a=(uintptr_t)gl_textures,b=(uintptr_t)gl_flats;
 size_t stride=flat_stride();
 if(!m)return 0;
 if(gl_textures&&p>=a&&p-a<gl_numtextures*sizeof(*gl_textures)&&(p-a)%sizeof(*gl_textures)==0)return 0x10000u+2u*(unsigned)((p-a)/sizeof(*gl_textures));
 if(gl_flats&&p>=b&&p-b<gl_numtextures*stride&&(p-b)%stride==0)return 0x20000u+2u*(unsigned)((p-b)/stride);
 for(size_t i=0;i<gl_numtextures;i++){
  if(gl_textures&&gl_textures[i].mipmap.nextcolormap==m)return 0x10001u+2u*(unsigned)i;
  if(gl_flats&&flat_at(i)->nextcolormap==m)return 0x20001u+2u*(unsigned)i;
 }
 check(0,"unknown/unstable mipmap pointer");return 0;
}
static int block_of(void *p){for(unsigned i=0;i<nblocks;i++)if(blocks[i].p==p)return(int)i;return -1;}
static void *allocate(size_t n,void **owner) {
 check(nblocks<MAX_BLOCKS,"allocation count bound");
 size_t bytes=n?n:1,aligned=(bytes+63u)&~63u;
 check(arena_used+aligned<=sizeof arena,"fixture arena capacity");
 void *p=arena+arena_used;arena_used+=aligned;
 blocks[nblocks].p=p;blocks[nblocks].bytes=n;blocks[nblocks].owner=owner;blocks[nblocks].live=1;nblocks++;
 memset(p,0,bytes);if(owner)*owner=p;return p;
}
static void release(void *p){if(!p)return;int b=block_of(p);check(b>=0&&blocks[b].live,"double/unknown free");if(blocks[b].owner){check(*blocks[b].owner==p,"zone owner points elsewhere");*blocks[b].owner=NULL;owner_calls++;}blocks[b].live=0;}
static void *test_calloc(size_t n,size_t width){calloc_calls++;if(calloc_calls<=2){if(calloc_calls==1)saved_wall_bytes=n*width;else saved_flat_bytes=n*width;}if(callback_mode==3&&calloc_calls==1){event(71,(unsigned)gl_numtextures,0);HWR_InitMapTextures();}if(fail_calloc==calloc_calls)return NULL;return allocate(n*width,NULL);}
static void test_free(void *p){if(p){unsigned id;if(p==gl_flats)id=2;else if(p==gl_textures)id=1;else id=mip_id((GLMipmap_t*)p);event(40,id,0);}release(p);}
static void Z_Free(void *p){unsigned id=0;if(p){int b=block_of(p);check(b>=0,"unknown zone free");id=(unsigned)blocks[b].bytes;}event(41,id,!!p);release(p);}
static void Z_ChangeTag(void *p,int tag){if(p){int b=block_of(p);check(b>=0&&blocks[b].live,"tagging freed data");}event(42,(unsigned)tag,!!p);}
static void Z_SetUser(void *p,void **owner){int b=block_of(p);check(b>=0&&blocks[b].live,"stealing freed data");blocks[b].owner=owner;*owner=p;owner_calls++;event(43,(unsigned)blocks[b].bytes,1);}
static void *Z_Malloc(size_t n,int tag,void **owner){event(44,(unsigned)n,(unsigned)tag);owner_calls++;return allocate(n,owner);}
static void *Z_TryMallocAlign(size_t n,int tag,void **owner,size_t align){event(45,(unsigned)n,(unsigned)tag);check(align==sizeof(void*),"regen alignment changed");if(fail_zone)return NULL;owner_calls++;return allocate(n,owner);}
static const UINT8 *R_TryGetFlatForTexture(size_t n){event(46,(unsigned)n,regen_null);return regen_null?NULL:flat_pixels;}
static const UINT8 *R_GetFlatForTexture(INT32 n){event(47,(unsigned)n,0);return flat_pixels;}
static void HWR_GCacheFlush(void){event(48,(unsigned)gl_numtextures,gl_maptexturesloaded);}
static void PS2TexC_Reset(void){event(49,0,0);}
static void Z_ClearRebuildCosts(void){event(50,0,0);}
__attribute__((noreturn)) static void I_Error(const char *fmt,...){(void)fmt;errors++;event(51,2,(unsigned)gl_numtextures);check(expected_error,"unexpected I_Error");longjmp(unwind,2);__builtin_unreachable();}
__attribute__((noreturn)) static void Z_GuardThrow(const char *why){(void)why;errors++;event(51,1,(unsigned)gl_numtextures);check(expected_error,"unexpected guard throw");check(!gl_textures&&!gl_flats&&!gl_numtextures&&!gl_maptexturesloaded,"guard exposed partial texture tables");
#if FLAT_CANDIDATE && defined(PS2) && defined(PS2_PROFILE) && !defined(PS2_NOOPT_FLAT_METADATA)
 check(!gl_flats_cut,"guard exposed stale layout");
#endif
 longjmp(unwind,1);__builtin_unreachable();}
static void HWR_GenerateTexture(INT32 n,GLMapTexture_t *tex,GLMipmap_t *m){(void)tex;event(52,(unsigned)n,mip_id(m));}
static void snapshot(GLMipmap_t *m){
 if(!m){event(60,0,0);return;}
 GLMipmap_t copy=*m;unsigned id=mip_id(m);
 if(m->data){int b=block_of(m->data);check(b>=0&&blocks[b].live,"mipmap names dead data");check(blocks[b].bytes==(size_t)m->width*m->height,"pixel byte extent changed");hash_bytes(m->data,blocks[b].bytes);}
 copy.data=(void*)(uintptr_t)!!m->data;copy.nextcolormap=(GLMipmap_t*)(uintptr_t)mip_id(m->nextcolormap);copy.colormap=NULL;
#ifdef PS2_PROFILE
 copy.ps2_twin=(GLMipmap_t*)(uintptr_t)mip_id(m->ps2_twin);
#endif
 event(60,id,0);hash_bytes(&copy,sizeof copy);
}
static struct { GLMipmap_t *m; void *data; } driver[16];
static unsigned ndriver;
static void HWR_PS2_RegenerateMipmap(GLMipmap_t *m);
static void *HWR_PS2_StealData(GLMipmap_t *m,void **owner);
static void test_set(GLMipmap_t *m){event(61,mip_id(m),0);snapshot(m);layout_callback_probe();
#ifdef PS2_PROFILE
 HWR_PS2_RegenerateMipmap(m);
 if(steal_data&&m->data){check(ndriver<16,"driver fixture slots");driver[ndriver].m=m;HWR_PS2_StealData(m,&driver[ndriver].data);ndriver++;}
#endif
 m->downloaded=7;snapshot(m);}
static void test_delete(GLMipmap_t *m){unsigned id=mip_id(m);deletes++;event(62,id,0);snapshot(m);layout_callback_probe();for(size_t i=0;i<gl_numtextures;i++){if(gl_textures[i].mipmap.nextcolormap==m)event(63,0x10000u+2u*(unsigned)i,1);if(flat_at(i)->nextcolormap==m)event(63,0x20000u+2u*(unsigned)i,1);}for(unsigned i=0;i<ndriver;i++)if(driver[i].m==m&&driver[i].data){Z_Free(driver[i].data);driver[i].m=NULL;}m->downloaded=0;}
static struct { void(*pfnDeleteTexture)(GLMipmap_t*);void(*pfnSetTexture)(GLMipmap_t*); } HWD={test_delete,test_set};
static void HWR_SetCurrentTexture(GLMipmap_t *m){selected=m;event(64,mip_id(m),0);snapshot(m);layout_callback_probe();}
#define HWR_PATCH_UNLOCKED(p) Z_ChangeTag((p),PU_HWRCACHE_UNLOCKED)
#define calloc test_calloc
#define free test_free
/* SOURCE_INSERTION */
#undef calloc
#undef free

static void layout_callback_probe(void){
 if(!callback_mode||callback_mode==3)return;
 event(70,callback_mode,++callback_probes);
 if(callback_mode==2){
  /* Argv changes at a callback do not change a table's captured layout. */
  INT32 argc_saved=myargc;char **argv_saved=myargv;
  char *other[]={"callback","-hwgo","65536"};myargc=3;myargv=other;
  if(gl_flats&&gl_numtextures>3){size_t stride=flat_stride();check((uintptr_t)flat_at(3)-(uintptr_t)gl_flats==3*stride,"callback argv changed captured stride");}
  myargc=argc_saved;myargv=argv_saved;return;
 }
 /* Exercise the real initializer at all callback boundaries. Preserve the
  * caller's live tables, then install a bounded byte-view probe and restore
  * them before callback return; no stale/null table is dereferenced. */
 static GLMapTexture_t probe[8] __attribute__((aligned(64)));
 void *flats_saved=gl_flats;GLMapTexture_t *walls_saved=gl_textures;
 boolean loaded_saved=gl_maptexturesloaded;
#if FLAT_CANDIDATE && defined(PS2) && defined(PS2_PROFILE) && !defined(PS2_NOOPT_FLAT_METADATA)
 _Static_assert(sizeof gl_flats_cut==sizeof(size_t),"full-width captured cut");
 _Static_assert(__builtin_offsetof(GLMapTexture_t,mipmap)==0,"first-member conversion");
 _Static_assert(sizeof(GLMapTexture_t)%_Alignof(GLMipmap_t)==0&&sizeof(GLMipmap_t)%_Alignof(GLMipmap_t)==0,"both selected strides preserve alignment");
 size_t cut_saved=gl_flats_cut;
#endif
 HWR_InitMapTextures();
 gl_flats=(void*)probe;
 check((uintptr_t)flat_at(3)-(uintptr_t)gl_flats==3*sizeof(GLMapTexture_t),"callback init reset must retain legacy stride");
#if FLAT_CANDIDATE && defined(PS2) && defined(PS2_PROFILE) && !defined(PS2_NOOPT_FLAT_METADATA)
 check(gl_flats_cut==0,"callback initializer did not reset cut");
 gl_flats_cut=sizeof(GLMapTexture_t)-sizeof(GLMipmap_t);
 check((uintptr_t)flat_at(3)-(uintptr_t)gl_flats==3*sizeof(GLMipmap_t),"callback compact stride mismatch");
 gl_flats_cut=cut_saved;
#endif
 gl_flats=flats_saved;gl_textures=walls_saved;gl_maptexturesloaded=loaded_saved;
}

static void decimal(unsigned n){char s[16];unsigned at=0;do{s[at++]=(char)('0'+n%10u);n/=10u;}while(n);while(at){char ch[2]={s[--at],0};out(ch);}}
static void field(const char *name,unsigned v,int comma){out("\"");out(name);out("\":");decimal(v);if(comma)out(",");}
static void output(void){unsigned live=0;for(unsigned i=0;i<nblocks;i++)live+=blocks[i].live;out("{");field("hash",trace_hash,1);field("events",events,1);field("errors",errors,1);field("deletes",deletes,1);field("owners",owner_calls,1);field("live",live,1);field("calls",calloc_calls,1);field("count",(unsigned)saved_count,1);field("wall_bytes",(unsigned)saved_wall_bytes,1);field("flat_bytes",(unsigned)saved_flat_bytes,1);field("compact",(unsigned)saved_compact,0);out("}\n");}
static int requested_compact(void){INT32 saved=found;UINT32 off=0;if(M_CheckParm("-hwgo")&&M_IsNextParm())off=(UINT32)atoi(M_GetNextParm());found=saved;return !(off&0x10000u);}
static void record_layout(void){saved_count=gl_numtextures;
#if FLAT_CANDIDATE && defined(PS2) && defined(PS2_PROFILE) && !defined(PS2_NOOPT_FLAT_METADATA)
 saved_compact=!!gl_flats_cut;check(saved_compact==(callback_mode==3?0:requested_compact()),"legacy renderer changed pointer stride");
#else
 saved_compact=0;
#endif
 check(saved_wall_bytes==gl_numtextures*sizeof(GLMapTexture_t),"wall allocation changed");check(saved_flat_bytes==gl_numtextures*flat_stride(),"flat allocation changed");
 for(size_t i=0;i<gl_numtextures;i++){GLMipmap_t zero;memset(&zero,0,sizeof zero);check(!memcmp(flat_at(i),&zero,sizeof zero),"table not fully zero-initialized");check((uintptr_t)flat_at(i)-(uintptr_t)gl_flats==i*flat_stride(),"flat pointer stride changed");}
}
static void touch(size_t n,boolean chroma){levelflat_t f={1,(INT32)n};HWR_GetLevelFlat(&f,chroma);check(selected== (chroma?flat_at((size_t)texturetranslation[n])->nextcolormap:flat_at((size_t)texturetranslation[n])),"selected identity differs from persistent slot");}
static void metadata(GLMipmap_t *m,unsigned seed){
#ifdef PS2_PROFILE
 m->ps2_planfr=0xfedcba98u^seed;m->ps2_ihfr=0x89abcdefu+seed;m->ps2_imfr=0xffffffffu-seed;m->ps2_full_fr=0x80000000u+seed;m->ps2_gcv=0x7fffffffu-seed;m->ps2_spv=0xffff0000u+seed;m->ps2_pi=(UINT16)(0xffffu-seed);m->ps2_uw=0xffffu;m->ps2_uh=0x8001u;m->ps2_h255=2;m->ps2_want=7;m->ps2_ihint=5;m->ps2_vis=1;m->ps2_nup=255;m->ps2_drop=4;m->ps2_cost=254;
#else
 (void)m;(void)seed;
#endif
}
static void drop(GLMipmap_t *m){if(m->data)Z_Free(m->data);m->downloaded=0;event(65,mip_id(m),0);}
static void cleanup_loose_zone(void){for(unsigned i=0;i<nblocks;i++)if(blocks[i].live&&blocks[i].owner){blocks[i].owner=NULL;blocks[i].live=0;}}
__attribute__((used)) static int test_main(int argc,char **argv){
 check(argc>=2,"scenario required");const char *scenario=argv[1];myargc=argc-1;myargv=argv+1;
 callback_mode=!strcmp(scenario,"callback-init")?1u:!strcmp(scenario,"callback-argv")?2u:!strcmp(scenario,"callback-calloc")?3u:0u;
 for(unsigned i=0;i<sizeof flat_pixels;i++)flat_pixels[i]=(UINT8)((i*37u)^(i>>3));
 for(unsigned i=0;i<MAX_TEXTURES;i++){texture_values[i].width=1+(int)(i%31);texture_values[i].height=1+(int)(i%29);textures[i]=&texture_values[i];texturetranslation[i]=(INT32)i;}
#ifdef FLAT_FREESTANDING32
#ifdef PS2_PROFILE
#ifndef PS2_NOOPT_MIPMAP_LAYOUT
 _Static_assert(sizeof(GLMipmap_t)==76&&sizeof(GLMapTexture_t)==84,"actual EE canonical metadata sizes");
#else
 _Static_assert(sizeof(GLMipmap_t)==88&&sizeof(GLMapTexture_t)==96,"actual EE original metadata sizes");
#endif
#endif
#endif
 found=77;
#if FLAT_CANDIDATE && defined(PS2) && defined(PS2_PROFILE) && !defined(PS2_NOOPT_FLAT_METADATA)
 boolean wanted=(boolean)requested_compact();check(HWR_UseCompactFlatMetadata()==wanted,"startup guard differs from canonical parser");check(found==77,"layout query changed shared command cursor");
#endif
 HWR_InitMapTextures();
 size_t n=!strcmp(scenario,"zero")?0:!strcmp(scenario,"large")?4740:17;
 if(!strcmp(scenario,"fail-first"))fail_calloc=1;
 if(!strcmp(scenario,"fail-second"))fail_calloc=2;
 expected_error=fail_calloc||!strcmp(scenario,"injected");
 if(!setjmp(unwind))HWR_LoadMapTextures(n);
 if(!gl_maptexturesloaded){
#ifndef PS2
  /* The generic I_Error is fatal in production. Observe the unchanged fatal
   * state, then reclaim it only inside this fixture. */
  if(gl_textures)release(gl_textures);if(gl_flats)release(gl_flats);gl_textures=NULL;gl_flats=NULL;gl_numtextures=0;
#endif
  fail_calloc=0;expected_error=0;calloc_calls=0;HWR_LoadMapTextures(n);
 }
 record_layout();
 if(!strcmp(scenario,"injected")){
  expected_error=1;
  if(!setjmp(unwind))HWR_LoadMapTextures(n);
  if(!gl_maptexturesloaded){
#ifndef PS2
   if(gl_textures)release(gl_textures);if(gl_flats)release(gl_flats);gl_textures=NULL;gl_flats=NULL;gl_numtextures=0;
#endif
   expected_error=0;calloc_calls=0;HWR_LoadMapTextures(n);
  }
  saved_count=gl_numtextures;
 }
 if(!n||!strcmp(scenario,"unused")){HWR_FreeMapTextures();output();return 0;}
 levelflat_t missing={LEVELFLAT_NONE,0},negative={1,-1};HWR_GetLevelFlat(&missing,true);HWR_GetLevelFlat(&negative,false);
 if(!strcmp(scenario,"fail-variant"))fail_calloc=calloc_calls+1;
 expected_error=!strcmp(scenario,"fail-variant");
 if(!setjmp(unwind))touch(0,true);
 fail_calloc=0;expected_error=0;
 touch(0,false);GLMipmap_t *identity0=selected;metadata(identity0,3);snapshot(identity0);
 touch(0,true);GLMipmap_t *identity1=selected;metadata(identity1,5);snapshot(identity1);
#ifdef PS2_PROFILE
 check(identity0->regen_kind==2&&identity1->regen_kind==2&&identity0->regen_id==0&&identity1->regen_id==0&&identity1->ps2_twin==identity0,"regen/twin identity changed");
#endif
 touch(0,false);check(selected==identity0,"primary identity moved");touch(0,true);check(selected==identity1,"variant identity moved");
 currently_batching=true;touch(2,false);GLMipmap_t *batch0=selected;touch(2,true);GLMipmap_t *batch1=selected;currently_batching=false;
 if(!strcmp(scenario,"purge")||!strcmp(scenario,"regen-null")||!strcmp(scenario,"regen-fail")||!strcmp(scenario,"steal")){
#ifdef PS2_PROFILE
  drop(identity0);drop(identity1);regen_null=!strcmp(scenario,"regen-null");fail_zone=!strcmp(scenario,"regen-fail");steal_data=!strcmp(scenario,"steal");
  HWR_PS2_RegenerateMipmap(identity0);HWR_PS2_RegenerateMipmap(identity1);snapshot(identity0);snapshot(identity1);
  if(regen_null||fail_zone)check(!identity0->data&&!identity1->data,"regen failure installed partial data");
  regen_null=fail_zone=0;test_set(identity0);test_set(identity1);
#else
  identity1->data=NULL;drop(identity0);touch(0,false);touch(0,true);
#endif
 }
 test_set(batch0);test_set(batch1);snapshot(batch0);snapshot(batch1);
 /* Force source and translated texture numbers to differ on repeated calls. */
 texturetranslation[1]=2;touch(1,false);check(selected==batch0,"translated index identity changed");texturetranslation[1]=1;
 if(!strcmp(scenario,"reload")){
  /* Change argv after construction: teardown must use its captured layout. */
  char *other[]={"reload","-hwgo",requested_compact()?"65536":"0"};myargc=3;myargv=other;
  HWR_FreeMapTextures();cleanup_loose_zone();calloc_calls=0;HWR_LoadMapTextures(n+19);record_layout();touch(3,false);HWR_FreeMapTextures();
  calloc_calls=0;HWR_LoadMapTextures(0);record_layout();HWR_FreeMapTextures();
 }else HWR_FreeMapTextures();
 check(!gl_textures&&!gl_flats&&!gl_numtextures&&!gl_maptexturesloaded,"teardown retained table state");
#if FLAT_CANDIDATE && defined(PS2) && defined(PS2_PROFILE) && !defined(PS2_NOOPT_FLAT_METADATA)
 check(!gl_flats_cut,"teardown retained layout state");
#endif
 cleanup_loose_zone();output();return 0;
}
#ifndef FLAT_FREESTANDING32
int main(int argc,char **argv){return test_main(argc,argv);}
#endif
