/* Independent complete GS A+D state/packet oracle; exact LTO code on EE. */
#include "common.h"
#ifdef NEXT9_GS_EE
#include <kernel.h>
#endif

hwfixture_t H;
vfixture_t V;
vufixture_t VU;
static qw_t rings[NBUF][BUF_QW + 4] __attribute__((aligned(16)));
static hwfixture_t initial, expected;
static qw_t wanted[24];
static u32 cases, register_writes, rng = 0x912bad67u;
static u32 flushes, vu_ends, observed_valid[2], expected_flushes, expected_vu_ends;
static u32 completion_clear;
static volatile u32 observer;
typedef void (*flush_t)(const gsstate_t *);
static flush_t volatile functions[2] = {base_state_flush, cur_state_flush};

static u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static u64 rnd64(void)
{
    u32 low = rnd();
    u32 high = rnd();
    return (u64)low | ((u64)high << 32);
}

/* These hooks model the ownership boundary and record the state seen there.
   Actual DMA/VU completion and image output belong to the real-driver suite. */
void vu_chunk_end(void)
{
    observed_valid[vu_ends < 1 ? 0 : 1] = H.gsr.valid;
    vu_ends++;
    H.gsr.valid &= ~completion_clear;
    VU.open = 0;
    V.chunk_open = 0;
}

void pk_flush(void)
{
    observed_valid[1] = H.gsr.valid;
    flushes++;
    H.cur = (H.cur + 1) % NBUF;
    H.wr = 1;
    H.run = 0;
}

static int fail(const char *why, u32 valid, u32 dirty, u32 uses, u32 cld, u32 tag)
{
    printf("GS NEXT9 FAIL %s case=%u valid=%u dirty=%u uses=%u cld=%u version=%u\n",
           why, cases, valid, dirty, uses, cld, tag);
    return 1;
}

/* Native register order and numeric addresses are deliberately independent of
   the production conditional tree, masks, constants and array element width. */
static u32 oracle(const gsstate_t *s, u32 *entry_valid)
{
    static const u8 order[10] = {6, 5, 7, 0, 1, 8, 2, 4, 3, 9};
    static const u8 addresses[10] = {0x47, 0x42, 0x08, 0x06, 0x14, 0x4e, 0x4c, 0x40, 0x3d, 0x01};
    u64 desired[10], old[10];
    u32 count = 0;
    memcpy(desired, s, sizeof desired);
    memcpy(old, &expected.gsr, sizeof old);
    memset(wanted, 0xa5, sizeof wanted);
    for (u32 j = 0; j < 10; j++)
    {
        u32 field = order[j];
        if ((field == 8 && !s->use_fog) || ((field == 2 || field == 3 || field == 4) && !s->use_tex))
            continue;
        int different = old[field] != desired[field];
        if (field == 3)
            different = ((old[field] ^ desired[field]) << 3) != 0 || (desired[field] >> 61) != 0;
        if (!(expected.gsr.valid & (1u << field)) || different)
        {
            wanted[1 + count].d[0] = desired[field];
            wanted[1 + count].d[1] = addresses[field];
            count++;
            old[field] = desired[field];
            expected.gsr.valid |= 1u << field;
        }
    }
    memcpy(&expected.gsr, old, sizeof old);
    *entry_valid = expected.gsr.valid;
    expected_flushes = expected_vu_ends = 0;
    if (count)
    {
        if (V.chunk_open && VU.open)
        {
            expected_vu_ends = 1;
            expected.gsr.valid &= ~completion_clear;
        }
        if (expected.wr + count + 1 > 8192)
        {
            expected_flushes = 1;
            expected.cur = (expected.cur + 1) % 4;
            expected.wr = 1;
            expected.run = 0;
        }
        wanted[0].d[0] = (u64)count | (1ull << 15) | (1ull << 60);
        wanted[0].d[1] = 14;
        expected.wr += count + 1;
        expected.serial++;
        expected.st.state_writes += count;
    }
    return count;
}

static int check(u32 valid, u32 dirty, u32 uses, u32 cld, u32 remaining, u32 mode)
{
    gsstate_t s, saved;
    qw_t expected_window[24];
    u64 desired[10], old[10];
    for (u32 i = 0; i < 10; i++)
    {
        desired[i] = rnd64();
        if (i == 3) desired[i] = (desired[i] & 0x1fffffffffffffffull) | ((u64)cld << 61);
        old[i] = desired[i] ^ ((dirty & (1u << i)) ? (1ull << ((cases + i * 7) & 63)) : 0);
        if (i == 3 && !(dirty & (1u << i)))
            old[i] = (old[i] & 0x1fffffffffffffffull) | ((u64)((cases >> 4) & 7) << 61);
    }
    memcpy(&s, desired, sizeof desired);
    s.use_tex = (uses & 1) ? ((mode & 1) ? -1 : 1) : 0;
    s.use_fog = (uses & 2) ? ((mode & 2) ? 7 : 1) : 0;
    saved = s;
    memcpy(&H.gsr, old, sizeof old);
    H.gsr.valid = valid | 0x5a930000u;
    H.serial = cases & 1 ? 0xffffffffu : 31u;
    H.st.state_writes = cases & 1 ? 0xfffffffeu : 11u;
    H.cur = (int)((cases >> 3) & 3);
    H.wr = 8192 - remaining;
    H.run = H.wr > 17 ? H.wr - 17 : 0;
    initial = expected = H;
    V.chunk_open = (mode & 4) != 0;
    VU.open = (mode & 8) != 0;
    int chunk = V.chunk_open, vu_open = VU.open;
    completion_clear = (mode & 16) ? (512u | 8u | 4u) : 512u;
    u32 entry_valid;
    u32 n = oracle(&s, &entry_valid);
    u32 out_cur = (u32)expected.cur;
    u32 out_at = n ? expected.wr - n - 1 : initial.wr;
    if (out_at + n + 1 > BUF_QW) abort();
    memset(expected_window, 0xa5, sizeof expected_window);
    if (n) memcpy(expected_window + 1, wanted, (n + 1) * sizeof(qw_t));
    for (u32 tag = 0; tag < 2; tag++)
    {
        H = initial;
        V.chunk_open = chunk;
        VU.open = vu_open;
        flushes = vu_ends = observed_valid[0] = observed_valid[1] = 0;
        /* The complete emitted packet, immediate pre/post canaries, exterior
           ring canaries and all private-state bytes are checked. */
        memset(rings[out_cur] + out_at - 1, 0xa5, (n + 3) * sizeof(qw_t));
        functions[tag](&s);
        if (memcmp(&s, &saved, sizeof s)) return fail("input write", valid, dirty, uses, cld, tag);
        if (memcmp(&H, &expected, sizeof H)) return fail("whole private state/canary", valid, dirty, uses, cld, tag);
        if (flushes != expected_flushes || vu_ends != expected_vu_ends)
            return fail("ownership hook count", valid, dirty, uses, cld, tag);
        if ((vu_ends && observed_valid[0] != entry_valid) ||
            (flushes && observed_valid[1] != (entry_valid & (vu_ends ? ~completion_clear : ~0u))))
            return fail("state at ownership boundary", valid, dirty, uses, cld, tag);
        if (memcmp(rings[out_cur] + out_at - 1, expected_window, (n + 3) * sizeof(qw_t)))
            return fail("complete A+D packet/canary", valid, dirty, uses, cld, tag);
        for (u32 j = 0; j < NBUF; j++)
            for (u32 k = 0; k < 4; k++)
                if (rings[j][BUF_QW + k].d[0] != 0xa5a5a5a5a5a5a5a5ull || rings[j][BUF_QW + k].d[1] != 0xa5a5a5a5a5a5a5a5ull)
                    return fail("ring exterior canary", valid, dirty, uses, cld, tag);
    }
    cases++;
    register_writes += n;
    return 0;
}

#ifdef NEXT9_GS_EE
static u32 cycles(void) { u32 value; __asm__ volatile("mfc0 %0,$9" : "=r"(value)); return value; }
static u32 timed(flush_t fn, const gsstate_t *s, u32 valid, u32 dirty, u32 calls)
{
    u64 desired[10], old[10];
    memcpy(desired, s, sizeof desired);
    for (u32 i = 0; i < 10; i++) old[i] = desired[i] ^ ((dirty & (1u << i)) ? 1u : 0u);
    u32 irq = DIntr();
    u32 begin = cycles();
    for (u32 i = 0; i < calls; i++)
    {
        memcpy(&H.gsr, old, sizeof old);
        H.gsr.valid = valid;
        H.wr = 1;
        fn(s);
        observer ^= H.wr;
    }
    u32 elapsed = cycles() - begin;
    if (irq) EIntr();
    return elapsed;
}

static int bench(void)
{
    u32 kind = 0;
    const u32 masks[] = {0u, 1u, 2u, 4u, 8u, 16u, 32u, 64u, 128u, 256u, 512u, 1023u, 341u, 682u};
    for (u32 uses = 0; uses < 4; uses++)
        for (u32 index = 0; index < sizeof masks / sizeof masks[0] + 2; index++)
        {
            gsstate_t s;
            u64 values[10];
            u32 valid = index == 14 ? 0 : 1023, dirty = index >= 14 ? 0 : masks[index];
            u32 cld = index == 15;
            for (u32 i = 0; i < 10; i++) values[i] = 0x123456789abcdef0ull + i;
            values[3] &= 0x1fffffffffffffffull;
            values[3] |= (u64)cld << 61;
            memcpy(&s, values, sizeof values);
            s.use_tex = uses & 1;
            s.use_fog = uses & 2;
            V.chunk_open = VU.open = 0;
            H.cur = 0;
            for (u32 rep = 0; rep < 9; rep++)
            {
                u32 times[2];
                hwfixture_t actual[2];
                qw_t packets[2][14];
                qw_t expected_packet[14];
                u64 old[10];
                for (u32 i = 0; i < 10; i++) old[i] = values[i] ^ ((dirty & (1u << i)) ? 1u : 0u);
                memcpy(&H.gsr, old, sizeof old);
                H.gsr.valid = valid;
                H.cur = H.run = 0;
                H.wr = 1;
                H.serial = 17;
                H.st.state_writes = 31;
                hwfixture_t timed_initial = H;
                expected = H;
                u32 entry_valid;
                u32 count = oracle(&s, &entry_valid);
                if (count)
                {
                    expected.serial = 17 + 2048;
                    expected.st.state_writes = 31 + count * 2048;
                }
                memset(expected_packet, 0xa5, sizeof expected_packet);
                if (count) memcpy(expected_packet + 1, wanted, (count + 1) * sizeof(qw_t));
                for (u32 turn = 0; turn < 2; turn++)
                {
                    u32 tag = (turn + rep) & 1;
                    H = timed_initial;
                    memset(rings[0], 0xa5, sizeof packets[tag]);
                    times[tag] = timed(functions[tag], &s, valid, dirty, 2048);
                    actual[tag] = H;
                    memcpy(packets[tag], rings[0], sizeof packets[tag]);
                    if (memcmp(&actual[tag], &expected, sizeof H) || memcmp(packets[tag], expected_packet, sizeof expected_packet))
                        return fail("timed independent state/packet/canary", valid, dirty, uses, cld, tag);
                }
                if (memcmp(&actual[0], &actual[1], sizeof H) || memcmp(packets[0], packets[1], sizeof packets[0]))
                    return fail("timed state/packet/canary", valid, dirty, uses, cld, 1);
                printf("GSNEXT9BENCH kind=%u valid=%u dirty=%u uses=%u cld=%u calls=2048 rep=%u base=%u current=%u\n",
                       kind, valid, dirty, uses, cld, rep, times[0], times[1]);
            }
            kind++;
        }
    return 0;
}
#endif

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    memset(&H, 0xb6, sizeof H);
    memset(rings, 0xa5, sizeof rings);
    for (u32 i = 0; i < NBUF; i++) H.ring[i] = rings[i];
    for (u32 valid = 0; valid < 1024; valid++)
    {
#if defined(NEXT9_GS_EE) || defined(NEXT9_GS_SHORT)
        const u32 dirty_masks[] = {0u, 1023u, valid, 1023u ^ valid};
        for (u32 j = 0; j < 4; j++)
            for (u32 uses = 0; uses < 4; uses++)
                if (check(valid, dirty_masks[j], uses, 0, 32, 0)) return 1;
#else
        for (u32 dirty = 0; dirty < 1024; dirty++)
            if (check(valid, dirty, (valid ^ dirty) & 3, 0, 32, 0)) return 1;
#endif
    }
    /* Explicit truthy flags, all eight CLD values, every dirty register bit,
       every ring boundary, both VU predicates, all four buffers and wraps. */
    for (u32 remaining = 1; remaining <= 13; remaining++)
        for (u32 uses = 0; uses < 4; uses++)
            for (u32 cld = 0; cld < 8; cld++)
                for (u32 mode = 0; mode < 32; mode++)
                {
                    u32 valid = rnd() & 1023;
                    u32 dirty = rnd() & 1023;
                    if (check(valid, dirty, uses, cld, remaining, mode)) return 1;
                }
    for (u32 i = 0; i < 8192; i++)
    {
        u32 valid = rnd() & 1023;
        u32 dirty = rnd() & 1023;
        u32 uses = rnd() & 3;
        u32 cld = rnd() & 7;
        u32 remaining = 1 + rnd() % 16;
        u32 mode = rnd() & 31;
        if (check(valid, dirty, uses, cld, remaining, mode)) return 1;
    }
    printf("GS NEXT9 PASS cases=%u register_writes=%u\n", cases, register_writes);
#ifdef NEXT9_GS_EE
    if (bench()) return 1;
#endif
    printf("GS NEXT9 DONE observer=%u\n", observer);
    return 0;
}
