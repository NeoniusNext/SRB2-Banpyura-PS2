/* Complete production pack reader; only engine service boundaries are stubs. */
#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <stddef.h>
static ssize_t HeaderRead(int fd, void *data, size_t size);
#define CONS_Alert IoAlert
#define read HeaderRead
#include WPACK_SOURCE
#undef read
#undef CONS_Alert
static size_t CorpusReadLumpN(wpack_t *pack, FILE *fp, UINT32 index, const lumpinfo_t *lump,
	void *dest, size_t size, size_t offset);
#define main corpus_main
#define WPack_ReadLumpN CorpusReadLumpN
#include PACK_HOST_SOURCE
#undef WPack_ReadLumpN
#undef main

static unsigned checks, cases, alerts, callbacks, reentry_mode, in_callback;
static FILE *callback_file;
static int truncate_fd = -1;
static unsigned truncate_read, observed_reads;
static FILE *literal_output;
static unsigned char literal_seen[65536];
static uint64_t digest = UINT64_C(1469598103934665603);
#define REQUIRE(c) do { ++checks; if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static ssize_t HeaderRead(int fd, void *data, size_t size)
{
	ssize_t value = read(fd, data, size);
	if (fd == truncate_fd && ++observed_reads == truncate_read)
		REQUIRE(!ftruncate(fd, 80));
	return value;
}

static size_t CorpusReadLumpN(wpack_t *pack, FILE *fp, UINT32 index, const lumpinfo_t *lump,
	void *dest, size_t size, size_t offset)
{
	size_t got = WPack_ReadLumpN(pack, fp, index, lump, dest, size, offset);
	if (literal_output && index < 65536 && !literal_seen[index] && offset == 0 && size == lump->size && got == size)
	{
		REQUIRE(fwrite(dest, 1, got, literal_output) == got);
		literal_seen[index] = 1;
	}
	return got;
}

static void hash_bytes(const void *p, size_t n)
{
	const unsigned char *b = p;
	while (n--) { digest ^= *b++; digest *= UINT64_C(1099511628211); }
}

/* A rejected header can report through the console. Reenter the actual parser
 * before the outer rejection returns, both for the same and a different FILE. */
void IoAlert(alerttype_t level, const char *fmt, ...)
{
	(void)level; (void)fmt;
	++alerts;
	if (reentry_mode && !in_callback)
	{
		pkstream_t *s = StreamFind(callback_file);
		srp2_header_t h;
		srp2_ext_t ext;
		unsigned char raw[sizeof s->first];
		boolean was_checked = s ? s->hdrok : false;
		in_callback = 1;
		++callbacks;
		if (s && !was_checked) memcpy(raw, s->first, sizeof raw);
		if (s->magic) REQUIRE(PeekMagic(s) == (s->magic > 0));
		else if (reentry_mode == 3) REQUIRE(PeekMagic(s));
		REQUIRE(ReadHeader(callback_file, "callback", &h, &ext) == (reentry_mode >= 2));
		if (reentry_mode == 1)
		{
			REQUIRE(!s->hdrok);
			REQUIRE(!memcmp(raw, s->first, sizeof raw));
		}
		in_callback = 0;
	}
}

static void make_image(unsigned char *image, unsigned version, unsigned variant)
{
	srp2_header_t h = {{0}};
	srp2_ext_t ext = {0};
	unsigned i;
	memset(image, 0, 16384);
	memcpy(h.magic, "SRP2", 4);
	h.version = version;
	h.headersize = SRP2_HEADER_SIZE;
	h.flags = variant & 1;
	h.numlumps = 3;
	h.tableoffset = 2048;
	h.pooloffset = 4096;
	h.poolsize = 31;
	h.dataoffset = 12288;
	h.filesize = 16384;
	h.blocksize = WPACK_BLOCK;
	for (i = 0; i < 5; ++i) h.reserved[i] = 0x13572468u + i + variant;
	memcpy(image, &h, sizeof h);
	if (version == 1)
	{
		srp2_entry_t entries[3];
		memset(entries, 0, sizeof entries);
		for (i = 0; i < 3; ++i)
		{
			entries[i].position = h.dataoffset + 64 * i;
			entries[i].size = entries[i].disksize = 1;
			entries[i].longname = 9;
			image[entries[i].position] = (unsigned char)('A' + i);
		}
		memcpy(image + h.tableoffset, entries, sizeof entries);
		memcpy(image + h.pooloffset, "test.bin\0test\0", 14);
		/* Bytes after a v1 header are not a parsed extension. */
		for (i = 64; i < 128; ++i) image[i] = (unsigned char)(i * 37 + variant);
		return;
	}
	if (variant & 2) h.flags |= SRP2_FLAG_HEAD;
	ext.extsize = sizeof ext;
	ext.headoffset = 6144;
	ext.headbytes = 16;
	ext.crcoffset = 8192;
	ext.chktable = 0x12482549u + variant;
	ext.chkpool = 0x24709569u + variant;
	ext.chkhead = 0x56789ABCu + variant;
	ext.chkcrc = 0xCCEEAA77u + variant;
	for (i = 0; i < 8; ++i) ext.reserved[i] = 0xABCDEF00u + i + variant;
	memcpy(image, &h, sizeof h);
	memcpy(image + 64, &ext, sizeof ext);
}

static void write_image(FILE *fp, const unsigned char *image, size_t size)
{
	REQUIRE(!fflush(fp));
	REQUIRE(!ftruncate(fileno(fp), 0));
	REQUIRE(pwrite(fileno(fp), image, size, 0) == (ssize_t)size);
	clearerr(fp);
}

static FILE *new_image(const unsigned char *image, size_t size)
{
	FILE *fp = tmpfile();
	REQUIRE(fp != NULL);
	write_image(fp, image, size);
	return fp;
}

static void expected_parse(const unsigned char *image, srp2_header_t *h, srp2_ext_t *ext)
{
	memcpy(h, image, sizeof *h);
	if (h->version >= 2) memcpy(ext, image + 64, sizeof *ext);
	else memset(ext, 0, sizeof *ext);
}

static void successful_parse(FILE *fp, const unsigned char *image, int expect_cached)
{
	srp2_header_t h, expected_h;
	srp2_ext_t ext, expected_ext;
	wpack_iostat_t before, after;
	pkstream_t *s = StreamFind(fp);
	expected_parse(image, &expected_h, &expected_ext);
	memset(&before, 0, sizeof before); memset(&after, 0, sizeof after);
	WPack_GetStats(&before);
	REQUIRE(ReadHeader(fp, "valid", &h, &ext));
	WPack_GetStats(&after);
	REQUIRE(s == StreamFind(fp) && s->hdrok);
	REQUIRE(!memcmp(&h, &expected_h, sizeof h));
	REQUIRE(!memcmp(&ext, &expected_ext, sizeof ext));
	REQUIRE(!memcmp(&s->h, &expected_h, sizeof h));
	REQUIRE(!memcmp(&s->ext, &expected_ext, sizeof ext));
	if (expect_cached) REQUIRE(!memcmp(&before, &after, sizeof before));
	hash_bytes(&h, sizeof h); hash_bytes(&ext, sizeof ext);
}

static void failed_parse(FILE *fp, unsigned reenter)
{
	srp2_header_t h;
	srp2_ext_t ext;
	pkstream_t *s = StreamFind(fp);
	unsigned char raw[sizeof s->first];
	REQUIRE(s != NULL && !s->hdrok);
	memcpy(raw, s->first, sizeof raw);
	callback_file = fp;
	reentry_mode = reenter;
	REQUIRE(!ReadHeader(fp, "invalid", &h, &ext));
	reentry_mode = 0;
	REQUIRE(s == StreamFind(fp) && !s->hdrok);
	REQUIRE(!memcmp(raw, s->first, sizeof raw));
	hash_bytes(raw, sizeof raw);
}

static void close_image(FILE *fp)
{
	StreamDrop(fp);
	REQUIRE(!fclose(fp));
}

/* Mutations cover each distinct header/extension validation group, including
 * bounds wrapping, unsupported versions, layouts and flags. */
static const struct { unsigned offset; UINT32 value; } corruptions[] = {
	{4, 0}, {4, 3}, {8, 63}, {40, 65535}, {36, 16383},
	{16, 0}, {16, 65536}, {28, 0}, {12, 4},
	{20, 0}, {20, 2049}, {20, 18432}, {20, 16384},
	{24, 2048}, {24, 4097}, {24, 18432}, {28, 16384},
	{32, 4096}, {32, 12289}, {32, 18432},
	{64, 63}, {72, 0}, {72, 68}, {72, 15},
	{68, 6145}, {76, 8193}, {68, 4096},
	{68, 0xFFFFE000u}, {76, 6144}, {76, 0xFFFFF800u}, {76, 12288}
};

int main(int argc, char **argv)
{
	unsigned char image[16384], other[16384], bad[16384];
	unsigned version, variant, n, direct, i, entry, change;
	FILE *fp, *files[8];
	void *setup;
	if (argc > 1)
	{
		const char *literal_path = getenv("IO_LITERAL_OUT");
		int status;
		if (literal_path) { literal_output = fopen(literal_path, "wb"); REQUIRE(literal_output != NULL); }
		status = corpus_main(argc, argv);
		if (literal_output) REQUIRE(!fclose(literal_output));
		return status;
	}
	REQUIRE(sizeof(srp2_header_t) == 64 && sizeof(srp2_ext_t) == 64);
	/* Cold probe, successful transition, repeated cached copies and a source
 * changed/truncated after success (the established cache semantics). */
	for (version = 1; version <= 2; ++version)
	for (variant = 0; variant < 4; ++variant)
	for (direct = 0; direct < 2; ++direct)
	{
		make_image(image, version, variant);
		fp = new_image(image, sizeof image);
		if (direct)
		{
			pkstream_t *s = StreamFor(fp);
			memset(s->first, 0xA7, sizeof s->first);
		}
		else
		{
			setup = WPack_SetupHandle(fp);
			REQUIRE(setup != NULL); free(setup);
			REQUIRE(WPack_Detect(fp));
			REQUIRE(!StreamFind(fp)->hdrok);
			REQUIRE(!memcmp(StreamFind(fp)->first, image, 128));
		}
		successful_parse(fp, image, !direct);
		for (i = 0; i < 3; ++i) successful_parse(fp, image, 1);
		make_image(other, version, variant ^ 1);
		write_image(fp, other, sizeof other);
		successful_parse(fp, image, 1);
		write_image(fp, other, 17);
		successful_parse(fp, image, 1);
		REQUIRE(PeekMagic(StreamFind(fp)) == !direct);
		successful_parse(fp, image, 1);
		/* A setup with the same FILE address deliberately discards the cache. */
		write_image(fp, other, sizeof other);
		setup = WPack_SetupHandle(fp);
		REQUIRE(setup != NULL); free(setup);
		REQUIRE(!StreamFind(fp)->hdrok);
		successful_parse(fp, other, 1);
		close_image(fp); ++cases;
	}
	/* Public operations can parse an unprobed replacement slot. A later Detect
 * still probes the current file, while a successful parsed cache remains the
 * old one when the signature stays valid. */
	for (entry = 0; entry < 3; ++entry)
	for (change = 0; change < 4; ++change)
	{
		make_image(image, 1, 0);
		fp = new_image(image, sizeof image);
		REQUIRE(StreamFor(fp) != NULL);
		if (entry == 1) REQUIRE(WPack_VerifyNMUS(fp) == 1);
		else if (entry == 2)
		{
			UINT16 count;
			void *pool;
			boolean nonmusic;
			wpack_t *pack;
			lumpinfo_t *lumps = WPack_GetLumps(fp, NULL, &count, &pool, &nonmusic, &pack);
			REQUIRE(lumps != NULL && count == 3 && !nonmusic);
			for (i = 0; i < count; ++i)
			{
				unsigned char data = 0;
				REQUIRE(WPack_ReadLumpN(pack, fp, i, &lumps[i], &data, 1, 0) == 1);
				REQUIRE(data == 'A' + i);
			}
			Z_Free(lumps); Z_Free(pool); WPack_Close(pack);
		}
		successful_parse(fp, image, entry != 0);
		REQUIRE(StreamFind(fp)->magic == 0);
		memcpy(other, image, sizeof other);
		if (change == 1) other[12] = 1;
		if (change == 2) memcpy(other, "PWAD", 4);
		write_image(fp, other, change == 3 ? 17 : sizeof other);
		REQUIRE(WPack_Detect(fp) == (change < 2));
		if (change < 2) successful_parse(fp, image, 1);
		else
		{
			REQUIRE(StreamFind(fp) == NULL);
			REQUIRE(ftell(fp) == 0);
			write_image(fp, image, sizeof image);
			REQUIRE(StreamFor(fp) != NULL);
			successful_parse(fp, image, 0);
		}
		close_image(fp); ++cases;
	}
	/* An I/O retry warning reenters a late probe and a cached header read. Each
 * invocation owns its temporary; neither can overwrite the parsed cache. */
	make_image(image, 2, 3);
	fp = new_image(image, sizeof image);
	REQUIRE(StreamFor(fp) != NULL);
	successful_parse(fp, image, 0);
	WPack_InjectErrors(1, 1);
	callback_file = fp; reentry_mode = 3;
	REQUIRE(WPack_Detect(fp));
	reentry_mode = 0;
	WPack_InjectErrors(0, 0);
	successful_parse(fp, image, 1);
	close_image(fp); ++cases;
	for (n = 0; n < sizeof corruptions / sizeof *corruptions; ++n)
	for (direct = 0; direct < 2; ++direct)
	{
		make_image(image, 2, 3);
		memcpy(bad, image, sizeof bad);
		memcpy(bad + corruptions[n].offset, &corruptions[n].value, sizeof(UINT32));
		fp = new_image(bad, sizeof bad);
		if (direct)
		{
			pkstream_t *s = StreamFor(fp);
			memset(s->first, 0xD3, sizeof s->first);
		}
		else REQUIRE(PeekMagic(StreamFor(fp)));
		failed_parse(fp, 1);
		failed_parse(fp, 0);
		write_image(fp, image, sizeof image);
		if (direct) successful_parse(fp, image, 0);
		else failed_parse(fp, 1); /* A corrected disk does not bypass its probe. */
		StreamDrop(fp);
		REQUIRE(PeekMagic(StreamFor(fp)));
		successful_parse(fp, image, 1);
		close_image(fp); ++cases;
	}
	/* Failed direct header/extension reads preserve every byte of raw storage. */
	for (n = 0; n < 3; ++n)
	{
		make_image(image, 2, 3);
		fp = new_image(image, n == 0 ? 17 : n == 1 ? 80 : sizeof image);
		memset(StreamFor(fp)->first, 0xE9, sizeof StreamFor(fp)->first);
		if (n == 2) StreamFind(fp)->size = -1;
		failed_parse(fp, 0);
		close_image(fp); ++cases;
	}
	/* The source can disappear between separate direct header and extension
 * reads. Change it at the real device boundary after the header was read. */
	make_image(image, 2, 3);
	fp = new_image(image, sizeof image);
	memset(StreamFor(fp)->first, 0xB5, sizeof StreamFor(fp)->first);
	truncate_fd = fileno(fp); truncate_read = 1; observed_reads = 0;
	failed_parse(fp, 0);
	truncate_fd = -1;
	REQUIRE(observed_reads >= 2);
	close_image(fp); ++cases;
	/* Exhausted device retries before parsing cannot publish either view. */
	fp = new_image(image, sizeof image);
	memset(StreamFor(fp)->first, 0x69, sizeof StreamFor(fp)->first);
	WPack_InjectErrors(1, 4);
	failed_parse(fp, 0);
	WPack_InjectErrors(0, 0);
	close_image(fp); ++cases;
	/* A console callback can parse another already open stream. */
	make_image(image, 2, 3); make_image(other, 1, 1);
	fp = new_image(image, sizeof image); files[0] = new_image(other, sizeof other);
	REQUIRE(PeekMagic(StreamFor(files[0])));
	image[4] = 0; write_image(fp, image, sizeof image);
	REQUIRE(PeekMagic(StreamFor(fp)));
	callback_file = files[0]; reentry_mode = 2;
	{ srp2_header_t h; srp2_ext_t ext; REQUIRE(!ReadHeader(fp, "callback-source", &h, &ext)); }
	reentry_mode = 0;
	successful_parse(files[0], other, 1);
	close_image(fp); close_image(files[0]); ++cases;
	/* Reopening an existing pack keeps its header cache, while explicitly
 * preparing a new resource with the same FILE identity drops the old cache. */
	{
		char first_path[] = "/tmp/next9-pack-a-XXXXXX";
		char second_path[] = "/tmp/next9-pack-b-XXXXXX";
		int first_fd = mkstemp(first_path), second_fd = mkstemp(second_path);
		pkstream_t *s;
		REQUIRE(first_fd >= 0 && second_fd >= 0);
		make_image(image, 2, 3); make_image(other, 1, 1);
		REQUIRE(write(first_fd, image, sizeof image) == sizeof image);
		REQUIRE(write(second_fd, other, sizeof other) == sizeof other);
		REQUIRE(!close(first_fd) && !close(second_fd));
		fp = fopen(first_path, "rb"); REQUIRE(fp != NULL);
		setup = WPack_SetupHandleEx(fp, first_path); REQUIRE(setup != NULL); free(setup);
		successful_parse(fp, image, 1);
		s = StreamFind(fp);
		REQUIRE(StreamReopen(s));
		REQUIRE(s == StreamFind(fp) && s->fd == fileno(fp));
		successful_parse(fp, image, 1);
		REQUIRE(freopen(second_path, "rb", fp) == fp);
		setup = WPack_SetupHandleEx(fp, second_path); REQUIRE(setup != NULL); free(setup);
		successful_parse(fp, other, 1);
		close_image(fp);
		REQUIRE(!unlink(first_path) && !unlink(second_path));
		++cases;
	}
	/* Every replacement slot is exercised. A displaced FILE gets its changed
 * current bytes when it is read again, with no stale parsed/raw alias. */
	WPack_Shutdown();
	for (i = 0; i < 8; ++i)
	{
		make_image(image, 1 + (i & 1), i & 3);
		files[i] = new_image(image, sizeof image);
		REQUIRE(PeekMagic(StreamFor(files[i])));
		successful_parse(files[i], image, 1);
	}
	REQUIRE(StreamFind(files[0]) == NULL && StreamFind(files[1]) == NULL);
	for (i = 0; i < 8; ++i)
	{
		make_image(image, 1 + (i & 1), i & 3);
		make_image(other, 1 + (i & 1), (i & 3) ^ 1);
		write_image(files[i], other, sizeof other);
		if (!StreamFind(files[i]))
		{
			REQUIRE(StreamFor(files[i]) != NULL);
			successful_parse(files[i], other, 0);
		}
		else successful_parse(files[i], image, 1);
		++cases;
	}
	for (i = 0; i < 8; ++i) close_image(files[i]);
	WPack_Shutdown();
	REQUIRE(iobuf == NULL && cbuf == NULL);
	printf("{\"cases\":%u,\"checks\":%u,\"alerts\":%u,\"callbacks\":%u,\"digest\":\"%016llx\",\"stream_bytes\":%zu,\"slots_bytes\":%zu,\"persistent_scratch_added\":0}\n",
		cases, checks, alerts, callbacks, (unsigned long long)digest, sizeof(pkstream_t), sizeof pkst);
	return 0;
}
