/* Deterministic Zone/engine service boundary, tracking live owner cells/tags. */
static unsigned long long fallback_checks;
static unsigned fixture_cases, fixture_regens, fixture_fail_regen, fixture_stream;
static unsigned fixture_want, fixture_rows, fixture_errors;
static const unsigned char *fixture_source;
static struct { void *p; void **owner; size_t bytes; int locked; } fixture_blocks[64];
static unsigned fixture_blocks_n, fixture_locks, fixture_unlocks;
static unsigned fixture_realloc_fail, fixture_realloc_calls;
static void fixture_check(int yes, const char *message)
{
    fallback_checks++;
    if (!yes) { fprintf(stderr,"VRAM FALLBACK FAIL case=%u: %s\n",fixture_cases,message);exit(1); }
}
static int fixture_block(void *p)
{
    for (unsigned i=0;i<fixture_blocks_n;i++)if(fixture_blocks[i].p==p)return (int)i;
    fixture_check(0,"live Zone block identity");return -1;
}
static void *fixture_allocate(size_t bytes,void **owner)
{
    void *p=malloc(bytes);fixture_check(p!=NULL,"engine Zone allocation");
    fixture_check(fixture_blocks_n<64,"bounded Zone block table");
    fixture_blocks[fixture_blocks_n++]=(typeof(fixture_blocks[0])){p,owner,bytes,0};
    if(owner)*owner=p;
    return p;
}
static void *fixture_realloc(void *p,size_t bytes)
{
    fixture_realloc_calls++;
    return fixture_realloc_fail?NULL:realloc(p,bytes);
}
#define realloc fixture_realloc
void HWR_PS2_LockData(void *p)
{
    fixture_blocks[fixture_block(p)].locked=1;fixture_locks++;
}
void HWR_PS2_UnlockData(void *p)
{
    fixture_blocks[fixture_block(p)].locked=0;fixture_unlocks++;
}
void HWR_PS2_FreeData(void *p)
{
    int i=fixture_block(p);
    if(fixture_blocks[i].owner&&*fixture_blocks[i].owner==p)*fixture_blocks[i].owner=NULL;
    fixture_blocks[i].p=NULL;free(p);
}
void *HWR_PS2_AllocData(size_t bytes,void **owner){return fixture_allocate(bytes,owner);}
void *HWR_PS2_StealData(GLMipmap_t *m,void **owner)
{
    void *p=m->data;int i=fixture_block(p);m->data=NULL;*owner=p;fixture_blocks[i].owner=owner;return p;
}
void HWR_PS2_RegenerateMipmap(GLMipmap_t *m)
{
    fixture_regens++;
    if(m->data||fixture_fail_regen)return;
    fixture_allocate((size_t)m->width*m->height,&m->data);
    memcpy(m->data,fixture_source,(size_t)m->width*m->height);
}
boolean HWR_PS2_FlatStreamable(const GLMipmap_t *m){return fixture_stream&&m->regen_kind==2;}
boolean HWR_PS2_FlatRows(const GLMipmap_t *m,UINT32 row,UINT32 rows,UINT8 *dst)
{
    fixture_check(row+rows<=m->height,"raw-flat row bounds");
    memcpy(dst,fixture_source+(size_t)row*m->width,(size_t)rows*m->width);fixture_rows++;return 1;
}
const UINT8 *HWR_PS2_FlatPin(const GLMipmap_t *m){(void)m;return NULL;}
void HWR_PS2_FlatUnpin(const UINT8 *p,size_t n){(void)p;(void)n;}
