/* Actual-source P4 packing/band fixture. Generated functions are supplied by
 * p4_pack_mmi_hosttest.py. Host runs test portable/fallback paths; EE execution
 * is required for the MMI path and its fourth negative control.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t u8;
typedef unsigned int u32;
typedef uint64_t u64;
#include "actual-functions.inc"

static unsigned checks, failures, cases;
static volatile unsigned sink;
static u8 source[32768] __attribute__((aligned(64)));
static u8 output[2048] __attribute__((aligned(64)));
static u8 expect[2048] __attribute__((aligned(64)));
static u8 saved[32768] __attribute__((aligned(64)));
static u8 big_source[1024*1024+32] __attribute__((aligned(64)));
static u8 big_output[1024*1024] __attribute__((aligned(64)));

static void check(int yes, const char *why)
{
    checks++;
    if (!yes && failures++ < 12) printf("PACK FAIL %s case=%u\n",why,cases);
}

/* The runner extracts this literal scalar function from pinned commit
 * dbc98504e1ad2cc6c2e8e1d24a6ef5d2f31d6ebf; there is no copied model. */
static void reference(const fillctx_t *c,u32 row,u8 *dst)
{
    baseline_fill_p4((void *)c,row,dst);
}

static void row_case(u32 width,u32 dx,u32 dy,u32 src_off,u32 dst_off,u32 band,int whole)
{
    fillctx_t c;
    u32 i,row,bytes=width/2u;
    u8 *d=output+32+dst_off;
    memset(&c,0,sizeof c);
    c.sw=(width << dx)+(whole?0u:16u);c.sh=3u << dy;
    c.W2=width;c.H2=3;c.dx=dx;c.dy=dy;c.src=source+src_off;
    for(i=0;i<c.sw*c.sh;i++)source[src_off+i]=(u8)(band*16u+((i*73u+i/11u)&15u));
    memcpy(saved,source,sizeof source);
    for(row=0;row<c.H2;row++)
    {
        cases++;
        memset(output,0xa5,sizeof output);
        reference(&c,row,expect);
        candidate_fill_p4(&c,row,d);
        check(!memcmp(expect,d,bytes),"all original nibbles, lane order and row samples");
        for(i=0;i<32u+dst_off;i++)check(output[i]==0xa5,"destination prefix unchanged");
        for(i=0;i<32u;i++)check(d[bytes+i]==0xa5,"destination suffix unchanged");
    }
    check(!memcmp(saved,source,sizeof source),"source immutable");
}

static void pairs(void)
{
    fillctx_t c;
    u32 band,a,b;
    memset(&c,0,sizeof c);c.src=source;c.sw=c.W2=32;c.sh=c.H2=1;
    for(band=0;band<16;band++)for(a=0;a<16;a++)for(b=0;b<16;b++)
    {
        u32 i;
        for(i=0;i<32;i++)source[i]=(u8)(band*16u+((i&1)?b:a));
        candidate_fill_p4(&c,0,output);
        cases++;
        for(i=0;i<16;i++)check(output[i]==a+16u*b,"all bands, pairs and vector lanes");
    }
}

static void bands(void)
{
    static const u32 counts[]={80,128,256,2048,4093,4096};
    u32 band,n,off,i;
    for(band=0;band<16;band++)for(n=0;n<=65;n++)for(off=0;off<16;off++)
    {
        u32 got=0xdeadbeefu;
        int ok;
        cases++;
        for(i=0;i<n;i++)source[off+i]=(u8)(band*16u+((i*7)&15u));
        ok=candidate_p4_band(source+off,n,&got);
        check(ok==(n!=0),"band count zero/positive");
        check(got==(n?band:0xdeadbeefu),"band publish only on complete success");
        if(n>1)
        {
            source[off+n-1]^=0x80u;got=0xdeadbeefu;
            check(!candidate_p4_band(source+off,n,&got) && got==0xdeadbeefu,"last source byte rejects before publish");
        }
    }
    for(band=0;band<16;band++)for(i=0;i<sizeof counts/sizeof counts[0];i++)for(off=0;off<2;off++)
    {
        u32 k;n=counts[i];
        memset(source+off,(int)(band*16u+7u),n);
        for(k=0;k<96;k++)
        {
            u32 at=k<32?k:k<64?n/2u+k-48u:n-96u+k;
            u32 got=0xdeadbeefu;
            u8 old=source[off+at];
            cases++;source[off+at]^=0x10;
            check(!candidate_p4_band(source+off,n,&got) && got==0xdeadbeefu,"all MMI lanes and distant groups reject before publish");
            source[off+at]=old;
        }
        {
            u32 got=0xdeadbeefu;
            check(candidate_p4_band(source+off,n,&got) && got==band,"large bands and scalar tails preserve all source bytes");
        }
    }
}

static void exact_allocations(void)
{
    /* The host sanitizer sees exact bounds after the input/output tail. */
    u32 width,off;
    for(width=2;width<=130;width+=2)for(off=0;off<16;off++)
    {
        fillctx_t c;
        u32 i,row,bytes=width/2;
        u8 *s=malloc(width*3u+off),*d=malloc(bytes+off);
        check(s!=NULL && d!=NULL,"exact allocation");
        if(!s||!d)exit(2);
        memset(&c,0,sizeof c);c.src=s+off;c.sw=c.W2=width;c.sh=c.H2=3;
        for(i=0;i<width*3u;i++)s[off+i]=(u8)(0xa0u+((i*11u)&15u));
        for(row=0;row<3;row++)
        {
            cases++;reference(&c,row,expect);candidate_fill_p4(&c,row,d+off);
            check(!memcmp(expect,d+off,bytes),"exact allocation tail");
        }
        free(s);free(d);
    }
}

static void mutation_controls(void)
{
    fillctx_t c;
    unsigned i,detected=0;
    memset(&c,0,sizeof c);c.src=source;c.sw=c.W2=32;c.sh=c.H2=1;
    for(i=0;i<32;i++)source[i]=(u8)(0x90u+((i*7u+i/9u)&15u));
    reference(&c,0,expect);
    mutation_shift(&c,0,output);detected+=memcmp(expect,output,16)!=0;
    mutation_gather(&c,0,output);detected+=memcmp(expect,output,16)!=0;
    mutation_mask(&c,0,output);detected+=memcmp(expect,output,16)!=0;
    check(detected==3,"executed three independent packing mutations");
#ifdef PACK_BAND_MUTATION
    {
        u32 got=0xdeadbeefu;
        memset(source,0x91,32);source[15]=0x81;
        detected+=mutation_band(source,32,&got)!=0;
        check(detected==4,"native upper register half reduction mutation rejected");
    }
    printf("PACK mutations detected=%u expected=4\n",detected);
#else
    printf("PACK mutations detected=%u expected=3\n",detected);
#endif
}

static void corpus(const char *path)
{
    FILE *f=fopen(path,"rb");
    u8 magic[8];u32 count,i,total=0,packed=0,headers[4];
    check(f!=NULL,"stock corpus opened");if(!f)return;
    if(fread(magic,1,8,f)!=8 || memcmp(magic,"P4CORP12",8))
    {check(0,"stock corpus magic");fclose(f);return;}
    check(1,"stock corpus magic");
    if(fread(&count,4,1,f)!=1 || count>65536u)
    {check(0,"stock corpus count");fclose(f);return;}
    check(1,"stock corpus count");
    for(i=0;i<count;i++)
    {
        fillctx_t c;
        u32 at,got=0xdeadbeefu,n,ok;
        if(fread(headers,4,4,f)!=4)
        {check(0,"stock corpus dimensions");break;}
        check(1,"stock corpus dimensions");
        n=headers[3];
        if(headers[0]<2u || headers[0]>1024u || (headers[0]&1u)
            || !headers[1] || headers[1]>1024u
            || n!=headers[0]*headers[1]
            || (headers[2]!=0xffffffffu && headers[2]>=16u))
        {check(0,"stock texture exact bounds");break;}
        check(1,"stock texture exact bounds");
        if(fread(big_source,1,n,f)!=n)
        {check(0,"every stock source byte read");break;}
        check(1,"every stock source byte read");
        ok=(u32)candidate_p4_band(big_source,n,&got);
        check(ok==(headers[2]!=0xffffffffu),"every stock eligibility decision");
        check(got==(ok?headers[2]:0xdeadbeefu),"every stock original CLUT band");
        if(ok)
        {
            u32 y;
            memset(&c,0,sizeof c);c.src=big_source;c.sw=c.W2=headers[0];c.sh=c.H2=headers[1];
            for(y=0;y<c.H2;y++)candidate_fill_p4(&c,y,big_output+y*(c.W2/2));
            for(at=0;at<n;at++)
            {
                u32 index=16u*got+((big_output[at/2]>>((at&1u)*4u))&15u);
                check(index==big_source[at],"every original packed stock palette index");
            }
            packed+=n;
        }
        total+=n;
    }
    check(i==count && fgetc(f)==EOF,"stock corpus consumed exactly");fclose(f);
    printf("PACK corpus textures=%u source_bytes=%u packed_source_bytes=%u\n",count,total,packed);
}

#ifdef __mips__
typedef void (*fill_t)(void *,u32,u8 *);
static inline u32 count(void){u32 t;__asm__ volatile("mfc0 %0,$9":"=r"(t));return t;}
static u32 bench_fill(fill_t fill,u32 width,u32 height,u32 off)
{
    fillctx_t c;
    u32 rep,y,best=~0u;
    memset(&c,0,sizeof c);c.src=big_source+off;c.sw=c.W2=width;c.sh=c.H2=height;
    for(rep=0;rep<7;rep++)
    {
        u32 start=count();
        for(y=0;y<height;y++)fill(&c,y,big_output+y*(width/2u));
        u32 elapsed=count()-start;
        sink+=big_output[rep];if(elapsed<best)best=elapsed;
    }
    return best;
}
static void benchmarks(void)
{
    static const u32 widths[]={8,16,24,40,64,128,256,512,1024};
    u32 i,off,n,got,rep,best,before;
    for(n=0;n<sizeof big_source;n++)big_source[n]=(u8)(0x90u+((n*7u+n/11u)&15u));
    for(i=0;i<sizeof widths/sizeof widths[0];i++)for(off=0;off<=1;off++)
    {
        u32 width=widths[i],height=1024;
        u32 before=bench_fill(baseline_fill_p4,width,height,off);
        u32 after=bench_fill(candidate_fill_p4,width,height,off);
        printf("PACK bench width=%u height=%u src_off=%u scalar=%u candidate=%u texels=%u\n",width,height,off,before,after,width*height);
    }
    for(off=0;off<2;off++)
    {
        before=~0u;
        best=~0u;
        for(rep=0;rep<7;rep++)
        {
            u32 start=count();int ok=baseline_p4_band(big_source+off,1024u*1024u,&got);
            u32 elapsed=count()-start;sink+=got+(u32)ok;if(elapsed<before)before=elapsed;
            start=count();ok=candidate_p4_band(big_source+off,1024u*1024u,&got);
            elapsed=count()-start;sink+=got+(u32)ok;if(elapsed<best)best=elapsed;
        }
        printf("PACK band texels=1048576 src_off=%u scalar=%u candidate=%u sink=%u\n",off,before,best,(unsigned)sink);
    }
}
#endif

int main(int argc,char **argv)
{
    static const u32 widths[]={2,6,8,10,14,16,18,24,30,32,40,48,64,72,80,96,120,128,136,192,256,264,512,1008,1024};
    u32 i,band,so,ds;
    pairs();bands();exact_allocations();mutation_controls();
    for(i=0;i<sizeof widths/sizeof widths[0];i++)for(so=0;so<16;so++)for(ds=0;ds<16;ds++)
        row_case(widths[i],0,0,so,ds,8,1);
    for(band=0;band<16;band++)for(i=0;i<sizeof widths/sizeof widths[0];i++)
    {
        row_case(widths[i],0,0,0,0,band,1);
        row_case(widths[i],1,0,0,0,band,1);
        row_case(widths[i],0,1,0,0,band,1);
        row_case(widths[i],0,0,0,0,band,0);
    }
    corpus(argc>1?argv[1]:"host:full-texels.bin");
    printf("PACK selftest checks=%u cases=%u failures=%u\n",checks,cases,failures);
#ifdef __mips__
    if(!failures)benchmarks();
#endif
    printf("PACK DONE failures=%u\n",failures);fflush(stdout);
    return failures?1:0;
}
