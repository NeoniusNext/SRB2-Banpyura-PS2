/* Independent bitmap/state oracle for the actual frozen/current VRAM allocators.
 * Native high/free calls use complete unmodified default-LTO code sections.
 */
#include "common.h"
#include <setjmp.h>
#include <stdarg.h>
#ifdef NEXT10_GS_EE
#include <kernel.h>
#endif

state_t base_H, cur_H;
#ifdef NEXT10_GS_EE
struct { u8 before[64]; state_t state; u8 after[64]; } guarded_H;
__asm__(".globl H\n.type H,@object\n.set H,guarded_H+64\n.size H,36896\n");
#else
state_t H;
#endif
static state_t pristine, saved[2], expected[2];
static u8 bitmap[VRAM_BLOCKS];
static jmp_buf failure_return;
static int catch_failure, aborts, messages;
static u32 cases, allocations, frees, defensive, digest = 2166136261u;
static u32 random_state = 0x17643a91u;

void capture_output(const char *format, ...)
{
    if (strcmp(format, "PS2 GS: VRAM free-list capacity exhausted\n"))
    {
        printf("GS NEXT10 FAIL unexpected diagnostic\n");
        exit(1);
    }
    messages++;
}

void capture_abort(void)
{
    aborts++;
    if (catch_failure)
        longjmp(failure_return, 1);
    printf("GS NEXT10 FAIL unexpected abort\n");
    exit(1);
}

static u32 random_word(void)
{
    random_state = random_state * 1664525u + 1013904223u;
    return random_state;
}

static state_t *state(int tag) { return tag ? &cur_H : &base_H; }
static u32 start(int tag, int i) { return tag ? cur_H.cur.free_range[i].start : base_H.base.free_start[i]; }
static u32 length(int tag, int i) { return tag ? cur_H.cur.free_range[i].len : base_H.base.free_len[i]; }
static int count(int tag) { return tag ? cur_H.cur.free_n : base_H.base.free_n; }
static u32 used(int tag) { return tag ? cur_H.cur.used_blocks : base_H.base.used_blocks; }
static void set_count(int tag, int n) { if (tag) cur_H.cur.free_n = n; else base_H.base.free_n = n; }
static void set_used(int tag, u32 n) { if (tag) cur_H.cur.used_blocks = n; else base_H.base.used_blocks = n; }
static void set_range(int tag, int i, u32 s, u32 n)
{
    if (tag) { cur_H.cur.free_range[i].start = (u16)s; cur_H.cur.free_range[i].len = (u16)n; }
    else { base_H.base.free_start[i] = (u16)s; base_H.base.free_len[i] = (u16)n; }
}

static int fail(const char *why, int op, u32 a, u32 b)
{
    printf("GS NEXT10 FAIL case=%u op=%d a=%u b=%u reason=%s\n", cases, op, a, b, why);
    return 0;
}

static int check_exterior(void)
{
#ifdef NEXT10_GS_EE
    unsigned i;
    for (i = 0; i < sizeof guarded_H.before; i++)
        if (guarded_H.before[i] != 0x69 || guarded_H.after[i] != 0x96)
            return 0;
#endif
    return 1;
}

/* Production helpers have no callbacks on valid paths. Only their pre-existing
 * capacity-error diagnostic/abort are capture boundaries, exercised separately.
 */
static int invoke(int tag, int op, u32 a, u32 b, u32 *result)
{
    int fatal;
#ifdef NEXT10_GS_EE
    if (op)
        memcpy(&H, state(tag), sizeof H);
#endif
    catch_failure = 1;
    fatal = setjmp(failure_return);
    if (!fatal)
    {
#ifdef NEXT10_GS_EE
        if (op == 1)
            *result = tag ? cur_engine_vram_alloc_hi(a, b) : base_engine_vram_alloc_hi(a, b);
        else if (op == 2)
        {
            if (tag) cur_engine_vram_free(a, b); else base_engine_vram_free(a, b);
            *result = 0;
        }
        else
#endif
        if (op == 0)
            *result = tag ? cur_src_vram_alloc(a, b) : base_src_vram_alloc(a, b);
        else if (op == 1)
            *result = tag ? cur_src_vram_alloc_hi(a, b) : base_src_vram_alloc_hi(a, b);
        else
        {
            if (tag) cur_src_vram_free(a, b); else base_src_vram_free(a, b);
            *result = 0;
        }
    }
    catch_failure = 0;
#ifdef NEXT10_GS_EE
    if (op)
        memcpy(state(tag), &H, sizeof H);
#endif
    return fatal;
}

/* Build the implementation states from a block-occupancy map, not from a copy
 * of either free-list update algorithm. All inactive entries also have values.
 */
static void reset_states(u32 seed)
{
    unsigned i, b, occupied = 0;
    int n = 0, tag;
    for (i = 0; i < H_BYTES; i++)
        pristine.bytes[i] = (u8)(seed + i * 13u + (i >> 4));
    base_H = cur_H = pristine;
    for (i = 0; i < VRAM_RANGES; i++)
        for (tag = 0; tag < 2; tag++)
            set_range(tag, (int)i, (i * 97u + seed) & 65535u, (i * 103u + seed + 1u) & 65535u);
    for (b = 0; b < VRAM_BLOCKS;)
    {
        unsigned first;
        if (bitmap[b]) { b++; occupied++; continue; }
        first = b;
        while (b < VRAM_BLOCKS && !bitmap[b]) b++;
        for (tag = 0; tag < 2; tag++) set_range(tag, n, first, b - first);
        n++;
    }
    for (tag = 0; tag < 2; tag++) { set_count(tag, n); set_used(tag, occupied); }
}

static int check_states(int oracle, int op, u32 a, u32 b)
{
    int i, tag;
    unsigned p = 0, occupied = 0, ranges = 0;
    if (!check_exterior()) return fail("exterior state canary", op, a, b);
    if (count(0) != count(1) || used(0) != used(1)) return fail("counter mismatch", op, a, b);
    for (i = 0; i < VRAM_RANGES; i++)
        if (start(0, i) != start(1, i) || length(0, i) != length(1, i))
            return fail("complete active/inactive range mismatch", op, a, b);
    for (tag = 0; tag < 2; tag++)
        if (memcmp(state(tag)->bytes, pristine.bytes, 2288) ||
            memcmp(state(tag)->bytes + 35068, pristine.bytes + 35068, H_BYTES - 35068))
            return fail("other driver state modified", op, a, b);
    if (oracle)
    {
        while (p < VRAM_BLOCKS)
        {
            unsigned first;
            if (bitmap[p]) { occupied++; p++; continue; }
            first = p;
            while (p < VRAM_BLOCKS && !bitmap[p]) p++;
            if (ranges >= (unsigned)count(0) || start(0, (int)ranges) != first || length(0, (int)ranges) != p - first)
                return fail("independent occupancy/range oracle", op, a, b);
            ranges++;
        }
        if (ranges != (unsigned)count(0) || occupied != used(0))
            return fail("independent occupancy/accounting oracle", op, a, b);
    }
    digest = (digest ^ (used(0) + (u32)count(0) * 65537u + a + b + (u32)op)) * 16777619u;
    cases++;
    return 1;
}

static u32 oracle_allocate(u32 n, u32 alignment, int high)
{
    unsigned p = 0;
    int chosen = -1;
    while (p < VRAM_BLOCKS)
    {
        unsigned first, end, candidate;
        if (bitmap[p]) { p++; continue; }
        first = p;
        while (p < VRAM_BLOCKS && !bitmap[p]) p++;
        end = p;
        if (end - first < n) continue;
        candidate = high ? ((end - n) & ~(alignment - 1u)) : ((first + alignment - 1u) & ~(alignment - 1u));
        if (candidate < first || candidate + n > end) continue;
        chosen = (int)candidate;
        if (!high) break;
    }
    if (chosen >= 0)
        memset(bitmap + chosen, 1, n);
    return chosen < 0 ? 0u : (u32)chosen;
}

static int step(int op, u32 a, u32 b, u32 *returned)
{
    u32 old = 0, now = 0, want;
    unsigned i;
    int old_fatal, new_fatal;
    if (op == 2)
    {
        for (i = a; i < a + b; i++)
            if (i >= VRAM_BLOCKS || !bitmap[i]) return fail("invalid fixture free", op, a, b);
        memset(bitmap + a, 0, b);
        want = 0;
        frees++;
    }
    else { want = oracle_allocate(a, b, op == 1); allocations++; }
    old_fatal = invoke(0, op, a, b, &old);
    new_fatal = invoke(1, op, a, b, &now);
    if (old_fatal || new_fatal || old != want || now != want) return fail("return/fatal oracle", op, a, b);
    if (!check_states(1, op, a, b)) return 0;
    digest = (digest ^ want) * 16777619u;
    if (returned) *returned = want;
    return 1;
}

static void pattern(unsigned kind)
{
    unsigned p;
    memset(bitmap, 1, sizeof bitmap);
    for (p = 0; p < VRAM_BLOCKS; p++)
    {
        if (kind == 1) bitmap[p] = 0;
        else if (kind == 2 && p >= VRAM_BLOCKS - 2) bitmap[p] = 0;
        else if (kind == 3 && (p & 1)) bitmap[p] = 0;
        else if (kind == 4 && p >= 1 && p < 129) bitmap[p] = 0;
        else if (kind == 5 && ((p / 4) & 1)) bitmap[p] = 0;
        else if (kind == 6 && p >= 4 && p < 12) bitmap[p] = 0;
        else if (kind == 7 && (p % 37) < 13) bitmap[p] = 0;
        else if (kind == 8 && ((p >= 1 && p < 9) || (p >= 16 && (p & 1)))) bitmap[p] = 0;
    }
}

static int exact_matrix(void)
{
    static const u32 widths[] = {1,2,3,4,7,8,16,31,32,63,64,127,128,255,256,511,512,1023,1024,4096,16384};
    static const u32 aligns[] = {1,2,4,8,16,32,64,128,256,1024};
    static const u32 populations[] = {0,1,2,3,7,16,31,64,127,512,2048,8192};
    unsigned kind, wi, ai, hi, ci, where, p;
    for (kind = 0; kind < 9; kind++)
        for (wi = 0; wi < sizeof widths / sizeof widths[0]; wi++)
            for (ai = 0; ai < sizeof aligns / sizeof aligns[0]; ai++)
                for (hi = 0; hi < 2; hi++)
                {
                    pattern(kind);
                    reset_states(kind * 131u + wi * 17u + ai);
                    if (!step((int)hi, widths[wi], aligns[ai], NULL)) return 0;
                }
    for (ci = 0; ci < sizeof populations / sizeof populations[0]; ci++)
        for (where = 0; where < 3; where++)
        {
            unsigned n = populations[ci], target;
            memset(bitmap, 1, sizeof bitmap);
            for (p = 0; p < n; p++) bitmap[2 * p + 1] = 0;
            target = where == 0 ? 0 : where == 1 ? (n ? n & ~1u : 7) : (n < 8192 ? 2 * n + 2 : 16382);
            reset_states(ci * 7u + where);
            if (!step(2, target, 1, NULL)) return 0;
        }
    return 1;
}

static int traces(void)
{
    struct allocation { u32 s, n; } live[2048];
    unsigned trace, it;
#ifdef NEXT10_GS_SHORT
    const unsigned ntrace = 2, iterations = 600;
#else
    const unsigned ntrace = 12, iterations = 4000;
#endif
    for (trace = 0; trace < ntrace; trace++)
    {
        unsigned nlive = 0;
        memset(bitmap, 0, sizeof bitmap);
        bitmap[0] = 1; /* Production pool bases are positive; zero is the failure sentinel. */
        reset_states(trace + 0xB1u);
        for (it = 0; it < iterations; it++)
        {
            const u32 choose = random_word();
            if (((choose >> 16) & 7u) < 5u && nlive < 2048)
            {
                const u32 nr = random_word();
                const u32 ar = random_word();
                const u32 hr = random_word();
                const u32 n = 1u + (nr >> 16) % 511u, align = 1u << ((ar >> 16) % 11u);
                u32 address;
                if (!step((int)((hr >> 16) & 1u), n, align, &address)) return 0;
                if (address) { live[nlive].s = address; live[nlive].n = n; nlive++; }
            }
            else if (nlive)
            {
                const u32 select = random_word();
                const unsigned i = (select >> 16) % nlive;
                if (!step(2, live[i].s, live[i].n, NULL)) return 0;
                live[i] = live[--nlive];
            }
        }
        while (nlive)
        {
            const struct allocation a = live[--nlive];
            if (!step(2, a.s, a.n, NULL)) return 0;
        }
        if (count(0) != 1 || start(0, 0) != 1 || length(0, 0) != VRAM_BLOCKS - 1)
            return fail("trace did not restore whole pool", 2, trace, 0);
    }
    return 1;
}

static int capacity_errors(void)
{
    int i, tag, op;
    u32 output;
    memset(bitmap, 1, sizeof bitmap);
    reset_states(61);
    /* Defensive full-array branch, distinct from valid sorted/merged bitmap states. */
    for (tag = 0; tag < 2; tag++)
    {
        set_count(tag, VRAM_RANGES);
        set_used(tag, 100);
        for (i = 0; i < VRAM_RANGES; i++) set_range(tag, i, 1, 3);
    }
    for (op = 0; op < 2; op++)
    {
        for (tag = 0; tag < 2; tag++)
            if (invoke(tag, op, 1, 2, &output) || output) return fail("capacity split guard", op, 1, 2);
        if (!check_states(0, op, 1, 2)) return 0;
        defensive++;
    }
    for (tag = 0; tag < 2; tag++)
        if (!invoke(tag, 2, 10, 1, &output)) return fail("capacity fatal omitted", 2, 10, 1);
    if (messages != 2 || aborts != 2 || !check_states(0, 2, 10, 1)) return 0;
    defensive++;
    return 1;
}

#ifdef NEXT10_GS_EE
static inline u32 cycles(void) { u32 n; __asm__ volatile("mfc0 %0,$9" : "=r"(n) : : "memory"); return n; }
static inline u32 interrupt_begin(void)
{
    u32 previous;
    __asm__ volatile("mfc0 %0,$12\n\tdi\n\tsync.p" : "=r"(previous) : : "memory");
    return previous;
}
static inline void interrupt_end(u32 previous)
{
    if (previous & 0x10000u) __asm__ volatile("ei\n\tsync.p" : : : "memory");
}

static void timing_pattern(int op, unsigned n, int kind, u32 *a, u32 *b)
{
    unsigned i;
    memset(bitmap, 1, sizeof bitmap);
    for (i = 0; i < n; i++) bitmap[2 * i + 1] = 0;
    if (op == 1)
    {
        *a = kind == 0 ? 1 : 2;
        *b = 1;
        if (kind == 1 && n && n < 8192)
        {
            /* One larger low range; the high search skips all later singletons. */
            memset(bitmap, 1, sizeof bitmap);
            bitmap[1] = bitmap[2] = 0;
            for (i = 1; i < n; i++) bitmap[3 + 2 * i] = 0;
        }
    }
    else
    {
        if (kind == 0 && n < 8192)
        {
            memset(bitmap, 1, sizeof bitmap);
            for (i = 0; i < n; i++) bitmap[2 * i + 4] = 0;
        }
        *a = kind == 0 ? (n < 8192 ? 1 : 0) : kind == 1 ? (n >= 2 ? 2 : 0) : (n < 8192 ? 2 * n + 2 : 16382);
        *b = 1;
    }
}

static u32 time_one(int tag, int op, u32 a, u32 b, int cold)
{
    u32 before, elapsed, previous, returned = 0;
    u32 (*volatile alloc)(u32,u32) = tag ? cur_engine_vram_alloc_hi : base_engine_vram_alloc_hi;
    void (*volatile release)(u32,u32) = tag ? cur_engine_vram_free : base_engine_vram_free;
    memcpy(&H, &saved[tag], sizeof H);
    if (cold)
    {
        FlushCache(WRITEBACK_DCACHE);
        FlushCache(INVALIDATE_DCACHE);
        FlushCache(INVALIDATE_ICACHE);
    }
    previous = interrupt_begin();
    before = cycles();
    if (op == 1) returned = alloc(a, b);
    else release(a, b);
    elapsed = cycles() - before;
    interrupt_end(previous);
    if (elapsed >= 0x80000000u || memcmp(&H, &expected[tag], sizeof H) || !check_exterior())
    {
        printf("GS NEXT10 FAIL timed state tag=%d op=%d a=%u b=%u\n", tag, op, a, b);
        exit(1);
    }
    digest ^= returned;
    return elapsed;
}

static int timings(void)
{
    static const u32 populations[] = {0,1,2,3,7,16,31,64,127,512,2048,8192};
    unsigned ci, row = 0;
    int op, kind, cold, rep, iteration, order;
    const int iterations = 8;
    for (cold = 0; cold < 2; cold++)
        for (op = 1; op <= 2; op++)
            for (ci = 0; ci < sizeof populations / sizeof populations[0]; ci++)
                for (kind = 0; kind < 3; kind++)
                {
                    u32 a, b;
                    timing_pattern(op, populations[ci], kind, &a, &b);
                    reset_states(row + 0xA3u);
                    saved[0] = base_H; saved[1] = cur_H;
                    if (!step(op, a, b, NULL)) return 0;
                    expected[0] = base_H; expected[1] = cur_H;
                    for (rep = 0; rep < 9; rep++)
                    {
                        u32 sums[2] = {0,0};
                        for (iteration = 0; iteration < iterations; iteration++)
                            for (order = 0; order < 2; order++)
                            {
                                const int tag = order ^ (rep & 1);
                                sums[tag] += time_one(tag, op, a, b, cold);
                            }
                        printf("NEXT10 TIMING row=%u op=%d count=%u kind=%d cache=%d rep=%d old=%u new=%u iters=%d\n",
                               row, op, populations[ci], kind, cold, rep, sums[0], sums[1], iterations);
                    }
                    row++;
                }
    return row == 144;
}
#endif

int main(void)
{
#ifdef NEXT10_GS_EE
    memset(guarded_H.before, 0x69, sizeof guarded_H.before);
    memset(guarded_H.after, 0x96, sizeof guarded_H.after);
#else
    (void)saved; (void)expected;
#endif
    if (!exact_matrix() || !traces() || !capacity_errors()) return 1;
    printf("GS NEXT10 EXACT cases=%u allocations=%u frees=%u defensive=%u state_bytes=%llu digest=%08x\n",
           cases, allocations, frees, defensive, (unsigned long long)cases * H_BYTES * 2u, digest);
#ifdef NEXT10_GS_EE
    if (!timings()) return 1;
#endif
    printf("GS NEXT10 DONE\n");
    return 0;
}
