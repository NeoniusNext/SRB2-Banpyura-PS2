// Host-profile only (tools/ps2/build_host_profile.ps1 -Lz4Source): the two LZ4 block decoders w_pack.c uses, written from the
// LZ4 block format description, for machines without the lz4 library sources. The EE build links the real liblz4.
// Every length is checked: a corrupt input returns a negative value like the library does.
#include <string.h>
#include "lz4.h"

static int decode(const unsigned char *src, int srcsize, unsigned char *dst, int dstcap, int target, int partial)
{
	const unsigned char *ip = src, *iend = src + srcsize;
	unsigned char *op = dst, *oend = dst + dstcap;
	int limit = partial ? target : dstcap;

	if (srcsize <= 0)
		return -1;
	if (limit > dstcap)
		limit = dstcap;
	while (ip < iend)
	{
		unsigned token = *ip++;
		size_t lit = token >> 4, mlen;
		unsigned b;
		size_t offset;

		if (lit == 15)
		{
			do
			{
				if (ip >= iend)
					return -1;
				b = *ip++;
				lit += b;
			} while (b == 255);
		}
		if ((size_t)(iend - ip) < lit)
			return -1;
		if ((size_t)(oend - op) < lit)
		{
			if (!partial)
				return -1;
			lit = (size_t)(oend - op);
		}
		memcpy(op, ip, lit);
		ip += lit;
		op += lit;
		if (partial && op - dst >= limit)
			return (int)(op - dst) > limit ? limit : (int)(op - dst);
		if (ip >= iend)
			break; // the last sequence has literals only
		if (iend - ip < 2)
			return -1;
		offset = ip[0] | ((size_t)ip[1] << 8);
		ip += 2;
		if (offset == 0 || offset > (size_t)(op - dst))
			return -1;
		mlen = token & 15;
		if (mlen == 15)
		{
			do
			{
				if (ip >= iend)
					return -1;
				b = *ip++;
				mlen += b;
			} while (b == 255);
		}
		mlen += 4;
		if ((size_t)(oend - op) < mlen)
		{
			if (!partial)
				return -1;
			mlen = (size_t)(oend - op);
		}
		{
			const unsigned char *mp = op - offset;
			size_t i;

			for (i = 0; i < mlen; i++)
				op[i] = mp[i]; // may overlap: byte by byte
			op += mlen;
		}
		if (partial && op - dst >= limit)
			return limit;
	}
	return (int)(op - dst);
}

int LZ4_decompress_safe(const char *src, char *dst, int compressedSize, int dstCapacity)
{
	return decode((const unsigned char *)src, compressedSize, (unsigned char *)dst, dstCapacity, dstCapacity, 0);
}

int LZ4_decompress_safe_partial(const char *src, char *dst, int compressedSize, int targetOutputSize, int dstCapacity)
{
	return decode((const unsigned char *)src, compressedSize, (unsigned char *)dst, dstCapacity, targetOutputSize, 1);
}
