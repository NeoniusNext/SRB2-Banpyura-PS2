/* Current engine hooks missing from the historical standalone hw_test fixture.
 * Test-only: the fixture owns texture storage and does not create a zone arena. */
#include "doomdef.h"
#include "z_zone.h"
#include "ps2/hw/ps2_hwd.h"
#include "ps2/hw/ps2_hwd_dbg.h"
#include "ps2/ps2_models.h"
#include "ps2/ps2_sys.h"
#include <kernel.h>
#include <malloc.h>
#include <timer.h>

UINT32 hwr_texsig = 1;
INT32 ps2hwt_patchtag;
boolean ps2hwt_comp_old;

void Z_SetRebuildCost(void **user, UINT32 cycles) { (void)user; (void)cycles; }
void Z_SetHWCacheLRU(boolean on, size_t freemin, size_t cap) { (void)on; (void)freemin; (void)cap; }
size_t Z_ArenaFree(void) { return 32u * 1024u * 1024u; }
UINT32 Z_FrameCount(void)
{
	ps2hwd_stats_t st;
	PS2HWD_GetStats(&st, 0);
	return st.frames;
}
void *PS2Models_TryAlloc(size_t size, INT32 tag, void *user, INT32 alignbits)
{
	(void)tag; (void)user;
	return memalign((size_t)1 << alignbits, size);
}

/* Same polled kernel-clock wait and priority handoff as i_system.c, without
 * the unrelated engine timer/profile dependencies or SDK alarm library. */
void PS2_SleepUs(UINT32 us)
{
	ee_thread_status_t st;
	const s32 self = GetThreadId();
	const u64 dest = GetTimerSystemTime() + (u64)us * (kBUSCLK / 1000) / 1000;
	s32 prio = -1;

	if (ReferThreadStatus(self, &st) >= 0 && st.current_priority < 120)
	{
		prio = st.current_priority;
		ChangeThreadPriority(self, 120);
	}
	while ((s64)(dest - GetTimerSystemTime()) > 0)
		;
	if (prio >= 0)
		ChangeThreadPriority(self, prio);
}
