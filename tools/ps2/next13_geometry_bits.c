/* Compare complete source-extracted PBPut output in memory. Host arithmetic is
 * an additional oracle; the EE MIN/MAX operand-selection and O/U proof comes
 * from the original instruction definitions and actual linked bodies. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "geometry-result.h"

void geometry_baseline(const uint32_t *,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,geometry_result_t *);
void geometry_candidate(const uint32_t *,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,geometry_result_t *);

static uint32_t rng = 0x51C0FFEEu;
static uint32_t word(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

int main(void)
{
    static const uint32_t special[] = {
        0,0x80000000u,1,0x80000001u,0x007FFFFFu,0x807FFFFFu,
        0x00800000u,0x80800000u,0x3F800000u,0xBF800000u,
        0x7F7FFFFFu,0xFF7FFFFFu,0x7F800000u,0xFF800000u,
        0x7F800001u,0xFF800001u,0x7FBFFFFFu,0xFFBFFFFFu,
        0x7FC00000u,0xFFC00000u,0x7FFFFFFFu,0xFFFFFFFFu
    };
    uint32_t input[1250];
    geometry_result_t base,cur;
    unsigned i,j,n,needext,planwant,overlap,initial;
    unsigned long cases=0, alias_cases=0, irregular_cases=0;
    for(i=0;i<262144;i++) {
#ifdef NEXT13_GEOMETRY_REACHABLE_ONLY
        n=i<248?3+i:(i%257==0?250:3+i%31);
#else
        n=i<251?i:(i%257==0?250:3+i%31);
#endif
        needext=(i>>1)&1;planwant=(i>>2)&1;overlap=(i>>3)%8;initial=(i>>6)&3;
        for(j=0;j<1250;j++)input[j]=word();
        if(i<sizeof special/sizeof special[0]*sizeof special/sizeof special[0]) {
            for(j=0;j<n;j++) {
                input[5*j+3]=special[(i+j)%(sizeof special/sizeof special[0])];
                input[5*j+4]=special[(i/(sizeof special/sizeof special[0])+j)%(sizeof special/sizeof special[0])];
            }
        }
        geometry_baseline(input,n,needext,planwant,overlap,initial,&base);
        geometry_candidate(input,n,needext,planwant,overlap,initial,&cur);
        if(memcmp(&base,&cur,sizeof base)) {
            const uint32_t *a=(const uint32_t *)(const void *)&base,*b=(const uint32_t *)(const void *)&cur;
            for(j=0;j<sizeof base/sizeof *a;j++)if(a[j]!=b[j])break;
            fprintf(stderr,"PBPUT mismatch case=%u n=%u extent=%u plan=%u alias=%u initial=%u word=%u base=%08x candidate=%08x\n",
                i,n,needext,planwant,overlap,initial,j,a[j],b[j]);return 1;
        }
        cases++;if(overlap)alias_cases++;if(n<3)irregular_cases++;
    }
    printf("PBPUT BITS PASS cases=%lu alias=%lu direct-helper-n-under-3=%lu special-encodings=%zu full-result-bytes=%zu\n",
        cases,alias_cases,irregular_cases,sizeof special/sizeof special[0],sizeof base);
    return 0;
}
