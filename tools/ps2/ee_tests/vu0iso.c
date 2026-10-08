// VU0 isolation between the renderer (main thread: vf1..vf18, ACC, Q, clip flag) and the audio decoder (higher priority thread: the VU0 kernels of
// src/ps2/vorbis with the save/restore protocol of ps2_vu0a.h).  The main thread keeps patterns in the registers and checks them in a tight loop while
// the audio thread runs MDCT transforms and PCM conversions in between (it preempts the main thread at arbitrary points).  Expected: 0 corrupted reads.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <kernel.h>
#include <timer.h>
#include <delaythread.h>
#include "../../../src/ps2/vorbis/mdct.c"
#include "../../../src/ps2/ps2_pcmconv.h"

static volatile int quit, audio_runs, audio_busy_skips;
static char stack[32768] __attribute__((aligned(16)));
extern void *_gp;
static float pat[18][4] __attribute__((aligned(16)));
static float in_[2048] __attribute__((aligned(16))), l_[2048] __attribute__((aligned(16))), r_[2048] __attribute__((aligned(16)));
static int16_t pcm_[4096] __attribute__((aligned(16)));
static mdct_lookup lk;

static void audio(void *arg)
{
	int i;
	(void)arg;
	mdct_init(&lk, 1024);
	for (i = 0; i < 2048; i++) { in_[i] = (float)((i * 7919) % 1000) - 500.0f; l_[i] = in_[i] * (1.0f / 1024.0f); r_[i] = -l_[i]; }
	while (!quit)
	{
		static float buf[1024] __attribute__((aligned(16)));
		memcpy(buf, in_, sizeof buf);
		mdct_backward(&lk, buf, buf);
		PS2_FloatsToS16Stereo(pcm_, l_, r_, 1024);
		audio_runs++;
		DelayThread(300);
	}
	ExitThread();
}

int main(void)
{
	ee_thread_t t;
	int tid, i, it, bad_vf = 0, bad_acc = 0, bad_q = 0, bad_clip = 0, loops = 0;
	u32 clipw, qbits, q0 = 0, clip0 = 0;
	float qv[4] __attribute__((aligned(16))), accv[4] __attribute__((aligned(16))), one[4] __attribute__((aligned(16))) = {1, 1, 1, 1}, two[4] __attribute__((aligned(16))) = {2, 3, 4, 5};
	ChangeThreadPriority(GetThreadId(), 8);
	for (i = 0; i < 18; i++) { pat[i][0] = i + 1; pat[i][1] = (i + 1) * 0.5f; pat[i][2] = (i + 1) * 0.25f; pat[i][3] = (i + 1) * 2.0f; }
	memset(&t, 0, sizeof t);
	t.func = (void *)audio; t.stack = stack; t.stack_size = sizeof stack; t.gp_reg = &_gp; t.initial_priority = 5;
	tid = CreateThread(&t);
	// registers: vf1..vf18 patterns, ACC = vf1*vf2, Q = 1/vf2.x, clip flag from vclipw
	__asm__ volatile(
		"lqc2 $vf1, 0x00(%0)\n lqc2 $vf2, 0x10(%0)\n lqc2 $vf3, 0x20(%0)\n lqc2 $vf4, 0x30(%0)\n lqc2 $vf5, 0x40(%0)\n lqc2 $vf6, 0x50(%0)\n"
		"lqc2 $vf7, 0x60(%0)\n lqc2 $vf8, 0x70(%0)\n lqc2 $vf9, 0x80(%0)\n lqc2 $vf10, 0x90(%0)\n lqc2 $vf11, 0xa0(%0)\n lqc2 $vf12, 0xb0(%0)\n"
		"lqc2 $vf13, 0xc0(%0)\n lqc2 $vf14, 0xd0(%0)\n lqc2 $vf15, 0xe0(%0)\n lqc2 $vf16, 0xf0(%0)\n lqc2 $vf17, 0x100(%0)\n lqc2 $vf18, 0x110(%0)\n"
		"vmula.xyzw $ACC, $vf1, $vf2\n"
		"vdiv $Q, $vf0w, $vf3x\n"
		"vclipw.xyz $vf4, $vf4\n"
		: : "r"(pat) : "memory");
	StartThread(tid, NULL);
	for (it = 0; it < 2500000; it++)
	{
		float v[18][4] __attribute__((aligned(16)));
		__asm__ volatile(
			"sqc2 $vf1, 0x00(%1)\n sqc2 $vf2, 0x10(%1)\n sqc2 $vf3, 0x20(%1)\n sqc2 $vf4, 0x30(%1)\n sqc2 $vf5, 0x40(%1)\n sqc2 $vf6, 0x50(%1)\n"
			"sqc2 $vf7, 0x60(%1)\n sqc2 $vf8, 0x70(%1)\n sqc2 $vf9, 0x80(%1)\n sqc2 $vf10, 0x90(%1)\n sqc2 $vf11, 0xa0(%1)\n sqc2 $vf12, 0xb0(%1)\n"
			"sqc2 $vf13, 0xc0(%1)\n sqc2 $vf14, 0xd0(%1)\n sqc2 $vf15, 0xe0(%1)\n sqc2 $vf16, 0xf0(%1)\n sqc2 $vf17, 0x100(%1)\n sqc2 $vf18, 0x110(%1)\n"
			"vmaddx.xyzw $vf19, $vf0, $vf0\n"   // vf19 = ACC + vf0 * vf0.x (vf0.x = 0): ACC
			"vnop\nvnop\nvnop\nvnop\n"
			"sqc2 $vf19, 0(%2)\n"
			"vaddq.xyzw $vf20, $vf0, $Q\n"      // vf20 = vf0 + Q
			"vnop\nvnop\nvnop\nvnop\n"
			"sqc2 $vf20, 0(%3)\n"
			"cfc2 %0, $vi18\n"
			: "=r"(clipw) : "r"(v), "r"(accv), "r"(qv) : "memory");
		for (i = 0; i < 18; i++) if (v[i][0] != pat[i][0] || v[i][1] != pat[i][1] || v[i][2] != pat[i][2] || v[i][3] != pat[i][3]) bad_vf++;
		// ACC = vf1 * vf2 = pat[0] * pat[1] lane-wise, + vf0.x*vf0 (0) -> x,y,z same; w: + 1*0 = same
		if (accv[0] != pat[0][0] * pat[1][0] || accv[1] != pat[0][1] * pat[1][1] || accv[2] != pat[0][2] * pat[1][2]) bad_acc++;
		memcpy(&qbits, &qv[0], 4);
		if (it == 0) { q0 = qbits; clip0 = clipw; }
		else { if (qbits != q0) bad_q++; if (clipw != clip0) bad_clip++; }
		loops++;
		(void)one; (void)two;
	}
	quit = 1;
	printf("EETEST vu0iso: loops=%d audio_runs=%d corrupted vf reads=%d acc=%d q=%d clip=%d\n", loops, audio_runs, bad_vf, bad_acc, bad_q, bad_clip);
	printf("EETEST DONE\n");
	SleepThread();
	return 0;
}
