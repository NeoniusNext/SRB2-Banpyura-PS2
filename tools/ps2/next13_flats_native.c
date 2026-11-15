/* Runs complete shipping-LTO bodies with explicit SDK/callback boundaries.
 * This fixture measures allocator requests and checked ownership/identities;
 * whole-game ZA/spill allocation and cache/LOD policy need separate runs. */
#include <kernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stddef.h>
#include <setjmp.h>
#include <malloc.h>
#include "hardware/hw_data.h"
#include "hardware/hw_drv.h"
#include "r_defs.h"
#include "p_setup.h"
#include "native-state.h"

void base_HWR_LoadMapTextures(size_t);
void cur_HWR_LoadMapTextures(size_t);
void base_HWR_FreeMapTextures(void);
void cur_HWR_FreeMapTextures(void);
void base_HWR_GetLevelFlat(levelflat_t *, boolean);
void cur_HWR_GetLevelFlat(levelflat_t *, boolean);
static void (*loaders[2])(size_t)={base_HWR_LoadMapTextures,cur_HWR_LoadMapTextures};
static void (*freers[2])(void)={base_HWR_FreeMapTextures,cur_HWR_FreeMapTextures};
static void (*getters[2])(levelflat_t*,boolean)={base_HWR_GetLevelFlat,cur_HWR_GetLevelFlat};
_Static_assert(sizeof(GLMipmap_t)==76&&sizeof(GLMapTexture_t)==84,"actual shipping metadata ABI");
_Static_assert(sizeof(struct hwdriver_s)==152&&offsetof(struct hwdriver_s,pfnSetTexture)==36&&offsetof(struct hwdriver_s,pfnDeleteTexture)==44,"actual shipping driver callback ABI");
_Static_assert(offsetof(levelflat_t,type)==9&&offsetof(levelflat_t,texture_id)==12,"actual shipping level-flat ABI");

static unsigned tag,hash,events,deletes,alloc_calls,fail_call,quiet,expect_error,errors;
static unsigned alloc_a[32],alloc_b[32],free_calls;
static size_t allocated_bytes,freed_bytes;
static void *alloc_p[32];
static unsigned char global_initial[16384];
static size_t global_offsets[128];
static jmp_buf unwind;
static texture_t texture_values[32],*texture_ptrs[32];
static INT32 translations[32];
static unsigned n,compact;
static unsigned word(void *p){return *(unsigned*)p;}
static void *pointer(void *p){return *(void**)p;}
static void die(const char *why){printf("FLATS NEXT13 FAIL %s tag=%u events=%u\n",why,tag,events);abort();}
static void require(int ok,const char *why){if(!ok)die(why);}
static void bytes(const void *p,size_t len){const unsigned char*s=p;while(len--)hash=(hash^*s++)*16777619u;}
static void event(unsigned a,unsigned b,unsigned c){if(quiet)return;events++;bytes(&a,4);bytes(&b,4);bytes(&c,4);}
static GLMipmap_t *flat(unsigned i){return (GLMipmap_t*)((unsigned char*)pointer(native_state[tag].flats)+i*(compact?76u:84u));}
static unsigned mipid(const GLMipmap_t*m){
 uintptr_t p=(uintptr_t)m,w=(uintptr_t)pointer(native_state[tag].walls),f=(uintptr_t)pointer(native_state[tag].flats);
 unsigned stride=compact?76u:84u;
 if(!m)return 0;
 if(w&&p>=w&&p-w<n*84u&&(p-w)%84u==0)return 0x10000u+2u*((p-w)/84u);
 if(f&&p>=f&&p-f<n*stride&&(p-f)%stride==0)return 0x20000u+2u*((p-f)/stride);
 for(unsigned i=0;i<n;i++){
  if(((GLMapTexture_t*)w)[i].mipmap.nextcolormap==m)return 0x10001u+2u*i;
  if(flat(i)->nextcolormap==m)return 0x20001u+2u*i;
 }
 die("unstable or unknown mipmap identity");return 0;
}
static void snapshot(GLMipmap_t*m){if(quiet)return;if(!m){event(10,0,0);return;}GLMipmap_t c=*m;
 c.data=(void*)(uintptr_t)!!c.data;c.nextcolormap=(GLMipmap_t*)(uintptr_t)mipid(c.nextcolormap);c.ps2_twin=(GLMipmap_t*)(uintptr_t)mipid(c.ps2_twin);c.colormap=NULL;event(10,mipid(m),0);bytes(&c,sizeof c);
}
void *flat_native_calloc(size_t a,size_t b){
 unsigned k=alloc_calls++;require(k<32,"fixture allocation records");alloc_a[k]=a;alloc_b[k]=b;
 if(fail_call&&alloc_calls==fail_call){alloc_p[k]=NULL;return NULL;}
 void*p=calloc(a,b);alloc_p[k]=p;if(p)allocated_bytes+=a*b;return p;
}
void flat_native_free(void*p){unsigned id=0;if(p){if(p==pointer(native_state[tag].walls))id=1;else if(p==pointer(native_state[tag].flats))id=2;else id=mipid(p);unsigned found=0;for(unsigned i=0;i<alloc_calls;i++)if(alloc_p[i]==p){freed_bytes+=alloc_a[i]*alloc_b[i];alloc_p[i]=NULL;found=1;break;}require(found,"unknown or duplicate free");}event(11,id,!!p);free_calls++;free(p);}
void flat_native_flush(void){event(12,word(native_state[tag].count),word(native_state[tag].loaded));}
void flat_native_zone_free(void*p){event(13,!!p,0);require(!p,"unexpected external zone owner in callback fixture");}
void flat_native_zone_tag(void*p,int value){event(14,!!p,(unsigned)value);require(!p,"unexpected pixel ownership in callback fixture");}
void flat_native_touch(GLMipmap_t*m){event(15,mipid(m),0);snapshot(m);}
void flat_native_record(GLMipmap_t*m){(void)m;die("unexpected geometry record path");}
void low_unexpected(void){die("unspecified actual code boundary");}
void low_output(const char*fmt,...){(void)fmt;die("unexpected actual output");}
void flat_native_error(const char*fmt,...){(void)fmt;errors++;require(expect_error,"unexpected I_Error");longjmp(unwind,2);}
void flat_native_guard(const char*why){(void)why;errors++;require(expect_error,"unexpected guard throw");require(!pointer(native_state[tag].walls)&&!pointer(native_state[tag].flats)&&!word(native_state[tag].count)&&!word(native_state[tag].loaded),"partial tables survived guard");longjmp(unwind,1);}
static void set_texture(GLMipmap_t*m){event(16,mipid(m),0);snapshot(m);m->downloaded=7;}
static void delete_texture(GLMipmap_t*m){deletes++;event(17,mipid(m),0);snapshot(m);m->downloaded=0;}
static void capture_globals(void){size_t off=0;unsigned count=sizeof native_globals/sizeof native_globals[0];require(count<128,"fixture state record bound");for(unsigned i=0;i<count;i++){global_offsets[i]=off;off+=native_globals[i].bytes;require(off<=sizeof global_initial,"fixture complete state capacity");memcpy(global_initial+global_offsets[i],native_globals[i].p,native_globals[i].bytes);}}
static void reset_globals(void){for(unsigned i=0;i<sizeof native_globals/sizeof native_globals[0];i++)memcpy(native_globals[i].p,global_initial+global_offsets[i],native_globals[i].bytes);}
static void configure(unsigned which,char **argv,unsigned argc){
 reset_globals();tag=which;hash=2166136261u;events=deletes=alloc_calls=free_calls=errors=0;allocated_bytes=freed_bytes=0;memset(alloc_p,0,sizeof alloc_p);
 *(unsigned*)native_state[tag].argc=argc;*(char***)native_state[tag].argv=argv;
 *(texture_t***)native_state[tag].textures=texture_ptrs;*(INT32**)native_state[tag].translation=translations;
 struct hwdriver_s*d=(void*)native_state[tag].driver;d->pfnSetTexture=set_texture;d->pfnDeleteTexture=delete_texture;
 compact=which;for(unsigned i=1;i<argc;i++)if(!strcasecmp(argv[i],"-hwgo")){if(i+1<argc&&argv[i+1][0]!='-'&&argv[i+1][0]!='+')compact=which&&!((UINT32)atoi(argv[i+1])&0x10000u);break;}
}
typedef struct{unsigned hash,events,deletes,errors,calls,frees,wall,flat,requested,resident;}result_t;
static result_t exercise(unsigned which,unsigned count,char**argv,unsigned argc,unsigned failure){
 configure(which,argv,argc);n=count;fail_call=failure;expect_error=!!failure;
 unsigned before=(unsigned)mallinfo().uordblks;
 if(!setjmp(unwind))loaders[tag](n);
 unsigned after=(unsigned)mallinfo().uordblks;
 if(failure){require(errors==1,"missing allocation error");require(!pointer(native_state[tag].walls)&&!pointer(native_state[tag].flats),"failed table retained storage");}
 else{
  require(alloc_calls==2&&alloc_a[0]==n&&alloc_b[0]==84&&alloc_a[1]==n&&alloc_b[1]==(compact?76u:84u),"initial fullN two-calloc requests changed");
  require(word(native_state[tag].count)==n&&word(native_state[tag].loaded),"constructor count/loaded mismatch");
  GLMipmap_t zero;memset(&zero,0,sizeof zero);for(unsigned i=0;i<n;i++)require(!memcmp(flat(i),&zero,sizeof zero),"nonzero unused metadata");
  levelflat_t f;memset(&f,0,sizeof f);f.type=LEVELFLAT_TEXTURE;f.texture_id=0;
  getters[tag](&f,false);GLMipmap_t*a=pointer(native_state[tag].selected);require(a==flat(0),"primary identity");
  getters[tag](&f,true);GLMipmap_t*b=pointer(native_state[tag].selected);require(b==a->nextcolormap&&b->ps2_twin==a,"twin identity");
  a->ps2_planfr=0xfedcba98u;a->ps2_pi=65535;a->ps2_uw=65535;a->ps2_cost=255;b->ps2_spv=0xffffffffu;
  getters[tag](&f,false);require(pointer(native_state[tag].selected)==a,"primary identity moved");getters[tag](&f,true);require(pointer(native_state[tag].selected)==b,"twin identity moved");
  *(unsigned*)native_state[tag].batching=1;f.texture_id=1;translations[1]=2;getters[tag](&f,false);require(pointer(native_state[tag].selected)==flat(2),"translated identity");getters[tag](&f,true);translations[1]=1;
  freers[tag]();require(deletes==2*n+2,"unused/twin callback count");require(!pointer(native_state[tag].walls)&&!pointer(native_state[tag].flats)&&!word(native_state[tag].count)&&!word(native_state[tag].loaded),"teardown state");
 }
 require(allocated_bytes==freed_bytes,"native requested payload leak");
 result_t r={hash,events,deletes,errors,alloc_calls,free_calls,n*84u,n*(compact?76u:84u),(unsigned)allocated_bytes,after-before};return r;
}
static u32 cyc(void){u32 value;__asm__ volatile("mfc0 %0,$9":"=r"(value)::"memory");return value;}
static u32 begin(void){u32 value;__asm__ volatile("mfc0 %0,$12\n\tdi\n\tsync.p":"=r"(value)::"memory");return value;}
static void end(u32 value){if(value&0x10000u)__asm__ volatile("ei\n\tsync.p":::"memory");}
static void benchmark(char**argv,unsigned argc,unsigned mode){
 for(unsigned which=0;which<2;which++){
  configure(which,argv,argc);n=17;fail_call=expect_error=0;loaders[tag](n);*(unsigned*)native_state[tag].batching=1;
  levelflat_t f;memset(&f,0,sizeof f);f.type=LEVELFLAT_TEXTURE;
  for(unsigned i=0;i<8;i++){f.texture_id=(INT32)i;getters[tag](&f,false);getters[tag](&f,true);}
  quiet=1;
  for(unsigned cold=0;cold<2;cold++)for(unsigned rep=0;rep<9;rep++){
   if(cold){FlushCache(0);FlushCache(1);FlushCache(2);}u32 state=begin(),start=cyc();
   void(*volatile fn)(levelflat_t*,boolean)=getters[tag];
   for(unsigned i=0;i<4096;i++){f.texture_id=(INT32)(i&7);fn(&f,(boolean)(i&1));}
   u32 elapsed=cyc()-start;end(state);printf("FLATS13 TIMING mode=%u tag=%u cold=%u rep=%u calls=4096 cycles=%u\n",mode,tag,cold,rep,elapsed);
  }
  quiet=0;freers[tag]();
 }
}
int main(void){
 printf("FLATS13 CALLBACK FIXTURE actual-LTO, SDK allocation boundary; game ZA/spill residency excluded\n");
 capture_globals();for(unsigned i=0;i<32;i++){texture_values[i].width=(INT16)(i+1);texture_values[i].height=(INT16)(i+2);texture_ptrs[i]=&texture_values[i];translations[i]=(INT32)i;}
 char*arg0[]={"flat"};char*arg1[]={"flat","-hwgo","65536"};char*arg2[]={"flat","-HWGO","65600"};char*arg3[]={"flat","-hwgo","-65536"};char*arg4[]={"flat","-hwgo","0","-hwgo","65536"};char*arg5[]={"flat","-hwgo","65536tail"};
 char**args[]={arg0,arg1,arg2,arg3,arg4,arg5};unsigned counts[]={1,17,4740};unsigned argc[]={1,3,3,3,5,3};
 for(unsigned ai=0;ai<6;ai++)for(unsigned ci=0;ci<3;ci++)for(unsigned failure=0;failure<3;failure++){
  if(counts[ci]==1&&failure==0)continue; /* translated-index exercise uses slot2 */
  result_t r[2];r[0]=exercise(0,counts[ci],args[ai],argc[ai],failure);r[1]=exercise(1,counts[ci],args[ai],argc[ai],failure);
  require(r[0].hash==r[1].hash&&r[0].events==r[1].events&&r[0].deletes==r[1].deletes&&r[0].errors==r[1].errors&&r[0].calls==r[1].calls&&r[0].frees==r[1].frees,"actual native callback trace mismatch");
  printf("FLATS13 CASE mode=%u count=%u failure=%u hash=%08x events=%u deletes=%u calls=%u oldflat=%u newflat=%u old_sdk_resident=%u new_sdk_resident=%u\n",ai,counts[ci],failure,r[0].hash,r[0].events,r[0].deletes,r[0].calls,r[0].flat,r[1].flat,r[0].resident,r[1].resident);
 }
 benchmark(arg0,1,0);benchmark(arg1,3,1);printf("FLATS NEXT13 DONE\n");return 0;
}
