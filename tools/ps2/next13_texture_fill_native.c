#include "common.h"
#include <kernel.h>
#define main texture_exactness_main
#include "host-fixture.inc"
#undef main

static u32 ticks(void){u32 x;__asm__ volatile("mfc0 %0,$9":"=r"(x)::"memory");return x;}
static u32 begin_time(void){u32 x;__asm__ volatile("mfc0 %0,$12\n\tdi\n\tsync.p":"=r"(x)::"memory");return x;}
static void end_time(u32 x){if(x&0x10000u)__asm__ volatile("ei\n\tsync.p":::"memory");}

static int benchmark(void)
{
    static const u32 widths_[]={1,2,3,4,7,16,31,64,127,256,511,1024};
    u32 cache,format,scenario,wi,length,rep,order,rowid=0;
    for(cache=0;cache<2;cache++)for(format=0;format<2;format++)for(scenario=0;scenario<4;scenario++)
    for(wi=0;wi<12;wi++)for(length=0;length<2;length++)
    {
        const u32 width=widths_[wi],dx=scenario==2?1:scenario==3?3:0;
        const u32 dy=scenario==2?1:scenario==3?2:0,row=scenario==2?1:scenario==3?2:0;
        const u32 srcalign=scenario==1?1:scenario==3?15:0,dstalign=scenario==1?(format?4:1):scenario==3?(format?12:15):0;
        const u32 count=length?64:1;
        fillctx_t c;
        if(!fill_setup(&c,format,width,dx,dy,row,srcalign,0)||!check(&c,format,row,dstalign))return 0;
        for(rep=0;rep<9;rep++)
        {
            u32 times[2]={0,0};
            for(order=0;order<2;order++)
            {
                const u32 tag=order^(rep&1u);fill_fn volatile fn=functions[tag][format];u32 j,before,previous;
                memset(output,0xA6,sizeof output);
                if(cache){FlushCache(0);FlushCache(1);FlushCache(2);}
                previous=begin_time();before=ticks();
                for(j=0;j<count;j++)fn(&c,row,output+32+dstalign);
                times[tag]=ticks()-before;end_time(previous);
                if(!times[tag]||times[tag]>=0x80000000u||memcmp(output,wanted,sizeof output))
                {printf("FILL NEXT13 FAIL timing row=%u tag=%u\n",rowid,tag);return 0;}
            }
            printf("FILL13 TIMING row=%u fmt=%u scenario=%u width=%u dx=%u dy=%u source=%u dst=%u count=%u cache=%u rep=%u old=%u new=%u\n",
                rowid,format,scenario,width,dx,dy,srcalign,dstalign,count,cache,rep,times[0],times[1]);
        }
        rowid++;
    }
    return rowid==384;
}

int main(void)
{
    void *(*volatile keep_memmove)(void *,const void *,size_t)=memmove;
    keep_memmove(source,source,0);
    if(texture_exactness_main())return 1;
    if(!benchmark())return 1;
    printf("FILL NEXT13 DONE\n");return 0;
}
