/* Included after the actual reader and its existing host service stubs. */
static struct { unsigned char *raw, *ptr; size_t size; } scratch_blocks[32];
static size_t scratch_live, scratch_peak;
static unsigned scratch_attempts, scratch_failat, scratch_cases;

static void scratch_check(void)
{
	unsigned i, j;
	for (i = 0; i < 32; ++i)
		if (scratch_blocks[i].ptr)
			for (j = 0; j < 64; ++j)
				if (scratch_blocks[i].raw[j] != 0xA7 || scratch_blocks[i].ptr[scratch_blocks[i].size + j] != 0xA7)
					abort();
}

static void *ScratchAlloc(size_t n)
{
	unsigned i;
	unsigned char *p;
	if (++scratch_attempts == scratch_failat)
		return NULL;
	if (posix_memalign((void **)&p, 64, n + 128))
		abort();
	memset(p, 0xA7, n + 128);
	for (i = 0; i < 32 && scratch_blocks[i].ptr; ++i) {}
	if (i == 32) abort();
	scratch_blocks[i].raw = p;
	scratch_blocks[i].ptr = p + 64;
	scratch_blocks[i].size = n;
	scratch_live += n;
	if (scratch_live > scratch_peak) scratch_peak = scratch_live;
	return p + 64;
}

static void ScratchFree(void *p)
{
	unsigned i;
	if (!p) return;
	scratch_check();
	for (i = 0; i < 32; ++i)
		if (scratch_blocks[i].ptr == p)
		{
			scratch_live -= scratch_blocks[i].size;
			free(scratch_blocks[i].raw);
			memset(&scratch_blocks[i], 0, sizeof scratch_blocks[i]);
			return;
		}
	free(p);
}

static void scratch_require(int ok)
{
	if (!ok) { fprintf(stderr, "scratch check %u failed\n", scratch_cases); abort(); }
	++scratch_cases;
	scratch_check();
}

static unsigned char scratch_plain[65536], scratch_encoded[66000], scratch_dest[65664];
static FILE *scratch_fp;
static pkstream_t *scratch_stream;
static unsigned scratch_csize, scratch_bsize, scratch_pumps;
#define SCRATCH_POS 2185

static void scratch_read(unsigned lo, unsigned hi, int raw)
{
	unsigned i;
	long pos = SCRATCH_POS;
	memset(scratch_dest, 0xC3, sizeof scratch_dest);
	scratch_require(ReadBlock(scratch_stream, &pos, raw, raw ? scratch_bsize : scratch_csize,
		scratch_bsize, lo, hi, scratch_dest + 37, 0));
	scratch_require(pos == SCRATCH_POS + (long)(raw ? scratch_bsize : scratch_csize));
	scratch_require(!memcmp(scratch_dest + 37, scratch_plain + lo, hi - lo));
	for (i = 0; i < sizeof scratch_dest; ++i)
		if ((i < 37 || i >= 37 + hi - lo) && scratch_dest[i] != 0xC3) abort();
}

static void scratch_pump(void)
{
	/* Prefetch permits engine callbacks only after copying its I/O buffer. */
	unsigned char head[17];
	++scratch_pumps;
	scratch_read(1, scratch_bsize > 32 ? 32 : scratch_bsize, 0);
	scratch_require(ReadBulk(scratch_stream, SCRATCH_POS, head, sizeof head));
	scratch_require(!memcmp(head, scratch_encoded, sizeof head));
}

int main(int argc, char **argv)
{
	static const unsigned sizes[] = { 1, 2, 15, 4095, 32768, 64000, 65536 };
	unsigned medium, pattern, k, fail, i;
	size_t arena_bytes;
	if (argc > 1)
	{
		int status = pack_corpus_main(argc, argv);
		scratch_require(scratch_live == 0);
		return status;
	}
	for (fail = 0; fail <= 4; ++fail)
	{
		WPack_Shutdown();
		scratch_require(scratch_live == 0);
		scratch_attempts = 0;
		scratch_failat = fail;
		(void)AllocBuffers();
		scratch_failat = 0;
		scratch_require(AllocBuffers());
		scratch_require(AllocBuffers());
		/* Pack detection has already allocated the bounce buffer at open. */
		if (!iobuf) iobuf = ScratchAlloc(WPACK_BLOCK);
		scratch_require(scratch_live == (SCRATCH_BASELINE ? 3u : 2u) * WPACK_BLOCK);
	}
	arena_bytes = scratch_live;
	WPack_Shutdown();
	for (medium = 0; medium < WPK_MEDIA; ++medium)
	for (pattern = 0; pattern < 2; ++pattern)
	for (k = 0; k < sizeof sizes / sizeof *sizes; ++k)
	{
		unsigned offsets[6], n;
		lumpinfo_t lump;
		int encoded;
		unsigned char zeros[2048] = {0};
		scratch_bsize = sizes[k];
		for (i = 0; i < scratch_bsize; ++i)
			scratch_plain[i] = pattern ? (unsigned char)rnd() : (unsigned char)(i / 93 % 7);
		encoded = LZ4_compress_default((char *)scratch_plain, (char *)scratch_encoded, scratch_bsize, sizeof scratch_encoded);
		scratch_require(encoded > 0);
		if (encoded > WPACK_BLOCK) continue; /* Such a block is stored raw by the cooker. */
		scratch_csize = encoded;
		scratch_fp = tmpfile();
		scratch_require(scratch_fp != NULL);
		for (i = 0; i < SCRATCH_POS; ++i) fputc(0, scratch_fp);
		fwrite(scratch_encoded, 1, scratch_csize, scratch_fp);
		fwrite(zeros, 1, sizeof zeros, scratch_fp);
		fflush(scratch_fp);
		medium_forced = medium;
		scratch_stream = StreamFor(scratch_fp);
		scratch_require(AllocBuffers());
		offsets[0] = 0; offsets[1] = 1; offsets[2] = scratch_bsize / 2;
		offsets[3] = scratch_bsize - 1; offsets[4] = scratch_bsize;
		offsets[5] = scratch_bsize > 2048 ? 2047 : 0;
		for (n = 0; n < 3; ++n)
		{
			for (i = 0; i < 6; ++i)
			{
				unsigned lo = offsets[i];
				scratch_read(lo, scratch_bsize, 0);
				if (lo < scratch_bsize) scratch_read(lo, lo + 1, 0);
			}
			scratch_read(0, scratch_bsize, 0);
			if (n == 0)
			{
				memset(&lump, 0, sizeof lump);
				#ifdef W_LUMPLOCATION_ACCESSORS
				W_InitLumpPosition(&lump, SCRATCH_POS);
				#else
				lump.position = SCRATCH_POS;
				#endif
				lump.disksize = scratch_csize; lump.compression = CM_LZ4;
				WPack_PrefetchBegin(); WPack_PrefetchAdd(scratch_fp, &lump);
				scratch_require(WPack_PrefetchRun(131072, scratch_bsize >= 32 ? scratch_pump : NULL) > 0);
			}
			else WPack_PrefetchDrop();
		}
		WPack_Shutdown();
		scratch_require(scratch_live == 0);
		fclose(scratch_fp);
	}
	free(pf.items); pf.items = NULL; pf.cap = 0;
	printf("{\"cases\":%u,\"scratch_bytes\":%zu,\"peak_with_window\":%zu,\"prefetch_callbacks\":%u,\"remaining_bytes\":%zu}\n",
		scratch_cases, arena_bytes, scratch_peak, scratch_pumps, scratch_live);
	return 0;
}
