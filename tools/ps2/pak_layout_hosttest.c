/* Stock reader fixture, with literal full-read output for byte comparisons.
 * This changes no production reader code and links the real system liblz4. */
#include "doomdef.h"
#include "w_wad.h"
#include "w_pack.h"

static size_t LiteralReadLumpN(wpack_t *pack, FILE *fp, UINT32 index,
	const lumpinfo_t *lump, void *dest, size_t size, size_t offset);
#define main StockPackMain
#define WPack_ReadLumpN LiteralReadLumpN
#include "pack_hosttest.c"
#undef WPack_ReadLumpN
#undef main

static FILE *literal;
static unsigned char emitted[65536];

static size_t LiteralReadLumpN(wpack_t *pack, FILE *fp, UINT32 index,
	const lumpinfo_t *lump, void *dest, size_t size, size_t offset)
{
	size_t got = WPack_ReadLumpN(pack, fp, index, lump, dest, size, offset);
	if (literal && index < 65536 && !emitted[index] && offset == 0
		&& size == lump->size && got == size)
	{
		if (fwrite(dest, 1, got, literal) != got)
			I_Error("literal output write failed\n");
		emitted[index] = 1;
	}
	return got;
}

int main(int argc, char **argv)
{
	const char *path = getenv("PACK_LITERAL_OUT");
	int result;
	if (path && !(literal = fopen(path, "wb")))
		return 3;
	result = StockPackMain(argc, argv);
	if (literal && fclose(literal))
		return 3;
	return result;
}
