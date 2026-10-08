// Does the EE kernel save the VU0 (COP2) state on a thread switch?  A higher priority thread changes vf1, ACC, I and Q every 0.5 ms while the
// main thread keeps its own values and checks them in a tight loop.  Result: EETEST vu0ctx lines.
#include <kernel.h>
#include <stdio.h>
#include <string.h>
#include <tamtypes.h>
#include <timer.h>
#include <delaythread.h>

static volatile int quit, other_runs;
static char stack[16384] __attribute__((aligned(16)));
extern void *_gp;

static float pat_a[4] __attribute__((aligned(16))) = {1.0f, 2.0f, 3.0f, 4.0f};
static float pat_b[4] __attribute__((aligned(16))) = {9.0f, 9.0f, 9.0f, 9.0f};

static void other(void *arg)
{
	(void)arg;
	while (!quit)
	{
		u32 i7 = 0x40e00000u, q7 = 0x40e00000u; // 7.0f
		__asm__ volatile(
			"lqc2 $vf1, 0(%0)\n"
			"vmula.xyzw $ACC, $vf1, $vf1\n"
			"ctc2 %1, $vi21\n"
			"ctc2 %2, $vi22\n"
			"vnop\n"
			: : "r"(pat_b), "r"(i7), "r"(q7) : "memory");
		other_runs++;
		DelayThread(500);
	}
	ExitThread();
}

int main(int argc, char **argv)
{
	ee_thread_t t;
	ee_thread_status_t st;
	int tid, i, bad_vf = 0, bad_acc = 0, bad_i = 0, bad_q = 0;
	u32 i1 = 0x3fc00000u, q1 = 0x3fc00000u; // 1.5f
	(void)argc; (void)argv;
	ChangeThreadPriority(GetThreadId(), 8);
	ReferThreadStatus(GetThreadId(), &st);
	memset(&t, 0, sizeof t);
	t.func = (void *)other; t.stack = stack; t.stack_size = sizeof stack; t.gp_reg = &_gp; t.initial_priority = 5;
	tid = CreateThread(&t);
	StartThread(tid, NULL);
	__asm__ volatile(
		"lqc2 $vf1, 0(%0)\n"
		"vmula.xyzw $ACC, $vf1, $vf1\n"
		"ctc2 %1, $vi21\n"
		"ctc2 %2, $vi22\n"
		: : "r"(pat_a), "r"(i1), "r"(q1) : "memory");
	for (i = 0; i < 3000000; i++)
	{
		float v[4] __attribute__((aligned(16))), acc[4] __attribute__((aligned(16)));
		u32 ri, rq;
		__asm__ volatile(
			"sqc2 $vf1, 0(%2)\n"
			"vmaddx.xyzw $vf3, $vf0, $vf0\n"   // vf3 = ACC + vf0 * vf0.x (= ACC)
			"vnop\nvnop\nvnop\nvnop\n"
			"sqc2 $vf3, 0(%3)\n"
			"cfc2 %0, $vi21\n"
			"cfc2 %1, $vi22\n"
			: "=r"(ri), "=r"(rq) : "r"(v), "r"(acc) : "memory");
		if (v[0] != 1.0f || v[1] != 2.0f || v[2] != 3.0f || v[3] != 4.0f) bad_vf++;
		if (acc[0] != 1.0f || acc[1] != 4.0f || acc[2] != 9.0f) bad_acc++;
		if (ri != i1) bad_i++;
		if (rq != q1) bad_q++;
	}
	quit = 1;
	printf("EETEST vu0ctx other_runs=%d iterations=%d bad_vf=%d bad_acc=%d bad_I=%d bad_Q=%d (0 everywhere = the kernel preserves the state)\n", other_runs, i, bad_vf, bad_acc, bad_i, bad_q);
	printf("EETEST DONE\n");
	SleepThread();
	return 0;
}
