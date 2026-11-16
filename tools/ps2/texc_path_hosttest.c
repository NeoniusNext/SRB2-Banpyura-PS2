/* Actual TEXC opening code plus the production SRP2 reader. The existing
 * resource harness supplies guarded allocations and deterministic I/O faults. */
#define main ResourceRegressionMain
#include "resource_stream_opt17_hosttest.c"
#undef main

char srb2path[256], srb2home[256];
INT32 M_CheckParm(const char *name) { (void)name; return 0; }
boolean M_IsNextParm(void) { return false; }
const char *M_GetNextParm(void) { return NULL; }
void CONS_Printf(const char *fmt, ...) { (void)fmt; }

#define TC RTC
#include RESOURCE_TEXC_OPEN_SOURCE
#undef TC

int main(int argc, char **argv)
{
	const lumpinfo_t *l;
	UINT8 *expected, *actual;
	pkstream_t *s;
	wpack_iostat_t before, after;
	size_t opened;

	REQUIRE(argc == 5);
	strlcpy(srb2path, argv[1], sizeof srb2path);
	strlcpy(srb2home, argv[2], sizeof srb2home);
	REQUIRE(PS2TexC_Present());
	opened = live;
	s = StreamFind(RTC.handle);
	REQUIRE(s && !strcmp(s->pol->name, argv[3]));
	REQUIRE(!strcmp(s->path, argv[4]));
	REQUIRE(findblock(s->buf)->size == s->pol->cap);
	REQUIRE(RTC.budget == 1048576 && !RTC.arena);
	/* Cached Present calls must preserve the same handle and allocation set. */
	REQUIRE(PS2TexC_Present() && StreamFind(RTC.handle) == s && live == opened);

	l = RTC.lumps + RTC.keys[0].lump;
	expected = malloc(l->disksize);
	actual = malloc(l->disksize);
	REQUIRE(expected && actual && WPack_ReadRaw(RTC.handle, l, expected) == l->disksize);
	s->len = 0;
	WPack_GetStats(&before);
	/* The third attempt reopens the selected file. There is no TEXC.PAK in cwd. */
	WPack_InjectErrors(1, 2);
	REQUIRE(WPack_ReadRaw(RTC.handle, l, actual) == l->disksize);
	WPack_InjectErrors(0, 0);
	WPack_GetStats(&after);
	REQUIRE(!memcmp(actual, expected, l->disksize));
	REQUIRE(after.retries - before.retries == 2 && after.injected - before.injected == 2
		&& after.failures == before.failures);
	printf("TEXC_PATH policy=%s window=%u minread=%u retries=%u path=%s\n",
		s->pol->name, s->pol->cap, s->pol->minreq, after.retries - before.retries, s->path);
	free(expected);
	free(actual);
	Z_Free(RTC.keys);
	Z_Free(RTC.lumps);
	Z_Free(RTC.pool);
	tracked_free(RTC.iobuf);
	StreamDrop(RTC.handle);
	fclose(RTC.handle);
	WPack_Shutdown();
	REQUIRE(live == 0);
	printf("PASS checks=%u live=%zu\n", checks, live);
	return 0;
}
