#include "common.h"
#include <stdio.h>
#include <string.h>
static uint32_t state=0x178abc15u;
static uint32_t rnd(void){state^=state<<13;state^=state>>17;return state^=state<<5;}
static float rf(void){return(float)((int32_t)(rnd()%65537u)-32768)*(1.0f/128.0f);}
int main(void)
{
 unsigned failures=0;
 for(unsigned i=0;i<1000000;i++)
 {
  tile_plane_t p={0};float a[6],b[6];int64_t n1=(int32_t)rnd(),n2=(int32_t)rnd();
  if(!(i%7)){n1=(int64_t)(((uint64_t)rnd()<<32)|rnd());n2=(int64_t)(((uint64_t)rnd()<<32)|rnd());}
  for(int j=0;j<3;j++){p.rq[j]=rf();p.us[j]=rf();p.vs[j]=rf();}
  p.inv_det=1.0f/(1.0f+fabsf(rf())*16384.0f);if(i&1)p.inv_det=-p.inv_det;
  base_plane(&p,n1,n2,a);fast_plane(&p,n1,n2,b);
  if(memcmp(a,b,sizeof a)){if(failures<5)printf("TIP FAIL case=%u n1=%lld n2=%lld\n",i,(long long)n1,(long long)n2);failures++;}
 }
 printf("TIP COMPLETE checks=1000000 float_outputs=6000000 failures=%u\n",failures);return failures!=0;
}
