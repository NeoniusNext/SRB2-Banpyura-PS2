/* PS2-LOAD-24: see gettoken_hosttest.py. Drives orig_M_GetToken / fast_M_GetToken(Pooled) over texts and compares what they return. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

char *orig_M_GetToken(const char *);
void orig_M_UnGetToken(void);
char *fast_M_GetToken(const char *);
char *fast_M_GetTokenPooled(const char *);
void fast_M_UnGetToken(void);
void fast_M_FreeToken(char *);

static uint64_t seed = 0x9E3779B97F4A7C15ull;
static unsigned Rnd(void) { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return (unsigned)(seed >> 20); }

static long fails, tokens;

/* one text: the same calls on both; `unget` positions are chosen from the random stream */
static int Run(const char *name, const char *text, int use_unget, int pooled, long maxcalls)
{
	const char *first = text;
	long calls = 0;

	for (;;)
	{
		char *a = orig_M_GetToken(first), *b = pooled ? fast_M_GetTokenPooled(first) : fast_M_GetToken(first);

		first = NULL;
		calls++;
		tokens++;
		if ((a == NULL) != (b == NULL) || (a && strcmp(a, b)))
		{
			printf("%s: call %ld: orig '%s' fast '%s'\n", name, calls, a ? a : "(null)", b ? b : "(null)");
			return 1;
		}
		if (!a)
			break;
		free(a);
		if (pooled)
			fast_M_FreeToken(b);
		else
			free(b);
		if (use_unget && (Rnd() & 7) == 0)
		{
			orig_M_UnGetToken();
			fast_M_UnGetToken();
		}
		if (calls > maxcalls)
			break; /* (after an unterminated string both read behind the text: zeros, endless empty tokens) */
	}
	return 0;
}

int main(int argc, char **argv)
{
	static const char alpha[] = "abcxyz019.-+_ \t\r\n\n{}={};;,,\"\"//**/ /*\\";
	int i;
	long n;

	for (i = 1; i < argc; i++)
	{
		FILE *f = fopen(argv[i], "rb");
		long sz;
		char *buf;
		int mode;

		if (!f) { printf("%s: cannot open\n", argv[i]); fails++; continue; }
		fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
		buf = malloc(sz + 1);
		if (fread(buf, 1, sz, f) != (size_t)sz) { fails++; continue; }
		fclose(f);
		buf[sz] = 0;
		for (mode = 0; mode < 4; mode++)
			fails += Run(argv[i], buf, mode & 1, mode >> 1, sz * 2 + 100);
		free(buf);
	}
	/* random texts: short, from an alphabet full of delimiters, comment starts and quotes; the state (inside a comment) carries over from text to text, as it does in the game */
	for (n = 0; n < 600000; n++)
	{
		static char text[1024]; /* zero filled behind the text */
		int len = (int)(Rnd() % 40), k;

		memset(text, 0, sizeof text);

		for (k = 0; k < len; k++)
			text[k] = alpha[Rnd() % (sizeof alpha - 1)];
		text[len] = 0;
		fails += Run("random", text, (int)(Rnd() & 1), (int)(Rnd() & 1), 120);
		if (fails > 10)
			break;
	}
	/* embedded high bytes and long words (past the pool size of the pooled tokens) */
	for (n = 0; n < 2000; n++)
	{
		static char text[1024];
		int len = (int)(Rnd() % 300), k;

		memset(text, 0, sizeof text);

		for (k = 0; k < len; k++)
			text[k] = (Rnd() % 9 == 0) ? ' ' : (char)(1 + Rnd() % 254);
		text[len] = 0;
		fails += Run("bytes", text, 1, 1, 700);
	}
	printf("%ld tokens compared, %ld failures\n", tokens, fails);
	printf(fails ? "FAILED\n" : "ALL EQUAL\n");
	return fails != 0;
}
