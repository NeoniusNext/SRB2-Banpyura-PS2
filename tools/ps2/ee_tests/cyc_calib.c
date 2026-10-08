// How does PCSX2 charge EE instructions?  COP0 count for blocks of 1000 instructions of one kind (independent / dependent chains).
#include <stdio.h>
#include <stdint.h>
#include <kernel.h>
static inline unsigned CopCount(void) { unsigned v; __asm__ volatile("mfc0 %0,$9" : "=r"(v)); return v; }
static float f[64] __attribute__((aligned(16))) = {1,2,3,4,5,6,7,8};
#define R10(x) x x x x x x x x x x
#define R100(x) R10(R10(x))
int main(void)
{
	unsigned t0, t1;
	float a = 1.5f, b = 2.5f, c = 3.5f, d = 4.5f;
	int i;
	t0 = CopCount(); __asm__ volatile(R100("nop\n")); t1 = CopCount(); printf("EETEST 100 nop: %u\n", t1 - t0);
	t0 = CopCount(); __asm__ volatile(R100("addu $8,$9,$10\n") : : : "$8"); t1 = CopCount(); printf("EETEST 100 addu: %u\n", t1 - t0);
	t0 = CopCount(); __asm__ volatile(R100("add.s $f4,$f5,$f6\n") : : : "$f4"); t1 = CopCount(); printf("EETEST 100 add.s indep(same dest): %u\n", t1 - t0);
	t0 = CopCount(); __asm__ volatile(R100("add.s $f4,$f4,$f6\n") : : : "$f4"); t1 = CopCount(); printf("EETEST 100 add.s dependent chain: %u\n", t1 - t0);
	t0 = CopCount(); __asm__ volatile(R100("mul.s $f4,$f4,$f6\n") : : : "$f4"); t1 = CopCount(); printf("EETEST 100 mul.s dependent chain: %u\n", t1 - t0);
	t0 = CopCount(); __asm__ volatile(R100("mul.s $f4,$f5,$f6\n") : : : "$f4"); t1 = CopCount(); printf("EETEST 100 mul.s indep: %u\n", t1 - t0);
	t0 = CopCount(); __asm__ volatile(R100("lwc1 $f4,0(%0)\n") : : "r"(f) : "$f4"); t1 = CopCount(); printf("EETEST 100 lwc1: %u\n", t1 - t0);
	t0 = CopCount(); __asm__ volatile(R100("swc1 $f4,0(%0)\n") : : "r"(f) : "memory"); t1 = CopCount(); printf("EETEST 100 swc1: %u\n", t1 - t0);
	t0 = CopCount(); __asm__ volatile(R100("neg.s $f4,$f5\n") : : : "$f4"); t1 = CopCount(); printf("EETEST 100 neg.s: %u\n", t1 - t0);
	t0 = CopCount(); __asm__ volatile(R100("lqc2 $vf1,0(%0)\n") : : "r"(f) : "memory"); t1 = CopCount(); printf("EETEST 100 lqc2: %u\n", t1 - t0);
	t0 = CopCount(); __asm__ volatile(R100("sqc2 $vf1,0(%0)\n") : : "r"(f) : "memory"); t1 = CopCount(); printf("EETEST 100 sqc2: %u\n", t1 - t0);
	t0 = CopCount(); __asm__ volatile(R100("vadd $vf3,$vf1,$vf2\n")); t1 = CopCount(); printf("EETEST 100 vadd indep: %u\n", t1 - t0);
	t0 = CopCount(); __asm__ volatile(R100("vadd $vf3,$vf3,$vf2\n")); t1 = CopCount(); printf("EETEST 100 vadd dependent: %u\n", t1 - t0);
	t0 = CopCount(); __asm__ volatile(R100("vmulx.xy $vf3,$vf1,$vf2\n")); t1 = CopCount(); printf("EETEST 100 vmulx.xy: %u\n", t1 - t0);
	t0 = CopCount(); __asm__ volatile(R100("qmtc2 $8,$vf3\n") : : : ); t1 = CopCount(); printf("EETEST 100 qmtc2: %u\n", t1 - t0);
	t0 = CopCount(); __asm__ volatile(R100("qmfc2 $8,$vf3\n") : : : "$8"); t1 = CopCount(); printf("EETEST 100 qmfc2: %u\n", t1 - t0);
	t0 = CopCount(); __asm__ volatile(R100("pextlw $8,$9,$10\n") : : : "$8"); t1 = CopCount(); printf("EETEST 100 pextlw: %u\n", t1 - t0);
	t0 = CopCount(); __asm__ volatile(R100("lq $8,0(%0)\n") : : "r"(f) : "$8"); t1 = CopCount(); printf("EETEST 100 lq: %u\n", t1 - t0);
	t0 = CopCount(); __asm__ volatile(R100("ld $8,0(%0)\n") : : "r"(f) : "$8"); t1 = CopCount(); printf("EETEST 100 ld: %u\n", t1 - t0);
	t0 = CopCount(); __asm__ volatile(R100("sd $8,0(%0)\n") : : "r"(f) : "memory"); t1 = CopCount(); printf("EETEST 100 sd: %u\n", t1 - t0);
	(void)a; (void)b; (void)c; (void)d; (void)i;
	printf("EETEST DONE\n");
	SleepThread();
	return 0;
}
