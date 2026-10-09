// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 2013-2024 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  m_tokenizer.c
/// \brief Tokenizer

#include "m_tokenizer.h"
#include "z_zone.h"

#ifdef PS2_PROFILE
// PS2-LOAD-21: the copy of the text and the strlen of the copy in one pass, eight bytes at a time (the copy was a byte loop of the libc memcpy
// for an unaligned source and the strlen a memchr that cost three times as much as the copy: 5 M cycles for a 1 MB TEXTMAP). Returns the
// index of the first NUL of the text, or len where there is none: what strlen of the copy gave.
static UINT32 Tokenizer_CopyLen(char *dst, const char *src, size_t len)
{
	size_t i = 0;

	while (i + 8 <= len)
	{
		UINT64 w;

		memcpy(&w, src + i, 8);
		if (((w - 0x0101010101010101ull) & ~w & 0x8080808080808080ull) != 0) // a zero byte in the word
			break;
		memcpy(dst + i, &w, 8);
		i += 8;
	}
	if (i < len)
		memcpy(dst + i, src + i, len - i);
	for (; i < len; i++)
		if (!dst[i])
			return (UINT32)i;
	return (UINT32)len;
}
#endif

tokenizer_t *Tokenizer_Open(const char *inputString, size_t len, unsigned numTokens)
{
	tokenizer_t *tokenizer = Z_Malloc(sizeof(tokenizer_t), PU_STATIC, NULL);
	const size_t lenpan = 2;

#ifdef PS2_PROFILE
	tokenizer->zdup = Z_Malloc(len+lenpan, PU_STATIC, NULL); // PS2-102: the copy of a big TEXTMAP must not come out of the small C heap reserve
#else
	tokenizer->zdup = malloc(len+lenpan);
#endif
	for (size_t i = 0; i < lenpan; i++)
	{
		tokenizer->zdup[len+i] = 0x00;
	}

#ifdef PS2_PROFILE
	tokenizer->input = tokenizer->zdup;
	tokenizer->inputLength = Tokenizer_CopyLen(tokenizer->zdup, inputString, len);
#else
	tokenizer->input = M_Memcpy(tokenizer->zdup, inputString, len);
	tokenizer->inputLength = 0;
#endif
	tokenizer->startPos = 0;
	tokenizer->endPos = 0;
	tokenizer->inComment = 0;
	tokenizer->inString = 0;
	tokenizer->get = Tokenizer_Read;

	if (numTokens < 1)
		numTokens = 1;

	tokenizer->numTokens = numTokens;
	tokenizer->capacity = Z_Malloc(sizeof(UINT32) * numTokens, PU_STATIC, NULL);
	tokenizer->token = Z_Malloc(sizeof(char*) * numTokens, PU_STATIC, NULL);

	for (size_t i = 0; i < numTokens; i++)
	{
		tokenizer->capacity[i] = 1024;
		tokenizer->token[i] = (char*)Z_Malloc(tokenizer->capacity[i] * sizeof(char), PU_STATIC, NULL);
	}

#ifndef PS2_PROFILE
	tokenizer->inputLength = strlen(tokenizer->input);
#endif

	return tokenizer;
}

void Tokenizer_Close(tokenizer_t *tokenizer)
{
	if (!tokenizer)
		return;

	for (size_t i = 0; i < tokenizer->numTokens; i++)
		Z_Free(tokenizer->token[i]);
	Z_Free(tokenizer->capacity);
	Z_Free(tokenizer->token);
#ifdef PS2_PROFILE
	Z_Free(tokenizer->zdup);
#else
	free(tokenizer->zdup);
#endif
	Z_Free(tokenizer);
}

static boolean DetectLineBreak(tokenizer_t *tokenizer, size_t pos)
{
	if (tokenizer->input[pos] == '\n')
	{
		tokenizer->line++;
		return true;
	}

	return false;
}

static void DetectComment(tokenizer_t *tokenizer, UINT32 *pos)
{
	if (tokenizer->inComment)
		return;

	if (*pos >= tokenizer->inputLength - 1)
		return;

	if (tokenizer->input[*pos] != '/')
		return;

	// Single-line comment start
	if (tokenizer->input[*pos + 1] == '/')
		tokenizer->inComment = 1;
	// Multi-line comment start
	else if (tokenizer->input[*pos + 1] == '*')
		tokenizer->inComment = 2;
}

static void Tokenizer_ReadTokenString(tokenizer_t *tokenizer, UINT32 i)
{
	UINT32 tokenLength = tokenizer->endPos - tokenizer->startPos;
	if (tokenLength + 1 > tokenizer->capacity[i])
	{
		tokenizer->capacity[i] = tokenLength + 1;
		// Assign the memory. Don't forget an extra byte for the end of the string!
#ifdef PS2_PROFILE
		Z_Free(tokenizer->token[i]); // (the original leaves the old buffer in the zone)
#endif
		tokenizer->token[i] = (char *)Z_Malloc(tokenizer->capacity[i] * sizeof(char), PU_STATIC, NULL);
	}
	// Copy the string.
	M_Memcpy(tokenizer->token[i], tokenizer->input + tokenizer->startPos, (size_t)tokenLength);
	// Make the final character NUL.
	tokenizer->token[i][tokenLength] = '\0';
}

const char *Tokenizer_Read(tokenizer_t *tokenizer, UINT32 i)
{
	if (!tokenizer->input)
		return NULL;

	tokenizer->startPos = tokenizer->endPos;

	// If in a string, return the entire string within quotes, except without the quotes.
	if (tokenizer->inString == 1)
	{
		while (tokenizer->input[tokenizer->endPos] != '"' && tokenizer->endPos < tokenizer->inputLength)
		{
			DetectLineBreak(tokenizer, tokenizer->endPos);
			tokenizer->endPos++;
		}

		Tokenizer_ReadTokenString(tokenizer, i);
		tokenizer->inString = 2;
		return tokenizer->token[i];
	}
	// If just ended a string, return only a quotation mark.
	else if (tokenizer->inString == 2)
	{
		tokenizer->endPos = tokenizer->startPos + 1;
		tokenizer->token[i][0] = tokenizer->input[tokenizer->startPos];
		tokenizer->token[i][1] = '\0';
		tokenizer->inString = 0;
		return tokenizer->token[i];
	}

	// Try to detect comments now, in case we're pointing right at one
	DetectComment(tokenizer, &tokenizer->startPos);

	// Find the first non-whitespace char, or else the end of the string trying
	while ((tokenizer->input[tokenizer->startPos] == ' '
			|| tokenizer->input[tokenizer->startPos] == '\t'
			|| tokenizer->input[tokenizer->startPos] == '\r'
			|| tokenizer->input[tokenizer->startPos] == '\n'
			|| tokenizer->input[tokenizer->startPos] == '\0'
			|| tokenizer->inComment != 0)
			&& tokenizer->startPos < tokenizer->inputLength)
	{
		boolean inLineBreak = DetectLineBreak(tokenizer, tokenizer->startPos);

		// Try to detect comment endings now
		if (tokenizer->inComment == 1 && inLineBreak)
			tokenizer->inComment = 0; // End of line for a single-line comment
		else if (tokenizer->inComment == 2
			&& tokenizer->startPos < tokenizer->inputLength - 1
			&& tokenizer->input[tokenizer->startPos] == '*'
			&& tokenizer->input[tokenizer->startPos+1] == '/')
		{
			// End of multi-line comment
			tokenizer->inComment = 0;
			tokenizer->startPos++; // Make damn well sure we're out of the comment ending at the end of it all
		}

		tokenizer->startPos++;
		DetectComment(tokenizer, &tokenizer->startPos);
	}

	// If the end of the string is reached, no token is to be read
	if (tokenizer->startPos == tokenizer->inputLength)
	{
		tokenizer->endPos = tokenizer->inputLength;
		return NULL;
	}
	// Else, if it's one of these three symbols, capture only this one character
	else if (tokenizer->input[tokenizer->startPos] == ','
			|| tokenizer->input[tokenizer->startPos] == '{'
			|| tokenizer->input[tokenizer->startPos] == '}'
			|| tokenizer->input[tokenizer->startPos] == '['
			|| tokenizer->input[tokenizer->startPos] == ']'
			|| tokenizer->input[tokenizer->startPos] == '='
			|| tokenizer->input[tokenizer->startPos] == ':'
			|| tokenizer->input[tokenizer->startPos] == '%'
			|| tokenizer->input[tokenizer->startPos] == '@'
			|| tokenizer->input[tokenizer->startPos] == '"')
	{
		tokenizer->endPos = tokenizer->startPos + 1;
		tokenizer->token[i][0] = tokenizer->input[tokenizer->startPos];
		tokenizer->token[i][1] = '\0';
		if (tokenizer->input[tokenizer->startPos] == '"')
			tokenizer->inString = 1;

		return tokenizer->token[i];
	}

	// Now find the end of the token. This includes several additional characters that are okay to capture as one character, but not trailing at the end of another token.
	tokenizer->endPos = tokenizer->startPos + 1;
	while ((tokenizer->input[tokenizer->endPos] != ' '
			&& tokenizer->input[tokenizer->endPos] != '\t'
			&& tokenizer->input[tokenizer->endPos] != '\r'
			&& tokenizer->input[tokenizer->endPos] != '\n'
			&& tokenizer->input[tokenizer->endPos] != ','
			&& tokenizer->input[tokenizer->endPos] != '{'
			&& tokenizer->input[tokenizer->endPos] != '}'
			&& tokenizer->input[tokenizer->endPos] != '['
			&& tokenizer->input[tokenizer->endPos] != ']'
			&& tokenizer->input[tokenizer->endPos] != '='
			&& tokenizer->input[tokenizer->endPos] != ':'
			&& tokenizer->input[tokenizer->endPos] != '%'
			&& tokenizer->input[tokenizer->endPos] != '@'
			&& tokenizer->input[tokenizer->endPos] != ';'
			&& tokenizer->inComment == 0)
			&& tokenizer->endPos < tokenizer->inputLength)
	{
		tokenizer->endPos++;
		// Try to detect comment starts now; if it's in a comment, we don't want it in this token
		DetectComment(tokenizer, &tokenizer->endPos);
	}

	Tokenizer_ReadTokenString(tokenizer, i);
	return tokenizer->token[i];
}

#ifdef PS2_PROFILE
// PS2-LOAD-15: Tokenizer_SRB2Read below with the state in locals and the character tests in a table. It makes the same tokens and leaves the same
// startPos / endPos / inComment / line behind (tools/ps2/tokenizer_hosttest.c compares the two over the TEXTMAP of a real map and a million random inputs).
#define TCLS_SKIP 1 // between tokens: ' ' '\t' '\r' '\n' NUL '=' ';'
#define TCLS_END  2 // ends a plain token: ' ' '\t' '\r' '\n' ',' '{' '}' '=' ';'
static const UINT8 tokcls[256] =
{
	[0] = TCLS_SKIP, [' '] = TCLS_SKIP|TCLS_END, ['\t'] = TCLS_SKIP|TCLS_END, ['\r'] = TCLS_SKIP|TCLS_END, ['\n'] = TCLS_SKIP|TCLS_END,
	['='] = TCLS_SKIP|TCLS_END, [';'] = TCLS_SKIP|TCLS_END, [','] = TCLS_END, ['{'] = TCLS_END, ['}'] = TCLS_END,
};

// The next token: [*s, *e) of the input (a quoted string without its quotes). False where the original returned NULL (tokenizer->endPos is then inputLength).
// *kind: 0 a word or string, 1 one of , { }
static boolean Tokenizer_SRB2Next(tokenizer_t *t, UINT32 *s, UINT32 *e, UINT8 *kind)
{
	const char *in = t->input;
	const UINT32 len = t->inputLength;
	UINT32 sp = t->endPos, ep;
	UINT8 inc = t->inComment;
	int line = t->line;

	// Try to detect comments now, in case we're pointing right at one
	if (!inc && sp < len - 1 && in[sp] == '/')
	{
		if (in[sp + 1] == '/') inc = 1;
		else if (in[sp + 1] == '*') inc = 2;
	}

	// Find the first non-whitespace char, or else the end of the string trying
	while ((inc != 0 || (tokcls[(UINT8)in[sp]] & TCLS_SKIP)) && sp < len)
	{
		const UINT8 c = (UINT8)in[sp];

		if (c == '\n')
		{
			line++;
			if (inc == 1)
				inc = 0; // End of line for a single-line comment
		}
		else if (inc == 2 && sp < len - 1 && c == '*' && in[sp + 1] == '/')
		{
			inc = 0; // End of multi-line comment
			sp++; // Make damn well sure we're out of the comment ending at the end of it all
		}
		sp++;
		if (!inc && sp < len - 1 && in[sp] == '/')
		{
			if (in[sp + 1] == '/') inc = 1;
			else if (in[sp + 1] == '*') inc = 2;
		}
	}
	t->startPos = sp;
	t->line = line;
	t->inComment = inc;

	if (sp == len)
	{
		t->endPos = len;
		return false;
	}
	if (in[sp] == ',' || in[sp] == '{' || in[sp] == '}')
	{
		t->endPos = sp + 1;
		*s = sp;
		*e = sp + 1;
		*kind = 1;
		return true;
	}
	if (in[sp] == '"')
	{
		ep = ++sp;
		while (ep < len && in[ep] != '"')
		{
			if (in[ep] == '\n')
				line++;
			ep++;
		}
		t->startPos = sp;
		t->line = line;
		t->endPos = ep + 1;
		*s = sp;
		*e = ep;
		*kind = 0;
		return true;
	}

	// Now find the end of the token (a comment starting inside it ends it)
	ep = sp + 1;
	while (ep < len && !(tokcls[(UINT8)in[ep]] & TCLS_END))
	{
		ep++;
		if (in[ep] == '/' && ep < len - 1)
		{
			if (in[ep + 1] == '/') { inc = 1; break; }
			if (in[ep + 1] == '*') { inc = 2; break; }
		}
	}
	t->inComment = inc;
	t->endPos = ep;
	*s = sp;
	*e = ep;
	*kind = 0;
	return true;
}

const char *Tokenizer_SRB2Read(tokenizer_t *tokenizer, UINT32 i)
{
	UINT32 s, e, next;
	UINT8 kind;

	if (!tokenizer->input)
		return NULL;
	if (!Tokenizer_SRB2Next(tokenizer, &s, &e, &kind))
		return NULL;
	if (kind)
	{
		tokenizer->token[i][0] = tokenizer->input[s];
		tokenizer->token[i][1] = '\0';
		return tokenizer->token[i];
	}
	next = tokenizer->endPos;
	tokenizer->startPos = s;
	tokenizer->endPos = e;
	Tokenizer_ReadTokenString(tokenizer, i);
	tokenizer->endPos = next; // (past the closing quote of a string)
	return tokenizer->token[i];
}

// TextmapCount, inside a block: moves past the "}" that closes it without making a token of anything in between, looking at the bytes eight at a time. The rules of
// Tokenizer_SRB2Next that matter for finding it: a '}' outside a string is a token of its own (and the string "}" is the same token: it closed a block before too); a '"'
// that starts a token starts a string, in the middle of a word it is an ordinary character. A '/' outside a string may start a comment, and the original starts the comment
// check of a word one character late ("a//b" is one word), so a block with a '/' in it is done by the exact token loop instead. False when the input ends first or the
// closing token ends at or after `size` (the original loop stopped there without looking at the token). tokenizer->line is not counted here (nothing reads it).
#define SKIP_LO 0x0101010101010101ull
#define SKIP_HI 0x8080808080808080ull
static inline boolean SkipHasByte(UINT64 w, UINT8 c)
{
	const UINT64 x = w ^ (SKIP_LO * c);

	return ((x - SKIP_LO) & ~x & SKIP_HI) != 0;
}

static boolean Tokenizer_SRB2SkipBlockExact(tokenizer_t *tokenizer, UINT32 size)
{
	UINT32 s, e;
	UINT8 kind;

	while (Tokenizer_SRB2Next(tokenizer, &s, &e, &kind))
	{
		if (tokenizer->endPos >= size)
			return false;
		if (e - s == 1 && tokenizer->input[s] == '}')
			return true;
	}
	return false;
}

boolean Tokenizer_SRB2SkipBlock(tokenizer_t *tokenizer, UINT32 size)
{
	const char *in = tokenizer->input;
	const UINT32 len = tokenizer->inputLength;
	UINT32 p = tokenizer->endPos;
	boolean afterstring = true; // the byte before p ends a token or a string: a '"' at p starts a string

	if (!in)
		return false;
	if (tokenizer->inComment)
		return Tokenizer_SRB2SkipBlockExact(tokenizer, size);

	while (p < len)
	{
		UINT8 c;

		while (p + 8 <= len)
		{
			UINT64 w;

			memcpy(&w, in + p, 8);
			if (SkipHasByte(w, '}') || SkipHasByte(w, '"') || SkipHasByte(w, '/'))
				break;
			p += 8;
			afterstring = false;
		}
		if (p >= len)
			break;
		c = (UINT8)in[p];

		if (c == '/')
			return Tokenizer_SRB2SkipBlockExact(tokenizer, size);
		if (c == '}')
		{
			p++;
			tokenizer->startPos = p - 1;
			tokenizer->endPos = p;
			return p < size;
		}
		if (c == '"')
		{
			UINT32 q;

			if (!afterstring && p > 0)
			{
				const UINT8 b = (UINT8)in[p - 1];

				if (!((tokcls[b] & TCLS_SKIP) || b == ',' || b == '{' || b == '}'))
				{
					p++; // inside a word: an ordinary character
					continue;
				}
			}
			q = p + 1;
			while (q < len && in[q] != '"')
				q++;
			if (q - (p + 1) == 1 && in[p + 1] == '}') // the string "}"
			{
				tokenizer->startPos = p + 1;
				tokenizer->endPos = q + 1;
				return q + 1 < size;
			}
			p = q + 1;
			afterstring = true;
			continue;
		}
		p++;
		afterstring = false;
	}
	tokenizer->startPos = tokenizer->endPos = p > len ? p : len;
	return false;
}

// TextmapParse: the next "param value" pair of a block, or its closing "}". Returns 1 with the two strings, 0 at the "}" (or the end of the input).
// The ordinary pair (`name = value;`, `name = "string";` with ordinary white space) is cut out of the tokenizer's own copy of the text: the byte after each token is
// replaced by a NUL (it is a separator, or the closing quote, which nothing reads again: every block is parsed once, after the count pass) and no token is copied.
// Anything else (a comment, a ',' '{' '}' or '"' where a name or value should start, a name that ends at one of those, an input that ends) takes the original
// path, token by token, from the same position, so the answer is the same.
#ifdef TOK_STATS
long tok_generic_pairs;
#endif
int Tokenizer_SRB2ReadPair(tokenizer_t *t, const char **param, const char **val)
{
	char *in = t->zdup;
	const UINT32 len = t->inputLength;
	UINT32 p = t->endPos, s1, e1, s2, e2, next;
	UINT8 c;

	if (t->inComment || in != t->input)
		goto generic;

	// name
	while (p < len && (c = (UINT8)in[p], c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '=' || c == ';' || c == 0)) // (a NUL inside the text is a terminator written by an earlier pair: the original skips it too)
		p++;
	if (p >= len)
		goto generic;
	c = (UINT8)in[p];
	if (c == '}')
	{
		t->startPos = p;
		t->endPos = p + 1;
		*param = *val = NULL;
		return 0;
	}
	if (c == ',' || c == '{' || c == '"' || c == '/' || c == 0)
		goto generic;
	s1 = p;
	while (p < len && !(tokcls[(UINT8)in[p]] & TCLS_END))
	{
		if (in[p] == '/')
			goto generic;
		p++;
	}
	e1 = p;
	if (p >= len || !(tokcls[(UINT8)in[p]] & TCLS_SKIP) || in[p] == 0)
		goto generic; // the name ends at ',' '{' '}' or the end of the input

	// value
	p++;
	while (p < len && (c = (UINT8)in[p], c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '=' || c == ';' || c == 0)) // (a NUL inside the text is a terminator written by an earlier pair: the original skips it too)
		p++;
	if (p >= len)
		goto generic;
	c = (UINT8)in[p];
	if (c == '"')
	{
		s2 = ++p;
		while (p < len && in[p] != '"')
			p++;
		if (p >= len)
			goto generic;
		e2 = p; // the closing quote
		next = p + 1;
	}
	else
	{
		if (c == '}' || c == ',' || c == '{' || c == '/' || c == 0)
			goto generic;
		s2 = p;
		while (p < len && !(tokcls[(UINT8)in[p]] & TCLS_END))
		{
			if (in[p] == '/')
				goto generic;
			p++;
		}
		e2 = p;
		if (p >= len || !(tokcls[(UINT8)in[p]] & TCLS_SKIP) || in[p] == 0)
			goto generic;
		next = p;
	}
	in[e1] = '\0';
	in[e2] = '\0';
	t->startPos = s2;
	t->endPos = next;
	*param = in + s1;
	*val = in + s2;
	return 1;

generic:
#ifdef TOK_STATS
	tok_generic_pairs++;
#endif
	*param = Tokenizer_SRB2Read(t, 0);
	if (!*param || (((*param)[0] == '}') && !(*param)[1]))
		return 0;
	*val = Tokenizer_SRB2Read(t, 1);
	return 1;
}
#endif

#ifndef PS2_PROFILE
const char *Tokenizer_SRB2Read(tokenizer_t *tokenizer, UINT32 i)
{
	if (!tokenizer->input)
		return NULL;

	tokenizer->startPos = tokenizer->endPos;

	// Try to detect comments now, in case we're pointing right at one
	DetectComment(tokenizer, &tokenizer->startPos);

	// Find the first non-whitespace char, or else the end of the string trying
	while ((tokenizer->input[tokenizer->startPos] == ' '
			|| tokenizer->input[tokenizer->startPos] == '\t'
			|| tokenizer->input[tokenizer->startPos] == '\r'
			|| tokenizer->input[tokenizer->startPos] == '\n'
			|| tokenizer->input[tokenizer->startPos] == '\0'
			|| tokenizer->input[tokenizer->startPos] == '=' || tokenizer->input[tokenizer->startPos] == ';' // UDMF TEXTMAP.
			|| tokenizer->inComment != 0)
			&& tokenizer->startPos < tokenizer->inputLength)
	{
		boolean inLineBreak = DetectLineBreak(tokenizer, tokenizer->startPos);

		// Try to detect comment endings now
		if (tokenizer->inComment == 1 && inLineBreak)
			tokenizer->inComment = 0; // End of line for a single-line comment
		else if (tokenizer->inComment == 2
			&& tokenizer->startPos < tokenizer->inputLength - 1
			&& tokenizer->input[tokenizer->startPos] == '*'
			&& tokenizer->input[tokenizer->startPos+1] == '/')
		{
			// End of multi-line comment
			tokenizer->inComment = 0;
			tokenizer->startPos++; // Make damn well sure we're out of the comment ending at the end of it all
		}

		tokenizer->startPos++;
		DetectComment(tokenizer, &tokenizer->startPos);
	}

	// If the end of the string is reached, no token is to be read
	if (tokenizer->startPos == tokenizer->inputLength) {
		tokenizer->endPos = tokenizer->inputLength;
		return NULL;
	}
	// Else, if it's one of these three symbols, capture only this one character
	else if (tokenizer->input[tokenizer->startPos] == ','
			|| tokenizer->input[tokenizer->startPos] == '{'
			|| tokenizer->input[tokenizer->startPos] == '}')
	{
		tokenizer->endPos = tokenizer->startPos + 1;
		tokenizer->token[i][0] = tokenizer->input[tokenizer->startPos];
		tokenizer->token[i][1] = '\0';
		return tokenizer->token[i];
	}
	// Return entire string within quotes, except without the quotes.
	else if (tokenizer->input[tokenizer->startPos] == '"')
	{
		tokenizer->endPos = ++tokenizer->startPos;
		while (tokenizer->input[tokenizer->endPos] != '"' && tokenizer->endPos < tokenizer->inputLength)
		{
			DetectLineBreak(tokenizer, tokenizer->endPos);
			tokenizer->endPos++;
		}

		Tokenizer_ReadTokenString(tokenizer, i);
		tokenizer->endPos++;
		return tokenizer->token[i];
	}

	// Now find the end of the token. This includes several additional characters that are okay to capture as one character, but not trailing at the end of another token.
	tokenizer->endPos = tokenizer->startPos + 1;
	while ((tokenizer->input[tokenizer->endPos] != ' '
			&& tokenizer->input[tokenizer->endPos] != '\t'
			&& tokenizer->input[tokenizer->endPos] != '\r'
			&& tokenizer->input[tokenizer->endPos] != '\n'
			&& tokenizer->input[tokenizer->endPos] != ','
			&& tokenizer->input[tokenizer->endPos] != '{'
			&& tokenizer->input[tokenizer->endPos] != '}'
			&& tokenizer->input[tokenizer->endPos] != '=' && tokenizer->input[tokenizer->endPos] != ';' // UDMF TEXTMAP.
			&& tokenizer->inComment == 0)
			&& tokenizer->endPos < tokenizer->inputLength)
	{
		tokenizer->endPos++;
		// Try to detect comment starts now; if it's in a comment, we don't want it in this token
		DetectComment(tokenizer, &tokenizer->endPos);
	}

	Tokenizer_ReadTokenString(tokenizer, i);
	return tokenizer->token[i];
}
#endif // !PS2_PROFILE

UINT32 Tokenizer_GetEndPos(tokenizer_t *tokenizer)
{
	return tokenizer->endPos;
}

void Tokenizer_SetEndPos(tokenizer_t *tokenizer, UINT32 newPos)
{
	tokenizer->endPos = newPos;
}
