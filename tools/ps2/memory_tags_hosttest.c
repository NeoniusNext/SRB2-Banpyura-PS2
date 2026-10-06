/* Exercise production taglist operations with every signed 16-bit tag and the real EE zone. */
#define main zone_suite_main
#include "zone_hosttest.c"
#undef main
typedef int16_t INT16;
typedef uint16_t UINT16;
typedef uint8_t bitarray_t;
#include "taglist.h"
#include MEMORY_TAGS_SOURCE

static void hash_list(uint64_t *hash, const taglist_t *list)
{
    *hash ^= list->count; *hash *= 1099511628211ull;
    for (size_t i = 0; i < list->count; i++)
    {
        *hash ^= (UINT16)list->tags[i]; *hash *= 1099511628211ull;
    }
}

int main(void)
{
    taglist_t *lists;
    taglist_t multi = {0}, equal = {0}, singleton = {0};
    zastats_t st;
    uint64_t hash = 1469598103934665603ull;
    size_t i;
    check(sizeof(taglist_t) == 8, "EE taglist ABI/containing map structure sizes unchanged");
    ship_sides = za_twosided; ship_prefer = za_prefer;
    arena_reset(8u << 20, ship_sides, ship_prefer, 0);
    lists = Z_Calloc(65536 * sizeof(*lists), PU_LEVEL, NULL);
    for (i = 0; i < 65536; i++)
    {
        mtag_t value = (mtag_t)i;
        Tag_FSet(&lists[i], value);
        Tag_Add(&lists[i], value);
        check(lists[i].count == 1 && Tag_FGet(&lists[i]) == value, "all singleton tags, including -1/zero preserved");
        check(Tag_Find(&lists[i], value) && !Tag_Find(&lists[i], (mtag_t)(value + 1)), "find handles full signed tag range");
        Tag_FSet(&lists[i], (mtag_t)(value ^ 0x5555));
        hash_list(&hash, &lists[i]);
    }
    ZA_Stats(&st); consistent();
    printf("singletons hash=%llu used=%zu blocks=%zu allocs=%zu\n",
        (unsigned long long)hash, st.used, st.usedblocks, st.allocs);
    for (i = 0; i < 4096; i++)
    {
        mtag_t value = (mtag_t)(i * 31);
        Tag_Add(&multi, value); Tag_Add(&equal, value);
        Tag_Add(&multi, value);
        check(Tag_Compare(&multi, &equal) && Tag_Share(&multi, &equal), "inline promotion/multitag growth identical");
    }
    /* Preserve the engine's pre-existing removal count and residual tail semantics. */
    for (i = 0; i < 32; i++)
    {
        Tag_Remove(&multi, (mtag_t)(i * 31));
        check(multi.count == 4096, "legacy removal count unchanged");
        hash_list(&hash, &multi);
    }
    Tag_FSet(&singleton, -123); Tag_Remove(&singleton, -123);
    check(singleton.count == 1 && singleton.tags == NULL, "legacy singleton removal matches zero realloc");
    for (i = 0; i < 100; i++)
    {
        taglist_t fakeflat = lists[i];
        check(Tag_FGet(&fakeflat) == Tag_FGet(&lists[i]), "read-only fakeflat shallow alias remains valid");
    }
    printf("operations hash=%llu\n", (unsigned long long)hash);
    consistent(); Z_FreeTags(PU_LEVEL, PU_PURGELEVEL - 1); empty_zone();
    return 0;
}
