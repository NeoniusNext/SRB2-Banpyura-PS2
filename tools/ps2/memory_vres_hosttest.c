/* Run the actual virtual-map loader in the real PS2 zone, with resource I/O stubbed. */
#define main zone_suite_main
#include "zone_hosttest.c"
#undef main
typedef uint8_t UINT8;
typedef uint16_t UINT16;
typedef UINT32 lumpnum_t;
typedef struct { char identification[4]; INT32 numlumps, infotableofs; } wadinfo_t;
typedef struct { INT32 filepos, size; char name[8]; } filelump_t;
typedef struct { char name[9]; UINT8 *data; size_t size, filepos; } virtlump_t;
typedef struct { size_t numlumps; virtlump_t *vlumps; lumpnum_t wadlump; } virtres_t;
#define WADFILENUM(n) ((n) >> 16)
#define LUMPNUM(n) (n)
#define LONG(n) (n)
static unsigned char resource[900000];
static size_t resource_size, largest_read;
static volatile boolean embedded = true;
static boolean W_IsLumpWad(lumpnum_t n) { (void)n; return embedded; }
static size_t W_LumpLength(lumpnum_t n) { (void)n; return resource_size; }
static size_t W_ReadLumpHeader(lumpnum_t n, void *dest, size_t size, size_t offset)
{
    (void)n;
    check(offset <= resource_size && size <= resource_size - offset, "map partial read stays inside resource");
    if (size > largest_read) largest_read = size;
    memcpy(dest, resource + offset, size);
    return size;
}
static const char *W_CheckNameForNum(lumpnum_t n) { (void)n; return "MAP01"; }
static void *W_CacheLumpNum(lumpnum_t n, INT32 tag) { (void)n; (void)tag; check(!embedded, "nested WAD must never be fully cached"); return NULL; }
/* The unexecuted sibling-lump branch must still compile against a correctly typed table. */
#define wadfiles ((struct memory_wad_s **)wad_table)
struct memory_wad_s { UINT32 numlumps; };
static struct memory_wad_s *wad_table[1];
#include MEMORY_VRES_SOURCE

int main(void)
{
    wadinfo_t header = {{'P','W','A','D'}, 3, 800000};
    filelump_t entries[3] = {
        {13, 200000, {'V','E','R','T','E','X','E','S'}},
        {200013, 250000, {'L','I','N','E','D','E','F','S'}}, {450013, 0, "EMPTY"}};
    virtres_t *vres;
    zastats_t st;
    size_t i;
    (void)wadfiles;
    ship_sides = za_twosided; ship_prefer = za_prefer;
    arena_reset(600000, ship_sides, ship_prefer, 1);
    resource_size = sizeof resource;
    for (i = 0; i < sizeof resource; i++) resource[i] = (unsigned char)(i * 13);
    memcpy(resource, &header, sizeof header);
    memcpy(resource + header.infotableofs, entries, sizeof entries);
    vres = vres_GetMap(0);
    check(vres->numlumps == 3, "nested directory count");
    ZA_Stats(&st);
    check(st.globalpeak < 1000 && largest_read == sizeof entries, "directory-only load leaves payloads unread");
    for (i = 0; i < 3; i++)
    {
        check(vres->vlumps[i].size == (size_t)entries[i].size, "subresource size intact");
        check(!memcmp(vres->vlumps[i].name, entries[i].name, 8), "subresource name intact");
        check(vres->vlumps[i].data == NULL, "sublump read deferred until use");
        (void)vres_Data(vres, &vres->vlumps[i]);
        check(((uintptr_t)vres->vlumps[i].data & 15) == 0, "unaligned WAD offsets produce aligned zone payloads");
        if (entries[i].size)
            check(!memcmp(vres->vlumps[i].data, resource + entries[i].filepos, (size_t)entries[i].size), "subresource bytes intact");
        vres_Drop(&vres->vlumps[i]);
        check(vres->vlumps[i].data == NULL, "sublump dropped after conversion");
    }
    ZA_Stats(&st);
    check(st.globalpeak < 251000 && largest_read == 250000, "map peak contains only largest live sublump");
    consistent(); vres_Free(vres); empty_zone();
    entries[0].filepos = 899999;
    memcpy(resource + header.infotableofs, entries, sizeof entries);
    expecting_error = 1;
    if (!setjmp(error_jump)) { (void)vres_GetMap(0); check(0, "out-of-range sublump rejected"); }
    expecting_error = 0;
    check(strstr(error_text, "outside WAD") != NULL, "malformed map has useful bounds error");
    empty_zone();
    header.numlumps = INT32_MAX;
    memcpy(resource, &header, sizeof header);
    expecting_error = 1;
    if (!setjmp(error_jump)) { (void)vres_GetMap(0); check(0, "overflowing directory rejected"); }
    expecting_error = 0;
    check(strstr(error_text, "invalid map WAD directory") != NULL, "directory overflow refused before allocation");
    printf("PASS nested WAD lazy partial reads, exact/aligned payloads, <251000-byte peak in 600KB zone, bounds errors\n");
    return 0;
}
