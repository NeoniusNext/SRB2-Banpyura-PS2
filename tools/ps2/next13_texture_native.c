/* Executes complete game-LTO dec_level bodies, never a separately tuned helper. */
#include "common.h"
#include <kernel.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "native-state.h"
static void texture_native_palette(void);
static int texture_native_check(void);
#define TEXTURE_NATIVE_PALETTE() texture_native_palette()
#define TEXTURE_NATIVE_CHECK() texture_native_check()
#define main texture_exactness_main
#include "host-fixture.inc"
#undef main

static unsigned char native_expected[2][36896] __attribute__((aligned(16)));
static void texture_native_palette(void)
{
    unsigned tag;
    for (tag = 0; tag < 2; tag++)
    {
        memset(native_state[tag],0xC7,36896);
        memcpy(native_state[tag]+35264,H.pal,sizeof H.pal);
        memcpy(native_expected[tag],native_state[tag],36896);
    }
}
static int texture_native_check(void)
{
    return !memcmp(native_expected[0],native_state[0],36896) && !memcmp(native_expected[1],native_state[1],36896);
}
static u32 cycles(void) { u32 v; __asm__ volatile("mfc0 %0,$9" : "=r"(v) : : "memory"); return v; }
static u32 begin_time(void) { u32 v; __asm__ volatile("mfc0 %0,$12\n\tdi\n\tsync.p" : "=r"(v) : : "memory"); return v; }
static void end_time(u32 v) { if (v & 0x10000u) __asm__ volatile("ei\n\tsync.p" : : : "memory"); }

static void timing_source(u32 width,u32 level,u32 kind)
{
    u32 i, x, y, side=1u<<level, low=side/4, high=side-low-1;
    u8 *s=source+GUARD;
    memset(source,0x6D,sizeof source);
    for (i=0;i<width*width;i++) s[i]=(u8)(random_word()%255u);
    for (y=0;y<(width>>level);y++) for(x=0;x<(width>>level);x++)
    {
        const u32 at=(y*side+low)*width+x*side+low;
        const u32 dx=high-low,dy=dx*width;
        if (kind==0) { s[at]=7; s[at+dx]=7; s[at+dy]=7; s[at+dy+dx]=7; }
        else if(kind==1) { s[at]=255; s[at+dx]=255; s[at+dy]=255; s[at+dy+dx]=255; }
        else if(kind==3) s[at+(x%4==0?0:x%4==1?dx:x%4==2?dy:dy+dx)]=255;
        else if(kind==4) { s[at]=255;s[at+dy+dx]=255; }
        else if(kind==5) { s[at]=(u8)random_word();s[at+dx]=(u8)random_word();s[at+dy]=(u8)random_word();s[at+dy+dx]=(u8)random_word(); }
    }
}

static int benchmark(void)
{
    static const u32 widths[]={2,8,64,256,1024};
    u32 cache,kind,wi,li,rep,order,row=0;
    palette(3);
    for(cache=0;cache<2;cache++) for(kind=0;kind<6;kind++) for(wi=0;wi<5;wi++)
    for(li=0;li<(wi?2u:1u);li++)
    {
        const u32 width=widths[wi],level=li?3u:1u,n=(width>>level)*(width>>level);
        const u32 count=width<64?8u:1u;
        timing_source(width,level,kind);
        memset(wanted,0xA6,sizeof wanted); oracle(source+GUARD,width,width,level,wanted+GUARD);
        for(rep=0;rep<9;rep++)
        {
            u32 times[2]={0,0};
            for(order=0;order<2;order++)
            {
                const u32 tag=order^(rep&1u);
                texture_fn volatile fn=tag?cur_dec_level:base_dec_level;
                u32 j,before,previous;
                memset(output,0xA6,sizeof output);
                if(cache) { FlushCache(0);FlushCache(1);FlushCache(2); }
                previous=begin_time();before=cycles();
                for(j=0;j<count;j++)fn(source+GUARD,width,width,level,output+GUARD);
                times[tag]=cycles()-before;end_time(previous);
                if(times[tag]>=0x80000000u || memcmp(output,wanted,sizeof output) || !texture_native_check())
                {printf("TEXTURE NEXT13 FAIL timing row=%u tag=%u pixels=%u\n",row,tag,n);return 0;}
            }
            printf("TEXTURE13 TIMING row=%u width=%u level=%u kind=%u cache=%u count=%u rep=%u old=%u new=%u\n",
                row,width,level,kind,cache,count,rep,times[0],times[1]);
        }
        row++;
    }
    return row==108;
}

int main(void)
{
    if(texture_exactness_main())return 1;
    if(!benchmark())return 1;
    printf("TEXTURE NEXT13 DONE\n");
    return 0;
}
