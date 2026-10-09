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

	tokenizer->input = M_Memcpy(tokenizer->zdup, inputString, len);
	tokenizer->startPos = 0;
	tokenizer->endPos = 0;
	tokenizer->inputLength = 0;
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

#ifdef PS2_PROFILE
	{ // PS2-LOAD-15: the first NUL of the copy (what strlen found), looked for in the source bytes by the word-wise memchr
		const char *nul = memchr(tokenizer->input, 0, len);

		tokenizer->inputLength = nul ? (UINT32)(nul - tokenizer->input) : (UINT32)len;
	}
#else
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

// TextmapCount, inside a block: moves past the "}" that closes it without making a token of anything in between. The block is walked byte by byte with the same
// rules as Tokenizer_SRB2Next: outside a word or string a ',' '{' or '}' is a token of its own, a '"' starts a string (its content is the token: "\"}\"" closes a block
// too, as it did), '=' ';' and white space separate, a word ends at one of those, and "//" or "/*" starts a comment wherever it is met outside a string (a block comment
// may end with the "*/" that follows its own "/*" after one more character, as the original loop did: "/*/" is a whole comment). False when the input ends first or the
// closing token ends at or after `size` (the original loop stopped there without looking at the token). tokenizer->line is not counted here (nothing reads it).
boolean Tokenizer_SRB2SkipBlock(tokenizer_t *tokenizer, UINT32 size)
{
	const char *in = tokenizer->input;
	const UINT32 len = tokenizer->inputLength;
	UINT32 p = tokenizer->endPos;
	UINT8 inc = tokenizer->inComment;
	UINT32 close = len; // index of the closing '}' (or of the closing quote of a string "}")
	boolean found = false;

	if (!in)
		return false;

	if (inc == 1) // (a single-line comment left open by the last token: ends at the next line break)
	{
		while (p < len && in[p] != '\n')
			p++;
		if (p < len) { p++; inc = 0; }
	}
	else if (inc == 2) // (a block comment left open)
	{
		while (p < len && !(p < len - 1 && in[p] == '*' && in[p + 1] == '/'))
			p++;
		if (p < len) { p += 2; inc = 0; }
	}

	while (inc == 0 && p < len)
	{
		const UINT8 c = (UINT8)in[p];

		if (c == '/' && p < len - 1 && (in[p + 1] == '/' || in[p + 1] == '*'))
		{
			if (in[p + 1] == '/')
			{
				while (p < len && in[p] != '\n')
					p++;
				if (p < len) p++; // (the line break ends the comment)
			}
			else
			{
				p++; // the loop of the original starts at the "/" and looks for "*/" from the next byte on
				while (p < len && !(p < len - 1 && in[p] == '*' && in[p + 1] == '/'))
					p++;
				if (p < len) p += 2;
			}
			if (p >= len)
				break;
			continue;
		}
		if (c == '}')
		{
			close = p;
			found = true;
			p++;
			break;
		}
		if (c == '"') // a string: its content is the token
		{
			UINT32 q = p + 1;

			while (q < len && in[q] != '"')
				q++;
			if (q - (p + 1) == 1 && in[p + 1] == '}') // the string "}"
			{
				p = q + 1;
				found = true;
				break;
			}
			p = q + 1;
			continue;
		}
		if (c == ',' || c == '{' || (tokcls[c] & TCLS_SKIP))
		{
			p++;
			continue;
		}
		// a word: up to the next separator, or to a comment that starts inside it
		p++;
		while (p < len && !(tokcls[(UINT8)in[p]] & TCLS_END))
		{
			p++;
			if (in[p] == '/' && p < len - 1 && (in[p + 1] == '/' || in[p + 1] == '*'))
				break;
		}
	}
	(void)close;
	tokenizer->inComment = 0;
	tokenizer->endPos = p;
	tokenizer->startPos = found ? p - 1 : p;
	if (!found)
	{
		if (p > len)
			tokenizer->endPos = p;
		return false;
	}
	return p < size;
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
