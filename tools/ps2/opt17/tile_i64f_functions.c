#include <stdint.h>
__attribute__((noinline)) float old_cast(int64_t v){return(float)v;}
__attribute__((noinline)) float direct_cast(int64_t v){return(float)(int32_t)v;}
__attribute__((noinline)) float nearest_cast(int64_t value)
{
 int32_t n=(int32_t)value;
 if((int64_t)n!=value)return(float)value;
 if(n>=-16777216 && n<=16777216)return(float)n;
 const uint32_t sign=(uint32_t)n&0x80000000u;
 const uint32_t mag=n<0?0u-(uint32_t)n:(uint32_t)n;
 unsigned shift=1;uint32_t top=mag>>24;
 if(top>=16u){shift+=4;top>>=4;}
 if(top>=4u){shift+=2;top>>=2;}
 if(top>=2u)shift++;
 uint32_t mant=mag>>shift;
 const uint32_t rem=mag&((1u<<shift)-1u),half=1u<<(shift-1u);
 if(rem>half ||(rem==half &&(mant&1u)))mant++;
 const uint32_t bits=sign+((shift+150u)<<23)+(mant-0x800000u);
 union{uint32_t u;float f;}out={bits};return out.f;
}

__attribute__((noinline)) float safe_cast(int64_t value)
{
 int32_t n=(int32_t)value;
 if((int64_t)n==value && ((n>=-16777216 && n<=16777216) || !((uint32_t)n&127u)))return(float)n;
 return(float)value;
}
