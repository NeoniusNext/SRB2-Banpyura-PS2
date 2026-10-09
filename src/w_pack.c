// SONIC ROBO BLAST 2 (PS2 port)
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  w_pack.c
/// \brief SRP2 cooked packs: lumpinfo_t[] straight from the pack, LZ4 lump reader (format: docs/PACK_FORMAT.md)

#include "doomdef.h"
#include "w_wad.h"
#include "z_zone.h"
#include "w_pack.h"
#include "ps2/ps2_loadprof.h" // PS2-LOAD-1

#include <lz4.h>
#include <limits.h>
#include <errno.h>
#include <unistd.h>
#include <strings.h>

#ifdef PS2
#include <malloc.h>
#include "ps2/ps2_sys.h" // PS2_SleepUs
#define WPACK_ALIGNED_ALLOC(size) memalign(64, (size))
#else
#include <time.h>
#define WPACK_ALIGNED_ALLOC(size) malloc(size)
#define Z_TryMallocAlign(s, t, u, a) Z_MallocAlign(s, t, u, a) // host test: no zone, no failure
#endif

#define SRP2_VERSION_MIN 1 // docs/PACK_FORMAT.md: v1 (index + payload), v2 (adds the head table, the per-lump CRC32 table and the index checksums)
#define SRP2_VERSION_MAX 2
#define SRP2_HEADER_SIZE 64
#define SRP2_FLAG_NONMUSIC 1
#define SRP2_FLAG_HEAD 2 // v2: head table + CRC32 table present
#define SRP2_CODEC_RAW 0
#define SRP2_CODEC_LZ4 1
#define SRP2_RAWBLOCK 0x80000000u // block index: this block is stored raw

typedef struct
{
	char magic[4];
	UINT32 version;
	UINT32 headersize;
	UINT32 flags;
	UINT32 numlumps;
	UINT32 tableoffset;
	UINT32 pooloffset;
	UINT32 poolsize;
	UINT32 dataoffset;
	UINT32 filesize;
	UINT32 blocksize;
	UINT32 reserved[5];
} srp2_header_t;

// v2 only, at byte 64 of the file (still inside the first sector)
typedef struct
{
	UINT32 extsize;     // 64
	UINT32 headoffset;  // head table: numlumps * headbytes, the first bytes of every decoded lump
	UINT32 headbytes;   // 16
	UINT32 crcoffset;   // numlumps UINT32: CRC32 of every decoded lump (-verifypack)
	UINT32 chktable;    // WPack_Check of the entry table, the string pool, the head table and the CRC table
	UINT32 chkpool;
	UINT32 chkhead;
	UINT32 chkcrc;
	UINT32 reserved[8];
} srp2_ext_t;

typedef struct
{
	UINT32 position;
	UINT32 disksize;
	UINT32 size;
	UINT32 fullname;
	UINT32 longname;
	UINT32 codec;
} srp2_entry_t;

// What an open v2 pack keeps (wadfile_t.pack): the head table lives from the pack opening until WPack_DropHeads (end of the start-up)
struct wpack_s
{
	UINT32 version;
	UINT32 numlumps;
	UINT32 headoffset, headbytes, crcoffset;
	UINT32 chkcrc;
	UINT8 *head; // numlumps * headbytes or NULL
	char name[64];
};

#define ENTRY_CHUNK 256 // 256 * 24 = 6144 bytes = 3 sectors per read
#define HEADTABLE_MINLUMPS 256 // packs with fewer lumps do not keep a head table (MUSIC.PAK: 215 lumps)
#define WPACK_SECTOR 2048

// ---- OPT13-IO (RS-01, RS-07): the device layer ----------------------------------------------------------------------------------------------------
// The packs used to be read through a 64 KiB stdio buffer per pack: every miss of the cache (a texture or a sprite met for the first time while playing,
// 0.5..5 KB of compressed data) cost one read of 64 KiB, and the header of a pack was parsed three times, each time dropping the buffer (docs/research/rsys/
// OPT13_RSYS.md, 11-20x more bytes than needed in the Hardware renderer). Now a pack stream is read below stdio (read() on its descriptor, whole sectors into
// 64-byte aligned buffers) through a WINDOW whose size depends on the medium the pack is on: a seek of a DVD costs more than the transfer of 64 KiB, so the DVD
// keeps 64 KiB windows; a USB stick or an SD card pays per command, so a miss reads the sectors it needs and a little more.
// Every device read is retried (RS-07) before the game gives up with a message that names the pack.
typedef enum { WPK_DVD, WPK_USB, WPK_SD, WPK_HDD, WPK_MEDIA } wpk_medium_t;

typedef struct
{
	const char *name;
	UINT32 cap;    // size of the window buffer (the largest device read of a miss)
	UINT32 minreq; // the smallest device read of a miss
	UINT32 ahead;  // device read of a miss that continues a sequential run (the start-up reads the pack in the order it is stored)
	UINT32 gap;    // a miss this many bytes in front of the end of the last read still continues a sequential run
	UINT32 merge;  // the prefetch pass (RS-02) reads through a hole of this many bytes between two lumps instead of starting another command
} wpack_policy_t;

// docs/research/rsys/OPT13_RSYS.md section 1 (assumptions, to be checked with -iobench on the console): cost of a command c, speed b, threshold T = c * b
static wpack_policy_t policies[WPK_MEDIA] = {
	{ "dvd", 65536, 65536, 65536, 0xFFFFFFFFu, 131072 }, // seek >> transfer: the window of the old stdio buffer (the default for every medium that is not recognised)
	{ "usb", 32768, 4096, 32768, 4096, 8192 },           // USB 1.1: c = 3 ms, b = 1 MB/s, T = 3 KB
	{ "sd", 32768, 4096, 32768, 4096, 8192 },            // MX4SIO: c = 1 ms, b = 3 MB/s
	{ "hdd", 65536, 16384, 65536, 65536, 65536 },        // ATA: c = 0.5 ms, b = 15 MB/s, seek 12 ms
};
static int medium_forced = -1;

typedef struct
{
	FILE *fp;      // the identity of the stream (what the engine holds in wadfile_t.handle)
	int fd;        // its descriptor: all device reads go through read() on it, never through stdio
	UINT8 *buf;    // the window, allocated at the first miss (64-byte aligned)
	long start;    // file offset of buf[0] (a multiple of the sector size)
	size_t len;    // valid bytes in buf
	long lastend;  // where the last device read ended (-1: none yet)
	unsigned run;  // misses in a row that continued a sequential run (the read-ahead grows with it)
	long size;     // size of the file (-1: not known)
	const wpack_policy_t *pol;
	int magic;     // 0: the first bytes were not looked at, 1: the file is a pack, -1: it is not
	UINT8 first[SRP2_HEADER_SIZE + sizeof (srp2_ext_t)]; // the first bytes of the file (read once, with the signature)
	boolean hdrok; // header and extension below are parsed and checked
	srp2_header_t h;
	srp2_ext_t ext;
	char path[192]; // for the re-open of a retry
} pkstream_t;

#define PK_STREAMS 6
static pkstream_t pkst[PK_STREAMS];
static unsigned pkst_next;

// All reads below bounce through 64-byte aligned buffers and whole-sector requests. Caller buffers may be unaligned.
static UINT8 *iobuf, *cbuf, *sbuf; // iobuf: bounce of the bulk reads (indexes, big spans)

static struct { UINT32 cmds, hits, misses, bulk, retries, failures, injected; UINT64 bytes; } pks;
static char pk_err[320];
static UINT32 inj_at, inj_count, inj_seen; // -pkioerr AT[,COUNT]: the AT-th device read attempt (1 = the first) and the COUNT - 1 attempts after it fail (test of RS-07)
static boolean stats_print;

const char *WPack_LastError(void)
{
	return pk_err;
}

void WPack_GetStats(wpack_iostat_t *out)
{
	out->cmds = pks.cmds;
	out->hits = pks.hits;
	out->misses = pks.misses;
	out->bulk = pks.bulk;
	out->retries = pks.retries;
	out->failures = pks.failures;
	out->injected = pks.injected;
	out->bytes = pks.bytes;
}

void WPack_InjectErrors(UINT32 at, UINT32 count)
{
	inj_at = at;
	inj_count = count ? count : 1;
	inj_seen = 0;
}

void WPack_PrintStats(boolean on)
{
	stats_print = on;
}

static boolean PolicyByName(const char *name, int *out)
{
	int i;

	for (i = 0; i < WPK_MEDIA; i++)
		if (!strcasecmp(name, policies[i].name))
		{
			*out = i;
			return true;
		}
	return false;
}

boolean WPack_SetMedium(const char *name)
{
	int m;

	if (!name || !strcasecmp(name, "auto"))
	{
		medium_forced = -1;
		return true;
	}
	if (!PolicyByName(name, &m))
		return false;
	medium_forced = m;
	return true;
}

// Experiments (-pkwin cap,minreq,ahead,gap): the same window for every medium; sizes in bytes, the buffer size at least 4 KiB and at most one LZ4 block (64 KiB)
void WPack_SetWindow(UINT32 cap, UINT32 minreq, UINT32 ahead, UINT32 gap)
{
	int i;

	cap = (cap + WPACK_SECTOR - 1) & ~(UINT32)(WPACK_SECTOR - 1);
	if (cap < 2 * WPACK_SECTOR)
		cap = 2 * WPACK_SECTOR;
	if (cap > WPACK_BLOCK)
		cap = WPACK_BLOCK;
	for (i = 0; i < WPK_MEDIA; i++)
	{
		policies[i].cap = cap;
		policies[i].minreq = min((minreq + WPACK_SECTOR - 1) & ~(UINT32)(WPACK_SECTOR - 1), cap);
		policies[i].ahead = min((ahead + WPACK_SECTOR - 1) & ~(UINT32)(WPACK_SECTOR - 1), cap);
		policies[i].gap = gap;
		if (policies[i].minreq < WPACK_SECTOR)
			policies[i].minreq = WPACK_SECTOR;
	}
}

// The medium a path is on, by its device name. Anything that is not recognised (host:, mc0:, a path without a device) gets the window of the DVD, which is never worse than the
// stdio buffer it replaces.
static const wpack_policy_t *PolicyForPath(const char *path)
{
	int m = WPK_DVD;

	if (medium_forced >= 0)
		return &policies[medium_forced];
	if (path)
	{
		if (!strncasecmp(path, "mass", 4) || !strncasecmp(path, "usb", 3))
			m = WPK_USB;
		else if (!strncasecmp(path, "mx4sio", 6))
			m = WPK_SD;
		else if (!strncasecmp(path, "hdd", 3) || !strncasecmp(path, "pfs", 3))
			m = WPK_HDD;
	}
	return &policies[m];
}

static void PkSleepMs(UINT32 ms)
{
	if (!ms)
		return;
#ifdef PS2
	PS2_SleepUs(ms * 1000); // not DelayThread: PS2-NET-3 (the alarm library loses wake-ups)
#else
	{
		struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

		nanosleep(&ts, NULL);
	}
#endif
}

static pkstream_t *StreamFind(FILE *fp)
{
	unsigned i;

	for (i = 0; i < PK_STREAMS; i++)
		if (pkst[i].fp == fp)
			return &pkst[i];
	return NULL;
}

static void StreamDrop(FILE *fp)
{
	pkstream_t *s = StreamFind(fp);

	if (s)
	{
		free(s->buf);
		memset(s, 0, sizeof *s);
	}
}

static void StreamSetPath(pkstream_t *s, const char *path)
{
	if (path && strlen(path) < sizeof s->path)
	{
		strcpy(s->path, path);
		if (!s->buf) // the window of another policy can only be taken before its buffer exists
			s->pol = PolicyForPath(path);
	}
}

static pkstream_t *StreamNew(FILE *fp, const char *path)
{
	unsigned i;
	pkstream_t *s = NULL;

	for (i = 0; i < PK_STREAMS; i++)
		if (!pkst[i].fp)
		{
			s = &pkst[i];
			break;
		}
	if (!s) // more streams than slots (add-on packs): the oldest slot is reused; its pack reads on with a new slot at its next miss
	{
		s = &pkst[pkst_next++ % PK_STREAMS];
		free(s->buf);
	}
	memset(s, 0, sizeof *s);
	s->fp = fp;
	s->fd = fileno(fp);
	s->lastend = -1;
	s->size = -1;
	s->pol = PolicyForPath(path);
	StreamSetPath(s, path);
	{
		off_t end = lseek(s->fd, 0, SEEK_END);

		if (end >= 0)
			s->size = (long)end;
	}
	return s;
}

static pkstream_t *StreamFor(FILE *fp)
{
	pkstream_t *s = StreamFind(fp);

	return s ? s : StreamNew(fp, NULL);
}

// the file was cut under us (removable medium): once the descriptor is renewed the reads go on at the same places
static boolean StreamReopen(pkstream_t *s)
{
	if (!s->path[0])
		return false;
	if (!freopen(s->path, "rb", s->fp)) // the FILE keeps its address (the engine holds it); on failure it is gone: the caller gives up for good
		return false;
	setvbuf(s->fp, NULL, _IONBF, 0);
	s->fd = fileno(s->fp);
	return true;
}

// One attempt: reads req bytes at start into dst (short only at the end of the file)
static boolean DevReadRaw(pkstream_t *s, long start, UINT8 *dst, size_t req, size_t *gotp, int *errp)
{
	size_t done = 0;

	errno = 0;
	if (lseek(s->fd, (off_t)start, SEEK_SET) != (off_t)start)
	{
		*errp = errno ? errno : EIO;
		return false;
	}
	while (done < req)
	{
		ssize_t r = read(s->fd, dst + done, req - done);

		if (r < 0)
		{
			*errp = errno ? errno : EIO;
			return false;
		}
		if (r == 0)
			break;
		done += (size_t)r;
	}
	*gotp = done;
	return true;
}

#define WPACK_TRIES 4 // the first read and three more; with 4 failures in a row the game stops (docs/GATES/g1/opt13-IO.md, RS-07)
static const UINT32 retry_ms[WPACK_TRIES] = { 0, 100, 250, 1000 };

// One device command of the pack reader, with the retries of RS-07: a short read inside the file or an error is tried again after a pause (the second time after the
// stream was opened again; a stick that dropped off the bus is back after a moment, a scratch on a disc is read again by the drive).
static boolean DevRead(pkstream_t *s, long start, UINT8 *dst, size_t req, size_t *gotp)
{
	unsigned attempt;
	int err = 0;
	size_t got = 0;

	for (attempt = 0; attempt < WPACK_TRIES; attempt++)
	{
		boolean ok;

		if (attempt)
		{
			pks.retries++;
			CONS_Alert(CONS_WARNING, "%s: read of %u bytes at %ld failed (%s), trying again (%u of %d)\n", s->path[0] ? s->path : "pack", (unsigned)req, start,
				err ? strerror(err) : "short read", attempt + 1, WPACK_TRIES);
			PkSleepMs(retry_ms[attempt]);
			if (attempt >= 2 && !StreamReopen(s))
				break;
		}
		pks.cmds++;
		got = 0;
		if (inj_at && ++inj_seen >= inj_at && inj_seen < inj_at + inj_count)
		{
			pks.injected++;
			err = EIO;
			continue;
		}
		ok = DevReadRaw(s, start, dst, req, &got, &err);
		if (ok && (got == req || s->size < 0 || start + (long)got >= s->size))
		{
			pks.bytes += got;
			*gotp = got;
			return true;
		}
		if (ok)
			err = 0; // a short read in the middle of the file
	}
	pks.failures++;
	snprintf(pk_err, sizeof pk_err, "%s: the read of %u bytes at offset %ld failed %d times (%s) - check the disc, the cable or the stick", s->path[0] ? s->path : "pack",
		(unsigned)req, start, WPACK_TRIES, err ? strerror(err) : "short read");
	return false;
}

// Bulk read of [pos, pos + size) in whole sectors through the bounce buffer: the indexes of a pack and the lumps and blocks bigger than a window
static boolean ReadBulk(pkstream_t *s, long pos, void *dest, size_t size)
{
	UINT8 *out = dest;
	size_t done = 0;

	if (pos < 0 || size > (size_t)(LONG_MAX - pos))
		return false;
	if (!iobuf)
		iobuf = WPACK_ALIGNED_ALLOC(WPACK_BLOCK);
	if (!iobuf)
		return false;
	pks.bulk++;
	while (done < size)
	{
		size_t skip = (size_t)pos % WPACK_SECTOR;
		size_t want = min(size - done, WPACK_BLOCK - skip);
		size_t request = (skip + want + WPACK_SECTOR - 1) & ~(size_t)(WPACK_SECTOR - 1), got;
		long start = pos - (long)skip;

		LP_BEGIN(lpr);

		if (s->size >= 0 && start + (long)request > s->size) // the end of the file: never ask for what is not there
		{
			if (start >= s->size)
				return false;
			request = (size_t)(s->size - start);
		}
		if (!DevRead(s, start, iobuf, request, &got))
			return false;
		LP_END(PK_FREAD, lpr);
		if (got < skip + want) // never copy bytes not actually read (signature probes may see short files)
			return false;
		memcpy(out + done, iobuf + skip, want);
		done += want;
		pos += (long)want;
		s->lastend = start + (long)got;
	}
	return true;
}

// A miss of the window: reads the sectors that hold [pos, pos + n) and, when the miss continues a sequential run, as much as the policy says. `hint` is the number of
// bytes after pos the caller knows it needs next (the blocks of one lump).
static boolean Fill(pkstream_t *s, long pos, size_t n, size_t hint)
{
	const wpack_policy_t *p = s->pol;
	const long s0 = pos - pos % WPACK_SECTOR;
	const size_t lead = (size_t)(pos - s0);
	size_t want = max(n, hint), req, got;

	if (want > p->cap - lead)
		want = p->cap - lead;
	req = (lead + want + WPACK_SECTOR - 1) & ~(size_t)(WPACK_SECTOR - 1);
	if (s->lastend >= 0 && s0 >= s->lastend && (UINT64)(s0 - s->lastend) <= p->gap)
	{
		// a sequential run: read ahead, twice as much with every miss that continues it (the start-up reads a pack in the order it is stored; a lump met by chance while
		// playing does not start a run)
		const unsigned run = ++s->run;
		const size_t grown = run < 8 ? (size_t)p->minreq << run : p->ahead;

		if (req < p->ahead && req < grown)
			req = min(grown, (size_t)p->ahead);
	}
	else
		s->run = 0;
	if (req < p->minreq)
		req = p->minreq;
	if (req > p->cap)
		req = p->cap;
	if (s->size >= 0 && s0 + (long)req > s->size)
	{
		if (s0 >= s->size)
			return false;
		req = (size_t)(s->size - s0);
	}
	if (!s->buf)
	{
		s->buf = WPACK_ALIGNED_ALLOC(p->cap);
		if (!s->buf)
			return false;
	}
	pks.misses++;
	s->len = 0;
	if (!DevRead(s, s0, s->buf, req, &got))
		return false;
	s->start = s0;
	s->len = got;
	s->lastend = s0 + (long)got;
	return got > lead;
}

static boolean InWindow(const pkstream_t *s, long pos, size_t n)
{
	return s->buf && s->len && pos >= s->start && pos + (long)n <= s->start + (long)s->len;
}

// ---- OPT13-IO (RS-02): the working set of a level in one sorted pass ------------------------------------------------------------------------------
// In the Hardware renderer nothing is read ahead: every texture, flat and sprite is read from the pack when it is first drawn, one lump at a time, in the order the camera meets
// them (975 reads for 358 different lumps in 700 frames of DEMO_002). The caller lists the lumps the level will probably need (WPack_PrefetchAdd); WPack_PrefetchRun sorts them
// by position, reads them in one pass (holes smaller than the medium's `merge` are read through) and keeps their STORED bytes in one cache block (PU_CACHE, owned by `pf.blob`:
// the zone takes it back under pressure and clears the pointer; every later read of a listed lump then finds the bytes in the block, or falls back to the window).
// The block holds compressed bytes: decoding happens as before, the content that comes out of the reader is the same.
typedef struct { FILE *fp; UINT32 pos, disk, blobofs; } pf_item_t;

static struct
{
	pf_item_t *items;
	UINT32 n, cap;
	UINT8 *blob;
	UINT32 bloblen;
	UINT32 hits, hitbytes, runs, ranges;
} pf;

static const UINT8 *PfLookup(FILE *fp, long pos, size_t n)
{
	UINT32 lo = 0, hi = pf.n;

	if (!pf.blob || !pf.n || pos < 0)
		return NULL;
	while (lo < hi) // the last item that starts at or before (fp, pos)
	{
		const UINT32 mid = (lo + hi) / 2;
		const pf_item_t *it = &pf.items[mid];

		if (it->fp < fp || (it->fp == fp && (long)it->pos <= pos))
			lo = mid + 1;
		else
			hi = mid;
	}
	if (!lo)
		return NULL;
	{
		const pf_item_t *it = &pf.items[lo - 1];

		if (it->fp != fp || pos + (long)n > (long)it->pos + (long)it->disk)
			return NULL;
		pf.hits++;
		pf.hitbytes += (UINT32)n;
		return pf.blob + it->blobofs + (pos - (long)it->pos);
	}
}

void WPack_PrefetchBegin(void)
{
	pf.n = 0;
}

void WPack_PrefetchDrop(void)
{
	if (pf.blob)
		Z_Free(pf.blob);
	pf.blob = NULL;
	pf.n = 0;
}

void WPack_PrefetchAdd(FILE *fp, const lumpinfo_t *l)
{
	if (!fp || !l || !l->disksize || l->disksize > 98304 || l->position > LONG_MAX)
		return;
	if (l->compression != CM_NOCOMPRESSION && l->compression != CM_LZ4)
		return;
	if (pf.n == pf.cap)
	{
		const UINT32 cap = pf.cap ? pf.cap * 2 : 1024;
		pf_item_t *grown = realloc(pf.items, cap * sizeof *grown);

		if (!grown || cap > 65536)
			return;
		pf.items = grown;
		pf.cap = cap;
	}
	pf.items[pf.n].fp = fp;
	pf.items[pf.n].pos = (UINT32)l->position;
	pf.items[pf.n].disk = (UINT32)l->disksize;
	pf.items[pf.n].blobofs = 0;
	pf.n++;
}

static int PfCmp(const void *a, const void *b)
{
	const pf_item_t *x = a, *y = b;

	if (x->fp != y->fp)
		return x->fp < y->fp ? -1 : 1;
	return x->pos < y->pos ? -1 : x->pos > y->pos;
}

// Reads the lumps listed since WPack_PrefetchBegin. `budget`: at most this many stored bytes are kept. `pump` (may be NULL) is called between the device reads (the network
// keeps its time-out alive). Returns the bytes kept in the block (0: nothing was prefetched: no room, or nothing listed).
UINT32 WPack_PrefetchRun(UINT32 budget, void (*pump)(void))
{
	UINT32 i, j, k, total = 0, uniq = 0;
	UINT8 *blob;

	if (pf.blob)
	{
		Z_Free(pf.blob);
		pf.blob = NULL;
	}
	if (!pf.n || !budget)
		return 0;
	qsort(pf.items, pf.n, sizeof *pf.items, PfCmp);
	for (i = 0; i < pf.n; i++) // the same lump listed twice (a texture used on many sides) counts once; what does not fit the budget is left out
	{
		const UINT32 size = (pf.items[i].disk + 15) & ~15u;

		if (uniq && pf.items[uniq - 1].fp == pf.items[i].fp && pf.items[uniq - 1].pos == pf.items[i].pos)
			continue;
		if (total + size > budget)
			continue;
		pf.items[uniq] = pf.items[i];
		pf.items[uniq].blobofs = total;
		total += size;
		uniq++;
	}
	pf.n = uniq;
	if (!total)
		return 0;
	blob = Z_TryMallocAlign(total, PU_CACHE, &pf.blob, 4);
	if (!blob)
	{
		pf.n = 0;
		pf.blob = NULL;
		return 0;
	}
	pf.blob = blob; // (the zone sets the owner itself; this is for the host test)
	if (!iobuf)
		iobuf = WPACK_ALIGNED_ALLOC(WPACK_BLOCK);
	if (!iobuf)
	{
		Z_Free(blob);
		pf.blob = NULL;
		pf.n = 0;
		return 0;
	}
	pf.runs++;
	// the reads: ranges of items of one file, holes up to `merge` read through
	for (i = 0; i < pf.n; i = j)
	{
		pkstream_t *s = StreamFor(pf.items[i].fp);
		const UINT32 hole = s->pol->merge;
		UINT32 end = pf.items[i].pos + pf.items[i].disk, from, to, first = i;
		long cs;

		for (j = i + 1; j < pf.n && pf.items[j].fp == pf.items[i].fp && pf.items[j].pos <= end + hole; j++)
			if (pf.items[j].pos + pf.items[j].disk > end)
				end = pf.items[j].pos + pf.items[j].disk;
		from = pf.items[i].pos - pf.items[i].pos % WPACK_SECTOR;
		to = (end + WPACK_SECTOR - 1) & ~(UINT32)(WPACK_SECTOR - 1);
		if (s->size >= 0 && (long)to > s->size)
			to = (UINT32)s->size;
		pf.ranges++;
		for (cs = from; cs < (long)to; cs += WPACK_BLOCK)
		{
			size_t want = min((size_t)((long)to - cs), (size_t)WPACK_BLOCK), got;

			if (!DevRead(s, cs, iobuf, want, &got)) // the lumps of this range stay unprefetched: the window reads them (and retries) when they are needed
			{
				for (k = first; k < j; k++)
					pf.items[k].disk = 0;
				break;
			}
			s->lastend = cs + (long)got;
			for (k = first; k < j; k++) // the part of each item inside this chunk
			{
				const long a = max((long)pf.items[k].pos, cs), b = min((long)pf.items[k].pos + (long)pf.items[k].disk, cs + (long)got);

				if ((long)pf.items[k].pos >= cs + (long)got)
					break;
				if (a < b)
					memcpy(blob + pf.items[k].blobofs + (a - (long)pf.items[k].pos), iobuf + (a - cs), (size_t)(b - a));
			}
			while (first < j && (long)pf.items[first].pos + (long)pf.items[first].disk <= cs + (long)got)
				first++;
		}
		if (pump)
			pump();
	}
	pf.bloblen = total;
	return total;
}

void WPack_PrefetchStats(UINT32 *hits, UINT32 *hitbytes, UINT32 *ranges, UINT32 *kept)
{
	*hits = pf.hits;
	*hitbytes = pf.hitbytes;
	*ranges = pf.ranges;
	*kept = pf.blob ? pf.bloblen : 0;
}

// [pos, pos + size) into dest, through the window (a big span goes through the bounce buffer: it would only push the window out)
static boolean ReadAt(pkstream_t *s, long pos, void *dest, size_t size)
{
	UINT8 *out = dest;

	if (pos < 0 || size > (size_t)(LONG_MAX - pos))
		return false;
	{
		const UINT8 *pre = PfLookup(s->fp, pos, size); // RS-02: the lump was read by the prefetch pass

		if (pre)
		{
			memcpy(dest, pre, size);
			return true;
		}
	}
	if (size >= s->pol->cap)
		return ReadBulk(s, pos, dest, size);
	while (size)
	{
		if (s->buf && s->len && pos >= s->start && pos < s->start + (long)s->len)
		{
			size_t n = min(size, (size_t)(s->start + (long)s->len - pos));

			pks.hits++;
			memcpy(out, s->buf + (pos - s->start), n);
			out += n;
			pos += (long)n;
			size -= n;
		}
		else if (!Fill(s, pos, size, 0))
			return false;
	}
	return true;
}

// A pointer to [pos, pos + n) inside the window (read from the device when the window does not hold it), or NULL: when n does not fit a window (*failed false: read it
// with ReadAt) or when the device read failed for good (*failed true). Good until the next read of the same stream. `hint`: see Fill.
static const UINT8 *PeekAt(pkstream_t *s, long pos, size_t n, size_t hint, boolean *failed)
{
	const UINT8 *pre;

	*failed = false;
	if (pos < 0 || n == 0)
		return NULL;
	if ((pre = PfLookup(s->fp, pos, n)) != NULL)
		return pre;
	if (n > s->pol->cap - WPACK_SECTOR)
		return NULL;
	if (InWindow(s, pos, n))
	{
		pks.hits++;
		return s->buf + (pos - s->start);
	}
	if (!Fill(s, pos, n, hint) || !InWindow(s, pos, n))
	{
		*failed = true;
		return NULL;
	}
	return s->buf + (pos - s->start);
}

long WPack_FileSize(FILE *handle)
{
	pkstream_t *s = StreamFind(handle);

	return s ? s->size : -1;
}

// Is the file a pack? (the first bytes, read below stdio: the stream's own buffer is chosen after the answer; the answer and the bytes are kept)
static boolean PeekMagic(pkstream_t *s)
{
	if (!s->magic)
	{
		memset(s->first, 0, sizeof s->first);
		s->magic = ReadBulk(s, 0, s->first, sizeof s->first) && memcmp(s->first, "SRP2", 4) == 0 ? 1 : -1;
		lseek(s->fd, 0, SEEK_SET); // what stdio will do next starts at the beginning, as for a stream nobody touched
	}
	return s->magic > 0;
}

boolean WPack_Detect(FILE *handle)
{
	pkstream_t *s = StreamFind(handle);

	if (!s)
		s = StreamNew(handle, NULL);
	if (PeekMagic(s))
		return true;
	// PS2-103: a file that is not a pack goes to the original loaders (ResGetLumpsWad reads the header from the current position)
	StreamDrop(handle);
	fseek(handle, 0, SEEK_SET);
	return false;
}

// Prepares the stream before ANY other operation on it. A pack stream is read below stdio: no stdio buffer (it would only be a second copy of the window, 64 KiB
// per pack); any other file (the zip and wad add-ons) gets the 64 KiB stdio buffer as before. Returns something free() accepts, NULL if memory is short.
void *WPack_SetupHandleEx(FILE *handle, const char *path)
{
	pkstream_t *s;
	void *buf;

	StreamDrop(handle); // a slot of an earlier stream with the same address
	pf.n = 0;           // and the prefetch list: its items are keyed by the address of the stream
	s = StreamNew(handle, path);
	if (PeekMagic(s))
	{
		setvbuf(handle, NULL, _IONBF, 0);
		return malloc(16);
	}
	StreamDrop(handle);
	buf = WPACK_ALIGNED_ALLOC(65536);
	if (buf && setvbuf(handle, buf, _IOFBF, 65536) != 0)
	{
		free(buf);
		buf = NULL;
	}
	return buf;
}

void *WPack_SetupHandle(FILE *handle)
{
	return WPack_SetupHandleEx(handle, NULL);
}

// The checksum of the index sections of a v2 pack (tools/ps2/cook.py fletcher): little endian UINT32 words, a += w; b += a; folded to a ^ rotl(b, 16)
typedef struct { UINT32 a, b; } wpack_sum_t;

static void SumAdd(wpack_sum_t *sum, const void *data, size_t bytes)
{
	const UINT8 *p = data;
	UINT32 a = sum->a, b = sum->b;
	size_t i;

	for (i = 0; i + 4 <= bytes; i += 4)
	{
		a += (UINT32)p[i] | ((UINT32)p[i + 1] << 8) | ((UINT32)p[i + 2] << 16) | ((UINT32)p[i + 3] << 24);
		b += a;
	}
	if (i < bytes) // the data is zero padded to a whole word
	{
		UINT32 w = 0;
		size_t k;

		for (k = 0; i + k < bytes; k++)
			w |= (UINT32)p[i + k] << (8 * k);
		a += w;
		b += a;
	}
	sum->a = a;
	sum->b = b;
}

static UINT32 SumFold(const wpack_sum_t *sum)
{
	return sum->a ^ ((sum->b << 16) | (sum->b >> 16));
}

// Reads the header (and the v2 extension) of the pack at the start of the stream. `name` is for the messages only.
static boolean headsdropped; // the start-up is over: packs opened from now on keep no head table (WPack_DropHeads)

static boolean ReadHeader(FILE *handle, const char *name, srp2_header_t *h, srp2_ext_t *ext)
{
	pkstream_t *s = StreamFor(handle);
	long end;
	UINT8 first[SRP2_HEADER_SIZE + sizeof (srp2_ext_t)];

	if (s->hdrok) // OPT13-IO (RS-01): one parse of the header per pack (W_InitFile asked three times, each time reading it again)
	{
		*h = s->h;
		*ext = s->ext;
		return true;
	}
	if (s->magic > 0)
		memcpy(first, s->first, SRP2_HEADER_SIZE); // read when the stream was prepared
	else if (!ReadBulk(s, 0, first, SRP2_HEADER_SIZE))
	{
		CONS_Alert(CONS_ERROR, "%s: cannot read the pack header\n", name);
		return false;
	}
	memcpy(h, first, sizeof *h);
	if (memcmp(h->magic, "SRP2", 4) != 0)
	{
		CONS_Alert(CONS_ERROR, "%s: not a pack (bad signature)\n", name);
		return false;
	}
	end = s->size;
	h->version = LONG(h->version);
	h->headersize = LONG(h->headersize);
	h->flags = LONG(h->flags);
	h->numlumps = LONG(h->numlumps);
	h->tableoffset = LONG(h->tableoffset);
	h->pooloffset = LONG(h->pooloffset);
	h->poolsize = LONG(h->poolsize);
	h->dataoffset = LONG(h->dataoffset);
	h->filesize = LONG(h->filesize);
	h->blocksize = LONG(h->blocksize);
	if (h->version < SRP2_VERSION_MIN || h->version > SRP2_VERSION_MAX)
	{
		CONS_Alert(CONS_ERROR, "%s: pack version %u is not supported (this engine reads versions %d to %d)%s\n", name, (unsigned)h->version,
			SRP2_VERSION_MIN, SRP2_VERSION_MAX, h->version > SRP2_VERSION_MAX ? ": the pack is newer than the engine" : "");
		return false;
	}
	if (h->headersize != SRP2_HEADER_SIZE || h->blocksize != WPACK_BLOCK)
	{
		CONS_Alert(CONS_ERROR, "%s: unsupported pack header (header size %u, block size %u)\n", name, (unsigned)h->headersize, (unsigned)h->blocksize);
		return false;
	}
	if (end < 0 || h->filesize != (UINT32)end)
	{
		CONS_Alert(CONS_ERROR, "%s: pack is truncated or damaged (size %ld, expected %u)\n", name, end, (unsigned)h->filesize);
		return false;
	}
	memset(ext, 0, sizeof *ext);
	if (h->version >= 2)
	{
		if (s->magic > 0)
			memcpy(ext, s->first + SRP2_HEADER_SIZE, sizeof *ext);
		else if (!ReadBulk(s, SRP2_HEADER_SIZE, ext, sizeof *ext))
		{
			CONS_Alert(CONS_ERROR, "%s: cannot read the pack header extension\n", name);
			return false;
		}
		ext->extsize = LONG(ext->extsize);
		ext->headoffset = LONG(ext->headoffset);
		ext->headbytes = LONG(ext->headbytes);
		ext->crcoffset = LONG(ext->crcoffset);
		ext->chktable = LONG(ext->chktable);
		ext->chkpool = LONG(ext->chkpool);
		ext->chkhead = LONG(ext->chkhead);
		ext->chkcrc = LONG(ext->chkcrc);
	}
	if (h->numlumps == 0 || h->numlumps > UINT16_MAX || h->poolsize == 0
		|| (h->flags & ~(SRP2_FLAG_NONMUSIC | (h->version >= 2 ? SRP2_FLAG_HEAD : 0))) != 0
		|| h->filesize % WPACK_SECTOR != 0
		|| h->tableoffset < SRP2_HEADER_SIZE || h->tableoffset % WPACK_SECTOR != 0
		|| h->tableoffset > h->filesize
		|| h->numlumps * sizeof(srp2_entry_t) > h->filesize - h->tableoffset
		|| h->pooloffset < h->tableoffset + h->numlumps * sizeof(srp2_entry_t)
		|| h->pooloffset % WPACK_SECTOR != 0 || h->pooloffset > h->filesize
		|| h->poolsize > h->filesize - h->pooloffset
		|| h->dataoffset < h->pooloffset + h->poolsize
		|| h->dataoffset % WPACK_SECTOR != 0 || h->dataoffset > h->filesize)
	{
		CONS_Alert(CONS_ERROR, "%s: pack header layout is corrupt\n", name);
		return false;
	}
	if (h->version >= 2 && (h->flags & SRP2_FLAG_HEAD))
	{
		const UINT32 headend = ext->headoffset + h->numlumps * ext->headbytes, crcend = ext->crcoffset + h->numlumps * 4;

		if (ext->extsize != sizeof *ext || ext->headbytes == 0 || ext->headbytes > 64 || ext->headbytes % 4
			|| ext->headoffset % WPACK_SECTOR || ext->crcoffset % WPACK_SECTOR
			|| ext->headoffset < h->pooloffset + h->poolsize || headend < ext->headoffset || ext->crcoffset < headend || crcend < ext->crcoffset
			|| crcend > h->dataoffset)
		{
			CONS_Alert(CONS_ERROR, "%s: pack header extension is corrupt\n", name);
			return false;
		}
	}
	s->h = *h;
	s->ext = *ext;
	s->hdrok = true;
	return true;
}

int WPack_VerifyNMUS(FILE *handle)
{
	srp2_header_t h;
	srp2_ext_t ext;

	if (!ReadHeader(handle, "pack", &h, &ext))
		return -1;
	return (h.flags & SRP2_FLAG_NONMUSIC) ? 0 : 1;
}

static boolean AllocBuffers(void)
{
	if (!cbuf)
		cbuf = WPACK_ALIGNED_ALLOC(WPACK_BLOCK);
	if (!sbuf)
		sbuf = WPACK_ALIGNED_ALLOC(WPACK_BLOCK);
	return cbuf && sbuf;
}

lumpinfo_t *WPack_GetLumps(FILE *handle, const char *filename, UINT16 *nlmp, void **poolp, boolean *nonmusic, wpack_t **packp)
{
	srp2_header_t h;
	srp2_ext_t ext;
	srp2_entry_t chunk[ENTRY_CHUNK];
	lumpinfo_t *lumpinfo, *lump_p;
	char *pool;
	UINT32 i, n, done, prevend;
	wpack_sum_t sumtable = { 0, 0 }, sumpool = { 0, 0 };
	const char *shortname = filename ? filename : "pack";
	const boolean v2 = false;

	*nlmp = 0;
	*poolp = NULL;
	*nonmusic = false;
	if (packp)
		*packp = NULL;

	StreamSetPath(StreamFor(handle), filename); // the medium of the pack (the window), the name for the retry of a read
	if (!ReadHeader(handle, shortname, &h, &ext))
		return NULL;
	(void)v2;

	n = h.numlumps;
	prevend = h.dataoffset;

	// one block for every name of the pack (instead of two mallocs per lump)
	pool = Z_Malloc(h.poolsize, PU_STATIC, NULL);
	if (!ReadBulk(StreamFor(handle), (long)h.pooloffset, pool, h.poolsize) || pool[h.poolsize - 1] != '\0')
	{
		CONS_Alert(CONS_ERROR, "%s: pack string pool is corrupt\n", shortname);
		Z_Free(pool);
		return NULL;
	}
	if (h.version >= 2)
	{
		SumAdd(&sumpool, pool, h.poolsize);
		if (SumFold(&sumpool) != ext.chkpool)
		{
			CONS_Alert(CONS_ERROR, "%s: pack string pool is damaged (checksum)\n", shortname);
			Z_Free(pool);
			return NULL;
		}
	}

	lump_p = lumpinfo = Z_Malloc(n * sizeof (*lumpinfo), PU_STATIC, NULL);

	for (done = 0; done < n; done += ENTRY_CHUNK)
	{
		UINT32 count = min(n - done, ENTRY_CHUNK);

		if (!ReadBulk(StreamFor(handle), (long)h.tableoffset + (long)done * (long)sizeof (srp2_entry_t), chunk, sizeof (srp2_entry_t) * count))
		{
			CONS_Alert(CONS_ERROR, "%s: failed to read the pack table\n", shortname);
			Z_Free(lumpinfo);
			Z_Free(pool);
			return NULL;
		}
		if (h.version >= 2)
			SumAdd(&sumtable, chunk, sizeof (srp2_entry_t) * count);

		for (i = 0; i < count; i++, lump_p++)
		{
			const srp2_entry_t *e = &chunk[i];
			UINT32 position = LONG(e->position), disksize = LONG(e->disksize), size = LONG(e->size);
			UINT32 fullname = LONG(e->fullname), longname = LONG(e->longname), codec = LONG(e->codec);
			const char *full;
			const char *trimname, *dotpos;

			if (fullname >= h.poolsize || longname >= h.poolsize
				|| codec > SRP2_CODEC_LZ4
				|| (size == 0 && (disksize != 0 || codec != SRP2_CODEC_RAW))
				|| (size != 0 && (disksize == 0 || position < (h.version >= 2 ? h.dataoffset : prevend) || position > h.filesize
					|| disksize > h.filesize - position
					|| position % (size >= WPACK_BLOCK ? WPACK_SECTOR : 64) != 0))
				|| (codec == SRP2_CODEC_RAW && disksize != size)
				|| (codec == SRP2_CODEC_LZ4 && size <= WPACK_BLOCK && disksize > WPACK_BLOCK)
				|| (codec == SRP2_CODEC_LZ4 && size > WPACK_BLOCK
					&& disksize < (1 + (size - 1) / WPACK_BLOCK) * sizeof(UINT32)))
			{
				CONS_Alert(CONS_ERROR, "%s: pack table entry %u is corrupt\n", shortname, (unsigned)(done + i));
				Z_Free(lumpinfo);
				Z_Free(pool);
				return NULL;
			}
			if (size && h.version < 2)
				prevend = position + disksize; // v1: payloads follow each other without overlap (v2 stores identical lumps once)

			lump_p->position = position; // final position of the lump data
			lump_p->disksize = disksize;
			lump_p->diskpath = NULL;
			lump_p->size = size;

			full = pool + fullname;
			lump_p->fullname = (char *)full;
			lump_p->longname = pool + longname;

			// the 8 character name and its hash: same code as ResGetLumpsZip
			if ((trimname = strrchr(full, '/')) != 0)
				trimname++;
			else
				trimname = full;

			if ((dotpos = strrchr(trimname, '.')) == 0)
				dotpos = full + strlen(full);
			if (strlen(lump_p->longname) != (size_t)(dotpos - trimname)
				|| memcmp(lump_p->longname, trimname, (size_t)(dotpos - trimname)) != 0)
			{
				CONS_Alert(CONS_ERROR, "%s: pack entry %u has a corrupt longname\n", shortname, (unsigned)(done + i));
				Z_Free(lumpinfo);
				Z_Free(pool);
				return NULL;
			}

			memset(lump_p->name, '\0', 9);
			strncpy(lump_p->name, trimname, min(8, dotpos - trimname));
			lump_p->hash = quickncasehash(lump_p->name, 8);

			switch (codec)
			{
			case SRP2_CODEC_RAW:
				lump_p->compression = CM_NOCOMPRESSION;
				break;
			case SRP2_CODEC_LZ4:
				lump_p->compression = CM_LZ4;
				break;
			default:
				CONS_Alert(CONS_WARNING, "%s: Unsupported compression method\n", full);
				lump_p->compression = CM_UNSUPPORTED;
				break;
			}
		}
	}
	if (h.version >= 2 && SumFold(&sumtable) != ext.chktable)
	{
		CONS_Alert(CONS_ERROR, "%s: pack table is damaged (checksum)\n", shortname);
		Z_Free(lumpinfo);
		Z_Free(pool);
		return NULL;
	}

	if (packp && h.version >= 2 && (h.flags & SRP2_FLAG_HEAD))
	{
		// the head table: the first bytes of every lump, read as one sequential run; kept until WPack_DropHeads (the start-up reads some 22 000 patch headers)
		wpack_t *pk = Z_Calloc(sizeof *pk, PU_STATIC, NULL);

		pk->version = h.version;
		pk->numlumps = n;
		pk->headoffset = ext.headoffset;
		pk->headbytes = ext.headbytes;
		pk->crcoffset = ext.crcoffset;
		pk->chkcrc = ext.chkcrc;
		strlcpy(pk->name, shortname, sizeof pk->name);
		if (n >= HEADTABLE_MINLUMPS && !headsdropped)
		{
			size_t bytes = (size_t)n * ext.headbytes;
			wpack_sum_t sumhead = { 0, 0 };

			pk->head = Z_TryMallocAlign(bytes, PU_STATIC, NULL, 2);
			if (pk->head)
			{
				if (!ReadBulk(StreamFor(handle), (long)ext.headoffset, pk->head, bytes))
				{
					CONS_Alert(CONS_ERROR, "%s: failed to read the pack head table\n", shortname);
					Z_Free(pk->head);
					Z_Free(pk);
					Z_Free(lumpinfo);
					Z_Free(pool);
					return NULL;
				}
				SumAdd(&sumhead, pk->head, bytes);
				if (SumFold(&sumhead) != ext.chkhead)
				{
					CONS_Alert(CONS_ERROR, "%s: pack head table is damaged (checksum)\n", shortname);
					Z_Free(pk->head);
					Z_Free(pk);
					Z_Free(lumpinfo);
					Z_Free(pool);
					return NULL;
				}
			}
		}
		*packp = pk;
	}

	{
		// The window is allocated now, while the heap is empty (like the stdio buffer it replaces), not at the first miss while playing: there the arena has no room to spare
		// and the buffer would push cache blocks out or fragment the free space.
		pkstream_t *s = StreamFor(handle);

		if (!s->buf)
			s->buf = WPACK_ALIGNED_ALLOC(s->pol->cap);
		AllocBuffers(); // the decode buffers and the bounce buffer too (a failure shows at the first read that needs them)
		if (!iobuf)
			iobuf = WPACK_ALIGNED_ALLOC(WPACK_BLOCK);
	}
	*nlmp = (UINT16)n;
	*poolp = pool;
	*nonmusic = (h.flags & SRP2_FLAG_NONMUSIC) != 0;
	return lumpinfo;
}

void WPack_Shutdown(void)
{
	unsigned i;

	if (stats_print)
		printf("IOSTAT pack reader: device reads %u (%u bulk, %u window misses), %u window hits, %.2f MB, retries %u, failures %u, injected %u\n", (unsigned)pks.cmds, (unsigned)pks.bulk,
			(unsigned)pks.misses, (unsigned)pks.hits, (double)pks.bytes / 1048576.0, (unsigned)pks.retries, (unsigned)pks.failures, (unsigned)pks.injected);
	for (i = 0; i < PK_STREAMS; i++)
	{
		free(pkst[i].buf);
		memset(&pkst[i], 0, sizeof pkst[i]);
	}
	free(iobuf);
	free(cbuf);
	free(sbuf);
	iobuf = cbuf = sbuf = NULL;
}

// Delivers decoded bytes [lo, hi) of one block (decoded size bsize, stored in csize bytes at file offset *pos; *pos moves past the block)
// to dst (which receives byte lo). Returns false on a read/decode error.
static boolean ReadBlock(pkstream_t *s, long *pos, boolean raw, UINT32 csize, UINT32 bsize, UINT32 lo, UINT32 hi, UINT8 *dst, size_t hint)
{
	UINT32 want;
	unsigned pass;

	if (csize == 0 || csize > WPACK_BLOCK || bsize > WPACK_BLOCK || lo > hi || hi > bsize)
		return false;
	want = hi - lo;

	if (raw)
	{
		if (csize != bsize || !ReadAt(s, *pos + (long)lo, dst, want)) // only the bytes asked for
			return false;
		*pos += (long)csize;
		return true;
	}

	for (pass = 0; pass < 2; pass++)
	{
		boolean peekfailed, ok;
		const UINT8 *src = PeekAt(s, *pos, csize, hint, &peekfailed); // the block decoded straight from the window: no copy into cbuf
		LP_BEGIN(lpz);

		if (!src)
		{
			if (peekfailed || !ReadAt(s, *pos, cbuf, csize))
				return false;
			src = cbuf;
		}
		if (lo == 0 && want == bsize) // whole block straight into the destination
			ok = LZ4_decompress_safe((const char *)src, (char *)dst, (int)csize, (int)bsize) == (int)bsize;
		else if (lo == 0) // head of the block (patch headers etc): stop as soon as enough is decoded
			ok = LZ4_decompress_safe_partial((const char *)src, (char *)dst, (int)csize, (int)want, (int)want) == (int)want;
		else if (LZ4_decompress_safe((const char *)src, (char *)sbuf, (int)csize, (int)bsize) != (int)bsize)
			ok = false;
		else
		{
			memcpy(dst, sbuf + lo, want);
			ok = true;
		}
		LP_END(PK_LZ4, lpz);
		if (ok)
		{
			*pos += (long)csize;
			return true;
		}
		s->len = 0; // a block that does not decode: once more from the device (a transfer error that the drive did not report; RS-07)
		if (pass == 0)
			pks.retries++;
	}
	snprintf(pk_err, sizeof pk_err, "%s: a block of %u bytes at offset %ld does not decode - the pack is damaged (run -verifypack)", s->path[0] ? s->path : "pack", (unsigned)csize, *pos);
	return false;
}

size_t WPack_ReadLump(FILE *handle, const lumpinfo_t *l, void *dest, size_t size, size_t offset)
{
	UINT8 *out = dest;
	UINT32 usize = (UINT32)l->size;
	UINT32 index[64];
	UINT32 *idx = index;
	UINT32 nb, first, last, b, remaining;
	long pos;
	size_t done = 0, ahead = 0;
	pkstream_t *s;

	if (l->size > UINT32_MAX || l->position > LONG_MAX || l->disksize > (unsigned long)LONG_MAX - l->position
		|| !dest || !size || offset >= usize || size > usize - offset)
		return 0;
	s = StreamFor(handle);
	pk_err[0] = '\0';
	if (l->compression == CM_NOCOMPRESSION)
	{
		if (l->disksize != usize)
			return 0;
		return ReadAt(s, (long)(l->position + offset), dest, size) ? size : 0;
	}
	if (l->compression != CM_LZ4 || !AllocBuffers())
		return 0;

	if (usize <= WPACK_BLOCK) // one LZ4 block, no index
	{
		long p1 = (long)l->position;

		return ReadBlock(s, &p1, false, (UINT32)l->disksize, usize, (UINT32)offset, (UINT32)(offset + size), out, 0) ? size : 0;
	}

	// u32 index[nb] (bit 31: stored raw) followed by the blocks
	nb = 1 + (usize - 1) / WPACK_BLOCK;
	if (nb * sizeof *idx > l->disksize)
		return 0;
	first = (UINT32)(offset / WPACK_BLOCK);
	last = (UINT32)((offset + size - 1) / WPACK_BLOCK);
	if (nb > sizeof index / sizeof index[0])
	{
		idx = malloc(nb * sizeof *idx);
		if (!idx)
			return 0;
	}

	if (!ReadAt(s, (long)l->position, idx, nb * sizeof *idx))
		goto end;

	pos = (long)(l->position + nb * sizeof *idx);
	remaining = (UINT32)(l->disksize - nb * sizeof *idx);
	for (b = 0; b < nb; b++) // validate the entire index before delivering even a partial read
	{
		UINT32 entry = LONG(idx[b]), csize = entry & ~SRP2_RAWBLOCK;
		UINT32 bsize = min(WPACK_BLOCK, usize - b * WPACK_BLOCK);

		if (!csize || csize > WPACK_BLOCK || csize > remaining
			|| ((entry & SRP2_RAWBLOCK) && csize != bsize))
			goto end;
		remaining -= csize;
		if (b < first)
			pos += (long)csize;
		else if (b <= last)
			ahead += csize; // the bytes of the blocks to read: a miss of the window takes as much of them as fits
	}
	if (remaining != 0)
		goto end;

	for (b = first; b <= last; b++) // the blocks first..last are contiguous: *pos walks over them
	{
		UINT32 entry = LONG(idx[b]);
		UINT32 bstart = b * WPACK_BLOCK;
		UINT32 bsize = min(WPACK_BLOCK, usize - bstart);
		UINT32 lo = (UINT32)max(offset, bstart) - bstart;
		UINT32 hi = (UINT32)min(offset + size, (size_t)bstart + bsize) - bstart;

		if (!ReadBlock(s, &pos, (entry & SRP2_RAWBLOCK) != 0, entry & ~SRP2_RAWBLOCK, bsize, lo, hi, out + done, ahead))
			break;
		ahead -= min(ahead, (size_t)(entry & ~SRP2_RAWBLOCK));
		done += hi - lo;
	}

end:
	if (idx != index)
		free(idx);
	return done;
}

// ---- PS2-LOAD-10: head table, integrity check ------------------------------------------------------------------------------------------------

static wpack_t *packs[16]; // the open packs that keep a head table (WPack_DropHeads, WPack_Close)

void WPack_Register(wpack_t *pk)
{
	int i;

	for (i = 0; i < (int)(sizeof packs / sizeof packs[0]); i++)
		if (!packs[i])
		{
			packs[i] = pk;
			return;
		}
}

void WPack_Close(wpack_t *pk)
{
	int i;

	if (!pk)
		return;
	for (i = 0; i < (int)(sizeof packs / sizeof packs[0]); i++)
		if (packs[i] == pk)
			packs[i] = NULL;
	Z_Free(pk->head);
	Z_Free(pk);
}

// The start-up is over: the head tables (235 KB for the four game packs) give their memory back; later reads take the normal path
void WPack_DropHeads(void)
{
	int i;

	headsdropped = true;
	for (i = 0; i < (int)(sizeof packs / sizeof packs[0]); i++)
		if (packs[i] && packs[i]->head)
		{
			Z_Free(packs[i]->head);
			packs[i]->head = NULL;
		}
}

size_t WPack_ReadLumpN(wpack_t *pk, FILE *handle, UINT32 lumpindex, const lumpinfo_t *l, void *dest, size_t size, size_t offset)
{
	if (pk && pk->head && lumpindex < pk->numlumps && offset <= pk->headbytes && size <= pk->headbytes - offset && size
		&& offset + size <= l->size)
	{
		// the whole request lies in the first bytes of the lump: the head table has them (zero padded past the end of a short lump, not asked for here)
		memcpy(dest, pk->head + (size_t)lumpindex * pk->headbytes + offset, size);
		return size;
	}
	return WPack_ReadLump(handle, l, dest, size, offset);
}

static UINT32 crctab[256];

static UINT32 Crc32(UINT32 crc, const UINT8 *p, size_t n)
{
	crc = ~crc;
	while (n--)
		crc = crctab[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
	return ~crc;
}

// -verifypack: every lump is decoded and its CRC32 compared with the table the cooker wrote. Returns the number of damaged lumps (0 = the pack is intact).
// Reads the whole pack (the 100 MB MUSIC.PAK takes a while on a slow medium): never on the normal start-up path.
UINT32 WPack_Verify(wpack_t *pk, FILE *handle, const lumpinfo_t *lumps, UINT32 numlumps, void (*report)(UINT32 lump, const char *what))
{
	UINT32 i, bad = 0, k;
	UINT8 *buf = NULL;
	size_t bufsize = 0;
	wpack_sum_t sum = { 0, 0 };
	UINT8 *table;

	if (!pk || !pk->crcoffset || numlumps != pk->numlumps)
		return 0; // a v1 pack carries no CRC table
	for (i = 0; i < 256; i++)
	{
		UINT32 c = i;

		for (k = 0; k < 8; k++)
			c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
		crctab[i] = c;
	}
	table = malloc((size_t)numlumps * 4);
	if (!table || !ReadBulk(StreamFor(handle), (long)pk->crcoffset, table, (size_t)numlumps * 4))
	{
		free(table);
		report(0xFFFFFFFFu, "cannot read the CRC table");
		return 1;
	}
	SumAdd(&sum, table, (size_t)numlumps * 4);
	if (SumFold(&sum) != pk->chkcrc)
	{
		free(table);
		report(0xFFFFFFFFu, "the CRC table is damaged");
		return 1;
	}
	for (i = 0; i < numlumps; i++)
	{
		const lumpinfo_t *l = &lumps[i];
		UINT32 want = (UINT32)table[i * 4] | ((UINT32)table[i * 4 + 1] << 8) | ((UINT32)table[i * 4 + 2] << 16) | ((UINT32)table[i * 4 + 3] << 24);
		UINT32 crc = 0;
		size_t done = 0;

		if (l->size > bufsize)
		{
			free(buf);
			bufsize = l->size;
			buf = malloc(bufsize);
			if (!buf)
			{
				free(table);
				report(i, "out of memory");
				return bad + 1;
			}
		}
		while (done < l->size)
		{
			size_t chunk = min((size_t)l->size - done, (size_t)(4 * WPACK_BLOCK));

			if (WPack_ReadLump(handle, l, buf + done, chunk, done) != chunk)
				break;
			done += chunk;
		}
		if (done == l->size)
			crc = Crc32(0, buf, done);
		if (done != l->size || crc != want)
		{
			bad++;
			report(i, done != l->size ? "cannot be read or decoded" : "CRC32 mismatch");
		}
	}
	free(buf);
	free(table);
	return bad;
}
