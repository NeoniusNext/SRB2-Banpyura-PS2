/* Complete tex_upload low-allocation path: independent block, GIF and state oracle. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef NEXT10_LOW_EE
#include "low-native.h"
#include "low-globals.h"
#define LOW_UP_HIST low_up_hist
#else
#include "low-host.h"
#define LOW_UP_HIST up_hist
#endif

enum { LOW_RECORDS = 8194, LOW_RING = 8192, LOW_SOURCE = 8192 };
static texrec_t low_records[LOW_RECORDS], low_wanted_records[LOW_RECORDS];
static qw_t low_ring[LOW_RING + 4], low_wanted_ring[LOW_RING + 4];
static u16 low_owners[VRAM_BLOCKS], low_wanted_owners[VRAM_BLOCKS];
static u8 low_source[LOW_SOURCE + 32] __attribute__((aligned(16)));
static u8 low_bitmap[VRAM_BLOCKS];
static __typeof__(H) low_initial_h, low_wanted_h;
static GLMipmap_t low_mip, low_wanted_mip;
static txstats_t low_wanted_tx;
static u32 low_cases, low_digest = 2166136261u, low_messages;
static int low_success;
static u32 low_block, low_blocks, low_width, low_height;
static unsigned low_initial_count;
#ifdef NEXT10_LOW_EE
static u8 *low_global_initial;
static unsigned low_global_bytes;
void low_unexpected(void)
{
    printf("GS LOW10 FAIL unexpected boundary call\n");
    exit(1);
}
void low_output(const char *format, ...)
{
    if (strncmp(format, "HWD spill: no screen victim ", 28)) low_unexpected();
    low_messages++;
}
static void low_globals_init(void)
{
    unsigned i, at = 0;
    for (i = 0; i < sizeof low_globals / sizeof low_globals[0]; i++) low_global_bytes += low_globals[i].bytes;
    low_global_initial = malloc(low_global_bytes);
    if (!low_global_initial) low_unexpected();
    for (i = 0; i < sizeof low_globals / sizeof low_globals[0]; i++)
    {
        memcpy(low_global_initial + at, low_globals[i].address, low_globals[i].bytes);
        at += low_globals[i].bytes;
    }
}
static int low_globals_check(void)
{
    unsigned i, at = 0;
    for (i = 0; i < sizeof low_globals / sizeof low_globals[0]; i++)
    {
        if (memcmp(low_global_initial + at, low_globals[i].address, low_globals[i].bytes)) return 0;
        at += low_globals[i].bytes;
    }
    return 1;
}
static void low_globals_reset(void)
{
    unsigned i, at = 0;
    for (i = 0; i < sizeof low_globals / sizeof low_globals[0]; i++)
    {
        memcpy(low_globals[i].address, low_global_initial + at, low_globals[i].bytes);
        at += low_globals[i].bytes;
    }
}
#endif

static u8 *low_range_bytes(void *state)
{
#if defined(NEXT10_LOW_EE) || defined(NEXT10_LOW_CURRENT)
    return (u8 *)state + offsetof(__typeof__(H), free_range);
#else
    return (u8 *)state + offsetof(__typeof__(H), free_start);
#endif
}
static void low_set_range(void *state, int tag, unsigned index, u32 first, u32 length)
{
    u8 *r = low_range_bytes(state);
    const u16 s = (u16)first, n = (u16)length;
    const unsigned si = tag ? 2 * index : index, ni = tag ? 2 * index + 1 : VRAM_RANGES + index;
    memcpy(r + si * sizeof s, &s, sizeof s); memcpy(r + ni * sizeof n, &n, sizeof n);
}
static unsigned low_power(u32 n)
{
    unsigned exponent = 0; while ((1u << exponent) < n) exponent++; return exponent;
}
/* This independently enumerates the GS PSMT8 page/block layout, rather than
 * using tex_footprint's rectangular-prefix shortcut or production tables.
 */
static u32 low_footprint(u32 width, u32 height)
{
    u32 x, y, last = 0;
    const u32 stride = (width + 127u) / 128u;
    for (y = 0; y < height; y += 16)
        for (x = 0; x < width; x += 16)
        {
            const u32 bx = (x / 16) & 7u, by = (y / 16) & 3u;
            const u32 block = (bx & 1u) + ((by & 1u) << 1) + ((bx & 2u) << 1) +
                              ((by & 2u) << 2) + ((bx & 4u) << 2);
            const u32 address = ((y / 64) * stride + x / 128) * 32 + block;
            if (address > last) last = address;
        }
    return last + 1;
}
static void low_packet_word(unsigned at, u64 value, u64 address)
{
    low_wanted_ring[at].d[0] = value; low_wanted_ring[at].d[1] = address;
}
static void low_packet_oracle(void)
{
    const u32 bytes = low_width * low_height, qwords = (bytes + 15u) / 16u;
    const u32 tbw = ((low_width + 127u) / 128u) * 2;
    u8 *data;
    low_packet_word(1, 4ull | (1ull << 60), 14);
    low_packet_word(2, (u64)low_block << 32 | (u64)tbw << 48 | 19ull << 56, 80);
    low_packet_word(3, 0, 81);
    low_packet_word(4, (u64)low_width | (u64)low_height << 32, 82);
    low_packet_word(5, 0, 83);
    low_packet_word(6, (u64)qwords | (1ull << 15) | (2ull << 58), 0);
    data = (u8 *)&low_wanted_ring[7];
    memcpy(data, low_source + 16, bytes);
    if (bytes & 15u) memset(data + bytes, 0, 16 - (bytes & 15u));
    low_packet_word(7 + qwords, 1ull | (1ull << 15) | (1ull << 60), 14);
    low_packet_word(8 + qwords, 0, 63);
}
static void low_make_pattern(unsigned count, unsigned kind, u32 n)
{
    unsigned i;
    u32 alignment = 1;
    while (alignment < n && alignment < 32) alignment *= 2;
    memset(low_bitmap, 1, sizeof low_bitmap);
    if (!count) return;
    if (count >= 8192 || kind == 2)
    {
        for (i = 0; i < count; i++) low_bitmap[2 * i + 1] = 0;
        return;
    }
    if (kind == 0 || kind == 3)
    {
        const u32 first = kind == 0 ? 32u : 3u;
        memset(low_bitmap + first, 0, n + alignment + 1);
        for (i = 1; i < count; i++) low_bitmap[512 + 2 * i] = 0;
    }
    else
    {
        for (i = 0; i + 1 < count; i++) low_bitmap[2 * i + 1] = 0;
        memset(low_bitmap + 2 * count + 3, 0, n + alignment + 1);
    }
}
static void low_active_ranges(void *state, int tag)
{
    unsigned p = 1, count = 0, occupied = 0;
    while (p < VRAM_BLOCKS)
    {
        unsigned first;
        if (low_bitmap[p]) { occupied++; p++; continue; }
        first = p;
        while (p < VRAM_BLOCKS && !low_bitmap[p]) p++;
        low_set_range(state, tag, count++, first, p - first);
    }
    ((__typeof__(H) *)state)->free_n = (int)count;
    ((__typeof__(H) *)state)->used_blocks = occupied;
}
static void low_prepare(int tag, unsigned count, unsigned kind, u32 width, u32 height)
{
    unsigned i, p, records = 0;
    u32 alignment = 1;
    low_width = width; low_height = height; low_blocks = low_footprint(width, height);
    while (alignment < low_blocks && alignment < 32) alignment *= 2;
#ifdef NEXT10_LOW_EE
    low_globals_reset();
#endif
    memset(&H, 0, sizeof H); memset(&TX, 0, sizeof TX); memset(&cfg, 0, sizeof cfg);
    memset(&V, 0, sizeof V);
#ifdef NEXT10_LOW_EE
    memset(&VU, 0, sizeof VU);
#endif
    memset(LOW_UP_HIST, 0, sizeof LOW_UP_HIST);
    low_messages = 0;
    memset(low_records, 0xA5, sizeof low_records);
    memset(low_owners, 0, sizeof low_owners);
    memset(low_ring, 0x96, sizeof low_ring);
    for (i = 0; i < sizeof low_source; i++) low_source[i] = (u8)(i * 37u + width + height);
    low_make_pattern(count, kind, low_blocks);
    for (p = 1; p < VRAM_BLOCKS;)
    {
        unsigned first;
        texrec_t *r;
        if (!low_bitmap[p]) { p++; continue; }
        first = p;
        while (p < VRAM_BLOCKS && low_bitmap[p]) low_owners[p++] = (u16)(records + 1);
        r = &low_records[records]; memset(r, 0, sizeof *r);
        r->blk = first; r->nblk = p - first; r->used = r->pin = 1; r->wgt = 0xFFFFFFFFu;
        r->prev = records ? (int)records - 1 : -1; r->next = (int)records + 1;
        r->share = r->sec = r->sec_next = r->pnext = r->ppar = -1;
        records++;
    }
    if (records) low_records[records - 1].next = -1;
    H.pool_base = 1; H.pool_blocks = VRAM_BLOCKS - 1; H.frame_no = 17;
    H.rec = low_records; H.rec_n = (int)records; H.rec_cap = LOW_RECORDS; H.rec_free = -1;
    H.lru_head = records ? 0 : -1; H.lru_tail = records ? (int)records - 1 : -1; H.lru_seq = 10000;
    H.cur_tex = -1; H.ring[0] = low_ring; H.cur = 0; H.wr = 1; H.serial = 57;
    blk_owner_p = &low_owners;
    for (i = 0; i < VRAM_RANGES; i++) low_set_range(&H, tag, i, i * 3u, i * 5u);
    low_active_ranges(&H, tag);
    low_initial_count = (unsigned)H.free_n;
#ifndef NEXT10_LOW_EE
    cap = low_ring + 1; cap_n = 0; ps2hwd_dbg_flags = 0; ps2hwd_fx2 = 0;
#endif
    memset(&low_mip, 0, sizeof low_mip); low_mip.format = GL_TEXFMT_P_8;
    low_mip.data = low_source + 16; low_mip.width = (u16)width; low_mip.height = (u16)height;
    low_initial_h = low_wanted_h = H; low_wanted_mip = low_mip;
    memcpy(low_wanted_records, low_records, sizeof low_records);
    memcpy(low_wanted_owners, low_owners, sizeof low_owners);
    memcpy(low_wanted_ring, low_ring, sizeof low_ring);
    memset(&low_wanted_tx, 0, sizeof low_wanted_tx);
    low_success = 0; low_block = 0;
    for (p = 1; p < VRAM_BLOCKS;)
    {
        unsigned first, chosen;
        if (low_bitmap[p]) { p++; continue; }
        first = p;
        while (p < VRAM_BLOCKS && !low_bitmap[p]) p++;
        chosen = (first + alignment - 1u) & ~(alignment - 1u);
        if (chosen + low_blocks <= p)
        {
            low_success = 1; low_block = chosen; memset(low_bitmap + chosen, 1, low_blocks); break;
        }
    }
    if (low_success)
    {
        texrec_t *r = &low_wanted_records[records];
        const unsigned tw = low_power(width), th = low_power(height);
        memset(r, 0, sizeof *r); r->used = 1; r->owner = &low_mip;
        r->prev = r->share = r->sec = r->sec_next = r->pnext = r->ppar = -1;
        r->next = low_wanted_h.lru_head; r->seq = ++low_wanted_h.lru_seq;
        if (low_wanted_h.lru_head != -1) low_wanted_records[low_wanted_h.lru_head].prev = (int)records;
        else low_wanted_h.lru_tail = (int)records;
        low_wanted_h.lru_head = (int)records; low_wanted_h.rec_n++;
        r->blk = low_block; r->nblk = low_blocks; r->w = r->sw = (u16)width; r->h = r->sh = (u16)height;
        r->tw = (u8)tw; r->th = (u8)th; r->tbw = (u8)(((width + 127) / 128) * 2); r->psm = 19; r->clut = 1;
        r->stamp = H.frame_no; r->uw = width; r->vh = height;
        r->npot_x = (width & (width - 1u)) != 0; r->npot_y = (height & (height - 1u)) != 0;
        r->region = (u8)(r->npot_x | (r->npot_y << 1));
        r->sxs = (float)width / (float)(1u << tw); r->sys = (float)height / (float)(1u << th);
        low_active_ranges(&low_wanted_h, tag);
        low_wanted_h.serial += 2; low_wanted_h.wr += 8 + (width * height + 15) / 16;
        low_wanted_h.st.uploads++; low_wanted_h.st.upload_bytes += width * height;
        low_wanted_mip.downloaded = records + 1;
        for (p = low_block; p < low_block + low_blocks; p++) low_wanted_owners[p] = (u16)(records + 1);
        low_wanted_tx.up_n[2] = 1; low_wanted_tx.up_blk[2] = low_blocks;
        low_packet_oracle();
    }
    else
    {
        low_wanted_tx.alloc_fail = 1;
        if (H.pool_blocks - H.used_blocks >= low_blocks)
        {
            low_wanted_tx.alloc_frag = 1; low_wanted_tx.alloc_frag_blk = low_blocks;
        }
    }
}

static int low_check(int tag, int returned)
{
    unsigned i;
    const u32 history_slot = (u32)(((uintptr_t)&low_mip >> 4) * 2654435761u) >> 21;
    H.st.cyc_tex = 0; /* Observational CP0 statistic, not rendered data; timer is checked independently. */
    if (strcmp(tex_fail_why, low_success ? "ok" : "no VRAM block (pool)")) goto fail;
    if (returned != (low_success ? low_initial_h.rec_n : -1)) goto fail;
    if (memcmp(&H, &low_wanted_h, sizeof H)) { printf("LOW field H\n"); goto fail; }
    if (memcmp(low_records, low_wanted_records, sizeof low_records)) { printf("LOW field records\n"); goto fail; }
    if (memcmp(low_ring, low_wanted_ring, sizeof low_ring)) { printf("LOW field packet\n"); goto fail; }
    if (memcmp(low_owners, low_wanted_owners, sizeof low_owners)) { printf("LOW field owners\n"); goto fail; }
    if (memcmp(&low_mip, &low_wanted_mip, sizeof low_mip)) { printf("LOW field mipmap\n"); goto fail; }
    if (memcmp(&TX, &low_wanted_tx, sizeof TX)) { printf("LOW field TX\n"); goto fail; }
    for (i = 0; i < 2048; i++)
        if (LOW_UP_HIST[i].m != (low_success && i == history_slot ? &low_mip : NULL) ||
            LOW_UP_HIST[i].frame1 != (low_success && i == history_slot ? H.frame_no + 1 : 0)) goto fail;
    for (i = 0; i < sizeof low_source; i++)
        if (low_source[i] != (u8)(i * 37u + low_width + low_height)) goto fail;
#ifdef NEXT10_LOW_EE
    if (!low_globals_check() || low_messages != (unsigned)!low_success) goto fail;
    for (i = 0; i < sizeof V; i++) if (V[i]) goto fail;
    for (i = 0; i < sizeof VU; i++) if (VU[i]) goto fail;
    if (resource_error_handler || ps2hwd_dbg_flags || blk_owner_p != &low_owners) goto fail;
    for (i = 0; i < sizeof cfg; i++) if (((const u8 *)&cfg)[i]) goto fail;
#endif
    low_digest = (low_digest ^ low_block ^ low_blocks ^ (u32)returned ^ (u32)H.free_n ^ H.used_blocks) * 16777619u;
    low_cases++;
    return 1;
fail:
    printf("GS LOW10 FAIL tag=%d case=%u dimensions=%ux%u ranges=%u result=%d want=%d\n",
           tag, (unsigned)low_cases, (unsigned)low_width, (unsigned)low_height, low_initial_count, returned,
           low_success ? low_initial_h.rec_n : -1);
    return 0;
}
static int low_call(int tag)
{
#ifdef NEXT10_LOW_EE
    int (*volatile function)(GLMipmap_t *) = tag ? cur_tex_upload : base_tex_upload;
    return function(&low_mip);
#else
    (void)tag; return tex_upload(&low_mip);
#endif
}

#ifdef NEXT10_LOW_EE
static u32 low_cycles(void) { u32 v; __asm__ volatile("mfc0 %0,$9" : "=r"(v) : : "memory"); return v; }
static u32 low_begin(void) { u32 v; __asm__ volatile("mfc0 %0,$12\n\tdi\n\tsync.p" : "=r"(v) : : "memory"); return v; }
static void low_end(u32 v) { if (v & 0x10000u) __asm__ volatile("ei\n\tsync.p" : : : "memory"); }
#endif

int main(void)
{
    static const unsigned counts[] = {0,1,2,7,127,8192};
    static const unsigned dims[][2] = {{1,1},{64,16}};
    unsigned ci, di, kind, row = 0;
    int tag, first_tag = 0, last_tag = 1;
#ifdef NEXT10_LOW_EE
    int cold, rep, order, returned;
    low_globals_init();
#else
#ifdef NEXT10_LOW_CURRENT
    first_tag = last_tag = 1;
#else
    first_tag = last_tag = 0;
#endif
#endif
    for (ci = 0; ci < sizeof counts / sizeof counts[0]; ci++)
        for (di = 0; di < sizeof dims / sizeof dims[0]; di++)
            for (kind = 0; kind < 4; kind++)
                for (tag = first_tag; tag <= last_tag; tag++)
                {
                    low_prepare(tag, counts[ci], kind, dims[di][0], dims[di][1]);
                    if (!low_check(tag, low_call(tag))) return 1;
                }
    printf("GS LOW10 EXACT cases=%u digest=%08x\n", (unsigned)low_cases, (unsigned)low_digest);
#ifdef NEXT10_LOW_EE
    for (cold = 0; cold < 2; cold++)
        for (ci = 0; ci < sizeof counts / sizeof counts[0]; ci++)
            for (di = 0; di < sizeof dims / sizeof dims[0]; di++)
                for (kind = 0; kind < 4; kind++)
                {
                    for (rep = 0; rep < 9; rep++)
                    {
                        u32 times[2] = {0,0};
                        for (order = 0; order < 2; order++)
                        {
                            u32 before, previous;
                            tag = order ^ (rep & 1);
                            low_prepare(tag, counts[ci], kind, dims[di][0], dims[di][1]);
                            if (cold) { FlushCache(0); FlushCache(1); FlushCache(2); }
                            previous = low_begin(); before = low_cycles(); returned = low_call(tag);
                            times[tag] = low_cycles() - before; low_end(previous);
                            if (!low_check(tag, returned) || times[tag] >= 0x80000000u) return 1;
                        }
                        printf("LOW10 TIMING row=%u count=%u dim=%u kind=%u cache=%d rep=%d old=%u new=%u\n",
                               row, counts[ci], di, kind, cold, rep, (unsigned)times[0], (unsigned)times[1]);
                    }
                    row++;
                }
    if (row != 96) return 1;
#else
    (void)row;
#endif
    printf("GS LOW10 DONE\n");
    return 0;
}
