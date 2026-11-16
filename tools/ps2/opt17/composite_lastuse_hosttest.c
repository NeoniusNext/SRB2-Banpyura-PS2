/* Actual functions/types are inserted by composite_lastuse_hosttest.py.
 * The services below are explicit host boundaries; no emulator is used. */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <math.h>
typedef uint8_t UINT8;typedef uint16_t UINT16;typedef uint32_t UINT32;typedef uint64_t UINT64;
typedef int16_t INT16;typedef int32_t INT32;typedef int64_t INT64;typedef int boolean;typedef INT32 fixed_t;typedef UINT32 lumpnum_t;
#define false 0
#define true 1
#define ATTRPACK __attribute__((packed))
#define FRACUNIT 65536
#define FRACBITS 16
#define SHORT(x) (x)
#define LONG(x) (x)
#define M_Memcpy memcpy
#define max(a,b) ((a)>(b)?(a):(b))
#define min(a,b) ((a)<(b)?(a):(b))
#define stricmp strcasecmp
#define HWR_PATCHES_CHROMAKEY_COLORINDEX 255
#define HWR_TRYPATCH_MIN (64u<<10)
#define PNG_HEADER_SIZE 8
#define PICFMT_FLAT 1
#define PICFMT_PATCH 2
#define CONS_WARNING 1
#define CONS_Alert(...) ((void)0)
#define CONS_Printf(...) ((void)0)
enum {PU_STATIC=1,PU_PATCH=14,PU_RENDERWORK=26,PU_CACHE=49,PU_HWRCACHE=48};
static unsigned long long checks,cases,compared_bytes;
static unsigned long long digest=1469598103934665603ull;
static const char *case_name="initial";
static void check(int x,const char *why){checks++;if(!x){fprintf(stderr,"LASTUSE FAIL %s: %s\n",case_name,why);exit(101);}}
__attribute__((noreturn)) static void I_Error(const char *fmt,...){va_list a;va_start(a,fmt);vfprintf(stderr,fmt,a);va_end(a);fputc('\n',stderr);exit(102);}
/* TYPES */
typedef struct {UINT16 wad,lump;patch_t *p;} base_cpatch_t;
typedef struct {UINT16 wad,lump;patch_t *p;INT32 last;} ps2_cpatch_t;
#define PS2_CPATCH_MAX 32
static boolean ps2_slow_composite,ps2hwt_comp_old;
static unsigned ps2hwd_dbg_flags,ps2hwt_mkpatch_n,ps2hwt_mkpatch_cyc,hwr_texsig;
static unsigned ps2hwt_now(void){static unsigned n;return ++n;}
static texture_t *texture_slot[1],**textures=texture_slot;
static RGBA_t palette[256],*pMasterPalette=palette;
static UINT8 trans_tables[9][65536];
static GLColormap_t cmap;
static RGBA_t *HWR_GetTexturePalette(void){return palette;}
static RGBA_t V_GetMasterColor(UINT8 index){return palette[index];}
static UINT8 NearestColor(UINT8 r,UINT8 g,UINT8 b){unsigned best=~0u;UINT8 found=0;for(unsigned i=0;i<256;i++){int dr=r-palette[i].s.red,dg=g-palette[i].s.green,db=b-palette[i].s.blue;unsigned d=dr*dr+dg*dg+db*db;if(d<best){best=d;found=(UINT8)i;}}return found;}
static UINT8 *R_GetTranslucencyTable(INT32 n){check(n>=1&&n<=9,"real stock TRANS table index");return trans_tables[n-1];}
static void HWR_FreeTexture(patch_t *p){check(!p->hardware,"nonheld hardware source must remain live");}
static unsigned int textureformat=GL_TEXFMT_P_8;
static void Patch_CalcDataSizes(softwarepatch_t*,size_t*,size_t*);
static void Patch_MakeColumns(softwarepatch_t*,size_t,INT16,UINT8*,column_t*,post_t*,boolean);
static void Patch_Free(patch_t*);
static patch_t *Patch_CreateFromDoomPatch(softwarepatch_t*);
static patch_t *Patch_TryCreateFromDoomPatch(softwarepatch_t*);
typedef struct {UINT32 wad,lump,kind;char *name;UINT8 *raw;size_t rawsize,ee_patch;patch_t *cached;void *rawcached;unsigned builds;unsigned pinned;} resource_t;
static resource_t *resources, *lookup[4][16384];static size_t resource_count;
static int loading=-1,phase,fail_try,texc_present,texc_success;
static size_t ee_live,ee_peak;
static unsigned alloc_calls,try_calls;
static struct mem {void *p;size_t n,ee;void **owner;int live,charged,resource;} ledger[4096];
static size_t ledger_high;
static int mem_index(void *p){for(size_t i=0;i<ledger_high;i++)if(ledger[i].live&&ledger[i].p==p)return(int)i;return -1;}
static void *allocate(size_t n,int tag,void *owner){
 size_t i=0;while(i<ledger_high&&ledger[i].live)i++;check(i<4096,"allocation ledger capacity");if(i==ledger_high)ledger_high++;
 void *p=NULL;check(posix_memalign(&p,64,n?n:1)==0,"host backing allocation");memset(p,0,n?n:1);
 size_t ee=n;if(tag==PU_PATCH&&loading>=0)ee=resources[loading].ee_patch;
 ledger[i]=(struct mem){p,n,ee,owner,1,phase,tag==PU_PATCH?loading:-1};if(owner)*(void**)owner=p;
 if(phase){ee_live+=ee;if(ee_live>ee_peak)ee_peak=ee_live;}alloc_calls++;return p;
}
static void *Z_Malloc(size_t n,int tag,void *owner){return allocate(n,tag,owner);}
static void *Z_Calloc(size_t n,int tag,void *owner){return allocate(n,tag,owner);}
static void *Z_TryMallocAlign(size_t n,int tag,void *owner,int align){(void)align;try_calls++;if(fail_try==1)return NULL;return allocate(n,tag,owner);}
static void Z_Free(void *p){if(!p)return;int i=mem_index(p);check(i>=0,"free only live allocation");int r=ledger[i].resource;if(r>=0&&resources[r].cached==p)check(!resources[r].pinned,"nonheld cached source preserved");if(ledger[i].charged){check(ee_live>=ledger[i].ee,"allocation accounting");ee_live-=ledger[i].ee;}if(ledger[i].owner){check(*ledger[i].owner==p,"live owner before free");*ledger[i].owner=NULL;}ledger[i].live=0;free(p);}
static void *Z_Realloc(void *p,size_t n,int tag,void *owner){if(!p)return allocate(n,tag,owner);int i=mem_index(p);check(i>=0,"resize live block");size_t old=ledger[i].n;void*q=allocate(n,tag,NULL);memcpy(q,p,old<n?old:n);Z_Free(p);int j=mem_index(q);ledger[j].owner=owner;if(owner)*(void**)owner=q;return q;}
static void Z_ChangeTag(void *p,int tag){(void)tag;check(mem_index(p)>=0,"retag live allocation");}
static void Z_SetUser(void *p,void **owner){int i=mem_index(p);check(i>=0,"set live allocation owner");ledger[i].owner=owner;if(owner)*owner=p;}
static void Z_SetRebuildCost(void *owner,unsigned ticks){(void)owner;(void)ticks;}
static resource_t *entry(UINT16 w,UINT16 l){check(w<4&&l<16384&&lookup[w][l],"archive resource identity");return lookup[w][l];}
static size_t W_LumpLengthPwad(UINT16 w,UINT16 l){return entry(w,l)->rawsize;}
static size_t W_ReadLumpHeaderPwad(UINT16 w,UINT16 l,void *dest,size_t n,size_t off){resource_t*r=entry(w,l);if(!n)n=r->rawsize-off;check(off<=r->rawsize&&n<=r->rawsize-off,"archive range");memcpy(dest,r->raw+off,n);return n;}
static boolean Picture_IsLumpCooked(const UINT8 *p,size_t n){return n>=8&&!memcmp(p,"\211SRPIC\r\n",8);}
static boolean Picture_IsLumpPNG(const UINT8 *p,size_t n){return Picture_IsLumpCooked(p,n);}
static void *Picture_TryCookedPatch(const UINT8*p,size_t n){check(Picture_IsLumpCooked(p,n),"cooked marker");return Patch_TryCreateFromDoomPatch((softwarepatch_t*)(p+8));}
static void *Picture_PNGConvert(const UINT8*p,int fmt,void*w,void*h,void*t,void*l,size_t n,void*out,unsigned flags){(void)fmt;(void)w;(void)h;(void)t;(void)l;(void)out;(void)flags;check(Picture_IsLumpCooked(p,n),"all stock PNGs are actual cooked patches");return Patch_CreateFromDoomPatch((softwarepatch_t*)(p+8));}
static void *Picture_Convert(int in,void*p,int out,size_t n,void*s,INT32 w,INT32 h,INT32 l,INT32 t,unsigned flags){(void)in;(void)p;(void)out;(void)n;(void)s;(void)w;(void)h;(void)l;(void)t;(void)flags;I_Error("unexpected nonpatch source in patch corpus");}
static void *W_GetCachedPatchNumPwad(UINT16 w,UINT16 l){return entry(w,l)->cached;}
static patch_t *load_patch(UINT16 w,UINT16 l,int tryonly){
 resource_t *r=entry(w,l);if(r->cached)return r->cached;if(tryonly&&fail_try==2)return NULL;
 UINT8 *raw=Z_Malloc(r->rawsize,PU_RENDERWORK,NULL);memcpy(raw,r->raw,r->rawsize);UINT8 *data=raw+(Picture_IsLumpCooked(raw,r->rawsize)?8:0);
 loading=(int)(r-resources);patch_t *p=tryonly?Patch_TryCreateFromDoomPatch((softwarepatch_t*)data):Patch_CreateFromDoomPatch((softwarepatch_t*)data);loading=-1;Z_Free(raw);
 if(p){Z_SetUser(p,(void**)&r->cached);r->builds++;}return p;
}
static void *W_CachePatchNumPwadNoGL(UINT16 w,UINT16 l,INT32 tag){(void)tag;return load_patch(w,l,0);}
static void *W_CachePatchNumPwad(UINT16 w,UINT16 l,INT32 tag){(void)tag;return load_patch(w,l,0);}
static void *W_TryCachePatchNumPwad(UINT16 w,UINT16 l,INT32 tag){(void)tag;return load_patch(w,l,1);}
static void *W_CacheLumpNumPwad(UINT16 w,UINT16 l,INT32 tag){resource_t*r=entry(w,l);if(!r->rawcached){r->rawcached=Z_Malloc(r->rawsize,tag,&r->rawcached);memcpy(r->rawcached,r->raw,r->rawsize);}return r->rawcached;}
static boolean PS2TexC_Present(void){return texc_present;}
static boolean PS2TexC_Fetch(INT32 n,UINT8*p,size_t bytes){(void)n;if(texc_success)memset(p,0x37,bytes);return texc_success;}
static UINT32 quickncasehash(const char*p,size_t n){UINT32 h=2166136261u;while(n--&&*p)h=(h^(unsigned)toupper((unsigned char)*p++))*16777619u;return h;}
static char *strupr(char*p){for(char*q=p;*q;q++)*q=(char)toupper((unsigned char)*q);return p;}
#define LUMPNUM(n) ((UINT16)(n))
#define WADFILENUM(n) ((UINT16)((n)>>16))
static lumpnum_t W_GetTexPatchLumpNum(const char *name){
 for(int category=0;category<3;category++)for(int w=3;w>=0;w--)for(size_t i=0;i<resource_count;i++){
  resource_t*r=&resources[i];if(r->kind||r->wad!=(unsigned)w)continue;
  if(category==0&&strncasecmp(r->name,"Patches/",8))continue;if(category==1&&strncasecmp(r->name,"Textures/",9))continue;
  const char*s=strrchr(r->name,'/');s=s?s+1:r->name;char shortname[9]={0};unsigned j=0;while(s[j]&&s[j]!='.'&&j<8){shortname[j]=s[j];j++;}
  if(!strcasecmp(shortname,name))return(r->wad<<16)|r->lump;
 }
 return UINT32_MAX; // Same unresolved-name result as W_CheckNumForPatchName.
}
/* FUNCTIONS */
static void read_exact(FILE*f,void*p,size_t n){check(fread(p,1,n,f)==n,"complete corpus read");}
static UINT32 read32(FILE*f){UINT32 n;read_exact(f,&n,4);return n;}
static void read_corpus(const char*path){
 FILE*f=fopen(path,"rb");check(f!=NULL,"open stock corpus");char magic[8];read_exact(f,magic,8);check(!memcmp(magic,"LASTUSE1",8),"corpus version");resource_count=read32(f);check(resource_count<30000,"stock resource count");
 UINT8 pal[768];read_exact(f,pal,768);for(unsigned i=0;i<256;i++){palette[i].s.red=pal[i*3];palette[i].s.green=pal[i*3+1];palette[i].s.blue=pal[i*3+2];palette[i].s.alpha=255;cmap.data[i]=(UINT8)((i*37)^0x5a);}
 read_exact(f,trans_tables,sizeof trans_tables);resources=calloc(resource_count,sizeof *resources);check(resources!=NULL,"resource catalog");
 for(size_t i=0;i<resource_count;i++){
  resource_t*r=&resources[i];r->wad=read32(f);r->lump=read32(f);r->kind=read32(f);size_t ns=read32(f);r->rawsize=read32(f);r->name=calloc(ns+1,1);r->raw=calloc(r->rawsize+1,1);check(r->name&&r->raw,"immutable source backing");read_exact(f,r->name,ns);read_exact(f,r->raw,r->rawsize);
  check(r->wad<4&&r->lump<16384&&!lookup[r->wad][r->lump],"unique catalog key");lookup[r->wad][r->lump]=r;
  if(!r->kind){size_t pixels=0,posts=0;UINT8 *raw=r->raw+(Picture_IsLumpCooked(r->raw,r->rawsize)?8:0);Patch_CalcDataSizes((softwarepatch_t*)raw,&pixels,&posts);INT16 width;memcpy(&width,raw,2);r->ee_patch=((48+(size_t)width*12+posts*12+15)&~(size_t)15)+pixels;}
 }
 check(fgetc(f)==EOF,"no unused corpus tail");fclose(f);
}
static void clean_sources(void){for(size_t i=0;i<resource_count;i++){resource_t*r=&resources[i];if(r->cached){r->pinned=0;r->cached->hardware=NULL;Patch_Free(r->cached);}Z_Free(r->rawcached);}check(ee_live==0,"all phase allocations freed");}
static unsigned *build_vector(void){unsigned*v=malloc(resource_count*sizeof *v);check(v!=NULL,"build vector");for(size_t i=0;i<resource_count;i++)v[i]=resources[i].builds;return v;}
static void clear_builds(void){for(size_t i=0;i<resource_count;i++)resources[i].builds=0;}
static size_t observed_old_peak,observed_new_peak;static unsigned total_base_builds,total_new_builds;
static void digest_bytes(const void*p,size_t n){const UINT8*q=p;while(n--)digest=(digest^*q++)*1099511628211ull;}
static void compare_one(texture_t*t,int format,int mapped,int old,int slow,int cached,int fail,int texc,int require_reduction){
 static GLMapTexture_t g[2];GLMipmap_t m[2]={{0}};unsigned *counts[2];size_t peaks[2],calls[2],tries[2];
 static char label[9];memcpy(label,t->name,8);label[8]=0;texture_slot[0]=t;case_name=label;UINT8 *expected=NULL;size_t bytes=(size_t)t->width*t->height*format2bpp(format);ps2hwd_dbg_flags=0;
 for(int role=0;role<2;role++){
  clean_sources();clear_builds();if(cached){for(INT32 i=0;i<t->patchcount;i+=2){resource_t*r=entry(t->patches[i].wad,t->patches[i].lump);load_patch(r->wad,r->lump,0);r->pinned=1;r->cached->hardware=(void*)0x1234;}clear_builds();}
  m[role].width=t->width;m[role].height=t->height;m[role].format=format;m[role].flags=TF_WRAPX|TF_WRAPY|TF_CHROMAKEYED;m[role].colormap=mapped?&cmap:NULL;
  ps2hwt_comp_old=old;ps2_slow_composite=slow;fail_try=fail;texc_present=texc;texc_success=texc==1;ee_peak=0;alloc_calls=try_calls=0;phase=1;
  if(role)candidate_generate(0,&g[role],&m[role]);else base_generate(0,&g[role],&m[role]);phase=0;
  fail_try=0;counts[role]=build_vector();peaks[role]=ee_peak;calls[role]=alloc_calls;tries[role]=try_calls;
  for(INT32 i=0;i<t->patchcount;i++){resource_t*r=entry(t->patches[i].wad,t->patches[i].lump);if(r->pinned)check(r->cached&&r->cached->hardware==(void*)0x1234,"caller-owned cached patch kept");else check(!r->cached,"all compositor-owned patches freed on return");}
  if(!role&&m[role].data){expected=malloc(bytes);check(expected!=NULL,"reference complete image");memcpy(expected,m[role].data,bytes);}
  if(role){check(!!m[0].data==!!m[1].data,"same output allocation behavior");if(m[role].data){check(!memcmp(expected,m[role].data,bytes),"every complete composed byte exact");compared_bytes+=bytes;digest_bytes(m[role].data,bytes);}check(m[0].flags==m[1].flags&&!memcmp(&g[0].scaleX,&g[1].scaleX,sizeof g[0].scaleX)&&!memcmp(&g[0].scaleY,&g[1].scaleY,sizeof g[0].scaleY),"metadata/scales exact");}
  // Keep only the pointer truth value needed by the next role, never follow freed bytes.
  if(role)Z_Free(m[role].data);else{Z_Free(m[role].data);if(expected)m[0].data=(void*)1;}
  clean_sources();
 }
 check(!memcmp(counts[0],counts[1],resource_count*sizeof **counts),"per-resource canonical build counts unchanged");check(calls[0]==calls[1]&&tries[0]==tries[1],"allocation/try decisions unchanged");
 for(size_t i=0;i<resource_count;i++){total_base_builds+=counts[0][i];total_new_builds+=counts[1][i];}free(counts[0]);free(counts[1]);free(expected);
 check(peaks[1]<=peaks[0],"candidate EE-equivalent payload peak never grows");if(require_reduction)check(peaks[1]<peaks[0],"last-use release lowers measured live payload");
 if(!memcmp(t->name,"SKY11",5)&&format==GL_TEXFMT_P_8&&!mapped&&!old&&!slow&&!cached&&!fail&&!texc){observed_old_peak=peaks[0];observed_new_peak=peaks[1];}
 cases++;
}
static texture_t*single_texture(resource_t*r){UINT8 *raw=r->raw+(Picture_IsLumpCooked(r->raw,r->rawsize)?8:0);INT16 width,height;memcpy(&width,raw,2);memcpy(&height,raw+2,2);texture_t*t=Z_Calloc(sizeof *t+sizeof(texpatch_t),PU_STATIC,NULL);memcpy(t->name,"single",6);t->type=TEXTURETYPE_SINGLEPATCH;t->width=width;t->height=height;t->patchcount=1;t->patches[0]=(texpatch_t){.wad=r->wad,.lump=r->lump,.alpha=255,.style=AST_COPY};return t;}
static void focused(void){
 resource_t *small[40];unsigned count=0;for(size_t i=0;i<resource_count&&count<40;i++)if(!resources[i].kind&&resources[i].rawsize<65536){small[count++]=&resources[i];}check(count==40,"focused input supply");
 texture_t*t=Z_Calloc(sizeof *t+80*sizeof(texpatch_t),PU_STATIC,NULL);memcpy(t->name,"focus",5);t->type=TEXTURETYPE_COMPOSITE;t->width=t->height=96;t->patchcount=80;
 for(int i=0;i<80;i++){resource_t*r=small[i%40];t->patches[i]=(texpatch_t){.originx=(INT16)(i%3-1),.originy=(INT16)(i%7-3),.wad=r->wad,.lump=r->lump,.flip=i%4,.alpha=(UINT8)(i%2?128:255),.style=i%3?AST_COPY:AST_TRANSLUCENT};}
 // Unique one-use inputs lower peak; overflow/repeated placements preserve the32-slot rule.
 t->patchcount=40;compare_one(t,GL_TEXFMT_P_8,0,0,0,0,0,0,1);t->patchcount=80;
 compare_one(t,GL_TEXFMT_P_8,0,0,0,0,0,0,0);
 compare_one(t,GL_TEXFMT_AP_88,1,0,1,1,0,0,0);
 compare_one(t,GL_TEXFMT_RGBA,0,1,1,0,0,0,0);
 compare_one(t,GL_TEXFMT_P_8,0,0,0,0,0,1,0);
 t->width=512;t->height=256;compare_one(t,GL_TEXFMT_P_8,0,0,0,0,1,0,0);
 Z_Free(t);
 // Existing large-source try failure remains a missing patch without a fatal allocation.
 for(size_t i=0;i<resource_count;i++)if(!resources[i].kind&&resources[i].rawsize>=65536&&!Picture_IsLumpCooked(resources[i].raw,resources[i].rawsize)){
  texture_t *large=single_texture(&resources[i]);large->width=large->height=64;compare_one(large,GL_TEXFMT_P_8,0,0,0,0,2,0,0);Z_Free(large);break;
 }
}
int main(int argc,char**argv){
 check(argc>=2,"stock corpus argument");read_corpus(argv[1]);focused();
 unsigned patches=0,definitions=0,missing_definitions=0;unsigned long long parsed_placements=0;
 if(argc<3){
  for(size_t i=0;i<resource_count;i++){
   resource_t*r=&resources[i];if(r->kind){char*token=M_GetTokenPooled((char*)r->raw);while(token){check(!strcasecmp(token,"WALLTEXTURE")||!strcasecmp(token,"TEXTURE"),"literal stock TEXTURES grammar");M_FreeToken(token);texture_t*t=R_ParseTexture(true);parsed_placements+=t->patchcount;int missing=0;for(INT32 j=0;j<t->patchcount;j++)missing|=t->patches[j].wad==UINT16_MAX;if(missing){missing_definitions++;fprintf(stderr,"MISSING_STOCK %.8s\n",t->name);}else{compare_one(t,GL_TEXFMT_P_8,0,0,0,0,0,0,0);compare_one(t,GL_TEXFMT_AP_88,1,0,1,1,0,0,0);compare_one(t,GL_TEXFMT_RGBA,0,0,0,0,0,0,0);}Z_Free(t);definitions++;token=M_GetTokenPooled(NULL);}}
   else{texture_t*t=single_texture(r);compare_one(t,GL_TEXFMT_P_8,0,0,0,0,0,0,0);Z_Free(t);patches++;}
  }
 }
 clean_sources();for(size_t i=0;i<ledger_high;i++)check(!ledger[i].live,"no allocation leaked after full corpus");
 for(size_t i=0;i<resource_count;i++){free(resources[i].name);free(resources[i].raw);}free(resources);
 printf("{\"checks\":%llu,\"cases\":%llu,\"patches\":%u,\"stock_TEXTURES_definitions\":%u,\"stock_missing_source_definitions\":%u,\"parsed_patch_placements\":%llu,\"compared_complete_bytes\":%llu,\"hash\":\"%016llx\",\"base_canonical_builds\":%u,\"candidate_canonical_builds\":%u,\"SKY11_base_EE_payload_peak\":%zu,\"SKY11_candidate_EE_payload_peak\":%zu}\n",checks,cases,patches,definitions,missing_definitions,parsed_placements,compared_bytes,digest,total_base_builds,total_new_builds,observed_old_peak,observed_new_peak);
 return 0;
}
