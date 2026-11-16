#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifdef TILE_I64F_EE
#include <sifrpc.h>
#include <stdarg.h>
static FILE *ee_log;
static int ti_printf(const char *format,...){va_list a;va_start(a,format);int n=vprintf(format,a);va_end(a);if(ee_log){va_start(a,format);vfprintf(ee_log,format,a);va_end(a);fflush(ee_log);}return n;}
#define printf ti_printf
#endif
float old_cast(int64_t);float direct_cast(int64_t);float nearest_cast(int64_t);float safe_cast(int64_t);
static uint32_t rng=0x17f10a51u,checks,failures,direct_mismatches;
static volatile uint32_t sink;
static uint32_t rnd(void){rng^=rng<<13;rng^=rng>>17;return rng^=rng<<5;}
static uint32_t bits(float f){union{float f;uint32_t u;}v={f};return v.u;}
static uint64_t timer(void){
#ifdef TILE_I64F_EE
 unsigned v;__asm__ volatile("mfc0 %0,$9":"=r"(v));return v;
#else
 struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return(uint64_t)t.tv_sec*1000000000u+t.tv_nsec;
#endif
}
static uint64_t delta(uint64_t a,uint64_t b){
#ifdef TILE_I64F_EE
 return(uint32_t)(a-b);
#else
 return a-b;
#endif
}
static void test(int64_t v)
{
 const uint32_t ref=bits(old_cast(v)),fast=bits(nearest_cast(v)),safe=bits(safe_cast(v));checks++;
 if(ref!=fast||ref!=safe){if(failures<8)printf("TI FAIL value=%lld ref=%08x nearest=%08x safe=%08x\n",(long long)v,ref,fast,safe);failures++;}
 if(v==(int64_t)(int32_t)v){const uint32_t direct=bits(direct_cast(v));if(ref!=direct){if(direct_mismatches<4)printf("TI DIRECT_DIFF value=%lld ref=%08x direct=%08x\n",(long long)v,ref,direct);direct_mismatches++;}}
}
static void boundaries(void)
{
 for(int64_t v=-65536;v<=65536;v++)test(v);
 for(unsigned e=0;e<32;e++)for(int d=-257;d<=257;d++){int64_t v=(INT64_C(1)<<e)+d;if(v<=INT32_MAX)test(v);if(v<=INT64_C(2147483648))test(-v);}
 /* Every dropped-bit pattern at each int32 exponent, both signs, odd/even mantissas and carries. */
 static const uint32_t mant[]={0x800000u,0x800001u,0x800002u,0x801234u,0x955555u,0xaaaaaau,0xfffffeu,0xffffffu};
 for(unsigned shift=1;shift<=8;shift++)for(unsigned m=0;m<sizeof mant/sizeof*mant;m++)for(uint32_t low=0;low<(1u<<shift);low++)
 {const uint64_t mag=((uint64_t)mant[m]<<shift)+low;if(mag<=INT32_MAX)test((int64_t)mag);if(mag<=UINT64_C(2147483648))test(-(int64_t)mag);}
 for(int d=-1024;d<=1024;d++){test(INT64_C(2147483648)+d);test(-INT64_C(2147483648)+d);test(INT64_C(4294967296)+d);test(-INT64_C(4294967296)+d);}
 test(INT64_MIN);test(INT64_MAX);test(0);
}
static int64_t values[4096];
static void benchmark(void)
{
 for(unsigned i=0;i<4096;i++)values[i]=(int32_t)rnd();
 for(unsigned rep=0;rep<3;rep++)for(unsigned which=0;which<4;which++)
 {
  uint32_t sum=0;uint64_t start=timer();
  for(unsigned r=0;r<40;r++)for(unsigned i=0;i<4096;i++)
  {float f=which==0?old_cast(values[i]):which==1?nearest_cast(values[i]):which==2?safe_cast(values[i]):direct_cast(values[i]);sum+=bits(f);}
  uint64_t took=delta(timer(),start);sink+=sum;
  printf("TI BENCH rep=%u which=%u calls=163840 ticks=%llu checksum=%u\n",rep,which,(unsigned long long)took,sum);
 }
}
int main(void)
{
#ifdef TILE_I64F_EE
 SifInitRpc(0);ee_log=fopen("host:/boot.txt","w");const unsigned random_count=300000;
#else
 const unsigned random_count=2000000;
#endif
 boundaries();for(unsigned i=0;i<random_count;i++)test((int32_t)rnd());
 for(unsigned i=0;i<10000;i++)test((int64_t)(((uint64_t)rnd()<<32)|rnd()));
 printf("TI CHECKS checks=%u failures=%u direct_mismatches=%u\n",checks,failures,direct_mismatches);
 benchmark();printf("TI COMPLETE checks=%u failures=%u direct_mismatches=%u sink=%u\n",checks,failures,direct_mismatches,sink);
#ifdef TILE_I64F_EE
 fflush(stdout);for(;;){}
#endif
 return failures!=0;
}
