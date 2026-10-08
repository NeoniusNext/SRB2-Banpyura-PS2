// Is VU0 vadd/vsub/vmul/vmulbc bit-equal to the EE FPU add.s/sub.s/mul.s?  (PCSX2 here; the same question for the real console is not answered by this test.)
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <kernel.h>

static float a[4] __attribute__((aligned(16))), b[4] __attribute__((aligned(16))), r[4] __attribute__((aligned(16)));
static float fa[4096], fb[4096];

static uint32_t bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

int main(void)
{
	unsigned seed = 7;
	int i, op, bad[4] = {0, 0, 0, 0}, first_shown[4] = {0, 0, 0, 0}, total = 0;
	static const char *names[4] = {"vadd", "vsub", "vmul", "vmulx"};
	for (i = 0; i < 4096; i++)
	{
		int e1, e2;
		seed = seed * 1664525u + 1013904223u; e1 = (seed >> 24) % 40 - 20;
		seed = seed * 1664525u + 1013904223u; e2 = (seed >> 24) % 40 - 20;
		seed = seed * 1664525u + 1013904223u; fa[i] = ((int)(seed >> 8) - 8388608) * (1.0f / 8388608.0f) * (e1 > 0 ? (float)(1 << e1) : 1.0f / (float)(1 << -e1));
		seed = seed * 1664525u + 1013904223u; fb[i] = ((int)(seed >> 8) - 8388608) * (1.0f / 8388608.0f) * (e2 > 0 ? (float)(1 << e2) : 1.0f / (float)(1 << -e2));
	}
	for (i = 0; i + 4 <= 4096; i += 4)
	{
		int l;
		for (op = 0; op < 4; op++)
		{
			memcpy(a, fa + i, 16); memcpy(b, fb + i, 16);
			__asm__ volatile("lqc2 $vf1, 0(%0)\n lqc2 $vf2, 0(%1)\n" : : "r"(a), "r"(b) : "memory");
			if (op == 0) __asm__ volatile("vadd $vf3, $vf1, $vf2\n sqc2 $vf3, 0(%0)\n" : : "r"(r) : "memory");
			if (op == 1) __asm__ volatile("vsub $vf3, $vf1, $vf2\n sqc2 $vf3, 0(%0)\n" : : "r"(r) : "memory");
			if (op == 2) __asm__ volatile("vmul $vf3, $vf1, $vf2\n sqc2 $vf3, 0(%0)\n" : : "r"(r) : "memory");
			if (op == 3) __asm__ volatile("vmulx $vf3, $vf1, $vf2\n sqc2 $vf3, 0(%0)\n" : : "r"(r) : "memory");
			for (l = 0; l < 4; l++)
			{
				volatile float x = a[l], y = (op == 3) ? b[0] : b[l], e;
				if (op == 0) e = x + y; else if (op == 1) e = x - y; else e = x * y;
				total++;
				if ((i & 63) == 0) printf("EETEST R %d %08x %08x %08x %08x\n", op, bits(x), bits(y), bits(r[l]), bits(e));
				if (bits(e) != bits(r[l]))
				{
					bad[op]++;
					if (first_shown[op]++ < 3) printf("EETEST %s lane %d: %a op %a: fpu %a (%08x) vu0 %a (%08x)\n", names[op], l, x, y, e, bits(e), r[l], bits(r[l]));
				}
			}
		}
	}
	printf("EETEST vu0arith: %d lane results, mismatches vadd %d vsub %d vmul %d vmulx %d\n", total, bad[0], bad[1], bad[2], bad[3]);
	printf("EETEST DONE\n");
	SleepThread();
	return 0;
}
