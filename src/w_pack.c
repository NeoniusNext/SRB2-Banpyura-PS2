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

#ifdef PS2
#include <malloc.h>
#define WPACK_ALIGNED_ALLOC(size) memalign(64, (size))
#else
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

// All file reads bounce through aligned, whole-sector requests. Caller buffers may be unaligned.
static UINT8 *iobuf, *cbuf, *sbuf;

static boolean ReadBytes(FILE *handle, void *dest, size_t size)
{
	UINT8 *out = dest;
	long pos = ftell(handle);
	size_t done = 0;

	if (pos < 0 || size > (size_t)(LONG_MAX - pos))
		return false;
	if (!iobuf)
		iobuf = WPACK_ALIGNED_ALLOC(WPACK_BLOCK);
	if (!iobuf)
		return false;
	while (done < size)
	{
		size_t skip = (size_t)pos % WPACK_SECTOR;
		size_t want = min(size - done, WPACK_BLOCK - skip);
		size_t request = (skip + want + WPACK_SECTOR - 1) & ~(size_t)(WPACK_SECTOR - 1);
		size_t got;

		LP_BEGIN(lpr);

		if (fseek(handle, pos - (long)skip, SEEK_SET) != 0)
			return false;
		got = fread(iobuf, 1, request, handle);
		LP_END(PK_FREAD, lpr);
		if (got < skip + want) // never copy bytes not actually read (signature probes may see short files)
			return false;
		memcpy(out + done, iobuf + skip, want);
		done += want;
		pos += (long)want;
	}
	return fseek(handle, pos, SEEK_SET) == 0;
}

boolean WPack_Detect(FILE *handle)
{
	char magic[4];

	if (fseek(handle, 0, SEEK_SET) != 0 || !ReadBytes(handle, magic, sizeof magic))
		return false;
	if (memcmp(magic, "SRP2", 4) == 0)
		return true;
	// PS2-103: a file that is not a pack goes to the original loaders (ResGetLumpsWad reads the header from the current position)
	fseek(handle, 0, SEEK_SET);
	return false;
}

void *WPack_SetupHandle(FILE *handle)
{
	void *buf = WPACK_ALIGNED_ALLOC(65536);

	if (buf && setvbuf(handle, buf, _IOFBF, 65536) != 0)
	{
		free(buf);
		buf = NULL;
	}
	return buf;
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
	long end;
	UINT8 first[SRP2_HEADER_SIZE + sizeof (srp2_ext_t)];

	if (fseek(handle, 0, SEEK_SET) != 0 || !ReadBytes(handle, first, SRP2_HEADER_SIZE))
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
	if (fseek(handle, 0, SEEK_END) != 0)
		return false;
	end = ftell(handle);
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
		if (fseek(handle, SRP2_HEADER_SIZE, SEEK_SET) != 0 || !ReadBytes(handle, ext, sizeof *ext))
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

	if (!ReadHeader(handle, shortname, &h, &ext))
		return NULL;
	(void)v2;

	n = h.numlumps;
	prevend = h.dataoffset;

	// one block for every name of the pack (instead of two mallocs per lump)
	pool = Z_Malloc(h.poolsize, PU_STATIC, NULL);
	if (fseek(handle, h.pooloffset, SEEK_SET) != 0 || !ReadBytes(handle, pool, h.poolsize) || pool[h.poolsize - 1] != '\0')
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
	if (fseek(handle, h.tableoffset, SEEK_SET) != 0)
	{
		Z_Free(lumpinfo);
		Z_Free(pool);
		return NULL;
	}

	for (done = 0; done < n; done += ENTRY_CHUNK)
	{
		UINT32 count = min(n - done, ENTRY_CHUNK);

		if (!ReadBytes(handle, chunk, sizeof (srp2_entry_t) * count))
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
				if (fseek(handle, ext.headoffset, SEEK_SET) != 0 || !ReadBytes(handle, pk->head, bytes))
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

	*nlmp = (UINT16)n;
	*poolp = pool;
	*nonmusic = (h.flags & SRP2_FLAG_NONMUSIC) != 0;
	return lumpinfo;
}

static boolean AllocBuffers(void)
{
	if (!cbuf)
		cbuf = WPACK_ALIGNED_ALLOC(WPACK_BLOCK);
	if (!sbuf)
		sbuf = WPACK_ALIGNED_ALLOC(WPACK_BLOCK);
	return cbuf && sbuf;
}

void WPack_Shutdown(void)
{
	free(iobuf);
	free(cbuf);
	free(sbuf);
	iobuf = cbuf = sbuf = NULL;
}

// Delivers decoded bytes [lo, hi) of one block (decoded size bsize, stored in csize bytes at the current file position)
// to dst (which receives byte lo). Returns false on a read/decode error.
static boolean ReadBlock(FILE *handle, boolean raw, UINT32 csize, UINT32 bsize, UINT32 lo, UINT32 hi, UINT8 *dst)
{
	UINT32 want;

	if (csize == 0 || csize > WPACK_BLOCK || bsize > WPACK_BLOCK || lo > hi || hi > bsize)
		return false;
	want = hi - lo;

	if (raw)
	{
		if (csize != bsize)
			return false;
		if (!ReadBytes(handle, cbuf, csize))
			return false;
		memcpy(dst, cbuf + lo, want);
		return true;
	}

	if (!ReadBytes(handle, cbuf, csize))
		return false;
	{
		boolean ok;
		LP_BEGIN(lpz);

		if (lo == 0 && want == bsize) // whole block straight into the destination
			ok = LZ4_decompress_safe((const char *)cbuf, (char *)dst, (int)csize, (int)bsize) == (int)bsize;
		else if (lo == 0) // head of the block (patch headers etc): stop as soon as enough is decoded
			ok = LZ4_decompress_safe_partial((const char *)cbuf, (char *)dst, (int)csize, (int)want, (int)want) == (int)want;
		else if (LZ4_decompress_safe((const char *)cbuf, (char *)sbuf, (int)csize, (int)bsize) != (int)bsize)
			ok = false;
		else
		{
			memcpy(dst, sbuf + lo, want);
			ok = true;
		}
		LP_END(PK_LZ4, lpz);
		return ok;
	}
}

size_t WPack_ReadLump(FILE *handle, const lumpinfo_t *l, void *dest, size_t size, size_t offset)
{
	UINT8 *out = dest;
	UINT32 usize = (UINT32)l->size;
	UINT32 index[64];
	UINT32 *idx = index;
	UINT32 nb, first, last, b, remaining;
	long pos;
	size_t done = 0;

	if (l->size > UINT32_MAX || l->position > LONG_MAX || l->disksize > (unsigned long)LONG_MAX - l->position
		|| !dest || !size || offset >= usize || size > usize - offset)
		return 0;
	if (l->compression == CM_NOCOMPRESSION)
	{
		if (l->disksize != usize || fseek(handle, (long)(l->position + offset), SEEK_SET) != 0)
			return 0;
		return ReadBytes(handle, dest, size) ? size : 0;
	}
	if (l->compression != CM_LZ4 || !AllocBuffers())
		return 0;

	if (usize <= WPACK_BLOCK) // one LZ4 block, no index
	{
		if (fseek(handle, (long)l->position, SEEK_SET) != 0)
			return 0;
		return ReadBlock(handle, false, (UINT32)l->disksize, usize, (UINT32)offset, (UINT32)(offset + size), out) ? size : 0;
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

	if (fseek(handle, (long)l->position, SEEK_SET) != 0 || !ReadBytes(handle, idx, nb * sizeof *idx))
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
	}
	if (remaining != 0)
		goto end;

	if (fseek(handle, pos, SEEK_SET) != 0) // blocks first..last are contiguous: no seeks in between
		goto end;

	for (b = first; b <= last; b++)
	{
		UINT32 entry = LONG(idx[b]);
		UINT32 bstart = b * WPACK_BLOCK;
		UINT32 bsize = min(WPACK_BLOCK, usize - bstart);
		UINT32 lo = (UINT32)max(offset, bstart) - bstart;
		UINT32 hi = (UINT32)min(offset + size, (size_t)bstart + bsize) - bstart;

		if (!ReadBlock(handle, (entry & SRP2_RAWBLOCK) != 0, entry & ~SRP2_RAWBLOCK, bsize, lo, hi, out + done))
			break;
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
	if (!table || fseek(handle, pk->crcoffset, SEEK_SET) != 0 || !ReadBytes(handle, table, (size_t)numlumps * 4))
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
