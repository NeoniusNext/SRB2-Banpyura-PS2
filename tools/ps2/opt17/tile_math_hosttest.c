#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#ifdef TILE_MATH_EE
#include <sifrpc.h>
#include <stdarg.h>
static FILE*ee_log;
static int tm_printf(const char*format,...){va_list a;va_start(a,format);int n=vprintf(format,a);va_end(a);if(ee_log){va_start(a,format);vfprintf(ee_log,format,a);va_end(a);fflush(ee_log);}return n;}
#define printf tm_printf
#endif
static unsigned rng=0x77195463u, failures, checks;
static uint64_t check_pixels;
static volatile uint64_t result_sink;
#define CHECK(c) do{checks++;if(!(c)){if(failures<5)printf("TM FAIL line=%d %s\n",__LINE__,#c);failures++;}}while(0)
static unsigned rnd(void){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
static float rf(float lo,float hi){return lo+(hi-lo)*(float)(rnd()&65535u)/65535.0f;}
static uint64_t timer(void){
#ifdef TILE_MATH_EE
 unsigned v;__asm__ volatile("mfc0 %0,$9":"=r"(v));return v;
#else
 struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return(uint64_t)t.tv_sec*1000000000u+t.tv_nsec;
#endif
}
static uint64_t delta(uint64_t end,uint64_t start){
#ifdef TILE_MATH_EE
 return(unsigned)(end-start);
#else
 return end-start;
#endif
}
static void check_origin(float v,int size,int wrap)
{
 int a=base_origin(v,size,wrap),b=fast_origin(v,size,wrap);checks++;
 if(a!=b){if(failures<5){union{float f;unsigned u;}x={v};printf("TM ORIGIN bits=%08x size=%d wrap=%d base=%d fast=%d\n",x.u,size,wrap,a,b);}failures++;}
}
static void origins(void)
{
 static const int sizes[]={1,2,3,16,31,32,33,63,64,65,127,128,129,256,512,1024,2048,4096,8192,16384};
 for(unsigned s=0;s<sizeof sizes/sizeof sizes[0];s++)for(int w=0;w<2;w++)
 {
  for(int k=-2048;k<=2048;k++)
  {
   float v=(float)k*32.0f;check_origin(v,sizes[s],w);check_origin(nextafterf(v,INFINITY),sizes[s],w);check_origin(nextafterf(v,-INFINITY),sizes[s],w);
  }
  check_origin(-0.0f,sizes[s],w);check_origin(INFINITY,sizes[s],w);check_origin(-INFINITY,sizes[s],w);check_origin(NAN,sizes[s],w);
  check_origin(0x1p35f,sizes[s],w);check_origin(-0x1p35f,sizes[s],w);check_origin(nextafterf(0x1p35f,0),sizes[s],w);check_origin(nextafterf(-0x1p35f,0),sizes[s],w);
 }
#ifdef TILE_MATH_EE
 const unsigned n=100000;
#else
 const unsigned n=2000000;
#endif
 for(unsigned i=0;i<n;i++){union{unsigned u;float f;}v={rnd()};check_origin(v.f,sizes[rnd()%(sizeof sizes/sizeof sizes[0])],rnd()&1);}
 printf("TM ORIGIN COMPLETE checks=%u failures=%u\n",checks,failures);
}
static input_t scene(int width,int height,unsigned variant)
{
 input_t in;memset(&in,0,sizeof in);in.width=width;in.height=height;in.sw=in.sizew=1024;in.sh=in.sizeh=512;
 in.wrapx=variant&1;in.wrapy=(variant>>1)&1;in.region=(variant>>2)&3;
 in.kx=(float)width*8.0f;in.ky=-(float)height*8.0f;in.ox=(2048.0f+width*.5f-.5f)*16+.5f;in.oy=(2048.0f+height*.5f-.5f)*16+.5f;
 in.piece.sb=variant&16?-3:0;in.piece.tb=variant&32?7:0;
 const float xy[3][2]={{-.9f,-.8f},{.8f,-.7f},{-.1f,.9f}};
 for(int j=0;j<3;j++){in.tri[j].w=1.0f+j*.3f;in.tri[j].x=xy[j][0]*in.tri[j].w;in.tri[j].y=xy[j][1]*in.tri[j].w;in.tri[j].a[0]=-.12f+j*.53f;in.tri[j].a[1]=.13f+j*.23f;}
 return in;
}
static void geometry(void)
{
 const size_t capacity=640u*512u;
 run_t*a=malloc(capacity*sizeof *a),*b=malloc(capacity*sizeof *b);CHECK(a&&b);if(!a||!b)exit(2);
#ifdef TILE_MATH_EE
 const unsigned count=80;
#else
 const unsigned count=800;
#endif
 for(unsigned i=0;i<count;i++)
 {
  input_t in=scene(64,48,i);
#ifndef TILE_MATH_EE
  if(i<8){in=scene(i&1?640:320,i&1?512:200,i);}
#endif
  if(i>=8)for(int j=0;j<3;j++){in.tri[j].w=rf(.11f,10);in.tri[j].x=rf(-6,6)*in.tri[j].w;in.tri[j].y=rf(-6,6)*in.tri[j].w;in.tri[j].a[0]=rf(-1000,1000);in.tri[j].a[1]=rf(-1000,1000);}
  output_t oa={a,capacity,0,0,0},ob={b,capacity,0,0,0};base_classify(&in,&oa);fast_classify(&in,&ob);
  CHECK(!oa.failed&&!ob.failed&&oa.count==ob.count&&oa.hash==ob.hash);if(oa.count==ob.count)CHECK(!memcmp(a,b,oa.count*sizeof *a));check_pixels+=(uint64_t)in.width*in.height;
 }
 free(a);free(b);printf("TM GEOMETRY COMPLETE bbox_pixel_upper=%llu failures=%u\n",(unsigned long long)check_pixels,failures);
}
static float values[4096];
static void benchmark(void)
{
 for(unsigned i=0;i<4096;i++)values[i]=(float)((int)(rnd()%262144)-131072)*.03125f;
 for(unsigned pass=0;pass<3;pass++)for(unsigned which=0;which<2;which++)
 {
  uint64_t sum=0,start=timer();
  for(unsigned r=0;r<60;r++)for(unsigned i=0;i<4096;i++)sum+=(unsigned)(which?fast_origin(values[i],1024,1):base_origin(values[i],1024,1));
  uint64_t took=delta(timer(),start);result_sink+=sum;printf("TM BENCH origin rep=%u which=%u calls=245760 ticks=%llu checksum=%llu\n",pass,which,(unsigned long long)took,(unsigned long long)sum);
 }
 input_t in=scene(160,112,3);output_t out={NULL,0,0,0,0};
 for(unsigned pass=0;pass<3;pass++)for(unsigned which=0;which<2;which++)
 {
  uint64_t sum=0,start=timer();
  for(unsigned r=0;r<30;r++){out.count=out.failed=0;out.hash=0;if(which)fast_classify(&in,&out);else base_classify(&in,&out);sum+=out.hash;}
  uint64_t took=delta(timer(),start);result_sink+=sum;printf("TM BENCH classify rep=%u which=%u calls=30 bbox_pixel_upper=537600 ticks=%llu checksum=%llu\n",pass,which,(unsigned long long)took,(unsigned long long)sum);
 }
}
int main(void)
{
#ifdef TILE_MATH_EE
 SifInitRpc(0);ee_log=fopen("host:/boot.txt","w");
#endif
 origins();geometry();benchmark();printf("TM COMPLETE checks=%u failures=%u sink=%llu\n",checks,failures,(unsigned long long)result_sink);
#ifdef TILE_MATH_EE
 fflush(stdout);FILE*f=fopen("host:tile-math-complete.txt","w");if(f){fprintf(f,"complete failures=%u\n",failures);fclose(f);}for(;;){}
#endif
 return failures!=0;
}
