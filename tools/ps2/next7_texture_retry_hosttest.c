/* Actual generator and patch conversion in the real zone. Only I/O/platform
 * services are boundary stubs. Each public output is normalized byte-for-byte. */
#define _GNU_SOURCE 1
#include <stdint.h>
#include <ctype.h>
#include <unistd.h>
static int netgame;
static int PS2Lua_InCall(void) { return 0; }
void PS2Spill_Reset(void) {}
typedef uint8_t UINT8;
#define max(a,b) ((a)>(b)?(a):(b))
#define main zone_suite_main
#include "zone_hosttest.c"
#undef main
#undef __attribute__
#define ATTRPACK __attribute__((packed))
typedef uint8_t UINT8;
typedef uint16_t UINT16;
typedef int16_t INT16;
typedef INT32 fixed_t;
typedef UINT16 lumpnum_t;
typedef INT32 pictureformat_t;
typedef UINT32 pictureflags_t;
typedef INT32 tic_t;
static tic_t leveltime;
enum { PICFMT_PATCH, PICFMT_FLAT, PICFLAGS_USE_TRANSPARENTPIXEL = 1 };
#define SHORT(x) (x)
#define LONG(x) (x)
#define PNG_HEADER_SIZE 8
#define TRANSPARENTPIXEL 255
#define CONS_WARNING 1
#define CONS_Alert(level, ...) CONS_Printf(__VA_ARGS__)
#define PS2_ZIPPNG 1
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
enum patchalphastyle { AST_COPY, AST_TRANSLUCENT, AST_ADD };
#include RETRY_POLICY
#include RETRY_TYPES
#ifdef NEXT7_RETRY_EE
_Static_assert(sizeof(void*)==4 && sizeof(size_t)==4 && sizeof(column_t)==12 && sizeof(post_t)==12,"actual EE32 ABI");
#include "retry_ee_data.h"
static const UINT8 *expected_bytes;
static size_t expected_position, expected_length;
static uint64_t verified_bytes;
#endif
static UINT8 *source[16];
static size_t source_size[16];
static unsigned source_count;
static FILE *snapshot_file;
static unsigned synthetic_mode;
static int active_kind, bad_id_mode;
static UINT16 numwadfiles=1;
static int source_format;
static patch_t *cached[16];
static UINT8 *cache_slot[2];
static column_t *column_slot[2];
static INT32 width_slot[2];
static size_t texturememory, flatmemory;
static unsigned final_calls, retry_allocs, retry_reads, normal_reads, converters;
static unsigned fail_final, fail_alloc, fail_read, change_source;
static int retry_phase, reader_ready = 1;
static uint64_t events;
static void *current_owner[4];
static unsigned char expected_live[4][1024];
typedef struct { size_t size; int compression; } lumpinfo_t;
enum { CM_NOCOMPRESSION, CM_LZ4, CM_DEFLATE };
typedef void wpack_t;
typedef struct { void *pool, *pack; FILE *handle; UINT16 numlumps; lumpinfo_t *lumpinfo; } wadfile_t;
static lumpinfo_t records[16];
static wadfile_t archive;
static wadfile_t *wadfiles[] = { &archive };
static void Patch_CalcDataSizes(softwarepatch_t *, size_t *, size_t *);
static void Patch_MakeColumns(softwarepatch_t *, size_t, INT16, UINT8 *, column_t *, post_t *, boolean);
#include RETRY_PATCH
static size_t W_LumpLengthPwad(UINT16 w, UINT16 l) { check(w<1 && wadfiles[w] && archive.lumpinfo && l<archive.numlumps,"original lump validation"); return source_size[l] + (source_format ? 8 : 0); }
static int bytes_read(UINT16 l, void *dest, size_t bytes, size_t offset, int retry)
{
    UINT8 *d = dest; size_t total = W_LumpLengthPwad(0,l);
    if (!bytes || bytes > total-offset) bytes=total-offset;
    if (retry && ++retry_reads == fail_read) return 0;
    for (size_t i=0;i<bytes;i++) {
        size_t p=offset+i;
        d[i]=source_format && p<8 ? (UINT8)"\211SRPIC\r\n"[p] : source[l][p-(source_format?8:0)];
    }
    if ((source_format == 2 || (retry && change_source == 1)) && bytes >= 8)
        memcpy(dest,"\211PNG\r\n\032\n",8);
    if (retry && change_source == 2 && bytes > PNG_HEADER_SIZE) memset(dest,0,bytes);
    return (int)bytes;
}
static size_t W_ReadLumpHeaderPwad(UINT16 w, UINT16 l, void *dest, size_t bytes, size_t offset)
{ check(w<1 && wadfiles[w] && archive.lumpinfo && l<archive.numlumps,"original lump validation"); normal_reads++; return (size_t)bytes_read(l,dest,bytes,offset,0); }
static boolean WPack_CanReadWithoutAlloc(FILE *f, const lumpinfo_t *l)
{ (void)f; return reader_ready && l && (l->compression==CM_NOCOMPRESSION || (l->compression==CM_LZ4 && l->size<=64u*65536u)); }
static size_t WPack_ReadLumpN(wpack_t *p, FILE *f, UINT32 l, const lumpinfo_t *r, void *dest, size_t bytes, size_t offset)
{ (void)p;(void)f;(void)r; return (size_t)bytes_read((UINT16)l,dest,bytes,offset,1); }
static void *W_CacheLumpNumPwad(UINT16 w, UINT16 l, INT32 tag)
{ size_t n=W_LumpLengthPwad(w,l); void *p=Z_Malloc(n,tag,NULL); W_ReadLumpHeaderPwad(w,l,p,n,0); return p; }
static void *W_TakeLumpNumPwad(UINT16 w,UINT16 l,INT32 tag,void **owner)
{ (void)w;(void)l;(void)tag;(void)owner; check(0,"unexpected flat transfer"); return NULL; }
static boolean Picture_IsLumpPNG(const UINT8 *p,size_t n)
{ return n>=8 && (!memcmp(p,"\211SRPIC\r\n",8)||!memcmp(p,"\211PNG\r\n\032\n",8)); }
static boolean Picture_IsLumpCooked(const UINT8 *p,size_t n)
{ return n>=16 && !memcmp(p,"\211SRPIC\r\n",8); }
static void *W_GetCachedPatchNumPwad(UINT16 w,UINT16 l)
{ (void)w; return cached[l]; }
static void *W_CachePatchNumPwad(UINT16 w,UINT16 l,INT32 tag)
{ (void)w; converters++; if(!cached[l]) { cached[l]=Patch_CreateFromDoomPatch((softwarepatch_t*)source[l]); Z_SetUser(cached[l],(void**)&cached[l]); } Z_ChangeTag(cached[l],tag); return cached[l]; }
static void *Picture_PNGConvert(const UINT8 *p,pictureformat_t fmt,INT32*w,INT32*h,INT16*t,INT16*l,size_t n,size_t*out,pictureflags_t fl)
{ (void)p;(void)fmt;(void)w;(void)h;(void)t;(void)l;(void)n;(void)out;(void)fl; converters++; return Patch_CreateFromDoomPatch((softwarepatch_t*)source[0]); }
static void *Picture_Convert(pictureformat_t in,void*p,pictureformat_t out,size_t n,size_t*os,INT32 w,INT32 h,INT32 l,INT32 t,pictureflags_t f)
{ (void)in;(void)p;(void)out;(void)n;(void)os;(void)w;(void)h;(void)l;(void)t;(void)f; converters++; return Patch_CreateFromDoomPatch((softwarepatch_t*)source[0]); }
static UINT8 ASTBlendPaletteIndexes(UINT8 bg,UINT8 fg,int style,UINT8 alpha)
{ return (UINT8)(bg*7u+fg*13u+(unsigned)style*3u+alpha); }
static INT32 ASTTextureBlendingThreshold[11]={0,25,50,75,100,125,150,175,200,225,255};
static void event(unsigned type,size_t n,INT32 tag,int owner)
{ events=(events^type)*1099511628211ull; events=(events^n)*1099511628211ull; events=(events^(unsigned)tag)*1099511628211ull; events=(events^(unsigned)owner)*1099511628211ull; }
static void *try_alloc(size_t n,INT32 tag,void*owner,INT32 align)
{ event(1,n,tag,owner!=NULL); if(retry_phase && ++retry_allocs==fail_alloc) return NULL; return Z_TryMallocAlign(n,tag,owner,align); }
static void *alloc(size_t n,INT32 tag,void*owner)
{ event(2,n,tag,owner!=NULL); check(!retry_phase || n<16384,"retry never uses unchecked large allocation"); return Z_Malloc(n,tag,owner); }
static void *zalloc(size_t n,INT32 tag,void*owner)
{ event(3,n,tag,owner!=NULL); if(retry_phase) check(n<16384,"retry never unchecked calloc work"); return Z_Calloc(n,tag,owner); }
static void *final_resize(void*p,size_t n,INT32 tag,void*owner,INT32 align)
{
    void *q; event(4,n,tag,owner!=NULL); final_calls++;check(final_calls<=2,"retry bound");
    if (fail_final && (final_calls==1 || fail_final==2)) q=NULL;
    else q=Z_TryReallocAlign(p,n,tag,owner,align);
    if(!q) { if((active_kind==6 || active_kind==7) && cached[0]) { Patch_Free(cached[0]); cached[0]=NULL; } retry_phase=1; if(change_source==3) reader_ready=0; if(change_source==4) archive.pool=NULL; if(change_source==5) records[0].compression=CM_DEFLATE; }
    return q;
}
#undef Z_Malloc
#undef Z_Calloc
#define Z_Malloc(s,t,u) alloc(s,t,u)
#define Z_Calloc(s,t,u) zalloc(s,t,u)
#define Z_TryMallocAlign(s,t,u,a) try_alloc(s,t,u,a)
#define Z_TryReallocAlign(p,s,t,u,a) final_resize(p,s,t,u,a)
#include RETRY_RENDERER
#if !defined(RETRY_COUNTED_PROFILE) && !defined(TEXTURE_255_IS_TRANSPARENT)
#error Missing actual default texture policy
#endif
#if !defined(RETRY_COUNTED_PROFILE) && defined(R_COUNTED_POSTS)
#error Counted-posts policy unexpectedly active in default fixture
#endif
#if !defined(PS2_NOOPT_TEXRETRY) && defined(RETRY_EXPECT_RETRY) && !defined(R_COMPOSITE_RETRY)
#error Default retry did not activate
#endif
#undef Z_TryMallocAlign
#undef Z_TryReallocAlign
#undef Z_Malloc
#undef Z_Calloc
#define Z_Malloc(s,t,u) Z_MallocAlign(s,t,u,0)
#define Z_Calloc(s,t,u) Z_CallocAlign(s,t,u,0)
static void load_sources(const char *dir)
{
#ifdef NEXT7_RETRY_EE
    (void)dir;source_count=EE_RESOURCE_COUNT;
    for(unsigned n=0;n<source_count;n++){source_size[n]=ee_source_sizes[n];source[n]=malloc(source_size[n]);check(source[n]!=NULL,"embedded source");memcpy(source[n],ee_sources[n],source_size[n]);}
#else
    char path[1024]; FILE*f; unsigned n;
    snprintf(path,sizeof path,"%s/resources.bin",dir);f=fopen(path,"rb");check(f!=NULL,"resource file");
    check(fread(&source_count,4,1,f)==1 && source_count<=16,"resource count");
    for(n=0;n<source_count;n++){uint32_t size;check(fread(&size,4,1,f)==1,"resource size");source_size[n]=size;source[n]=malloc(size);check(source[n]!=NULL,"resource storage");check(fread(source[n],1,size,f)==size,"resource bytes");}
    fclose(f);
#endif
}
static uint64_t mix(uint64_t h,size_t v){for(unsigned i=0;i<8;i++){h=(h^(UINT8)v)*1099511628211ull;v>>=8;}return h;}
static uint64_t snapshot(texture_t*t)
{
    uint64_t h=1469598103934665603ull; UINT8*p=cache_slot[1];
    h=mix(h,t->width);h=mix(h,t->height);h=mix(h,t->transparency);h=mix(h,t->flip);
    for(size_t i=0;i<(size_t)t->width*t->height;i++)h=(h^p[i])*1099511628211ull;
    for(INT32 x=0;x<t->width;x++){column_t*c=&column_slot[1][x];h=mix(h,c->num_posts);for(unsigned j=0;j<c->num_posts;j++){post_t*q=&c->posts[j];check(q->topdelta+q->length<=(unsigned)t->height,"post bounds");h=mix(h,q->topdelta);h=mix(h,q->length);h=mix(h,q->data_offset);}}
    return h;
}
static void write_bytes(const void *data,size_t bytes)
{
#ifdef NEXT7_RETRY_EE
    check(bytes<=expected_length-expected_position,"EE normalized expected bounds");
    check(!memcmp(expected_bytes+expected_position,data,bytes),"EE every normalized pixel/post byte exact");
    expected_position+=bytes;verified_bytes+=bytes;
#else
    check(fwrite(data,1,bytes,snapshot_file)==bytes,"complete byte snapshot");
#endif
}
static void write32(uint32_t v){UINT8 b[4]={(UINT8)v,(UINT8)(v>>8),(UINT8)(v>>16),(UINT8)(v>>24)};write_bytes(b,4);}
static void write_snapshot(texture_t*t,int fallback)
{
#ifdef NEXT7_RETRY_EE
    if(fallback)return;
    expected_bytes=ee_expected[active_kind];expected_length=ee_expected_sizes[active_kind];expected_position=0;
    check(expected_bytes!=NULL,"EE roomy reference exists");
#endif
    write32((uint32_t)fallback);write32((uint32_t)t->width);write32((uint32_t)t->height);
    if(fallback)return;
    write32((uint32_t)t->transparency);write32((uint32_t)t->flip);
    write_bytes(cache_slot[1],(size_t)t->width*t->height);
    for(INT32 x=0;x<t->width;x++){column_t*c=&column_slot[1][x];write32(c->num_posts);for(unsigned j=0;j<c->num_posts;j++){write32(c->posts[j].topdelta);write32(c->posts[j].length);write32((uint32_t)c->posts[j].data_offset);}}
#ifdef NEXT7_RETRY_EE
    check(expected_position==expected_length,"EE complete normalized reference consumed");
#endif
}
static void make_synthetic(unsigned mode)
{
    unsigned w=mode==13?3:64,h=mode==13?5:128;size_t off=8+4*w;
    synthetic_mode=mode;source_count=1;source[0]=malloc(70000);check(source[0]!=NULL,"synthetic buffer");memset(source[0],0,70000);
    INT16 wi=(INT16)w,hi=(INT16)h;memcpy(source[0],&wi,2);memcpy(source[0]+2,&hi,2);
    for(unsigned x=0;x<w;x++){
        uint32_t pos=(uint32_t)off;memcpy(source[0]+8+4*x,&pos,4);
        if(mode==10){source[0][off++]=0;source[0][off++]=(UINT8)h;source[0][off++]=0;for(unsigned y=0;y<h;y++)source[0][off++]=(UINT8)(x*17+y);source[0][off++]=0;}
        else if(mode!=11){for(unsigned y=x%2;y<h;y+=2){source[0][off++]=(UINT8)y;source[0][off++]=1;source[0][off++]=0;source[0][off++]=(UINT8)(x*17+y);source[0][off++]=0;}}
        source[0][off++]=255;
    }source_size[0]=off;
}
static int run_case(int kind,unsigned fail,unsigned afail,unsigned rfail,unsigned change)
{
    texture_t*t,*table[2];unsigned i;uint64_t h;int fallback;zastats_t st;void *fragment_barrier=NULL,*tail_pin=NULL;
    active_kind=kind;arena_reset(kind==5?800000:8u<<20,ship_sides,ship_prefer,1);
    final_calls=retry_allocs=retry_reads=normal_reads=converters=0;retry_phase=0;reader_ready=1;
    fail_final=fail;fail_alloc=afail;fail_read=rfail;change_source=change;events=1469598103934665603ull;texturememory=0;
    memset(cached,0,sizeof cached);memset(cache_slot,0,sizeof cache_slot);memset(column_slot,0,sizeof column_slot);
    source_format=(kind==1||kind==7)?1:kind==2?2:0;
    archive.pool=archive.pack=&archive;archive.handle=(FILE*)&archive;archive.lumpinfo=records;archive.numlumps=(UINT16)source_count;wadfiles[0]=&archive;
    for(i=0;i<source_count;i++){records[i].size=W_LumpLengthPwad(0,(UINT16)i);records[i].compression=CM_LZ4;}
    archive.pool=archive.pack=&archive;archive.handle=(FILE*)&archive;archive.lumpinfo=records;archive.numlumps=(UINT16)source_count;wadfiles[0]=&archive;
    t=calloc(1,sizeof(*t)+12*sizeof(texpatch_t));check(t!=NULL,"texture descriptor");
    t->type=kind==3?TEXTURETYPE_FLAT:TEXTURETYPE_COMPOSITE;t->width=256;t->height=2048;t->patchcount=12;
    if(kind>=10){t->width=kind==13?3:65;t->height=kind==13?5:129;t->patchcount=kind==12?2:3;}
    for(i=0;i<12;i++){t->patches[i].lump=i<8?(UINT16)i:8;t->patches[i].originx=i<8?0:(INT16)((i-8)*64);t->patches[i].originy=i<8?(INT16)(i*256):0;t->patches[i].style=kind==4?AST_TRANSLUCENT:AST_COPY;t->patches[i].alpha=128;}
    if(kind>=10)for(i=0;i<(unsigned)t->patchcount;i++){t->patches[i].lump=0;t->patches[i].originx=t->patches[i].originy=(INT16)((int)i-1);t->patches[i].flip=(UINT8)i;}
    if(kind==12)for(i=0;i<2;i++){t->patches[i].originx=t->patches[i].originy=0;t->patches[i].flip=0;}
    table[0]=table[1]=t;textures=table;texturecache=cache_slot;texturecolumns=column_slot;texturewidth=width_slot;numtextures=2;width_slot[1]=t->width;
    if(bad_id_mode==1)t->patches[0].wad=1;
    if(bad_id_mode==2)t->patches[0].lump=(UINT16)source_count;
    if(bad_id_mode==3)archive.lumpinfo=NULL;
    if(bad_id_mode==4)wadfiles[0]=NULL;
    Z_PurgeLock(true);
    if(kind==6||kind==7){cached[0]=Patch_CreateFromDoomPatch((softwarepatch_t*)source[0]);Z_SetUser(cached[0],(void**)&cached[0]);Z_ChangeTag(cached[0],PU_PATCH);}
    for(i=0;i<4;i++){current_owner[i]=Z_Malloc(1024,PU_CACHE,&current_owner[i]);memset(current_owner[i],(int)(i+53),1024);memcpy(expected_live[i],current_owner[i],1024);}
    if(kind==5) {
        size_t final_size=256u*2048u+256u*sizeof(column_t)+256u*sizeof(post_t);
        void*a=Z_MallocAlign(final_size,PU_LEVEL,NULL,6);
        fragment_barrier=Z_MallocAlign(128,PU_LEVEL,NULL,6);memset(fragment_barrier,0x6d,128);
        void*b=Z_MallocAlign(170000,PU_LEVEL,NULL,6);
        ZA_Stats(&st);tail_pin=Z_MallocAlign(st.freebytes-ZA_HDR-256,PU_LEVEL,NULL,6);
        Z_Free(a);Z_Free(b);consistent();
    }
    cons_clear(); r_fallbacktextures=0; R_GenerateTexture(1); consistent(); ZA_Stats(&st);
    fallback=r_fallbacktextures!=0;
    check(zpurgelock==1 && !zpinned && !ztry_realloc,"lock/pinning state restored");
    for(i=0;i<4;i++){check(current_owner[i] && !memcmp(current_owner[i],expected_live[i],1024),"live current-frame cache intact");}
    if(!fallback){h=snapshot(t);check(final_calls<=2,"at most one complete retry");}
    else h=0;
    write_snapshot(t,fallback);
    for(zablock_t*b=ZA_First();b;b=ZA_Next(b))check(ZA_ISFREE(b)||ZA_TAG(b)!=PU_RENDERWORK,"all unpublished work freed");
    printf("RETRY case=%d fail=%u afail=%u rfail=%u change=%u fallback=%d finals=%u allocs=%u reads=%u normalreads=%u converters=%u bytes=%zu hash=%016llx events=%016llx\n",kind,fail,afail,rfail,change,fallback,final_calls,retry_allocs,retry_reads,normal_reads,converters,texturememory,(unsigned long long)h,(unsigned long long)events);
    if(fragment_barrier){for(i=0;i<128;i++)check(((UINT8*)fragment_barrier)[i]==0x6d,"fragmentation barrier intact");}
    Z_PurgeLock(false);Z_Free(cache_slot[1]);Z_Free(fragment_barrier);Z_Free(tail_pin);for(i=0;i<source_count;i++)if(cached[i])Patch_Free(cached[i]);for(i=0;i<4;i++)Z_Free(current_owner[i]);empty_zone();free(t);
    return fallback;
}
int main(int argc,char**argv)
{
    setvbuf(stdout,NULL,_IONBF,0);
#ifdef NEXT7_RETRY_EE
    (void)argc;(void)argv;load_sources("embedded");
#else
    check(argc==3||argc==4,"usage fixture-dir snapshots [invalid-id]");if(argc==4)bad_id_mode=atoi(argv[3]);snapshot_file=fopen(argv[2],"wb");check(snapshot_file!=NULL,"snapshot output");load_sources(argv[1]);
#endif
    ship_sides=za_twosided;ship_prefer=za_prefer;
    if(bad_id_mode){run_case(0,0,0,0,0);check(0,"invalid source accepted");}
    for(int kind=0;kind<5;kind++){run_case(kind,0,0,0,0);run_case(kind,1,0,0,0);}
    run_case(5,0,0,0,0);
    for(int kind=6;kind<=7;kind++){run_case(kind,0,0,0,0);run_case(kind,1,0,0,0);}
    run_case(0,2,0,0,0);
    for(unsigned i=1;i<=18;i++)run_case(0,1,i,0,0);
    for(unsigned i=1;i<=24;i++)run_case(0,1,0,i,0);
    for(unsigned i=1;i<=5;i++)run_case(0,1,0,0,i);
    for(unsigned i=0;i<source_count;i++)free(source[i]);
    for(unsigned mode=10;mode<=13;mode++){
        make_synthetic(mode);run_case((int)mode,0,0,0,0);run_case((int)mode,1,0,0,0);run_case((int)mode,2,0,0,0);
        for(unsigned i=1;i<=9;i++)run_case((int)mode,1,i,0,0);
        for(unsigned i=1;i<=6;i++)run_case((int)mode,1,0,i,0);
        free(source[0]);
    }
    #ifdef NEXT7_RETRY_EE
    printf("RETRY DONE verified_bytes=%llu columns=%u posts=%u checks=%u\n",(unsigned long long)verified_bytes,(unsigned)sizeof(column_t),(unsigned)sizeof(post_t),checks);
#else
    check(fclose(snapshot_file)==0,"snapshot close");puts("RETRY DONE");
#endif
    return 0;
}
