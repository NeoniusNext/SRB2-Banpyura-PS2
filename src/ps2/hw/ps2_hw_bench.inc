// PS2 GS hardware renderer driver: -hwbench, what the instructions cost on the machine the numbers are made on (OPT11 round 2, VU2). Included by ps2_hwd.c.
//
// The cycle counts of the profile (COP0 Count) are only as good as the model of the CPU that makes them: PCSX2 does not model the caches, and charges the
// floating point and multiply instructions by a table. This prints, for loops of 1000 passes of one kind of operation, the cycles a pass costs over an empty loop,
// so that the code that is hot in the profile can be written for the machine the numbers come from (HWBENCH lines in the log).

#ifdef __mips__
#define BN 1000

static u32 bench_buf[256] __attribute__((aligned(64)));
static float bench_f[64] __attribute__((aligned(64)));

static u32 bench_time(int kind)
{
	u32 t0, t1, i;
	float a = 1.0001f, b = 0.9999f, c = 1.0f, d = 1.0f, e = 1.0f, g = 1.0f;
	u32 x = 0x811c9dc5u, y = 12345u;
	const float *pf = bench_f;
	u32 *pb = bench_buf;
	volatile u32 sink = 0;

	t0 = cyc();
	for (i = 0; i < BN; i++)
	{
		switch (kind)
		{
			case 0: // empty
				__asm__ volatile("" : "+r"(x));
				break;
			case 1: // dependent float multiplies (8)
				__asm__ volatile(
					"mul.s %0,%0,%1\n mul.s %0,%0,%1\n mul.s %0,%0,%1\n mul.s %0,%0,%1\n"
					"mul.s %0,%0,%1\n mul.s %0,%0,%1\n mul.s %0,%0,%1\n mul.s %0,%0,%1\n"
					: "+f"(c) : "f"(b));
				break;
			case 2: // independent float multiplies (8, four chains)
				__asm__ volatile(
					"mul.s %0,%0,%4\n mul.s %1,%1,%4\n mul.s %2,%2,%4\n mul.s %3,%3,%4\n"
					"mul.s %0,%0,%4\n mul.s %1,%1,%4\n mul.s %2,%2,%4\n mul.s %3,%3,%4\n"
					: "+f"(c), "+f"(d), "+f"(e), "+f"(g) : "f"(b));
				break;
			case 3: // dependent float adds (8)
				__asm__ volatile(
					"add.s %0,%0,%1\n add.s %0,%0,%1\n add.s %0,%0,%1\n add.s %0,%0,%1\n"
					"add.s %0,%0,%1\n add.s %0,%0,%1\n add.s %0,%0,%1\n add.s %0,%0,%1\n"
					: "+f"(c) : "f"(b));
				break;
			case 4: // independent float adds (8)
				__asm__ volatile(
					"add.s %0,%0,%4\n add.s %1,%1,%4\n add.s %2,%2,%4\n add.s %3,%3,%4\n"
					"add.s %0,%0,%4\n add.s %1,%1,%4\n add.s %2,%2,%4\n add.s %3,%3,%4\n"
					: "+f"(c), "+f"(d), "+f"(e), "+f"(g) : "f"(b));
				break;
			case 5: // float divides (4, dependent)
				__asm__ volatile(
					"div.s %0,%0,%1\n div.s %0,%0,%1\n div.s %0,%0,%1\n div.s %0,%0,%1\n"
					: "+f"(c) : "f"(a));
				break;
			case 6: // float square roots (4, dependent)
				__asm__ volatile(
					"sqrt.s %0,%0\n sqrt.s %0,%0\n sqrt.s %0,%0\n sqrt.s %0,%0\n"
					: "+f"(c));
				break;
			case 7: // integer multiplies (8, dependent, as the FNV hash)
				__asm__ volatile(
					"mult %0,%0,%1\n mult %0,%0,%1\n mult %0,%0,%1\n mult %0,%0,%1\n"
					"mult %0,%0,%1\n mult %0,%0,%1\n mult %0,%0,%1\n mult %0,%0,%1\n"
					: "+r"(x) : "r"(y) : "hi", "lo");
				break;
			case 8: // five words copied with lw/sw (8 times: 40 words)
				__asm__ volatile(
					"lw $8,0(%0)\n sw $8,0(%1)\n lw $8,4(%0)\n sw $8,4(%1)\n lw $8,8(%0)\n sw $8,8(%1)\n lw $8,12(%0)\n sw $8,12(%1)\n lw $8,16(%0)\n sw $8,16(%1)\n"
					"lw $8,20(%0)\n sw $8,20(%1)\n lw $8,24(%0)\n sw $8,24(%1)\n lw $8,28(%0)\n sw $8,28(%1)\n lw $8,32(%0)\n sw $8,32(%1)\n lw $8,36(%0)\n sw $8,36(%1)\n"
					: : "r"(pb), "r"(pb + 64) : "$8", "memory");
				break;
			case 9: // lq/sq (8 pairs)
				__asm__ volatile(
					"lq $8,0(%0)\n sq $8,0(%1)\n lq $8,16(%0)\n sq $8,16(%1)\n lq $8,32(%0)\n sq $8,32(%1)\n lq $8,48(%0)\n sq $8,48(%1)\n"
					"lq $8,64(%0)\n sq $8,64(%1)\n lq $8,80(%0)\n sq $8,80(%1)\n lq $8,96(%0)\n sq $8,96(%1)\n lq $8,112(%0)\n sq $8,112(%1)\n"
					: : "r"(pb), "r"(pb + 64) : "$8", "memory");
				break;
			case 10: // lwc1/swc1 (8 pairs)
				__asm__ volatile(
					"lwc1 $f4,0(%0)\n swc1 $f4,0(%1)\n lwc1 $f4,4(%0)\n swc1 $f4,4(%1)\n lwc1 $f4,8(%0)\n swc1 $f4,8(%1)\n lwc1 $f4,12(%0)\n swc1 $f4,12(%1)\n"
					"lwc1 $f4,16(%0)\n swc1 $f4,16(%1)\n lwc1 $f4,20(%0)\n swc1 $f4,20(%1)\n lwc1 $f4,24(%0)\n swc1 $f4,24(%1)\n lwc1 $f4,28(%0)\n swc1 $f4,28(%1)\n"
					: : "r"(pb), "r"(pb + 64) : "$f4", "memory");
				break;
			case 11: // memcpy 80 bytes, destination aligned to 4 only
				memcpy((char *)pb + 516, (const char *)pb + 4, 80);
				break;
			case 12: // VU0 macro: matrix times vector (4 instructions) and a store, 4 vectors
				__asm__ volatile(
					"lqc2 $vf1,0(%0)\n lqc2 $vf2,16(%0)\n lqc2 $vf3,32(%0)\n lqc2 $vf4,48(%0)\n lqc2 $vf5,64(%0)\n"
					"vmulax $ACC,$vf1,$vf5\n vmadday $ACC,$vf2,$vf5\n vmaddaz $ACC,$vf3,$vf5\n vmaddw $vf6,$vf4,$vf5\n"
					"vmulax $ACC,$vf1,$vf5\n vmadday $ACC,$vf2,$vf5\n vmaddaz $ACC,$vf3,$vf5\n vmaddw $vf7,$vf4,$vf5\n"
					"vmulax $ACC,$vf1,$vf5\n vmadday $ACC,$vf2,$vf5\n vmaddaz $ACC,$vf3,$vf5\n vmaddw $vf8,$vf4,$vf5\n"
					"vmulax $ACC,$vf1,$vf5\n vmadday $ACC,$vf2,$vf5\n vmaddaz $ACC,$vf3,$vf5\n vmaddw $vf9,$vf4,$vf5\n"
					"sqc2 $vf6,0(%1)\n sqc2 $vf7,16(%1)\n sqc2 $vf8,32(%1)\n sqc2 $vf9,48(%1)\n"
					: : "r"(pb), "r"(pb + 128) : "memory");
				break;
			case 13: // the same product on the FPU (4 vectors of 4 x 4 multiply-adds, scalar madd.s)
				__asm__ volatile(
					"lwc1 $f4,0(%0)\n lwc1 $f5,4(%0)\n lwc1 $f6,8(%0)\n lwc1 $f7,12(%0)\n"
					"mula.s $f4,$f5\n madd.s $f8,$f6,$f7\n mula.s $f4,$f5\n madd.s $f9,$f6,$f7\n mula.s $f4,$f5\n madd.s $f10,$f6,$f7\n mula.s $f4,$f5\n madd.s $f11,$f6,$f7\n"
					"mula.s $f4,$f5\n madd.s $f8,$f6,$f7\n mula.s $f4,$f5\n madd.s $f9,$f6,$f7\n mula.s $f4,$f5\n madd.s $f10,$f6,$f7\n mula.s $f4,$f5\n madd.s $f11,$f6,$f7\n"
					: : "r"(pb) : "$f4", "$f5", "$f6", "$f7", "$f8", "$f9", "$f10", "$f11", "memory");
				break;
			case 14: // an indirect call of a function that does nothing
				sink += x;
				break;
			case 15: // dependent loads (pointer chase through a ring of 16 words in cache)
				__asm__ volatile(
					"lw %0,0(%0)\n lw %0,0(%0)\n lw %0,0(%0)\n lw %0,0(%0)\n lw %0,0(%0)\n lw %0,0(%0)\n lw %0,0(%0)\n lw %0,0(%0)\n"
					: "+r"(pb));
				break;
			case 16: // float compare and branch (8)
				__asm__ volatile(
					"c.lt.s %1,%2\n bc1t 1f\n nop\n1: c.lt.s %1,%2\n bc1t 2f\n nop\n2: c.lt.s %1,%2\n bc1t 3f\n nop\n3: c.lt.s %1,%2\n bc1t 4f\n nop\n4:\n"
					: "+r"(x) : "f"(a), "f"(b));
				break;
			case 17: // float to int and back (4)
				__asm__ volatile(
					"cvt.w.s %0,%0\n cvt.s.w %0,%0\n cvt.w.s %0,%0\n cvt.s.w %0,%0\n cvt.w.s %0,%0\n cvt.s.w %0,%0\n cvt.w.s %0,%0\n cvt.s.w %0,%0\n"
					: "+f"(c));
				break;
			default:
				break;
		}
		if (kind == 15 && i == 0)
			pb = bench_buf;
	}
	t1 = cyc();
	(void)pf;
	return t1 - t0;
}

static void PS2HWD_Bench(void)
{
	static const char *const names[] = {"empty loop", "fmul dependent x8", "fmul independent x8", "fadd dependent x8", "fadd independent x8", "fdiv dependent x4", "fsqrt dependent x4",
		"imul dependent x8", "lw/sw x20 pairs", "lq/sq x8 pairs", "lwc1/swc1 x8 pairs", "memcpy 80 B (unaligned)", "VU0 4 vectors M*v", "FPU 8x(mul+madd)", "volatile store", "lw dependent x8",
		"c.lt+bc1t x4", "cvt x8"};
	u32 empty;
	int k;

	for (k = 0; k < 16; k++)
		bench_buf[k] = (u32)(uintptr_t)&bench_buf[(k + 1) & 15]; // a ring for the pointer chase
	for (k = 0; k < 64; k++)
		bench_f[k] = 1.0f;
	empty = bench_time(0);
	for (k = 0; k < 18; k++)
	{
		u32 t;

		if (k == 15)
			for (int j = 0; j < 16; j++)
				bench_buf[j] = (u32)(uintptr_t)&bench_buf[(j + 1) & 15];
		t = bench_time(k);
		I_OutputMsg("HWBENCH %-28s %6.2f cycles a pass (over the empty loop)\n", names[k], ((double)t - (double)empty) / BN);
	}
}
#else
static void PS2HWD_Bench(void) {}
#endif
