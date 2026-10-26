/* Host test of src/ps2/ps2_safefile.c (OPT13-IO, RS-08): saves that survive a power-off, a full card and a medium that cannot rename.
 * The module's file operations are replaced by ones that fail, run out of room or "lose the power" (longjmp) at a chosen step, on real files in a scratch directory.
 * usage: safefile_hosttest DIR      (DIR: an empty scratch directory)
 * Invariant after ANY interruption + PS2Safe_Recover: the file holds the complete OLD content or the complete NEW content, never a mix and never nothing. */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../src/ps2/ps2_safefile.h"

static char dir[512], name[600], oldtext[200], newtext[5000];
static int failures;

/* fault injection: the step counter counts every call of an operation; step == die_at: the power goes (longjmp); fail_op: that operation always fails;
 * room_left: bytes fwrite can still write (a full card), -1 = unlimited */
static int step, die_at, fail_rename, fail_rename_from_name, fail_remove_name, room_left;
static jmp_buf power;

static void tick(void)
{
	if (++step == die_at)
		longjmp(power, 1);
}

static FILE *t_fopen(const char *n, const char *m) { tick(); return fopen(n, m); }
static size_t t_fwrite(const void *p, size_t s, size_t n, FILE *f)
{
	size_t total = s * n, put;

	tick();
	if (room_left >= 0 && (size_t)room_left < total)
	{
		put = fwrite(p, 1, (size_t)room_left, f);
		room_left = 0;
		return put / s;
	}
	if (room_left >= 0)
		room_left -= (int)total;
	return fwrite(p, s, n, f);
}
static int t_fflush(FILE *f) { tick(); return fflush(f); }
static int t_fclose(FILE *f) { tick(); return fclose(f); }
static int t_rename(const char *a, const char *b)
{
	tick();
	if (fail_rename)
		return -1;
	if (fail_rename_from_name && strstr(a, ".tmp") == NULL)
		return -1; /* the medium cannot rename the existing file away */
	return rename(a, b);
}
static int t_remove(const char *n)
{
	tick();
	if (fail_remove_name && !strstr(n, ".tmp") && !strstr(n, ".bak"))
		return -1;
	return remove(n);
}
static const ps2safe_ops_t ops = { t_fopen, t_fwrite, t_fflush, t_fclose, t_rename, t_remove };

static void put(const char *text, size_t len)
{
	FILE *f = fopen(name, "wb");

	fwrite(text, 1, len, f);
	fclose(f);
}

static int read_all(const char *n, char *buf, size_t cap)
{
	FILE *f = fopen(n, "rb");
	size_t got;

	if (!f)
		return -1;
	got = fread(buf, 1, cap, f);
	fclose(f);
	return (int)got;
}

static void cleanup(void)
{
	char n[700];

	remove(name);
	snprintf(n, sizeof n, "%s.tmp", name);
	remove(n);
	snprintf(n, sizeof n, "%s.bak", name);
	remove(n);
}

static void check(int cond, const char *what)
{
	if (!cond)
	{
		fprintf(stderr, "FAIL: %s\n", what);
		failures++;
	}
}

static void reset_faults(void)
{
	step = 0;
	die_at = 0;
	fail_rename = fail_rename_from_name = fail_remove_name = 0;
	room_left = -1;
	PS2Safe_SetOps(&ops);
}

/* 0 = the file is the old content, 1 = the new content, -1 = neither */
static int which(void)
{
	static char buf[8000];
	int got = read_all(name, buf, sizeof buf);

	if (got == (int)strlen(oldtext) && !memcmp(buf, oldtext, (size_t)got))
		return 0;
	if (got == (int)sizeof newtext && !memcmp(buf, newtext, (size_t)got))
		return 1;
	return -1;
}

int main(int argc, char **argv)
{
	int k, outcome;
	char tmpn[700], bakn[700];
	FILE *f;
	char tmpname[300];

	if (argc != 2)
		return 3;
	snprintf(dir, sizeof dir, "%s", argv[1]);
	snprintf(name, sizeof name, "%s/gamedata.dat", dir);
	snprintf(tmpn, sizeof tmpn, "%s.tmp", name);
	snprintf(bakn, sizeof bakn, "%s.bak", name);
	snprintf(oldtext, sizeof oldtext, "the old content of the file, complete and whole");
	for (k = 0; k < (int)sizeof newtext; k++)
		newtext[k] = (char)('A' + k % 26);

	/* 1. an ordinary save replaces the file; no leftovers */
	cleanup();
	put(oldtext, strlen(oldtext));
	reset_faults();
	check(PS2Safe_Write(name, newtext, sizeof newtext), "ordinary save returns true");
	check(which() == 1, "ordinary save: the new content is in the file");
	check(access(tmpn, F_OK) != 0 && access(bakn, F_OK) != 0, "ordinary save: no .tmp / .bak left");

	/* 2. no old file */
	cleanup();
	reset_faults();
	check(PS2Safe_Write(name, newtext, sizeof newtext) && which() == 1, "save without an old file");

	/* 3. a full card: the temporary file cannot be written; the old file is untouched, the save reports false */
	cleanup();
	put(oldtext, strlen(oldtext));
	reset_faults();
	room_left = 1000;
	check(!PS2Safe_Write(name, newtext, sizeof newtext), "full card: returns false");
	check(which() == 0, "full card: the old file is untouched");
	check(access(tmpn, F_OK) != 0, "full card: the temporary file is removed");

	/* 4. a medium that cannot rename at all: the content is written over the file (as before), true */
	cleanup();
	put(oldtext, strlen(oldtext));
	reset_faults();
	fail_rename = 1;
	check(PS2Safe_Write(name, newtext, sizeof newtext) && which() == 1, "no rename: written over the file");
	check(access(tmpn, F_OK) != 0, "no rename: no .tmp left");
	/* and the next save does not try again (the module learned it) */
	step = 0;
	check(PS2Safe_Write(name, newtext, 100) , "no rename: second save works");

	/* 5. rename of the existing file away fails, remove works: remove + rename */
	cleanup();
	put(oldtext, strlen(oldtext));
	reset_faults();
	fail_rename_from_name = 1;
	check(PS2Safe_Write(name, newtext, sizeof newtext) && which() == 1, "rename-away impossible: remove + rename");

	/* 6. nothing about the old file can be changed (no rename, no remove): written over */
	cleanup();
	put(oldtext, strlen(oldtext));
	reset_faults();
	fail_rename_from_name = 1;
	fail_remove_name = 1;
	check(PS2Safe_Write(name, newtext, sizeof newtext) && which() == 1, "rename-away and remove impossible: written over");

	/* 7. the power goes at every step of a save: after Recover the file is whole, old or new */
	for (k = 1; k < 40; k++)
	{
		cleanup();
		put(oldtext, strlen(oldtext));
		reset_faults();
		die_at = k;
		if (setjmp(power) == 0)
		{
			PS2Safe_Write(name, newtext, sizeof newtext);
			break; /* a save that needs fewer steps than k: every interruption point was visited */
		}
		reset_faults(); /* power is back, the file system as it was left */
		PS2Safe_Recover(name);
		outcome = which();
		if (outcome < 0)
		{
			char msg[100];

			snprintf(msg, sizeof msg, "power loss at step %d: the file is neither old nor new after Recover", k);
			check(0, msg);
		}
	}
	check(k > 5, "the power-loss loop visited the steps of a save");
	printf("power loss: %d interruption points visited\n", k - 1);

	/* 8. the same without an old file: the file is absent or new */
	for (k = 1; k < 40; k++)
	{
		cleanup();
		reset_faults();
		die_at = k;
		if (setjmp(power) == 0)
		{
			PS2Safe_Write(name, newtext, sizeof newtext);
			break;
		}
		reset_faults();
		PS2Safe_Recover(name);
		outcome = which();
		if (!(outcome == 1 || access(name, F_OK) != 0))
		{
			char msg[100];

			snprintf(msg, sizeof msg, "no old file, power loss at step %d: a partial file is left", k);
			check(0, msg);
		}
	}

	/* 9. a leftover .bak next to the file does not matter; Recover does not touch an existing file */
	cleanup();
	put(oldtext, strlen(oldtext));
	{
		FILE *g = fopen(bakn, "wb");

		fputs("stale", g);
		fclose(g);
	}
	reset_faults();
	PS2Safe_Recover(name);
	check(which() == 0, "Recover leaves an existing file alone");
	check(PS2Safe_Write(name, newtext, sizeof newtext) && which() == 1, "save over a stale .bak");

	/* 10. streamed (config) writing */
	cleanup();
	put(oldtext, strlen(oldtext));
	reset_faults();
	f = PS2Safe_Begin(name, tmpname, sizeof tmpname);
	check(f != NULL, "Begin opens the temporary file");
	if (f)
	{
		fputs("// SRB2 configuration file.\nfpscap \"35\"\n", f);
		check(PS2Safe_End(f, tmpname, name), "End commits");
		check(read_all(name, newtext, sizeof newtext - 1) > 20 && !strncmp(newtext, "// SRB2 configuration file.", 27), "streamed content is in the file");
	}
	/* streamed, no rename: End reads the temporary file back and writes it over */
	cleanup();
	put(oldtext, strlen(oldtext));
	reset_faults();
	fail_rename = 1;
	f = PS2Safe_Begin(name, tmpname, sizeof tmpname);
	if (f)
	{
		fputs("// SRB2 configuration file.\nfpscap \"60\"\n", f);
		check(PS2Safe_End(f, tmpname, name), "streamed, no rename: End writes over the file");
		memset(newtext, 0, sizeof newtext);
		check(read_all(name, newtext, sizeof newtext - 1) > 20 && strstr(newtext, "fpscap \"60\""), "streamed, no rename: the content is in the file");
	}
	cleanup();
	PS2Safe_SetOps(NULL);
	printf("safefile host test: %d failures\n", failures);
	return failures ? 1 : 0;
}
