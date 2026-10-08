// PS2 GS hardware renderer driver: VIF1 as the transport of the GIF stream (included by ps2_hwd.c before ps2_hw_gs.inc). PS2-HW-44 (OPT9, HG).
//
// With V.on the chain of every ring buffer (ps2_hw_gs.inc) is read by DMA channel 1 instead of channel 2: each tag carries two VIF codes in its upper
// 64 bits (CHCR.TTE = 1, the tag data go to the VIF). A run of GIF data is sent as FLUSH + DIRECT n (PATH2); a run of VU1 data as UNPACK, and a
// zero length tag starts the VU1 program (MSCAL). Everything the driver emits stays strictly ordered, GS state, uploads and draws alike; the VU1
// program (ps2_hw_vu1.inc, vu1/ps2_hw_vu1.vsm) writes its GIF packets with XGKICK (PATH1), and FLUSH in front of every DIRECT run keeps the order
// between the two. V.on is chosen at start (-hwvu1); without it the ring is exactly what it was.

#define HWDBG_VU1 0x4000000 // (OPT9/OPT10: switched VIF1 + VU1 on; since OPT11 they are the default and this bit does nothing)
#define HWDBG_NOVU1 0x10000000 // OPT11 (PS2-HW-100): -hwdbg 268435456: no VIF1 / VU1 path, the EE transforms, clips, cuts and packs every polygon (A/B and the fallback for a GS that has no VU1)
#define HWDBG_NOLIGHTMERGE 0x08000000 // OPT11 (PS2-HW-106): -hwdbg 134217728: the light level of the sector stays in the key of a batch (A/B of the merge)
static int vu_noretarget, vu_nobretarget, vu_norecord, vu_bretmask = 7; // (-hwnobretarget: the batches only) // OPT11 (PS2-HW-107): -hwnoretarget: a draw of the plan of the last VU1 draw with another texture makes its own plan (A/B of the retarget)
#define HWDBG_NOUP 0x80000000u // -hwdbg -2147483648 (bit 31: the bits 0x8000000 and 0x10000000 are HT's LODDBG and IMMDBG). MEASUREMENT ONLY (OPT10 HG): every texture is one resident 256x256 dummy, nothing is uploaded: the CPU cost of the geometry without the texture traffic. The picture is wrong.

#define D1_CHCR ((volatile u32 *)0x10009000)
#define D1_MADR ((volatile u32 *)0x10009010)
#define D1_QWC ((volatile u32 *)0x10009020)
#define D1_TADR ((volatile u32 *)0x10009030)
#define VIF1_STAT ((volatile u32 *)0x10003C00)
#define VIF1_FBRST ((volatile u32 *)0x10003C10)
#define VU1_CODE ((volatile u64 *)0x11008000)
#define VU1_DATA ((volatile u32 *)0x1100C000)

#define VIF_NOP 0u
#define VIF_FLUSH 0x11000000u
#define VIF_STCYCL(cl, wl) (0x01000000u | ((u32)(wl) << 8) | (u32)(cl))
#define VIF_OFFSET(a) (0x02000000u | (u32)(a))
#define VIF_BASE(a) (0x03000000u | (u32)(a))
#define VIF_MSCAL(a) (0x14000000u | (u32)(a))
#define VIF_MPG(num, addr) (0x4A000000u | (((u32)(num) & 255u) << 16) | (u32)(addr))
#define VIF_DIRECT(n) (0x50000000u | ((u32)(n) & 0xFFFFu))
#define VIF_UNPACK_V4_32(addr, num, flg) (0x6C000000u | (((u32)(num) & 255u) << 16) | ((u32)(flg) << 15) | (u32)(addr))

#define VIF_DIRECT_MAX 65535u // qwords one DIRECT can carry

static struct
{
	int on; // the stream goes through VIF1
	int ready; // microcode uploaded
	int dmac; // handler id of DMAC_VIF1
	int dmac_set; // the handler is installed
	int chunk_open; // a VU1 data run is open (ps2_hw_vu1.inc)
} V;

// the VIF codes of the tag of a run of GIF data of n quadwords: wait for VU1 and its GIF packet, then send n quadwords through PATH2
static inline u64 vif_gif_codes(u32 n)
{
	return (u64)VIF_FLUSH | ((u64)VIF_DIRECT(n) << 32);
}

#define DCH_CHCR (*(V.on ? D1_CHCR : D2_CHCR))

static int vu1_boot(void); // upload the microcode, set the double buffer (ps2_hw_vu1.inc)
static void vu1_shutdown(void);
