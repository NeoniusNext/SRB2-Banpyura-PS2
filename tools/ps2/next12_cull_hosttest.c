// Host-only exact frontend replay and independent double clipping oracle.
// This is not a GS rasterizer or a target performance benchmark.
#include "common.h"

static uint32_t rng_state=0x6e657874u;
static unsigned tests,geometry_tests,replay_tests,query_tests;
static unsigned geometry_rejects,resource_witnesses,naive_classification_witnesses;
static uint32_t digest=2166136261u;

static void fail(const char *what,unsigned n)
{
 fprintf(stderr,"FAIL %s case %u\n",what,n);
 exit(1);
}

static uint32_t rnd(void)
{
 rng_state=rng_state*1664525u+1013904223u;
 return rng_state;
}

static float rf(float lo,float hi)
{
 return lo+(hi-lo)*(float)(rnd()>>8)*(1.0f/16777216.0f);
}

static uint32_t hash(uint32_t h,const void *p,size_t n)
{
 const unsigned char*b=p;
 while(n--)h=(h^*b++)*16777619u;
 return h;
}

// Five-plane homogeneous clipping, evaluated independently in doubles from the
// original submitted float matrix. Returning visible on nonfinite uncertainty
// makes this a conservative rejection oracle, not an invented NaN raster rule.
int oracle_visible(const float matrix[4][4],const FOutVector *v,unsigned n)
{
 double a[80][4],b[80][4],(*in)[4]=a,(*out)[4]=b;
 if(n<3||n>64)return 1;
 for(unsigned i=0;i<n;i++)for(unsigned j=0;j<4;j++) {
  in[i][j]=(double)matrix[j][0]*v[i].x+(double)matrix[j][1]*v[i].y+(double)matrix[j][2]*v[i].z+matrix[j][3];
  if(!isfinite(in[i][j]))return 1;
 }
 for(unsigned pl=0;pl<5&&n;pl++) {
  unsigned count=0;
  for(unsigned i=0;i<n;i++) {
   double *p=in[i],*q=in[(i+1)%n];
   double dp=pl==0?p[3]-p[0]:pl==1?p[3]+p[0]:pl==2?p[3]-p[1]:pl==3?p[3]+p[1]:p[3]+p[2];
   double dq=pl==0?q[3]-q[0]:pl==1?q[3]+q[0]:pl==2?q[3]-q[1]:pl==3?q[3]+q[1]:q[3]+q[2];
   if(dp>=0) {if(count>=79)return 1;memcpy(out[count++],p,4*sizeof(double));}
   if((dp>=0)!=(dq>=0)) {
    if(count>=79)return 1;
    double t=dp/(dp-dq);
    for(unsigned j=0;j<4;j++)out[count][j]=p[j]+t*(q[j]-p[j]);
    count++;
   }
  }
  n=count;double(*tmp)[4]=in;in=out;out=tmp;
 }
 return n>=3;
}

static void view(float m[4][4],float yaw,float pitch,float roll,int flip,int mirror,float aspect,float cx,float cy,float cz)
{
 double y=yaw,p=pitch,r=roll;
 double right[3]={cos(y),0,-sin(y)};
 double forward[3]={sin(y)*cos(p),sin(p),cos(y)*cos(p)};
 double up[3]={-sin(y)*sin(p),cos(p),-cos(y)*sin(p)};
 double rr[3],uu[3];
 for(unsigned k=0;k<3;k++) {rr[k]=(right[k]*cos(r)+up[k]*sin(r))*(mirror?-1:1);uu[k]=(up[k]*cos(r)-right[k]*sin(r))*(flip?-1:1);}
 double cam[3]={cx,cy,cz};
 double znear=4,zfar=32768,A=(zfar+znear)/(zfar-znear),B=-2*zfar*znear/(zfar-znear);
 memset(m,0,16*sizeof(float));
 for(unsigned k=0;k<3;k++) {m[0][k]=(float)rr[k];m[1][k]=(float)(uu[k]*aspect);m[2][k]=(float)(forward[k]*A);m[3][k]=(float)forward[k];}
 for(unsigned j=0;j<4;j++) {
  double d=j==2?B:0;
  for(unsigned k=0;k<3;k++)d-=(double)m[j][k]*cam[k];
  m[j][3]=(float)d;
 }
}

static void quad(FOutVector *v,float x,float y,float z,float w,float h)
{
 v[0]=(FOutVector){x-w,y-h,z,0,0};
 v[1]=(FOutVector){x+w,y-h,z,1,0};
 v[2]=(FOutVector){x+w,y+h,z,1,1};
 v[3]=(FOutVector){x-w,y+h,z,0,1};
}

static void geometry(void)
{
 float m[4][4],planes[5][4];FOutVector v[4];
 view(m,0,0,0,0,0,1,0,0,0);
 quad(v,0,0,16,2,2);
 if(cur_guard(m,1,0,0,PF_Translucent,v,planes))fail("inside",tests++);
 for(unsigned pl=0;pl<5;pl++) {
  // A tiny quad centred eight units outside one current normalized plane.
  float n2=planes[pl][0]*planes[pl][0]+planes[pl][1]*planes[pl][1]+planes[pl][2]*planes[pl][2];
  float a=(8-planes[pl][3])/n2;
  for(unsigned i=0;i<4;i++)v[i]=(FOutVector){a*planes[pl][0],a*planes[pl][1],a*planes[pl][2],0,0};
  unsigned out=cur_guard(m,1,0,0,PF_Translucent,v,NULL);
  if(!out||oracle_visible(m,v,4))fail("one common plane",tests++);
  geometry_rejects++;
  // On the original clip plane and at/below the conservative margin, keep it.
  for(unsigned edge=0;edge<4;edge++) {
   float distance=edge==0?0.0f:edge==1?0.5f:edge==2?1.0f:nextafterf(1.0f,0.0f);
   a=(distance-planes[pl][3])/n2;
   for(unsigned i=0;i<4;i++)v[i]=(FOutVector){a*planes[pl][0],a*planes[pl][1],a*planes[pl][2],0,0};
   out=cur_guard(m,1,0,0,PF_Translucent,v,NULL);
   if(out&&oracle_visible(m,v,4))fail("boundary",tests++);
  }
 }
 const FBITFIELD fallback[]={PF_NoTexture,PF_NoDepthTest,PF_WireFrame,PF_Corona,PF_Decal,PF_Ripple};
 quad(v,100,0,8,1,1);
 for(unsigned i=0;i<sizeof fallback/sizeof*fallback;i++)if(cur_guard(m,1,0,0,fallback[i]|PF_Translucent,v,NULL))fail("special flags",tests++);
 if(cur_guard(m,0,0,0,0,NULL,NULL)||cur_guard(m,1,1,0,0,NULL,NULL)||cur_guard(m,1,0,1,0,NULL,NULL))fail("invalid gate before access",tests++);
 for(unsigned corner=0;corner<4;corner++)for(unsigned axis=0;axis<3;axis++)for(unsigned value=0;value<5;value++) {
  quad(v,100,0,8,1,1);float *component=axis==0?&v[corner].x:axis==1?&v[corner].y:&v[corner].z;
  *component=value==0?NAN:value==1?INFINITY:value==2?-INFINITY:value==3?32768.0f:-32768.0f;
  if(cur_guard(m,1,0,0,PF_Translucent,v,NULL))fail("nonfinite/range fallback",tests++);
 }
 for(unsigned k=0;k<64;k++) {
  view(m,rf(-3.14f,3.14f),rf(-1.55f,1.55f),rf(-3.14f,3.14f),(int)(k&1),(int)((k>>1)&1),k&4?0.625f:1.25f,rf(-4096,4096),rf(-4096,4096),rf(-4096,4096));
  for(unsigned q=0;q<4096;q++) {
   for(unsigned i=0;i<4;i++)v[i]=(FOutVector){rf(-32767,32767),rf(-32767,32767),rf(-32767,32767),rf(-4,4),rf(-4,4)};
   unsigned out=cur_guard(m,1,0,0,PF_Translucent,v,NULL);
   geometry_tests++;
   if(out) {geometry_rejects++;if(oracle_visible(m,v,4))fail("double clip false rejection",geometry_tests);}
   digest=hash(digest,&out,sizeof out);
  }
 }
}

static fixture_t fixture(void)
{
 fixture_t f={0};f.n=3;f.frames=1;f.valid=1;f.shader=1;f.palette=1;f.renderwalls=1;f.record=1;
 for(unsigned k=0;k<MAXF;k++)view(f.matrix[k],0,0,0,0,0,1,0,0,0);
 for(unsigned t=0;t<MAXT;t++)f.tex[t]=(texture_input_t){1,0,32,32,0,1};
 for(unsigned i=0;i<MAXN;i++) {f.node[i].kind=3;f.node[i].id=(int)i+1;f.node[i].texture=(int)(i%MAXT);f.node[i].flags=PF_Translucent;f.node[i].alpha=128;f.node[i].light=192;quad(f.node[i].v,0,0,16,2,2);}
 return f;
}

static void compare(const fixture_t*f,const char *name,unsigned n)
{
 result_t a,b;base_run(f,&a);cur_run(f,&b);replay_tests++;
 if(memcmp(a.sorted,b.sorted,sizeof a.sorted)||memcmp(a.sorted_count,b.sorted_count,sizeof a.sorted_count))fail("both sort/barrier sequences",n);
 if(a.nevent!=b.nevent||memcmp(a.event,b.event,sizeof a.event)) {fprintf(stderr,"replay=%s events=%u/%u\n",name,a.nevent,b.nevent);fail("visible material/geometry sequence",n);}
 if(memcmp(a.wall_surf,b.wall_surf,sizeof a.wall_surf)||memcmp(a.wall_count,b.wall_count,sizeof a.wall_count))fail("lighting and hidden surfaces",n);
 if(memcmp(a.material,b.material,sizeof a.material)||a.texsig!=b.texsig) {fprintf(stderr,"replay=%s texsig=%u/%u\n",name,a.texsig,b.texsig);fail("classification/hwr_texsig state",n);}
 if(a.light_ids!=b.light_ids||a.lighting_calls!=b.lighting_calls||a.cube_calls!=b.cube_calls)fail("light-table order",n);
 if(a.record_hash!=b.record_hash||a.record_count!=b.record_count)fail("view-independent cache recording",n);
 if(memcmp(a.list_counts,b.list_counts,sizeof a.list_counts)||a.transforms!=b.transforms)fail("view/list lifetime",n);
 if(b.uploads>a.uploads||b.upload_bytes>a.upload_bytes)fail("modeled resource bound",n);
 if(b.upload_bytes<a.upload_bytes)resource_witnesses++;
 digest=hash(digest,&a.nevent,sizeof a.nevent);digest=hash(digest,a.event,a.nevent*sizeof*a.event);
}

static void replay(void)
{
 fixture_t f=fixture();
 // Two visible planes separated by a hidden wall: removing it before sorting
 // changes plane-height sort runs even while preserving drawcount numbers.
 f.node[0].kind=1;f.node[0].height=100*FRACUNIT;
 f.node[1].kind=3;quad(f.node[1].v,100,0,8,1,1);
 f.node[2].kind=1;f.node[2].height=10*FRACUNIT;
 compare(&f,"hidden wall plane barrier",tests++);
 // An existing transparent flag is reset by the original GetTexture when the
 // original image is nonresident. An unconditional geometric guard is unsafe.
 f=fixture();f.n=2;f.tex[0].metadata=3;f.node[0].texture=0;quad(f.node[0].v,100,0,8,1,1);f.node[1].texture=1;
 compare(&f,"naive classification counterexample",tests++);naive_classification_witnesses++;
 for(unsigned fault=0;fault<=14;fault++) {
  f=fixture();f.metadata_fault=fault;
  int yes=cur_query_case(&f);query_tests++;
  if(yes!=(fault==0))fail("metadata unknown/stale fallback",fault);
  quad(f.node[0].v,100,0,8,1,1);f.node[0].texture=0;f.node[1].texture=1;f.node[2].texture=2;
  compare(&f,"metadata lifecycle",tests++);
 }
 for(unsigned k=0;k<2048;k++) {
  f=fixture();f.n=1+rnd()%32;f.frames=1+rnd()%MAXF;f.evict_each_frame=(int)(rnd()&1);f.reload_frame=(k%7==0)?2:0;
  f.wire=k%17==0;f.fr_off=k%19==0?1u:0u;f.valid=k%23!=0;f.shader=k%5!=0;f.palette=k%7!=0;f.cube=k%11==0;f.record=k%3!=0;
  for(unsigned t=0;t<MAXT;t++){f.tex[t].metadata=(int)(rnd()%5);f.tex[t].source_has255=(int)(rnd()&1);f.tex[t].downloaded=(int)(rnd()&1);f.tex[t].data=(int)(rnd()&1);}
  for(unsigned frame=0;frame<f.frames;frame++)view(f.matrix[frame],rf(-3,3),rf(-1.3f,1.3f),rf(-1,1),(int)(rnd()&1),(int)(rnd()&1),k&4?0.625f:1.25f,rf(-64,64),rf(-64,64),rf(-64,64));
  for(unsigned i=0;i<f.n;i++) {
   node_input_t*n=&f.node[i];n->kind=1+(int)(rnd()%3);n->texture=(int)(rnd()%MAXT);n->height=((int)(rnd()%1000)+1)*FRACUNIT;n->light=(int)(rnd()%512)-128;n->alpha=(UINT8)rnd();n->cmap=(int)(rnd()&1);
   n->flags=1u<<(rnd()%7);if(k%13==0)n->flags|=PF_NoTexture;if(k%29==0)n->flags|=PF_Ripple;if(k%31==0)n->flags|=PF_NoDepthTest;
   quad(n->v,rf(-256,256),rf(-256,256),rf(-256,256),rf(0,64),rf(0,64));
  }
  compare(&f,"random multi-view replay",tests++);
 }
 // A cold unknown pair must classify normally; a warm known-no-key pair can
 // suppress an empty draw; subsequent view changes must draw it when visible.
 f=fixture();f.frames=3;f.evict_each_frame=1;quad(f.node[0].v,100,0,8,1,1);
 view(f.matrix[1],1.5f,0,0,0,0,1,0,0,0);compare(&f,"warm return/changed view",tests++);
 f.reload_frame=2;compare(&f,"source reload loses known state",tests++);
}

int main(void)
{
 geometry();replay();
 if(!geometry_rejects||!resource_witnesses||!naive_classification_witnesses)fail("missing positive witness",tests);
 printf("NEXT12_CULL_HOST PASS geometry=%u rejects=%u replay=%u query=%u modeled_resource_witnesses=%u naive_classification_witnesses=%u digest=%08x\n",geometry_tests,geometry_rejects,replay_tests,query_tests,resource_witnesses,naive_classification_witnesses,digest);
 return 0;
}
