// OPT13-IO (RS-08): see ps2_safefile.h.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps2_safefile.h"

static const ps2safe_ops_t libc_ops = { fopen, fwrite, fflush, fclose, rename, remove };
static const ps2safe_ops_t *ops = &libc_ops;
static boolean rename_failed_for_good; // the medium cannot rename: do not try again (the temporary file would only double the writes)

void PS2Safe_SetOps(const ps2safe_ops_t *o)
{
	ops = o ? o : &libc_ops;
	rename_failed_for_good = false;
}

static boolean Name(char *out, size_t size, const char *name, const char *suffix)
{
	size_t n = strlen(name), m = strlen(suffix);

	if (n + m + 1 > size)
		return false;
	memcpy(out, name, n);
	memcpy(out + n, suffix, m + 1);
	return true;
}

static boolean Exists(const char *name)
{
	FILE *f = ops->fopen(name, "rb");

	if (!f)
		return false;
	ops->fclose(f);
	return true;
}

// The old way: the content straight into the file, with every step checked
static boolean DirectWrite(const char *name, const void *source, size_t length)
{
	FILE *f = ops->fopen(name, "wb");
	boolean ok;

	if (!f)
		return false;
	ok = ops->fwrite(source, 1, length, f) == length;
	ok = ops->fflush(f) == 0 && ok;
	return ops->fclose(f) == 0 && ok;
}

// tmp holds the whole new content: it becomes name. False: the rename is not possible here (name is as it was)
static boolean Commit(const char *tmp, const char *name)
{
	char bak[256];
	boolean hadbak = false;

	if (rename_failed_for_good || !Name(bak, sizeof bak, name, ".bak"))
		return false;
	if (Exists(name))
	{
		ops->remove(bak); // the leftover of an earlier commit
		if (ops->rename(name, bak) == 0)
			hadbak = true;
		else if (ops->remove(name) != 0) // a medium that cannot rename a file away: the old one goes, the temporary one takes its place at once
		{
			rename_failed_for_good = true;
			return false;
		}
	}
	if (ops->rename(tmp, name) != 0)
	{
		if (hadbak)
			ops->rename(bak, name); // put the old file back
		else
			rename_failed_for_good = true;
		return false;
	}
	if (hadbak)
		ops->remove(bak);
	return true;
}

boolean PS2Safe_Write(const char *name, const void *source, size_t length)
{
	char tmp[256];
	FILE *f;
	boolean ok;

	if (rename_failed_for_good || !Name(tmp, sizeof tmp, name, ".tmp") || !(f = ops->fopen(tmp, "wb")))
		return DirectWrite(name, source, length);
	ok = ops->fwrite(source, 1, length, f) == length;
	ok = ops->fflush(f) == 0 && ok;
	ok = ops->fclose(f) == 0 && ok;
	if (!ok) // no room, or the medium went away: the old file was not touched
	{
		ops->remove(tmp);
		return false;
	}
	if (Commit(tmp, name))
		return true;
	ops->remove(tmp);
	return DirectWrite(name, source, length);
}

FILE *PS2Safe_Begin(const char *name, char *tmpname, size_t tmpsize)
{
	if (rename_failed_for_good || !Name(tmpname, tmpsize, name, ".tmp"))
		return NULL;
	return ops->fopen(tmpname, "w");
}

boolean PS2Safe_End(FILE *f, const char *tmpname, const char *name)
{
	boolean ok = !ferror(f);
	long size;
	void *copy = NULL;

	ok = ops->fflush(f) == 0 && ok;
	size = ok ? ftell(f) : -1;
	ok = ops->fclose(f) == 0 && ok;
	if (!ok)
	{
		ops->remove(tmpname);
		return false;
	}
	if (Commit(tmpname, name))
		return true;
	// no rename on this medium: the complete temporary file is read back and written over the file
	if (size >= 0 && (copy = malloc(size ? (size_t)size : 1)) != NULL)
	{
		FILE *r = ops->fopen(tmpname, "rb");
		boolean good = false;

		if (r)
		{
			good = fread(copy, 1, (size_t)size, r) == (size_t)size;
			ops->fclose(r);
		}
		ops->remove(tmpname);
		ok = good && DirectWrite(name, copy, (size_t)size);
		free(copy);
		return ok;
	}
	ops->remove(tmpname);
	return false;
}

void PS2Safe_Recover(const char *name)
{
	char bak[256];

	if (!Name(bak, sizeof bak, name, ".bak") || Exists(name) || !Exists(bak))
		return;
	ops->rename(bak, name);
}
