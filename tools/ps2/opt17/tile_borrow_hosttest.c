/* Real V9 allocator/converter/cache through the maintained GS packet recorder. */
#define main capture_fixture_main_unused
#include "tile_capture_hosttest.c"
#undef main

static unsigned long long borrow_checks, compared_bytes, conversion_cases;
static unsigned cases64, cases128, wide_indexed_cases, wide_screen_cases;
static void bcheck(int condition, const char *why)
{
    borrow_checks++;
    if (!condition) { fprintf(stderr,"BORROW FAIL %s (check %llu)\n",why,borrow_checks); exit(1); }
}

/* Every pool block is in exactly one free interval or one real owning record. */
static void coverage(void)
{
    u8 seen[VRAM_BLOCKS]={0};u32 owned=0,last=H.pool_base;
    bcheck(H.free_n>=0&&H.free_n<=VRAM_RANGES,"free-list entry capacity");
    for(int i=0;i<H.free_n;i++)
    {
        u32 s=H.free_range[i].start,n=H.free_range[i].len;
        bcheck(n&&s>=H.pool_base&&s+n<=H.pool_base+H.pool_blocks,"free interval inside pool");
        bcheck(s>=last&&(!i||s>last),"strict sorted disjoint coalesced free intervals");
        last=s+n;
        for(u32 b=s;b<last;b++) { bcheck(!seen[b]&&!blk_owner[b],"free interval has no owner");seen[b]=1; }
    }
    for(int i=0;i<H.rec_n;i++)if(H.rec[i].used&&H.rec[i].nblk&&H.rec[i].share==NOREC)
    {
        const texrec_t *r=&H.rec[i];
        bcheck(r->blk>=H.pool_base&&(u32)r->blk+r->nblk<=H.pool_base+H.pool_blocks,"used image inside pool");
        for(u32 b=r->blk;b<(u32)r->blk+r->nblk;b++)
        { bcheck(!seen[b]&&blk_owner[b]==i+1,"used owner identity");seen[b]=1;owned++; }
    }
    for(u32 b=H.pool_base;b<H.pool_base+H.pool_blocks;b++)bcheck(seen[b]==1,"complete disjoint pool coverage");
    bcheck(owned==H.used_blocks,"physical used-block accounting");
}

static int image_record(u32 blk,u32 blocks,int pin)
{
    int ri=rec_new();bcheck(ri!=NOREC,"image record allocation");
    H.rec[ri].blk=(u16)blk;H.rec[ri].nblk=(u16)blocks;H.rec[ri].pin=(u8)pin;
    own_set(ri);lru_push_head(ri);return ri;
}

static void allocator_cases(void)
{
    native_init(640,512,1);memset(&TX,0,sizeof TX);
    bcheck(H.pool_blocks==512,"PAL physical pool512");
    bcheck(tile_cache_get()!=NOREC,"create permanent64 image");
    const u32 anchor=H.rec[tile_cache_rec].blk,failed=TX.alloc_fail,evicted=H.st.evictions;
    bcheck(H.used_blocks==64&&tex_usable_blocks()==448,"permanent capacity448");coverage();
    tile_cache.valid=1;bcheck(tile_cache_try_borrow(),"claim adjacent free prefix");
    bcheck(H.rec[tile_cache_rec].blk==anchor-64&&H.rec[tile_cache_rec].nblk==128,"exact borrowed prefix");
    bcheck(tile_cache_borrowed_blocks()==64&&H.used_blocks==128&&tex_usable_blocks()==448&&!tile_cache.valid,"actual versus planning capacity");
    bcheck(tile_cache_try_borrow()&&H.used_blocks==128,"already borrowed means available without double claim");coverage();
    const u32 allocation=vram_alloc_evicting(448,PAGE_BLOCKS,0);
    bcheck(allocation==H.pool_base,"ordinary448 succeeds after actual allocator retry hook");
    int ri=image_record(allocation,448,1);
    bcheck(TX.alloc_fail==failed&&H.st.evictions==evicted,"prefix return precedes accounting and victims");
    bcheck(!tile_cache_borrowed_blocks()&&H.rec[tile_cache_rec].blk==anchor&&H.used_blocks==512,"reclaim preserves permanent anchor");coverage();
    bcheck(!tile_cache_try_borrow()&&H.rec[ri].used,"occupied prefix is never stolen");
    bcheck(TX.alloc_fail==failed&&H.st.evictions==evicted,"optional failure has no victim side effects");
    bcheck(!tile_cache_return_borrow(),"return without borrow is unchanged");
    tex_drop(ri,0);bcheck(tile_cache_try_borrow(),"exact64 prefix may be reclaimed again");coverage();
    tile_cache.valid=1;bcheck(tile_cache_return_borrow()&&!tile_cache.valid,"return changes and invalidates");coverage();
    bcheck(!tile_cache_return_borrow(),"return is idempotent");tex_free_all();

    native_init(640,512,1);bcheck(tile_cache_get()!=NOREC,"exact-range cache");
    u32 exact=vram_alloc(384,PAGE_BLOCKS);bcheck(exact!=0,"isolate an exact64 prefix interval");
    image_record(exact,384,1);bcheck(H.free_n==1&&H.free_range[0].len==64,"exact64 free-list witness");
    bcheck(tile_cache_try_borrow()&&!H.free_n,"claim removes exact64 free interval");coverage();
    bcheck(tile_cache_return_borrow()&&H.free_n==1&&H.free_range[0].len==64,"return inserts exact64 interval");coverage();tex_free_all();

    /* A different large hole is irrelevant when even one adjacent prefix block is used. */
    native_init(640,512,1);bcheck(tile_cache_get()!=NOREC,"create blocked-prefix cache");
    u32 b=vram_alloc_hi(32,PAGE_BLOCKS);bcheck(b!=0,"occupy adjacent32");
    ri=image_record(b,32,1);const u32 used=H.used_blocks;
    bcheck(!tile_cache_try_borrow()&&H.used_blocks==used&&H.rec[ri].used,"cannot borrow another hole");coverage();tex_free_all();
    printf("BORROW ALLOCATOR PAL448 retry=PASS owner_coverage=PASS no_eviction=PASS\n");
}

static void fragmented_case(void)
{
    /* Largest nonzero page-aligned physical pool; both sides of an aligned
       128-block image have even length, so alternating cells attain 8112. */
    native_init(640,512,1);H.pool_base=PAGE_BLOCKS;H.pool_blocks=VRAM_BLOCKS-PAGE_BLOCKS;
    H.free_n=0;H.used_blocks=0;memset(blk_owner,0,sizeof blk_owner);
    const u32 prefix=8192,anchor=prefix+64,end=prefix+128;
    tile_cache_rec=image_record(prefix,128,1);H.rec[tile_cache_rec].tiled=2;H.used_blocks=128;
    for(u32 b=H.pool_base;b<VRAM_BLOCKS;b++)
    {
        if(b>=prefix&&b<end)continue;
        if(!(b&1)) { H.free_range[H.free_n].start=(u16)b;H.free_range[H.free_n].len=1;H.free_n++; }
        else { image_record(b,1,1);H.used_blocks++; }
    }
    bcheck(H.free_n==8112,"highest valid aligned fragmentation witness");coverage();
    bcheck(H.free_n+1<VRAM_RANGES,"physical interval bound leaves return capacity");
    bcheck(tile_cache_return_borrow(),"return under maximal valid fragmentation");
    bcheck(H.free_n==8113&&H.rec[tile_cache_rec].blk==anchor,"return adds one valid disjoint interval");coverage();
    printf("BORROW FRAGMENTATION pool=[32,16384) before=8112 after=8113 capacity=8193 global_bound=8130+1\n");
    tex_free_all();
}

static int oracle_coord(int p,int n,int wrap)
{
    if(wrap){int v=p%n;return v<0?v+n:v;}return p<0?0:p>=n?n-1:p;
}
static int oracle_axis(int p,int t,int size,int region)
{
    if(!region)return p+t-1;
    /* Independent floor-remainder expression of the original texture window. */
    int v=(t-p+size/4)%size;if(v<0)v+=size;
    return p+v-size/4;
}
static u32 oracle_pixel(const tile_fill_t *c,int sx,int sy)
{
    const texrec_t *r=&c->source;const GLMipmap_t *m=r->owner;
    if(r->screen)return H.fb32?((const u32 *)(const void *)c->data)[(size_t)sy*H.fbw+sx]:((const u16 *)(const void *)c->data)[(size_t)sy*H.fbw+sx];
    size_t at=(size_t)sy*m->width+sx;
    switch(m->format)
    {
        case GL_TEXFMT_P_8:case GL_TEXFMT_ALPHA_8:return c->data[at];
        case GL_TEXFMT_AP_88:
        {
            u32 v=HWR_PS2_ReadAP88(m,(u32)sx,(u32)sy);
            if(r->psm==PSM_T8)return !(m->flags&TF_CHROMAKEYED)&&!(v>>8)?255:v&255;
            return H.pal[v&255]|(((v>>8)+1)/2<<24);
        }
        case GL_TEXFMT_RGBA:
        {
            u32 v=((const u32 *)(const void *)c->data)[at];
            if(r->psm==PSM_T8){if(!(v>>24))return 255;for(int k=0;k<256;k++)if(r->tile_palette[k]==(v&0xffffff))return(u32)k;return 255;}
            return(v&0xffffff)|(((v>>24)+1)/2<<24);
        }
        case GL_TEXFMT_ALPHA_INTENSITY_88:return c->data[2*at]*0x010101u|(((u32)c->data[2*at+1]+1)/2<<24);
        default:return 0;
    }
}

static void fill_oracle(const tile_fill_t *c,int w,int h,int bpp,u8 *dst)
{
    for(int y=0;y<h;y++)for(int x=0;x<w;x++)
    {
        int sx=oracle_coord(oracle_axis(c->x,x,w,c->region),c->source.w,c->wrapx);
        int sy=oracle_coord(oracle_axis(c->y,y,h,c->region),c->source.h,c->wrapy);
        u32 v=oracle_pixel(c,sx,sy);memcpy(dst+((size_t)y*w+x)*bpp,&v,(size_t)bpp);
    }
}

static void conversion_matrix(void)
{
    const int widths[]={1,2,31,32,63,64,65,127,128,129,1024};
    u8 *data=malloc(1031u*137u*4u),*got=malloc(131072),*want=malloc(131072);u32 palette[256];
    bcheck(data&&got&&want,"converter allocations");native_init(640,512,1);
    for(int k=0;k<256;k++)palette[k]=(u32)k|((u32)(255-k)<<8)|((u32)(k^117)<<16);
    memcpy(H.pal,palette,sizeof palette);
    for(int mode=0;mode<12;mode++)for(size_t wi=0;wi<sizeof widths/sizeof *widths;wi++)
    {
        GLMipmap_t m={0};texrec_t source={0};tile_fill_t c={0};
        for(size_t p=0;p<1031u*137u*4u;p++)data[p]=(u8)(p*73u+p/1031u+mode*17u);
        m.width=(u16)widths[wi];m.height=131;m.data=data;m.flags=(mode&1)?TF_CHROMAKEYED:0;
        m.format=mode==0?GL_TEXFMT_P_8:mode==1?GL_TEXFMT_ALPHA_8:mode<6?GL_TEXFMT_AP_88:mode<8?GL_TEXFMT_RGBA:GL_TEXFMT_ALPHA_INTENSITY_88;
        source.w=m.width;source.h=m.height;source.owner=&m;source.psm=mode==0||mode==1||mode==2||mode==4||mode==6?PSM_T8:PSM_CT32;
        if(mode>=10){source.screen=1;source.psm=mode==10?PSM_CT32:PSM_CT16S;H.fb32=mode==10;H.fbw=m.width+5;}
        source.tile_palette=palette;
        if(mode==4||mode==5){m.ps2_cpu=HWR_PS2_CPU_PATCH;m.ps2_uw=m.width>3?m.width-3:m.width;m.ps2_uh=126;}
        if(mode==6)for(size_t p=0;p<1031u*137u;p++)((u32 *)(void *)data)[p]=palette[(p*17u+p/131u)&255u]|(p%7?0xff000000u:0);
        c.source=source;c.data=data;
        for(int layout=0;layout<3;layout++)
        {
            c.region=layout==2?1|TILE_REGION_WIDE:layout;int w,h;
            if(!tile_layout_dimensions(&source,c.region,&w,&h))continue;
            const int bpp=source.psm==PSM_T8?1:source.screen&&!H.fb32?2:4;
            for(int wrap=0;wrap<4;wrap++)for(int origin=0;origin<8;origin++)
            {
                c.wrapx=wrap&1;c.wrapy=wrap&2;c.x=origin<3?origin*32-17:origin<6?source.w+origin*17:origin==6?-1000000:1000000;
                c.y=origin<3?origin*33-17:origin<6?source.h+origin*17:origin==6?-1000000:1000000;
                tile_fill_columns(&c);bcheck(c.run_count<=4,"compact run descriptor bound");
                if((mode==0||mode==1)&&source.w==2&&c.wrapx)bcheck(!c.run_count,"narrow wrapped source scalar fallback");
                if((mode==0||mode==1)&&source.w==1)bcheck(c.run_count==1,"constant row one descriptor");
                memset(got,0xA5,131072);memset(want,0xA5,131072);fill_oracle(&c,w,h,bpp,want);
                for(int y=0;y<h;y++)tile_fill(&c,(u32)y,got+(size_t)y*w*bpp);
                bcheck(!memcmp(got,want,131072),"per-axis scalar byte oracle including guard bytes");
                compared_bytes+=(unsigned long long)w*h*bpp;conversion_cases++;
                if(layout==2){if(source.screen)wide_screen_cases++;else wide_indexed_cases++;}
                else if(w==64)cases64++;else cases128++;
            }
        }
    }
    free(want);free(got);free(data);tex_free_all();
    printf("BORROW CONVERT cases=%llu bytes=%llu legacy64=%u v8_128=%u wideT8=%u wideCT32=%u real_packed_accessor=1\n",conversion_cases,compared_bytes,cases64,cases128,wide_indexed_cases,wide_screen_cases);
}

static void loader_cases(void)
{
    u8 *data=malloc(1024u*512u*4u),*expected=malloc(32768);bcheck(data&&expected,"loader source allocation");
    for(int screen=0;screen<2;screen++)
    {
        native_init(640,512,1);bcheck(tile_cache_get()!=NOREC,"loader permanent image");u32 anchor=H.rec[tile_cache_rec].blk;
        GLMipmap_t m={0};texrec_t source={0},out;tile_fill_t context={0};m.width=1024;m.height=512;m.format=GL_TEXFMT_P_8;m.data=data;
        for(size_t p=0;p<1024u*512u*4u;p++)data[p]=(u8)(p*53u+p/1024u);
        source.owner=screen?NULL:&m;source.screen=(u8)screen;source.w=1024;source.h=512;source.psm=screen?PSM_CT32:PSM_T8;
        if(screen){H.fbw=1024;H.scr_data[0]=data;}
        bcheck(!tile_load_wide(&source,80,100,1,1,&out),"wide requires claimed prefix");
        bcheck(tile_cache_try_borrow(),"loader claims prefix");borrow_refs=0;
        const int layouts[]={1,5,5,1,1,0,5};unsigned expected_uploads=0;
        for(unsigned step=0;step<sizeof layouts/sizeof *layouts;step++)
        {
            int layout=layouts[step],w,h;tile_layout_dimensions(&source,layout,&w,&h);
            cap_reset();unsigned uploads=H.st.uploads;
            bcheck(tile_load_impl(&source,80,100,1,1,layout,&out),"actual load succeeds");
            const int hit=step==2||step==4;expected_uploads+=!hit;
            bcheck(H.st.uploads==uploads+(unsigned)!hit,"shape transition upload");
            bcheck(out.blk==(layout==5?anchor-64:anchor),"ordinary load keeps permanent anchor");
            bcheck(out.nblk==(layout==5?128:64)&&out.w==w&&out.h==h&&out.tbw==(source.psm==PSM_T8?2:w/64),"non-square GS output descriptor");
            bcheck(out.tw==log2_u32((u32)w)&&out.th==log2_u32((u32)h),"non-square GS exponent descriptor");
            bcheck(!borrow_refs,"upload has no source DMA reference");
            if(!hit)
            {
                context.source=source;context.data=data;context.x=80;context.y=100;context.wrapx=context.wrapy=1;context.region=layout;
                int bpp=screen?4:1;fill_oracle(&context,w,h,bpp,expected);pkt_t p;size_t copied=0;
                for(u32 at=0;at<cap_n&&pkt_at(at,&p);at+=1+p.size)if(p.flg==2)
                {size_t n=p.size*16u;bcheck(!memcmp(&cap[at+1],expected+copied,n),"real copied upload payload matches scalar");copied+=n;}
                bcheck(copied==(size_t)w*h*bpp,"shape upload exact byte count");
                qw_t *saved=malloc((size_t)cap_n*sizeof *saved);bcheck(saved!=NULL,"copied packet snapshot");memcpy(saved,cap,(size_t)cap_n*sizeof *saved);
                data[0]^=0xff;bcheck(!memcmp(saved,cap,(size_t)cap_n*sizeof *saved),"queued copied payload independent of source mutation");data[0]^=0xff;free(saved);
            }
        }
        bcheck(H.st.uploads==expected_uploads,"all layout hit and miss accounting");coverage();
        tile_cache.valid=1;bcheck(tile_cache_return_borrow()&&!tile_cache.valid,"shrink invalidates shape cache");
        bcheck(!tile_load_wide(&source,80,100,1,1,&out),"wide fails after prefix reclaimed");
        if(screen)H.scr_data[0]=NULL;tex_free_all();
    }
    {texrec_t r={0};int w,h;r.screen=1;r.psm=PSM_CT16S;bcheck(!tile_wide_dimensions(&r,&w,&h)&&!w&&!h,"CT16 wide scope rejected");}
    free(expected);free(data);printf("BORROW LOADER shapes/cache/anchor/copied_bytes/no_refs=PASS\n");
}

int main(void)
{
    cap=calloc(CAP_MAX,sizeof *cap);bcheck(cap!=NULL,"GS packet recorder");
    allocator_cases();fragmented_case();conversion_matrix();loader_cases();
    printf("BORROW COMPLETE checks=%llu cases=%llu bytes=%llu failed=0\n",borrow_checks,conversion_cases,compared_bytes);return 0;
}
