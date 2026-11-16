/* Source-bound host fixtures. Generated common.h selects the actual-source adapters. */
#ifdef ROW_RUNS_QUOTIENT_PROPERTY
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
static uint32_t rng=0xd1356a73;static unsigned checks,failures,above32,row_checks,ceil_checks;
static uint32_t rnd(void){rng^=rng<<13;rng^=rng>>17;return rng^=rng<<5;}
static void divide(int64_t n,uint32_t d)
{
 int64_t expected=n/(int64_t)d,actual=v2_divide(n,d);checks++;above32+=(uint64_t)n>UINT32_MAX;
 if(expected!=actual){if(failures<10)printf("DIV FAIL numerator=%lld denominator=%u expected=%lld actual=%lld\n",(long long)n,d,(long long)expected,(long long)actual);failures++;}
}
static void row(const tile_plane_t*p,int x0,int x1,int y)
{
 int al,ar,bl,br;v1_inside(p,x0,x1,y,&al,&ar);v2_inside(p,x0,x1,y,&bl,&br);row_checks++;
 if(al!=bl||ar!=br){if(failures<10)printf("ROW FAIL x=%d/%d y=%d V1=%d/%d V2=%d/%d det=%lld\n",x0,x1,y,al,ar,bl,br,(long long)p->det);failures++;}
}
static void make_det(tile_plane_t*p)
{p->det=(int64_t)(p->x[1]-p->x[0])*(p->y[2]-p->y[0])-(int64_t)(p->x[2]-p->x[0])*(p->y[1]-p->y[0]);}
int main(void)
{
 const int64_t nums[]={0,1,2,0xffffffffll-4096,0xffffffffll-2,0xffffffffll-1,0xffffffffll,0x100000000ll,0x100000001ll,0x100001000ll,0x1ffffffffll,0x200000000ll,0x200000001ll,INT64_MAX};
 const uint32_t ds[]={1,2,3,15,16,17,31,32,33,65535,65536,65537,(1u<<20)-16,(1u<<20)-1,1u<<20,(1u<<21)-1};
 for(unsigned i=0;i<sizeof nums/sizeof *nums;i++)for(unsigned j=0;j<sizeof ds/sizeof *ds;j++)divide(nums[i],ds[j]);
 for(unsigned i=0;i<1000000;i++)
 {
  int64_t n=(int64_t)(((uint64_t)(rnd()&1u)<<32)|rnd());uint32_t d=1+rnd()%((1u<<21)-1);divide(n,d);
  divide(n+d-1,d);ceil_checks++;
  int64_t threshold=(int64_t)(UINT32_MAX/d)*d;
  for(int off=-1;off<=1;off++)if(threshold+off>=0)divide(threshold+off,d);
 }
 const int edge[]={0,1,15,16,32767,32768,65519,65520,65534,65535};
 for(unsigned i=0;i<1000000;i++)
 {
  tile_plane_t p={0};for(int j=0;j<3;j++){p.x[j]=(int)(rnd()&65535);p.y[j]=(int)(rnd()&65535);if(i<1000){p.x[j]=edge[rnd()%(sizeof edge/sizeof *edge)];p.y[j]=edge[rnd()%(sizeof edge/sizeof *edge)];}}
  make_det(&p);if(!p.det)continue;
  int x0=(int)(rnd()%4096)-2048,x1=x0+1+(int)(rnd()%(unsigned)(2048-x0)),y=(int)(rnd()%4096)-2048;row(&p,x0,x1,y);
  if(i<1000)for(int yedge=-2048;yedge<2048;yedge+=257)row(&p,-2048,2048,yedge);
 }
 printf("DIV COMPLETE quotient_checks=%u above_UINT32_MAX=%u ceil_constructions=%u exact_row_bounds=%u failures=%u\n",checks,above32,ceil_checks,row_checks,failures);return failures!=0;
}

#else
#include "common.h"
#include <stdio.h>
#include <time.h>
static unsigned rng=0xc16a397bu,failures,checks,cases;static uint64_t pixels,taps,unsupported_exact_samples;
static unsigned rnd(void){rng^=rng<<13;rng^=rng>>17;return rng^=rng<<5;}
static float rf(float lo,float hi){return lo+(hi-lo)*(float)(rnd()&65535u)/65535.0f;}
#define CHECK(c) do{checks++;if(!(c)){if(failures<10)printf("ROWS FAIL line=%d case=%u %s\n",__LINE__,cases,#c);failures++;}}while(0)
static input_t scene(int w,int h,unsigned variant)
{
 input_t in={0};in.width=w;in.height=h;in.extent=variant&1?64:128;in.sizew=in.sw=1024;in.sizeh=in.sh=1024;
 in.wrapx=(variant>>1)&1;in.wrapy=(variant>>2)&1;in.region=(variant>>3)&3;
 in.kx=w*8.0f;in.ky=-h*8.0f;in.ox=(2048.0f+w*.5f-.5f)*16+.5f;in.oy=(2048.0f+h*.5f-.5f)*16+.5f;
 const float xy[3][2]={{-.95f,-.9f},{.95f,-.85f},{-.1f,.95f}};
 const float uv[3][2]={{0,0},{681.75f,13.4f},{273.1f,720.05f}};
 for(int j=0;j<3;j++){in.tri[j].w=1.0f;in.tri[j].x=xy[j][0];in.tri[j].y=xy[j][1];in.tri[j].a[0]=uv[j][0]/1024;in.tri[j].a[1]=uv[j][1]/1024;}
 return in;
}
static int coord(int p,int size,int wrap){if(wrap){p%=size;return p<0?p+size:p;}return p<0?0:p>=size?size-1:p;}
static int uploaded(int raw,int origin,int extent,int size,int wrap)
{int relative=(int)((unsigned)(raw-origin+extent/4)&(unsigned)(extent-1))-extent/4;return coord(origin+relative,size,wrap);}
static void geometry(const input_t *in,int exact)
{
 size_t cap=(size_t)in->width*in->height;output_t a={0},b={0};a.coverage=calloc(cap,sizeof *a.coverage);b.coverage=calloc(cap,sizeof *b.coverage);a.ox=calloc(cap,sizeof *a.ox);a.oy=calloc(cap,sizeof *a.oy);b.ox=calloc(cap,sizeof *b.ox);b.oy=calloc(cap,sizeof *b.oy);
 a.alpha=malloc(cap*sizeof *a.alpha);b.alpha=malloc(cap*sizeof *b.alpha);if(!a.coverage||!b.coverage||!a.ox||!a.oy||!b.ox||!b.oy||!a.alpha||!b.alpha)exit(2);for(size_t at=0;at<cap;at++)a.alpha[at]=b.alpha[at]=17;
 base_classify(in,&a);fast_classify(in,&b);CHECK(!a.failed&&!b.failed);CHECK(a.fast==b.fast);CHECK(!memcmp(a.coverage,b.coverage,cap*sizeof *a.coverage));if(in->inject_late){CHECK(b.rejected>0&&b.groups_before_rejection>0&&b.pixels>0);printf("ROWS LATE rejected=%u groups_before_last_rejection=%u pixel_fallbacks=%u rects=%zu groups=%zu\n",b.rejected,b.groups_before_rejection,b.pixels,b.rectangles,b.groups);}
 if(exact){CHECK(a.rectangles==b.rectangles&&a.groups==b.groups&&a.boxes==b.boxes&&a.pixels==b.pixels);CHECK(!memcmp(a.coverage,b.coverage,cap*sizeof *a.coverage));CHECK(!memcmp(a.ox,b.ox,cap*sizeof *a.ox)&&!memcmp(a.oy,b.oy,cap*sizeof *a.oy));}
 int gx[3],gy[3];float rq[3],us[3],vs[3];
 for(int i=0;i<3;i++){rq[i]=1.0f/in->tri[i].w;gx[i]=(int)(in->ox+in->tri[i].x*rq[i]*in->kx);gy[i]=(int)(in->oy+in->tri[i].y*rq[i]*in->ky);us[i]=(in->tri[i].a[0]-in->piece.sb)*in->sw*rq[i];vs[i]=(in->tri[i].a[1]-in->piece.tb)*in->sh*rq[i];}
 int64_t det=(int64_t)(gx[1]-gx[0])*(gy[2]-gy[0])-(int64_t)(gx[2]-gx[0])*(gy[1]-gy[0]);
 if(det)for(int y=0;y<in->height;y++)for(int x=0;x<in->width;x++)
 {
  int fx=(2048+x)*16,fy=(2048+y)*16;int64_t n1=(int64_t)(fx-gx[0])*(gy[2]-gy[0])-(int64_t)(gx[2]-gx[0])*(fy-gy[0]);int64_t n2=(int64_t)(gx[1]-gx[0])*(fy-gy[0])-(int64_t)(fx-gx[0])*(gy[1]-gy[0]);int64_t n0=det-n1-n2;
  if(det>0?(n0<0||n1<0||n2<0):(n0>0||n1>0||n2>0))continue;
  size_t at=(size_t)y*in->width+x;pixels++;CHECK(a.coverage[at]==1&&b.coverage[at]==1);CHECK(a.alpha[at]==b.alpha[at]&&a.alpha[at]==108);
  double b1=(double)n1/det,b2=(double)n2/det,q=rq[0]+b1*(rq[1]-rq[0])+b2*(rq[2]-rq[0]);
  double u=(us[0]+b1*(us[1]-us[0])+b2*(us[2]-us[0]))/q,v=(vs[0]+b1*(vs[1]-vs[0])+b2*(vs[2]-vs[0]))/q;
  if(!(q>0)||!isfinite(u)||!isfinite(v)||fabs(u)>=0x1p20||fabs(v)>=0x1p20){CHECK(exact);unsupported_exact_samples++;continue;}
  int wx=in->wrapx&&!(in->region&1),wy=in->wrapy&&!(in->region&2);int iu=(int)floor(u),iv=(int)floor(v);
  for(int off=-1;off<=1;off++)
  {
   int sx=coord(iu+off,in->sizew,wx),sy=coord(iv+off,in->sizeh,wy);taps+=4;
   int ax=uploaded(iu+off,a.ox[at],in->extent,in->sizew,wx),ay=uploaded(iv+off,a.oy[at],in->extent,in->sizeh,wy);
   int bx=uploaded(iu+off,b.ox[at],in->extent,in->sizew,wx),by=uploaded(iv+off,b.oy[at],in->extent,in->sizeh,wy);
   if(ax!=sx||ay!=sy||bx!=sx||by!=sy){if(failures<10)printf("ROWS SOURCE case=%u x=%d y=%d uv=%.9g/%.9g off=%d base=%d/%d fast=%d/%d expected=%d/%d keys=%d/%d:%d/%d\n",cases,x,y,u,v,off,ax,ay,bx,by,sx,sy,a.ox[at],a.oy[at],b.ox[at],b.oy[at]);failures++;}
   checks++;
  }
 }
 free(a.coverage);free(b.coverage);free(a.ox);free(a.oy);free(b.ox);free(b.oy);free(a.alpha);free(b.alpha);cases++;
}
static uint64_t timer(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return(uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
static void benchmark(void)
{
 const int dimensions[][2]={{400,300},{512,512},{640,512}};
 for(unsigned size=0;size<3;size++)for(unsigned rep=0;rep<3;rep++)for(unsigned which=0;which<2;which++)
 {
  input_t in=scene(dimensions[size][0],dimensions[size][1],6);uint64_t started=timer();output_t out={0};
  for(unsigned r=0;r<20;r++){out=(output_t){0};if(which)fast_classify(&in,&out);else base_classify(&in,&out);}
  printf("ROWS BENCH size=%ux%u rep=%u which=%u calls=20 ticks=%llu boxes=%u pixels=%u rects=%zu groups=%zu work=%u\n",in.width,in.height,rep,which,(unsigned long long)(timer()-started),out.boxes,out.pixels,out.rectangles,out.groups,out.work);
 }
}
int main(void)
{
 for(unsigned i=0;i<16;i++){input_t in=scene(i&1?400:512,i&1?300:512,i);geometry(&in,0);}
 for(unsigned i=0;i<240;i++)
 {
  input_t in=scene(64,48,i);
  for(int j=0;j<3;j++){in.tri[j].w=rf(.15f,8);in.tri[j].x=rf(-2,2)*in.tri[j].w;in.tri[j].y=rf(-2,2)*in.tri[j].w;in.tri[j].a[0]=rf(-1,3);in.tri[j].a[1]=rf(-1,3);}
  if(i%11==0){in.sizew=in.sw=129;in.sizeh=in.sh=257;in.wrapx=in.wrapy=0;}
  if(i%7==0){cv_t swap=in.tri[1];in.tri[1]=in.tri[2];in.tri[2]=swap;}
  geometry(&in,0);
 }
 for(unsigned i=0;i<128;i++)
 {
  input_t in=scene(64,48,i);in.kx=in.ky=16;in.ox=in.oy=32768;
  const int xy[3][2]={{0,0},{63,2},{1,47}};
  float offset=(float)((int)(i%7)-3)*64.0f;
  if(i%4==1)offset=nextafterf(offset,INFINITY);
  if(i%4==2)offset=nextafterf(offset,-INFINITY);
  if(i%4==3)offset+=.5f;
  for(int j=0;j<3;j++)
  {
   float w=i&32?(j==0?.5f:j==1?1.0f:2.0f):1.0f;
   in.tri[j].w=w;in.tri[j].x=xy[j][0]*w;in.tri[j].y=xy[j][1]*w;
   in.tri[j].a[0]=(4.0f*xy[j][0]-2.0f*xy[j][1]+offset)/1024;
   in.tri[j].a[1]=(-1.0f*xy[j][0]+6.0f*xy[j][1]+offset)/1024;
  }
  if(i&64){cv_t swap=in.tri[1];in.tri[1]=in.tri[2];in.tri[2]=swap;}
  geometry(&in,0);
 }
 {input_t in=scene(512,512,6);in.kx=in.ky=16;in.ox=in.oy=32768;
  const int xy[3][2]={{-1000,-1000},{2000,-1000},{-1000,600}};
  for(int j=0;j<3;j++){in.tri[j].x=xy[j][0];in.tri[j].y=xy[j][1];in.tri[j].a[0]=(70.0f+.1f*xy[j][0])/1024;in.tri[j].a[1]=(140.0f+.1f*xy[j][1])/1024;}
  geometry(&in,0);
  output_t a={0},b={0};base_classify(&in,&a);fast_classify(&in,&b);CHECK(b.boxes==1&&b.rectangles==1&&b.groups==1);printf("ROWS CROP base_boxes=%u base_rects=%zu base_groups=%zu fast_boxes=%u fast_rects=%zu fast_groups=%zu\n",a.boxes,a.rectangles,a.groups,b.boxes,b.rectangles,b.groups);
 }
 for(unsigned i=0;i<16;i++)
 {
  input_t in=scene(32,24,i);
  for(int j=0;j<3;j++)
  {
   if(i%4==0){in.tri[j].w=-1;in.tri[j].x=-in.tri[j].x;in.tri[j].y=-in.tri[j].y;}
   if(i%4==1)in.tri[j].a[0]=1.0e7f+j*1.0e7f;
   if(i%4==2)in.tri[j].a[1]=NAN;
   if(i%4==3){float w=j==0?1.0e-8f:1.0e8f;in.tri[j].w=w;in.tri[j].x*=w;in.tri[j].y*=w;}
  }
  geometry(&in,1);
 }
#ifdef ROWS_FORCE_LATE
 {input_t in=scene(96,80,6);in.inject_late=1;geometry(&in,0);}
#endif
 benchmark();printf("ROWS COMPLETE cases=%u checks=%u inside_pixels=%llu source_taps=%llu unsupported_exact_samples=%llu failures=%u\n",cases,checks,(unsigned long long)pixels,(unsigned long long)taps,(unsigned long long)unsupported_exact_samples,failures);return failures!=0;
}

#endif
