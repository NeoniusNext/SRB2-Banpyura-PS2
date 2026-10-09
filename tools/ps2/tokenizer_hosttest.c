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
int Fast_Tokenizer_SRB2ReadPair(tokenizer_t *, const char **, const char **);

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
			/* the pair reader of TextmapParse against the loop it replaces, from the same place, to the end of the block */
			if (oka == 0 || 1)
			{
				tokenizer_t *a3 = Orig_Tokenizer_Open(text, len, 2), *b3 = Fast_Tokenizer_Open(text, len, 2);
				a3->line = b3->line = 0;
				for (long k = 0; k < calls; k++) { Orig_Tokenizer_SRB2Read(a3, 0); Fast_Tokenizer_SRB2Read(b3, 0); }
				for (int guard = 0; guard < 400; guard++)
				{
					const char *pa, *va = NULL, *pb, *vb = NULL;
					int ra, rb;

					if (a3->endPos > a3->inputLength)
						break;
					pa = Orig_Tokenizer_SRB2Read(a3, 0);
					if (!pa || !strcmp(pa, "}")) ra = 0; else { va = Orig_Tokenizer_SRB2Read(a3, 1); ra = 1; }
					rb = Fast_Tokenizer_SRB2ReadPair(b3, &pb, &vb);
					if (ra != rb || (ra && (strcmp(pa, pb) || (va == NULL) != (vb == NULL) || (va && strcmp(va, vb)))) || a3->endPos != b3->endPos || a3->inComment != b3->inComment)
					{
						printf("%s: pair after call %ld differs: orig %d '%s' '%s' end=%u c=%u, fast %d '%s' '%s' end=%u c=%u\n", name, calls, ra, pa ? pa : "(null)", va ? va : "(null)", a3->endPos, a3->inComment, rb, pb ? pb : "(null)", vb ? vb : "(null)", b3->endPos, b3->inComment);
						return 1;
					}
					if (!ra)
						break;
				}
				Orig_Tokenizer_Close(a3);
				Fast_Tokenizer_Close(b3);
			}
			Orig_Tokenizer_Close(a2);
			Fast_Tokenizer_Close(b2);
		}
	}
	Orig_Tokenizer_Close(a);
	Fast_Tokenizer_Close(b);
	return 0;
}


/* the whole text as TextmapParse reads it: keywords and "{" with the plain reader, the blocks pair by pair (original: two reads per pair; fast: ReadPair, in place) */
static int ComparePairs(const char *name, const char *text, size_t len)
{
	tokenizer_t *a = Orig_Tokenizer_Open(text, len, 2), *b = Fast_Tokenizer_Open(text, len, 2);
	long pairs = 0;

	a->line = b->line = 0;
	for (;;)
	{
		const char *ta = Orig_Tokenizer_SRB2Read(a, 0), *tb = Fast_Tokenizer_SRB2Read(b, 0);

		if (a->endPos > a->inputLength)
			break;
		if ((ta == NULL) != (tb == NULL) || (ta && strcmp(ta, tb)) || a->endPos != b->endPos || a->inComment != b->inComment)
		{
			printf("%s: keyword read differs after %ld pairs: '%s' end=%u / '%s' end=%u\n", name, pairs, ta ? ta : "(null)", a->endPos, tb ? tb : "(null)", b->endPos);
			return 1;
		}
		if (!ta)
			break;
		if (!strcmp(ta, "{") && a->input[a->startPos] == '{' && a->endPos == a->startPos + 1)
			for (;;)
			{
				const char *pa, *va = NULL, *pb, *vb = NULL;
				int ra, rb;

				if (a->endPos > a->inputLength)
					goto done;
				pa = Orig_Tokenizer_SRB2Read(a, 0);
				if (!pa || !strcmp(pa, "}")) ra = 0; else { va = Orig_Tokenizer_SRB2Read(a, 1); ra = 1; }
				rb = Fast_Tokenizer_SRB2ReadPair(b, &pb, &vb);
				pairs++;
				if (a->endPos > a->inputLength)
					goto done;
				if (ra != rb || (ra && (strcmp(pa, pb) || (va == NULL) != (vb == NULL) || (va && strcmp(va, vb)))) || a->endPos != b->endPos || a->inComment != b->inComment)
				{
					printf("%s: pair %ld differs: orig %d '%s' '%s' end=%u c=%u, fast %d '%s' '%s' end=%u c=%u\n", name, pairs, ra, pa ? pa : "(null)", va ? va : "(null)", a->endPos, a->inComment, rb, pb ? pb : "(null)", vb ? vb : "(null)", b->endPos, b->inComment);
					return 1;
				}
				if (!ra)
					break;
			}
	}
done:
	Orig_Tokenizer_Close(a);
	Fast_Tokenizer_Close(b);
	return 0;
}

/* PS2-LOAD-21: Tokenizer_Open's copy and length (eight bytes at a time) against the original (memcpy + strlen), for every length 0..130, a NUL at every
 * position or none, and sources that start at every offset 0..7 */
static int CompareOpen(void)
{
	static char src[512];
	long checked = 0;
	size_t len, off;
	int nulpos;

	for (len = 0; len <= 130; len++)
		for (off = 0; off < 8; off++)
			for (nulpos = -1; nulpos < (int)len; nulpos++)
			{
				tokenizer_t *a, *b;
				size_t i;

				for (i = 0; i < sizeof src; i++)
					src[i] = (char)('a' + (i * 7 + len) % 26);
				if (nulpos >= 0)
					src[off + nulpos] = 0;
				if (len && (len + off) % 3 == 0)
					src[off + len - 1] = (char)0x80; /* (high bytes: the zero test must not flag them) */
				a = Orig_Tokenizer_Open(src + off, len, 2);
				b = Fast_Tokenizer_Open(src + off, len, 2);
				checked++;
				if (a->inputLength != b->inputLength || memcmp(a->zdup, b->zdup, len + 2))
				{
					printf("open: len %zu off %zu nul %d: length orig %u fast %u (or the copy differs)\n", len, off, nulpos, a->inputLength, b->inputLength);
					return 1;
				}
				Orig_Tokenizer_Close(a);
				Fast_Tokenizer_Close(b);
			}
	printf("open: %ld copies and lengths equal\n", checked);
	return 0;
}

int main(int argc, char **argv)
{
	static const char alpha[] = "abcxyz019.-+_ \t\r\n\n{}={};;,,\"\"//**/ /*\\ "; /* (no NUL inside: the first NUL ends the text for both, and what follows is read past the end) */
	long n, fail = 0;
	int i;

	fail += CompareOpen();

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
		fail += ComparePairs(argv[i], buf, sz);
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
		fail += ComparePairs("random", text, len);
	}
	/* no slash at all, many quotes and braces: the eight-byte block skip */
	for (n = 0; n < 600000 && !fail; n++)
	{
		static const char alpha2[] = "ab1 \n\t{}\"\";=,";
		size_t len = 1 + Rnd() % 70, k;
		char text[80];

		for (k = 0; k < len; k++)
			text[k] = alpha2[Rnd() % (sizeof alpha2 - 1)];
		fail += Compare("noslash", text, len);
		fail += ComparePairs("noslash", text, len);
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
		fail += ComparePairs("udmf-words", text, len);
	}
	puts(fail ? "FAILED" : "ALL EQUAL");
	return fail != 0;
}
