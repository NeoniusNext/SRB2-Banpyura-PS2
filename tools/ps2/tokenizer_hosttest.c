/* PS2-LOAD-15: the PS2 Tokenizer_SRB2Read (src/m_tokenizer.c with PS2_PROFILE) against the original, on real TEXTMAPs and random UDMF-like text.
 *   cc -O2 -DTOKTEST_ORIG -DPS2_NOPROFILE ...   see tools/ps2/tokenizer_hosttest.py (builds both objects, links this file)
 * Each call returns a token (or NULL); the tokens, endPos, startPos, inComment and line after every call have to be equal, and so has the skip of a block.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct Tokenizer
{
	char *zdup;
	const char *input;
	unsigned numTokens;
	uint32_t *capacity;
	char **token;
	uint32_t startPos;
	uint32_t endPos;
	uint32_t inputLength;
	uint8_t inComment;
	uint8_t inString;
	int line;
	const char *(*get)(struct Tokenizer*, uint32_t);
} tokenizer_t;

tokenizer_t *Orig_Tokenizer_Open(const char *, size_t, unsigned);
void Orig_Tokenizer_Close(tokenizer_t *);
const char *Orig_Tokenizer_SRB2Read(tokenizer_t *, uint32_t);
tokenizer_t *Fast_Tokenizer_Open(const char *, size_t, unsigned);
void Fast_Tokenizer_Close(tokenizer_t *);
const char *Fast_Tokenizer_SRB2Read(tokenizer_t *, uint32_t);
int Fast_Tokenizer_SRB2SkipBlock(tokenizer_t *, uint32_t);

static unsigned long long seed = 0x9E3779B97F4A7C15ull;
static unsigned Rnd(void) { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return (unsigned)(seed >> 20); }

static int Compare(const char *name, const char *text, size_t len)
{
	tokenizer_t *a = Orig_Tokenizer_Open(text, len, 2), *b = Fast_Tokenizer_Open(text, len, 2);
	long calls = 0;

	a->line = b->line = 0; /* (never initialised by Tokenizer_Open) */

	for (;;)
	{
		unsigned slot = Rnd() & 1;
		const char *ta = Orig_Tokenizer_SRB2Read(a, slot), *tb = Fast_Tokenizer_SRB2Read(b, slot);

		calls++;
		if (a->endPos > a->inputLength) /* past the end of the text after an unterminated string: both read what lies behind the buffer */
			break;
		if ((ta == NULL) != (tb == NULL) || (ta && strcmp(ta, tb)) || a->endPos != b->endPos || a->startPos != b->startPos || a->inComment != b->inComment || a->line != b->line)
		{
			if (strcmp(name, "random") == 0) { printf("text:"); for (size_t q = 0; q < len; q++) printf(" %02x", (unsigned char)text[q]); printf("\n"); }
			printf("%s: call %ld differs: orig %s end=%u start=%u c=%u line=%d, fast %s end=%u start=%u c=%u line=%d\n", name, calls, ta ? ta : "(null)", a->endPos, a->startPos, a->inComment, a->line,
				tb ? tb : "(null)", b->endPos, b->startPos, b->inComment, b->line);
			return 1;
		}
		if (!ta)
			break;
		/* the block skip: from a "{" on, skip with the fast tokenizer and with the original's own loop; same position afterwards */
		if (ta[0] == '{' && !ta[1] && a->input[a->startPos] == '{' && a->endPos == a->startPos + 1 && (Rnd() & 3) == 0)
		{
			uint32_t cut = Rnd() % 3 == 0 ? Rnd() % 5 : 0, size = (uint32_t)(len > cut ? len - cut : 0);
			tokenizer_t *a2 = Orig_Tokenizer_Open(text, len, 2), *b2 = Fast_Tokenizer_Open(text, len, 2);
			const char *t;
			int okb, oka = 0;

			/* put both copies at the same place: read the same number of tokens */
			a2->line = b2->line = 0;
			for (long k = 0; k < calls; k++) { Orig_Tokenizer_SRB2Read(a2, 0); Fast_Tokenizer_SRB2Read(b2, 0); }
			while ((t = Orig_Tokenizer_SRB2Read(a2, 0)) && a2->endPos < size)
			{
				if (a2->endPos > a2->inputLength) break;
				if (t[0] == '}' && !t[1]) { oka = 1; break; }
			}
			okb = Fast_Tokenizer_SRB2SkipBlock(b2, size);
			if (oka != okb || (oka && (a2->endPos != b2->endPos || a2->inComment != b2->inComment)))
			{
				if (strcmp(name, "random") == 0) { printf("text(len %zu size %u):", len, size); for (size_t q = 0; q < len; q++) printf(" %02x", (unsigned char)text[q]); printf("\n"); }
				printf("%s: skip block after call %ld differs: orig %d end=%u, fast %d end=%u\n", name, calls, oka, a2->endPos, okb, b2->endPos);
				return 1;
			}
			if (oka && okb) /* the reads after the block continue the same way */
				for (int k = 0; k < 4; k++)
				{
					const char *ra = Orig_Tokenizer_SRB2Read(a2, 0), *rb = Fast_Tokenizer_SRB2Read(b2, 0);

					if (a2->endPos > a2->inputLength)
						break;
					if ((ra == NULL) != (rb == NULL) || (ra && strcmp(ra, rb)) || a2->endPos != b2->endPos || a2->inComment != b2->inComment)
					{
						printf("%s: read after the skipped block differs at call %ld/%d\n", name, calls, k);
						return 1;
					}
				}
			Orig_Tokenizer_Close(a2);
			Fast_Tokenizer_Close(b2);
		}
	}
	Orig_Tokenizer_Close(a);
	Fast_Tokenizer_Close(b);
	return 0;
}

int main(int argc, char **argv)
{
	static const char alpha[] = "abcxyz019.-+_ \t\r\n\n{}={};;,,\"\"//**/ /*\\ "; /* (no NUL inside: the first NUL ends the text for both, and what follows is read past the end) */
	long n, fail = 0;
	int i;

	for (i = 1; i < argc; i++)
	{
		FILE *f = fopen(argv[i], "rb");
		char *buf;
		long sz;

		if (!f) { printf("%s: cannot open\n", argv[i]); fail++; continue; }
		fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
		buf = malloc(sz + 1);
		fread(buf, 1, sz, f); fclose(f);
		fail += Compare(argv[i], buf, sz);
		printf("%s: %ld bytes compared\n", argv[i], sz);
		free(buf);
	}
	for (n = 0; n < 400000 && !fail; n++)
	{
		size_t len = 1 + Rnd() % 60, k;
		char text[80];

		for (k = 0; k < len; k++)
			text[k] = alpha[Rnd() % (sizeof alpha - 1)];
		fail += Compare("random", text, len);
	}
	/* bigger random texts built from UDMF words */
	for (n = 0; n < 2000 && !fail; n++)
	{
		static const char *w[] = { "thing", "linedef", "sidedef", "vertex", "sector", "namespace", "x", "y", "{", "}", "=", ";", "\"srb2\"", "\"a b\"", "// c\n", "/* d */", "\n", " ", "texturefloor", "123.5", "-7", "}\n", "{\n", "\"", "/", "*", ",", "a/b", "//", "/*" };
		static char text[40000];
		size_t len = 0, count = Rnd() % 3000;

		while (count--)
		{
			const char *p = w[Rnd() % (sizeof w / sizeof *w)];
			size_t l = strlen(p);
			if (len + l + 1 >= sizeof text) break;
			memcpy(text + len, p, l); len += l;
			if (Rnd() & 1) text[len++] = ' ';
		}
		fail += Compare("udmf-words", text, len);
	}
	puts(fail ? "FAILED" : "ALL EQUAL");
	return fail != 0;
}
