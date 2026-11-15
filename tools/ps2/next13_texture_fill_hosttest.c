#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SOURCE_CAP=1048576, OUTPUT_CAP=4224 };
static u8 source[SOURCE_CAP+96] __attribute__((aligned(16))), saved[sizeof source];
static u8 output[OUTPUT_CAP] __attribute__((aligned(16))),wanted[OUTPUT_CAP];
static u32 cases,pixels,digest=2166136261u,rng=0x413ab79du;
typedef void (*fill_fn)(void *,u32,u8 *);
static fill_fn volatile functions[2][2]={{base_fill_idx,base_fill_ia88},{cur_fill_idx,cur_fill_ia88}};
static u32 rnd(void){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}

static int check(fillctx_t *c,u32 format,u32 row,u32 offset)
{
    const u32 bytes=format?4:1,sbpp=format?2:1;
    const u32 sy=(row<<c->dy)+((1u<<c->dy)>>1);
    const size_t extent=(size_t)(c->src-source)+(size_t)c->sw*c->sh*sbpp+32;
    const fillctx_t initial=*c;
    u32 x,tag;
    if(extent>sizeof source || 32+offset+c->W2*bytes+32>sizeof output)abort();
    memcpy(saved,source,extent);memset(wanted,0xA6,sizeof wanted);
    for(x=0;x<c->W2;x++)
    {
        const u32 sx=(x<<c->dx)+((1u<<c->dx)>>1);
        const u8 *p=c->src+((size_t)sy*c->sw+sx)*sbpp;
        if(format)
        {
            const u32 alpha=((u32)p[1]*128u+127u)/255u;
            const u32 value=(u32)p[0]*0x010101u|(alpha<<24);
            memcpy(wanted+32+offset+x*4,&value,4);
        }
        else wanted[32+offset+x]=p[0];
    }
    for(tag=0;tag<2;tag++)
    {
        memset(output,0xA6,sizeof output);functions[tag][format](c,row,output+32+offset);
        if(memcmp(output,wanted,sizeof output)||memcmp(source,saved,extent)||memcmp(c,&initial,sizeof *c))
        {
            for(x=0;x<sizeof output && output[x]==wanted[x];x++){}
            printf("FILL NEXT13 FAIL tag=%u case=%u fmt=%u width=%u dx=%u dy=%u row=%u source=%u dst=%u at=%u got=%u wanted=%u\n",
                tag,cases,format,c->W2,c->dx,c->dy,row,(u32)(c->src-source),offset,x,x<sizeof output?output[x]:0,x<sizeof output?wanted[x]:0);
            return 0;
        }
    }
    for(x=0;x<c->W2*bytes;x++)digest=(digest^wanted[32+offset+x])*16777619u;
    cases++;pixels+=c->W2;return 1;
}

static int fill_setup(fillctx_t *c,u32 format,u32 width,u32 dx,u32 dy,u32 row,u32 align,u32 pattern)
{
    size_t i,n;
    memset(c,0xC7,sizeof *c);
    c->sw=(width<<dx)+3;c->sh=((row+1)<<dy)+1;c->W2=width;c->H2=row+1;c->dx=dx;c->dy=dy;
    c->src=source+32+align;c->mode=(int)rnd();c->holes=(int)rnd();
    n=(size_t)c->sw*c->sh*(format?2:1);
    if(n>SOURCE_CAP)return 0;
    memset(source,0x6D,n+96);
    for(i=0;i<n;i++)source[32+align+i]=pattern==0?(u8)rnd():pattern==1?255:pattern==2?0:(u8)i;
    return 1;
}

static int widths(void)
{
    fillctx_t c;u32 format,width,kind;
    for(format=0;format<2;format++)for(width=0;width<=1024;width++)for(kind=0;kind<4;kind++)
    {
#ifdef NEXT13_FILL_EE
        if(width>65 && width!=127 && width!=128 && width!=255 && width!=256 && width!=511 && width!=512 && width!=1023 && width!=1024)continue;
#endif
        const u32 dx=kind?kind-1:0,dy=(width+kind)%3,row=(width+kind)%4;
        if(fill_setup(&c,format,width,dx,dy,row,(width+kind)&15,kind)&&!check(&c,format,row,format?(width&3)*4:width&15))return 0;
    }
    return 1;
}

static int alignments(void)
{
    static const u32 widths_[]={1,2,3,4,7,16,31,64,127,256,511,1024};
    fillctx_t c;u32 format,wi,sa,da,shift;
    for(format=0;format<2;format++)for(wi=0;wi<12;wi++)for(sa=0;sa<16;sa++)
    for(da=0;da<16;da+=(format?4:1))for(shift=0;shift<3;shift++)
    {
#ifdef NEXT13_FILL_EE
        if(widths_[wi]>7 && ((sa!=0 && sa!=1 && sa!=7 && sa!=15) || (format?(da!=0 && da!=4 && da!=12):(da!=0 && da!=1 && da!=7 && da!=15))))continue;
#endif
        const u32 dx=shift?shift*2-1:0,dy=shift,row=shift;
        if(fill_setup(&c,format,widths_[wi],dx,dy,row,sa,(wi+sa+da)%4)&&!check(&c,format,row,da))return 0;
    }
    return 1;
}

static int shifts(void)
{
    fillctx_t c;u32 format,dx,dy,width,row;
    for(format=0;format<2;format++)for(dx=0;dx<=10;dx++)for(dy=0;dy<=10;dy++)
    for(width=1;width<=7;width+=3)for(row=0;row<4;row++)
        if(fill_setup(&c,format,width,dx,dy,row,(dx+dy)&15,0)&&!check(&c,format,row,format?12:7))return 0;
    return 1;
}

static int all_pairs(void)
{
    static const u32 aligns[]={0,1,7,15},dest[]={0,4,12};
    fillctx_t c;u32 ai,di,i,row;
    for(ai=0;ai<4;ai++)for(di=0;di<3;di++)
    {
        memset(&c,0,sizeof c);c.src=source+32+aligns[ai];c.sw=1024;c.sh=256;c.W2=1024;c.H2=256;
        memset(source,0x6D,sizeof source);
        for(i=0;i<262144;i++){source[32+aligns[ai]+2*i]=(u8)(i/4);source[33+aligns[ai]+2*i]=(u8)(i/1024);}
        for(row=0;row<256;row++)if(!check(&c,1,row,dest[di]))return 0;
    }
    return 1;
}

int main(void)
{
    if(!widths()||!alignments()||!shifts()||!all_pairs())return 1;
    printf("FILL NEXT13 PASS cases=%u pixels=%u digest=%08x\n",cases,pixels,digest);return 0;
}
