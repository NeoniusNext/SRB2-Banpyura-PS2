// SRB2 PS2 port: fast bulk memcpy / memset (PS2-163).
//
// newlib's memcpy on the EE is built for size: eight bytes per loop pass when both pointers are aligned (about 1.25 cycles per byte), a byte loop
// otherwise. The software renderer copies 40..200 KiB of screen per frame for the water effect (R_DrawSinglePlane) and clears row buffers, the
// hardware renderer copies texture data. The 128-bit load/store pair moves 16 bytes in about 3.5 cycles. Only the aligned bulk case is done
// here (both pointers 16-byte aligned, >= 64 bytes; memset: destination aligned, >= 64 bytes); everything else is newlib's, so the results are
// exactly those of the originals (memcpy of non-overlapping blocks, memset of a byte value).
//
// Linked through --wrap=memcpy/--wrap=memset (tools/ps2/build.py, SRB2_PS2_FASTMEM=0 turns it off; the --memprof build has its own wrappers).
// The loops are asm on purpose: a C loop here could be turned back into a call to memcpy by the compiler (endless recursion through the wrapper).

#if defined(_EE) && !defined(PS2_MEMPROF) && !defined(PS2_NO_FASTMEM)
#include <stddef.h>
#include <stdint.h>

void *__real_memcpy(void *dst, const void *src, size_t n);
void *__real_memset(void *dst, int c, size_t n);

void *__wrap_memcpy(void *dst, const void *src, size_t n);
void *__wrap_memset(void *dst, int c, size_t n);

void *__wrap_memcpy(void *dst, const void *src, size_t n)
{
	if (n >= 64 && !(((uintptr_t)dst | (uintptr_t)src) & 15))
	{
		unsigned char *d = (unsigned char *)dst;
		const unsigned char *s = (const unsigned char *)src;
		size_t blocks = n >> 6;
		unsigned long long t0, t1, t2, t3;

		__asm__ volatile(
			"1:\n\t"
			"lq %[t0],0(%[s])\n\t"
			"lq %[t1],16(%[s])\n\t"
			"lq %[t2],32(%[s])\n\t"
			"lq %[t3],48(%[s])\n\t"
			"addiu %[s],%[s],64\n\t"
			"sq %[t0],0(%[d])\n\t"
			"sq %[t1],16(%[d])\n\t"
			"sq %[t2],32(%[d])\n\t"
			"sq %[t3],48(%[d])\n\t"
			"addiu %[b],%[b],-1\n\t"
			"addiu %[d],%[d],64\n\t"
			"bnez %[b],1b\n\t"
			: [s] "+r"(s), [d] "+r"(d), [b] "+r"(blocks), [t0] "=&r"(t0), [t1] "=&r"(t1), [t2] "=&r"(t2), [t3] "=&r"(t3)
			:
			: "memory");
		n &= 63;
		while (n >= 16)
		{
			__asm__ volatile(
				"lq %[t0],0(%[s])\n\t"
				"sq %[t0],0(%[d])\n\t"
				: [t0] "=&r"(t0)
				: [s] "r"(s), [d] "r"(d)
				: "memory");
			s += 16;
			d += 16;
			n -= 16;
		}
		if (n)
			__real_memcpy(d, s, n);
		return dst;
	}
	return __real_memcpy(dst, src, n);
}

void *__wrap_memset(void *dst, int c, size_t n)
{
	if (n >= 64 && !((uintptr_t)dst & 15))
	{
		unsigned char *d = (unsigned char *)dst;
		size_t blocks = n >> 6;
		unsigned long long v;
		const unsigned int b = (unsigned int)c & 0xFFu;
		const unsigned int word = b * 0x01010101u;

		__asm__ volatile(
			"pextlw %[v],%[word],%[word]\n\t"
			"pcpyld %[v],%[v],%[v]\n\t"
			"1:\n\t"
			"sq %[v],0(%[d])\n\t"
			"sq %[v],16(%[d])\n\t"
			"sq %[v],32(%[d])\n\t"
			"sq %[v],48(%[d])\n\t"
			"addiu %[b],%[b],-1\n\t"
			"addiu %[d],%[d],64\n\t"
			"bnez %[b],1b\n\t"
			: [d] "+r"(d), [b] "+r"(blocks), [v] "=&r"(v)
			: [word] "r"(word)
			: "memory");
		n &= 63;
		if (n)
			__real_memset(d, c, n);
		return dst;
	}
	return __real_memset(dst, c, n);
}
#endif
