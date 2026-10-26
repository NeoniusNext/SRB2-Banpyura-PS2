/* OPT13-RCACHE: EE data-cache / memory latency probe. Pure EE program (no engine), prints one line per test:
 *   CB <test> ws=<bytes> cyc_per_op=<x.xx>
 * COP0 Count (mfc0 $9) counts CPU cycles on the console; in PCSX2 it counts its own cycle model (recompiler / interpreter / interpreter+EECache).
 * Purpose: (1) see whether PCSX2's EnableEECache charges cycles for D-cache misses at all, (2) give the numbers to run on a REAL console (same ELF).
 * Build: tools in docs/research/rcache/bench/build.sh */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <tamtypes.h>
#include <kernel.h>

static inline u32 cnt(void) { u32 v; __asm__ volatile("mfc0 %0,$9" : "=r"(v)); return v; }

typedef struct node { struct node *next; u32 pad[15]; } node_t;   /* 64 bytes = one line */

static u32 rng = 12345;
static u32 rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

/* ring of lines in random order inside buf[0..ws) */
static node_t *build_ring(u8 *buf, u32 ws, u32 uncached_off)
{
    u32 n = ws / 64, i;
    u32 *perm = (u32 *)malloc(n * 4);
    for (i = 0; i < n; i++) perm[i] = i;
    for (i = n - 1; i > 0; i--) { u32 j = rnd() % (i + 1); u32 t = perm[i]; perm[i] = perm[j]; perm[j] = t; }
    for (i = 0; i < n; i++) {
        node_t *a = (node_t *)(buf + perm[i] * 64);
        node_t *b = (node_t *)(buf + perm[(i + 1) % n] * 64);
        a->next = (node_t *)((u8 *)b + uncached_off);
    }
    { node_t *head = (node_t *)(buf + perm[0] * 64);
      free(perm);
      SyncDCache(buf, buf + ws);   /* write the ring back to RAM: the uncached view must see it */
      return head; }
}

static double chase(node_t *p, u32 hops)
{
    u32 i, t0, t1;
    for (i = 0; i < hops; i++) p = p->next;           /* warm */
    t0 = cnt();
    for (i = 0; i < hops; i++) p = p->next;           /* dependent loads */
    t1 = cnt();
    __asm__ volatile("" :: "r"(p));
    return (double)(t1 - t0) / hops;
}

/* independent loads, one per line, in address order (hardware prefetch-free; gives throughput) */
static double seqread(u8 *buf, u32 ws, u32 reps)
{
    u32 r, i, t0, t1, sum = 0, n = ws / 64;
    volatile u32 *v = (volatile u32 *)buf;
    for (i = 0; i < n; i++) sum += v[i * 16];
    t0 = cnt();
    for (r = 0; r < reps; r++) for (i = 0; i < n; i++) sum += v[i * 16];
    t1 = cnt();
    __asm__ volatile("" :: "r"(sum));
    return (double)(t1 - t0) / ((double)reps * n);
}

static double seqwrite(u8 *buf, u32 ws, u32 reps)
{
    u32 r, i, t0, t1, n = ws / 64;
    volatile u32 *v = (volatile u32 *)buf;
    for (i = 0; i < n; i++) v[i * 16] = i;
    t0 = cnt();
    for (r = 0; r < reps; r++) for (i = 0; i < n; i++) v[i * 16] = r + i;
    t1 = cnt();
    return (double)(t1 - t0) / ((double)reps * n);
}

/* all words of every line (sequential memcpy-like read, 4 B at a time) */
static double fullread(u8 *buf, u32 ws, u32 reps)
{
    u32 r, i, t0, t1, sum = 0, n = ws / 4;
    volatile u32 *v = (volatile u32 *)buf;
    t0 = cnt();
    for (r = 0; r < reps; r++) for (i = 0; i < n; i++) sum += v[i];
    t1 = cnt();
    __asm__ volatile("" :: "r"(sum));
    return (double)(t1 - t0) / ((double)reps * n);
}


/* ---- write-stream tests (the shape of the GS driver ring: 128-bit stores in order, then the DMA reads the buffer) ----
 * cached:  sq to the normal mapping, then SyncDCache over the range (what pk_flush does)
 * ucab:    sq to the 0x30000000 mapping (uncached accelerated: write-combining), no SyncDCache needed
 * spr:     sq to the scratchpad in 8 KB halves (16 KB), then a fromSPR DMA is NOT started here (this measures the CPU side only)
 * Correctness: the data written through each mapping is read back through the cached mapping and compared. */
static double stream_write(void *base, u32 bytes, u32 reps, int sync, u32 *bad, const u8 *cached_view)
{
    u32 r, i, t0, t1, n = bytes / 16;
    u64 pat0 = 0x0123456789ABCDEFull;
    t0 = cnt();
    for (r = 0; r < reps; r++) {
        u8 *d = (u8 *)base;
        for (i = 0; i < n; i++) {
            u64 a = pat0 + r + i, b = ~a;
            __asm__ volatile("pcpyld %0,%1,%2\n\tsq %0,0(%3)" : "=&r"(a) : "r"(b), "r"(a), "r"(d) : "memory");
            d += 16;
        }
        if (sync) SyncDCache(base, (u8 *)base + bytes);
    }
    t1 = cnt();
    *bad = 0;
    if (cached_view) {
        const u64 *v = (const u64 *)cached_view;
        for (i = 0; i < n; i++) {
            u64 a = pat0 + (reps - 1) + i;
            if (v[2 * i] != a || v[2 * i + 1] != ~a) (*bad)++;
        }
    }
    return (double)(t1 - t0) / ((double)reps * n);
}

static void write_tests(u8 *buf)
{
    u32 bad, ws;
    u32 sizes2[] = { 4096, 16384, 65536, 262144, 1048576 };
    int k;
    for (k = 0; k < 5; k++) {
        ws = sizes2[k];
        printf("CB wr_cached_sync ws=%u cyc_per_qw=%.2f\n", ws, stream_write(buf, ws, 8, 1, &bad, NULL));
        printf("CB wr_ucab ws=%u cyc_per_qw=%.2f\n", ws, stream_write((u8 *)((u32)buf | 0x30000000u), ws, 8, 0, &bad, buf));
        printf("CB wr_ucab_check ws=%u bad_qw=%u\n", ws, bad);
        printf("CB wr_uncached ws=%u cyc_per_qw=%.2f\n", ws, stream_write((u8 *)((u32)buf | 0x20000000u), ws, 8, 0, &bad, buf));
        printf("CB wr_uncached_check ws=%u bad_qw=%u\n", ws, bad);
    }
    printf("CB wr_spr ws=16384 cyc_per_qw=%.2f\n", stream_write((u8 *)0x70000000u, 16384, 64, 0, &bad, NULL));
}


/* ---- performance counter scan (COP0 PCCR, mtps/mfpc): run on the CONSOLE to see which event number counts what ----
 * PCSX2 only counts cycles. For every event number 0..31 both counters are armed for three workloads:
 *   A  pointer chase over 1 KB   (no D-cache misses, tiny code)
 *   B  pointer chase over 64 KB  (a D-cache miss per hop, DTLB hits)
 *   C  64 KB of straight-line code run 8 times (an I-cache miss per 16 instructions)
 * The event that is ~0 for A, ~hops for B is "data cache miss"; the one that is ~0 for A and ~1024*8 for C is "instruction cache miss".
 * PCCR layout used (EE Core User's Manual): bit31 CTE, EVENT0 = bits 9:5, EVENT1 = bits 19:15, U0/S0/K0/EXL0 = bits 4:1 and U1/S1/K1/EXL1 = bits 14:11. */
__asm__(".text\n.align 6\n.globl cb_bigcode\n.ent cb_bigcode\ncb_bigcode:\n.set noreorder\n.rept 16384\nnop\n.endr\njr $31\nnop\n.set reorder\n.end cb_bigcode\n");
extern void cb_bigcode(void);
static inline u32 rd_pc0(void) { u32 v; __asm__ volatile("mfpc %0,0" : "=r"(v)); return v; }
static inline u32 rd_pc1(void) { u32 v; __asm__ volatile("mfpc %0,1" : "=r"(v)); return v; }
static inline void wr_pccr(u32 v) { __asm__ volatile("mtps %0,0\n\tsync.p" :: "r"(v)); }
static inline void wr_pc0(u32 v) { __asm__ volatile("mtpc %0,0\n\tsync.p" :: "r"(v)); }
static inline void wr_pc1(u32 v) { __asm__ volatile("mtpc %0,1\n\tsync.p" :: "r"(v)); }
static void pccr_scan(u8 *buf)
{
    int ev, w;
    node_t *ringA = build_ring(buf, 1024, 0), *ringB = build_ring(buf + 4096, 65536, 0);
    for (ev = 0; ev < 32; ev++) {
        u32 res[3][2];
        for (w = 0; w < 3; w++) {
            u32 i;
            node_t *p = w == 0 ? ringA : ringB;
            wr_pccr(0);
            wr_pc0(0); wr_pc1(0);
            wr_pccr((1u << 31) | (0xFu << 1) | ((u32)ev << 5) | (0xFu << 11) | ((u32)ev << 15));
            if (w == 2) { for (i = 0; i < 8; i++) cb_bigcode(); }
            else { for (i = 0; i < 32768; i++) p = p->next; }
            wr_pccr(0);
            res[w][0] = rd_pc0(); res[w][1] = rd_pc1();
            __asm__ volatile("" :: "r"(p));
        }
        printf("CB pccr ev=%2d  A(1KB chase): pc0=%u pc1=%u | B(64KB chase 32768 hops): pc0=%u pc1=%u | C(8 x 64KB code): pc0=%u pc1=%u\n",
               ev, res[0][0], res[0][1], res[1][0], res[1][1], res[2][0], res[2][1]);
    }
}


/* ---- cache maintenance and bulk-copy tests (proposals H1/H2/R3): run on the CONSOLE; PCSX2 gives flat numbers ----
 *   sync_dirty / sync_clean: SyncDCache over N bytes right after writing them / when they are not resident (cycles per call and per 64-byte line)
 *   flush0: FlushCache(0) (write back + invalidate the whole 8 KB D-cache) after writing N bytes (N >= 8 KB: at most 128 lines are dirty)
 *   copy_lq / copy_lq_pref: the lq/sq loop of ps2_memops.c over N bytes with a cold source (the source is evicted by touching 64 KB first), without and with
 *   `pref 0(src+128)` per 64-byte block */
static void sync_tests(u8 *buf, u8 *junk)
{
    u32 sizes3[] = { 1024, 4096, 8192, 16384, 65536, 131072, 262144 };
    u32 k, i, t0, t1;
    for (k = 0; k < 7; k++) {
        u32 n = sizes3[k], reps = 16, lines = (n + 63) / 64;
        u32 acc = 0, accf = 0, accc = 0;
        for (i = 0; i < reps; i++) {
            u32 j;
            for (j = 0; j < n; j += 16) __asm__ volatile("sq $0,0(%0)" :: "r"(buf + j) : "memory");
            t0 = cnt(); SyncDCache(buf, buf + n); t1 = cnt(); acc += t1 - t0;
            t0 = cnt(); SyncDCache(buf, buf + n); t1 = cnt(); accc += t1 - t0;       /* second call: nothing resident */
            for (j = 0; j < n; j += 16) __asm__ volatile("sq $0,0(%0)" :: "r"(buf + j) : "memory");
            t0 = cnt(); FlushCache(0); t1 = cnt(); accf += t1 - t0;
        }
        printf("CB sync_dirty n=%u cyc_per_call=%u cyc_per_line=%.2f\n", n, acc / reps, (double)(acc / reps) / lines);
        printf("CB sync_clean n=%u cyc_per_call=%u cyc_per_line=%.2f\n", n, accc / reps, (double)(accc / reps) / lines);
        printf("CB flush0 n=%u cyc_per_call=%u\n", n, accf / reps);
    }
    (void)junk;
}

static inline void cp_block(u8 *d, const u8 *s, int pref)
{
    u64 t0, t1, t2, t3;
    if (pref) __asm__ volatile("pref 0,128(%0)" :: "r"(s));
    __asm__ volatile("lq %0,0(%4)\n\tlq %1,16(%4)\n\tlq %2,32(%4)\n\tlq %3,48(%4)\n\tsq %0,0(%5)\n\tsq %1,16(%5)\n\tsq %2,32(%5)\n\tsq %3,48(%5)"
                     : "=&r"(t0), "=&r"(t1), "=&r"(t2), "=&r"(t3) : "r"(s), "r"(d) : "memory");
}

static void copy_tests(u8 *buf, u8 *junk)
{
    u32 sizes4[] = { 4096, 32768, 262144 };
    u32 k, pref;
    for (k = 0; k < 3; k++) {
        for (pref = 0; pref < 2; pref++) {
            u32 n = sizes4[k], i, reps = 8, t0, t1, tot = 0;
            for (i = 0; i < reps; i++) {
                u32 j;
                for (j = 0; j < 65536; j += 64) junk[j] = (u8)j;           /* evict the source (64 KB >> 8 KB) */
                t0 = cnt();
                for (j = 0; j < n; j += 64) cp_block(buf + 1048576 + j, buf + j, (int)pref);
                t1 = cnt(); tot += t1 - t0;
            }
            printf("CB copy_lq%s n=%u cyc_per_64B=%.2f\n", pref ? "_pref" : "", n, (double)(tot / reps) / (n / 64));
        }
    }
}

static u32 sizes[] = { 1024, 2048, 4096, 6144, 8192, 12288, 16384, 32768, 65536, 262144, 1048576, 4194304 };

int main(void)
{
    u8 *buf = (u8 *)memalign(64, 4194304 + 4096);
    u32 k;
    printf("CB start buf=%p\n", buf);
    for (k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
        u32 ws = sizes[k], hops = ws >= 65536 ? 1 << 15 : 1 << 16, reps = 4194304 / ws; if (reps < 2) reps = 2; if (reps > 256) reps = 256;
        node_t *ring = build_ring(buf, ws, 0);
        printf("CB chase ws=%u cyc_per_op=%.2f\n", ws, chase(ring, hops));
        printf("CB seqread1 ws=%u cyc_per_op=%.2f\n", ws, seqread(buf, ws, reps));
        printf("CB seqwrite1 ws=%u cyc_per_op=%.2f\n", ws, seqwrite(buf, ws, reps));
        printf("CB fullread ws=%u cyc_per_op=%.2f\n", ws, fullread(buf, ws, reps));
    }
    /* uncached view of the same RAM (0x20000000 = UNCACHED mapping on the EE) */
    {
        u32 ws = 65536;
        node_t *ring = build_ring(buf, ws, 0x20000000u);
        printf("CB chase_uncached ws=%u cyc_per_op=%.2f\n", ws, chase((node_t *)((u8 *)ring + 0x20000000u), 1 << 13));
    }
    /* scratchpad (16 KB at 0x70000000) */
    {
        u8 *spr = (u8 *)0x70000000;
        node_t *ring = build_ring(spr, 8192, 0);
        printf("CB chase_spr ws=8192 cyc_per_op=%.2f\n", chase(ring, 1 << 14));
    }
    write_tests(buf);
    sync_tests(buf, buf + 2097152);
    copy_tests(buf, buf + 3145728);
    pccr_scan(buf);
    printf("CB done\n");
    return 0;
}
