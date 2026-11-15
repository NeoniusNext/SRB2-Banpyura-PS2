// Exact-source host drawnode trace driver. It is not an EE performance test.
#include "common.h"

static uint32_t rng=0x72616d13u;
static unsigned cases, callback_cases, growth_cases;
static uint64_t compared_bytes, digest=1469598103934665603ull;
static fixture_t fixture;
static ram_result_t a,b,c;

static void fail(const char *what,unsigned n)
{
 fprintf(stderr,"FAIL %s case %u\n",what,n);exit(1);
}
static uint32_t random_word(void) {rng=rng*1664525u+1013904223u;return rng;}
static uint64_t hash(uint64_t h,const void *p,size_t n)
{
 const unsigned char *s=p;while(n--)h=(h^*s++)*1099511628211ull;return h;
}
int oracle_visible(const float matrix[4][4],const FOutVector *v,unsigned n)
{
 (void)matrix;(void)v;return n>=3;
}
static void fill(unsigned n,unsigned distribution)
{
 memset(&fixture,0,sizeof fixture);fixture.n=n;fixture.frames=1;fixture.valid=1;
 fixture.shader=fixture.palette=fixture.renderwalls=fixture.record=1;
 for(unsigned i=0;i<MAXT;i++)fixture.tex[i]=(texture_input_t){1,0,32,32,0,1};
 for(unsigned i=0;i<n;i++) {
  node_input_t *p=&fixture.node[i];p->kind=distribution?1+(int)(random_word()%3):(int)(i%3)+1;
  p->id=(int)i+1;p->texture=(int)(random_word()%MAXT);p->light=(int)(random_word()%512)-128;p->alpha=(UINT8)random_word();p->cmap=(int)(random_word()&1);
  p->height=((int)(random_word()%33)-16)*FRACUNIT;p->flags=PF_Translucent;
  if(i%7==0)p->flags|=PF_NoTexture;if(i%11==0)p->flags|=PF_Ripple;
  for(unsigned j=0;j<4;j++)p->v[j]=(FOutVector){(float)i,(float)j,16.0f,(float)(j&1),(float)(j>>1)};
 }
}
static void compare(void)
{
 base_ram_run(&fixture,&a);cur_ram_run(&fixture,&b);off_ram_run(&fixture,&c);
 cases++;if(fixture.metadata_fault)callback_cases++;
 if(memcmp(&a.semantic,&b.semantic,sizeof a.semantic)||memcmp(&a.semantic,&c.semantic,sizeof a.semantic))fail("complete semantic state",cases);
 if(a.sort_hash!=b.sort_hash||a.compare_hash!=b.compare_hash||a.sorted_hash!=b.sorted_hash||a.qsort_calls!=b.qsort_calls||a.compare_calls!=b.compare_calls)fail("all qsort calls/comparator visits/results",cases);
 if(a.sort_hash!=c.sort_hash||a.compare_hash!=c.compare_hash||a.sorted_hash!=c.sorted_hash||a.qsort_calls!=c.qsort_calls||a.compare_calls!=c.compare_calls)fail("opt-out qsort trace",cases);
 if(a.pointers_hash!=b.pointers_hash||a.pointers_hash!=c.pointers_hash)fail("complete sorted pointer identity",cases);
 if(a.errors!=b.errors||a.unwinds!=b.unwinds||a.callbacks!=b.callbacks||a.errors!=c.errors||a.unwinds!=c.unwinds||a.callbacks!=c.callbacks)fail("failure/unwind/callback state",cases);
 if(a.error_hash!=b.error_hash||a.error_hash!=c.error_hash)fail("exact failure reason",cases);
 if(a.di_cap!=b.di_cap||a.di_bytes!=b.di_bytes||a.di_cap!=c.di_cap||a.di_bytes!=c.di_bytes)fail("unchanged full-width index capacity",cases);
 if(b.dn_bytes||b.dn_cap)fail("candidate redundant allocations",cases);
 if(a.dn_bytes!=c.dn_bytes||a.dn_cap!=c.dn_cap)fail("opt-out original storage",cases);
 if(a.dn_cap) {
  if(a.dn_cap!=a.di_cap||a.dn_bytes!=4*sizeof(void*)*a.di_cap)fail("original capacity accounting",cases);
  growth_cases++;
 }
 compared_bytes+=2*sizeof a.semantic;
 digest=hash(digest,&a.semantic,sizeof a.semantic);digest=hash(digest,&a.sort_hash,sizeof a.sort_hash);
 digest=hash(digest,&a.compare_hash,sizeof a.compare_hash);digest=hash(digest,&a.sorted_hash,sizeof a.sorted_hash);
 if(getenv("NEXT13_RAM_TRACE"))printf("TRACE %u %016llx %016llx %016llx %016llx\n",cases,(unsigned long long)hash(1469598103934665603ull,&a.semantic,sizeof a.semantic),(unsigned long long)a.sort_hash,(unsigned long long)a.compare_hash,(unsigned long long)a.sorted_hash);
}
int main(int argc,char **argv)
{
 if(argc>1&&!strcmp(argv[1],"index-failure")) {
  fill(129,1);fixture.evict_each_frame=2;compare();
  if(a.errors!=1||a.unwinds!=1||a.semantic.nevent)fail("common index alloc failure",cases);
  fill(129,1);compare();if(a.errors||a.semantic.nevent!=129)fail("failure retry",cases);
 } else if(argc>1&&!strcmp(argv[1],"node-failure")) {
  fill(129,1);fixture.evict_each_frame=3;
  base_ram_run(&fixture,&a);cur_ram_run(&fixture,&b);off_ram_run(&fixture,&c);
  if(a.errors!=1||a.unwinds!=1||c.errors!=1||c.unwinds!=1||b.errors||b.unwinds||b.semantic.nevent!=129||b.dn_bytes)fail("deleted node allocation failure",0);
  fill(193,1);compare();if(a.errors||a.semantic.nevent!=193)fail("deleted allocation failure retry",cases);
 } else if(argc>1&&!strcmp(argv[1],"callback-unwind")) {
  fill(288,1);fixture.metadata_fault=7;fixture.reload_frame=7;compare();
  if(a.errors!=1||a.unwinds!=1||a.semantic.nevent!=7)fail("draw abandonment",cases);
  fill(128,1);compare();if(a.errors||a.semantic.nevent!=128)fail("abandon/reacquire",cases);
 } else if(argc>1&&!strcmp(argv[1],"duplicate-drawcount")) {
  fill(64,1);fixture.fr_off=1;compare();
  if(a.errors!=1||a.unwinds!=1||a.semantic.nevent||!a.error_hash)fail("original duplicate count error",cases);
 } else {
  const unsigned boundaries[]={0,1,2,3,127,128,129,191,192,193,287,288,289,431,432,433,647,648,649,971,972,973,1457,1458,1459,2186,2187,2188,3280,3281,3282,4920,4921,4922,7380,7381,7382,8192};
  for(unsigned i=0;i<sizeof boundaries/sizeof*boundaries;i++){fill(boundaries[i],1);compare();}
  for(unsigned kind=1;kind<=3;kind++)for(unsigned n=0;n<6;n++) {
   fill(n,0);for(unsigned i=0;i<n;i++)fixture.node[i].kind=(int)kind;compare();
  }
  for(unsigned k=0;k<512;k++) {
   fill(1+random_word()%512,1);fixture.metadata_fault=k%8;fixture.cube=(int)(k&1);fixture.shader=(int)(k%3!=0);fixture.palette=(int)(k%5!=0);fixture.renderwalls=(int)(k%7!=0);fixture.record=(int)(k%2);
   if(k%17==0)for(unsigned i=0;i<fixture.n;i++){fixture.node[i].kind=1;fixture.node[i].height=(i&1)?FRACUNIT:-FRACUNIT;}
   compare();
  }
  if(!growth_cases||!callback_cases)fail("missing positive witnesses",cases);
 }
 printf("NEXT13_RAM_HOST PASS cases=%u callback_cases=%u growth_cases=%u compared_bytes=%llu digest=%016llx\n",cases,callback_cases,growth_cases,(unsigned long long)compared_bytes,(unsigned long long)digest);
 return 0;
}
