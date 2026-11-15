#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <math.h>
#include "shim.h"
#include "ps2_hwd_dbg.h"
#define CONS_WARNING 1
#define CONS_Alert(...) ((void)0)
#define CONS_Printf(...) ((void)0)
#include "ps2_hw_priv.inc"
#include "ps2_hw_regs.inc"
#include "ps2_hw_vif.inc"
#undef VIF1_FBRST
static u32 reset_register;
#define VIF1_FBRST (&reset_register)
static const u8 dummy_microcode[32] = {0};
#define VU1Draw_CodeStart dummy_microcode
#define VU1Draw_CodeEnd (dummy_microcode + 32)
#define VU1Draw_EntryInit dummy_microcode
#define VU1Draw_EntryDraw (dummy_microcode + 8)
#define VU1Spr_CodeStart dummy_microcode
#define VU1Spr_CodeEnd dummy_microcode
#define VU1Spr_EntryDraw dummy_microcode
static qw_t boot_packet[256];
static u32 boot_count;
static int vif1_send_wait(const void *p, u32 n)
{
	if (n > 256) abort();
	memcpy(boot_packet, p, n * sizeof(qw_t)); boot_count = n; return 1;
}
static void pk_flush(void) { abort(); }
static struct { texrec_t *r; } P;
#define DMAT_CNT 1ull
#include "executed.inc"
static qw_t memory[1024], planes[12];
static unsigned checks, packets, writes;
static void check(int ok, const char *what)
{
	checks++;
	if (!ok) { fprintf(stderr, "VU P4 memory proof failed: %s\n", what); exit(1); }
}
static void unpack(u32 command, const qw_t *data, u32 available)
{
	u32 count = (command >> 16) & 255u, address = command & 1023u;
	check((command >> 24) == 0x6cu && !(command & 0x8000u), "actual UNPACK is absolute V4_32");
	check(count && count <= available && address + count <= 1024, "actual UNPACK bounds");
	memcpy(memory + address, data, count * sizeof(qw_t)); packets++; writes += count;
}
static void replay_boot(void)
{
	u32 i;
	for (i = 0; i < boot_count; i++)
		if ((boot_packet[i].w[3] >> 24) == 0x6cu)
		{
			u32 count = (boot_packet[i].w[3] >> 16) & 255u;
			unpack(boot_packet[i].w[3], boot_packet + i + 1, boot_count - i - 1); i += count;
		}
}
static void replay_constants(void)
{
	u32 i = 0;
	while (i < H.run)
	{
		const qw_t *tag = H.ring[0] + i;
		u32 count = (u32)tag->d[0] & 0xffffu;
		check(i + 1 + count <= H.run, "actual DMA CNT payload bounds");
		if ((tag->w[3] >> 24) == 0x6cu) unpack(tag->w[3], tag + 1, count);
		i += 1 + count;
	}
	check(i == H.run, "entire actual constant DMA stream consumed");
}
int main(void)
{
	const u32 ranges[][2] = {{0, VU_PLANES}, {M_PL_VEC, M_PL_BITS + 6}, {VU_ROWTAB, VU_ROWTAB + 32}, {VU_BASE, VU_BASE + 2 * VU_OFFSET}, {M_OUT0, M_OUT1 + M_OUT1 - M_OUT0}, {M_LISTA, M_LISTC + M_LISTB - M_LISTA}, {M_RGBTMP, M_DBG + 1}};
	texrec_t record;
	u32 band, rows, what, i;
	memset(&record, 0, sizeof record); record.psm = PSM_T4; P.r = &record;
	check(vu1_boot(), "actual VU boot emits its plane table"); replay_boot();
	memcpy(planes, memory + VU_PLANES, sizeof planes);
	check(planes[8].w[0] == 0x80 && planes[8].w[1] == 0x205, "boot x-minus plane masks came from actual packet");
	H.ring[0] = calloc(BUF_QW, sizeof(qw_t)); check(H.ring[0] != NULL, "ring allocation");
	VU.rowmode = 1; VU.rlo = 0; VU.rhi = 31;
	for (band = 0; band < 16; band++) for (rows = 0; rows < 2; rows++) for (what = 0; what < 2; what++)
	{
		record.csa = (u8)band; H.cur = 0; H.wr = 1; H.run = 0;
		vu_consts_send((int)rows, (int)what); replay_constants();
		check(!memcmp(planes, memory + VU_PLANES, sizeof planes), "P4 constant send preserves all twelve boot plane quadwords");
		check(M_P4_TEX2 == VU_P4_TEX2, "C and VSM select the same actual address");
		check(memory[VU_P4_TEX2].d[0] == ((u64)PSM_T4 << 20 | (u64)band << 56) && memory[VU_P4_TEX2].d[1] == GSR_TEX2_1, "actual selector payload at the VSM read address");
		for (i = 0; i < sizeof ranges / sizeof ranges[0]; i++)
			check(VU_P4_TEX2 < ranges[i][0] || VU_P4_TEX2 >= ranges[i][1], "selector disjoint from every actual polygon/sprite region");
	}
	free(H.ring[0]);
	printf("{\"checks\":%u,\"constant_cases\":64,\"executed_UNPACK_packets\":%u,\"written_quadwords\":%u,\"selector_address\":%u,\"boot_plane_quadwords_unchanged\":12}\n", checks, packets, writes, VU_P4_TEX2);
	return 0;
}
