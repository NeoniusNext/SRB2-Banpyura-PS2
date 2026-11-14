/* Actual WPack readiness and allocation/state invariance proof. */
#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
static unsigned tracked_allocs, tracked_frees;
static size_t tracked_live;
static struct { void *ptr; size_t size; } owned[64];
static void *ReadyMalloc(size_t size)
{
    void *p = malloc(size);
    unsigned i;
    tracked_allocs++;
    if (p) {
        for (i=0;i<64 && owned[i].ptr;i++);
        if (i==64) abort();
        owned[i].ptr=p; owned[i].size=size; tracked_live+=size;
    }
    return p;
}
static void ReadyFree(void *p)
{
    unsigned i;
    if (!p) return;
    for (i=0;i<64 && owned[i].ptr!=p;i++);
    if (i==64) abort();
    tracked_live-=owned[i].size; owned[i].ptr=NULL; owned[i].size=0;
    tracked_frees++; free(p);
}
#define malloc ReadyMalloc
#define free ReadyFree
#include WPACK_SOURCE
#undef malloc
#undef free
#define main corpus_main
#include PACK_HOST_SOURCE
#undef main
static unsigned checks;
#define REQUIRE(c) do { checks++; if (!(c)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c); exit(1); } } while(0)
static boolean ready_unchanged(FILE *fp, const lumpinfo_t *record)
{
    pkstream_t before[PK_STREAMS];
    unsigned char counters[sizeof pks];
    char error[sizeof pk_err];
    UINT8 *oldi=iobuf,*oldc=cbuf;
    unsigned a=tracked_allocs,f=tracked_frees,next=pkst_next;
    size_t live=tracked_live;
    boolean value;
    memcpy(before,pkst,sizeof before); memcpy(counters,&pks,sizeof pks); memcpy(error,pk_err,sizeof error);
    value=WPack_CanReadWithoutAlloc(fp,record);
    REQUIRE(!memcmp(before,pkst,sizeof before)); REQUIRE(!memcmp(counters,&pks,sizeof pks));
    REQUIRE(!memcmp(error,pk_err,sizeof error)); REQUIRE(pkst_next==next);
    REQUIRE(iobuf==oldi && cbuf==oldc); REQUIRE(tracked_allocs==a && tracked_frees==f && tracked_live==live);
    return value;
}
int main(void)
{
    FILE *files[8];
    unsigned char data[8192],compressed[16384],dest[8192];
    lumpinfo_t raw={0}, lz={0};
    int n; unsigned i,a,f;
    size_t sizes[]={0,1,65536,64u*WPACK_BLOCK,64u*WPACK_BLOCK+1u,UINT32_MAX,SIZE_MAX};
    for(i=0;i<sizeof data;i++) data[i]=(unsigned char)((i*37)^(i>>5));
    for(i=0;i<8;i++) { files[i]=tmpfile(); REQUIRE(files[i]); REQUIRE(fwrite(data,1,sizeof data,files[i])==sizeof data); REQUIRE(!fflush(files[i])); }
    W_InitLumpPosition(&raw,0); raw.size=raw.disksize=sizeof data; raw.compression=CM_NOCOMPRESSION;
    REQUIRE(!ready_unchanged(NULL,&raw)); REQUIRE(!ready_unchanged(files[0],NULL)); REQUIRE(!ready_unchanged(files[0],&raw));
    REQUIRE(AllocBuffers()); REQUIRE(!ready_unchanged(files[0],&raw));
    REQUIRE(StreamNew(files[0],NULL)); REQUIRE(!ready_unchanged(files[0],&raw));
    REQUIRE(Fill(StreamFind(files[0]),0,128,0)); REQUIRE(ready_unchanged(files[0],&raw));
    for(i=0;i<sizeof sizes/sizeof sizes[0];i++) {
        raw.size=sizes[i]; REQUIRE(ready_unchanged(files[0],&raw));
        raw.compression=CM_LZ4; REQUIRE(ready_unchanged(files[0],&raw)==(sizes[i]<=64u*WPACK_BLOCK));
        raw.compression=CM_LZF; REQUIRE(!ready_unchanged(files[0],&raw));
        raw.compression=CM_UNSUPPORTED; REQUIRE(!ready_unchanged(files[0],&raw)); raw.compression=CM_NOCOMPRESSION;
    }
    raw.size=raw.disksize=sizeof data;
    { UINT8 *p=cbuf; cbuf=NULL; REQUIRE(!ready_unchanged(files[0],&raw)); cbuf=p; }
    { UINT8 *p=iobuf; iobuf=NULL; REQUIRE(!ready_unchanged(files[0],&raw)); iobuf=p; }
    a=tracked_allocs; f=tracked_frees;
    REQUIRE(WPack_ReadLumpN(NULL,files[0],0,&raw,dest,sizeof dest,0)==sizeof dest); REQUIRE(!memcmp(data,dest,sizeof data));
    REQUIRE(tracked_allocs==a && tracked_frees==f);
    StreamFind(files[0])->len=0; REQUIRE(ready_unchanged(files[0],&raw));
    REQUIRE(WPack_ReadLump(files[0],&raw,dest,sizeof dest,0)==sizeof dest); REQUIRE(!memcmp(data,dest,sizeof data));
    REQUIRE(tracked_allocs==a && tracked_frees==f);
    n=LZ4_compress_default((const char*)data,(char*)compressed,sizeof data,sizeof compressed); REQUIRE(n>0);
    REQUIRE(!ftruncate(fileno(files[1]),0)); REQUIRE(!fseek(files[1],0,SEEK_SET)); REQUIRE(fwrite(compressed,1,n,files[1])==(size_t)n); REQUIRE(!fflush(files[1]));
    W_InitLumpPosition(&lz,0); lz.size=sizeof data; lz.disksize=n; lz.compression=CM_LZ4;
    REQUIRE(StreamNew(files[1],NULL)); REQUIRE(Fill(StreamFind(files[1]),0,n,0)); REQUIRE(ready_unchanged(files[1],&lz));
    a=tracked_allocs; f=tracked_frees;
    REQUIRE(WPack_ReadLumpN(NULL,files[1],0,&lz,dest,sizeof dest,0)==sizeof dest); REQUIRE(!memcmp(data,dest,sizeof data));
    REQUIRE(tracked_allocs==a && tracked_frees==f);
    for(i=2;i<8;i++) { REQUIRE(StreamNew(files[i],NULL)); REQUIRE(Fill(StreamFind(files[i]),0,128,0)); REQUIRE(ready_unchanged(files[i],&raw)); }
    REQUIRE(!ready_unchanged(files[0],&raw)); REQUIRE(!ready_unchanged(files[1],&lz)); REQUIRE(ready_unchanged(files[7],&raw));
    StreamDrop(files[7]); REQUIRE(!ready_unchanged(files[7],&raw));
    WPack_Shutdown(); REQUIRE(tracked_live==0 && tracked_allocs==tracked_frees);
    for(i=0;i<8;i++) fclose(files[i]);
    printf("READY PASS checks=%u allocations=%u frees=%u live=%zu\n",checks,tracked_allocs,tracked_frees,tracked_live);
    return 0;
}
