// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 2013-2024 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  m_tokenizer.h
/// \brief Tokenizer

#ifndef __M_TOKENIZER__
#define __M_TOKENIZER__

#include "doomdef.h"

typedef struct Tokenizer
{
	char *zdup;
	const char *input;
	unsigned numTokens;
	UINT32 *capacity;
	char **token;
	UINT32 startPos;
	UINT32 endPos;
	UINT32 inputLength;
	UINT8 inComment; // 0 = not in comment, 1 = // Single-line, 2 = /* Multi-line */
	UINT8 inString; // 0 = not in string, 1 = in string, 2 = just left string
	int line;
	const char *(*get)(struct Tokenizer*, UINT32);
} tokenizer_t;

tokenizer_t *Tokenizer_Open(const char *inputString, size_t len, unsigned numTokens);
void Tokenizer_Close(tokenizer_t *tokenizer);

const char *Tokenizer_Read(tokenizer_t *tokenizer, UINT32 i);
const char *Tokenizer_SRB2Read(tokenizer_t *tokenizer, UINT32 i);
#ifdef PS2_PROFILE
boolean Tokenizer_SRB2SkipBlock(tokenizer_t *tokenizer, UINT32 size); // PS2-LOAD-15
int Tokenizer_SRB2ReadPair(tokenizer_t *tokenizer, const char **param, const char **val); // PS2-LOAD-17

// PS2-LOAD-25: the blocks of a TEXTMAP that only has ordinary pairs, scanned once (m_tokenizer.c)
typedef struct
{
	UINT32 s1, s2; // start of the name / of the value in the tokenizer's copy of the text
	UINT16 l1, l2; // their lengths
} tokpair_t;
typedef struct
{
	tokpair_t *pairs; // all the pairs of all the blocks, in the order of the text
	UINT32 npairs, pcap;
	UINT32 *bfirst; // per block: its first pair (and bfirst[nblocks] = npairs)
	UINT8 *btype; // per block: 0 thing, 1 linedef, 2 sidedef, 3 vertex, 4 sector
	UINT32 nblocks, bcap, btcap;
	UINT32 counts[5], counts_filled[5];
	UINT32 *list[5]; // per type: the block numbers of its blocks in order
} tokscan_t;
boolean Tokenizer_SRB2ScanBlocks(tokenizer_t *tokenizer, UINT32 size, tokscan_t *scan);
void Tokenizer_SRB2ScanParse(tokenizer_t *tokenizer, const tokscan_t *scan, int type, UINT32 num, void (*parser)(UINT32, const char *, const char *));
void Tokenizer_SRB2ScanFree(tokscan_t *scan);
// the same on the global tokenizer of M_TokenizerOpen (m_misc.c)
boolean M_TokenizerScanBlocks(UINT32 size, tokscan_t *scan);
void M_TokenizerScanParse(const tokscan_t *scan, int type, UINT32 num, void (*parser)(UINT32, const char *, const char *));
#endif
UINT32 Tokenizer_GetEndPos(tokenizer_t *tokenizer);
void Tokenizer_SetEndPos(tokenizer_t *tokenizer, UINT32 newPos);

#endif
