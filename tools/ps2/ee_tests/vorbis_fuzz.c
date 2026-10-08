// Truncated / damaged packets through the vendored decoder (EOP handling of the Huffman paths): reads the packets of an Ogg Vorbis file with libogg,
// cuts a random number of bytes off the end of some audio packets (and flips bits in a few), decodes with vorbis_synthesis / blockin / pcmout (half rate
// as ps2_music.c) and prints the FNV-1a hash of the PCM plus the count of decode errors.  The generic, fast and twin builds must print the same line.
//   usage: vorbis_fuzz file.ogg seed
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ogg/ogg.h>
#include <vorbis/codec.h>

static uint32_t rs;
static uint32_t rnd(void) { rs = rs * 1664525u + 1013904223u; return rs >> 8; }
static int16_t conv(float f)
{
	float p = f * 32768.0f; int t; float frac;
	if (p > 32768.0f) p = 32768.0f; else if (p < -32769.0f) p = -32769.0f;
	t = (int)p; frac = p - (float)t; t += (frac >= 0.5f) - (frac < -0.5f);
	return (int16_t)(t > 32767 ? 32767 : t < -32768 ? -32768 : t);
}

int main(int argc, char **argv)
{
	FILE *f = fopen(argv[1], "rb");
	ogg_sync_state oy; ogg_stream_state os; ogg_page og; ogg_packet op;
	vorbis_info vi; vorbis_comment vc; vorbis_dsp_state vd; vorbis_block vb;
	int headers = 0, inited = 0, errors = 0, packets = 0;
	uint64_t h = 14695981039346656037ull;
	char *buf;
	rs = (uint32_t)atoi(argv[2]) * 2654435761u + 1;
	ogg_sync_init(&oy); vorbis_info_init(&vi); vorbis_comment_init(&vc);
	while (packets < 1500)
	{
		int n;
		while (ogg_sync_pageout(&oy, &og) != 1)
		{
			buf = ogg_sync_buffer(&oy, 4096);
			n = (int)fread(buf, 1, 4096, f);
			if (n <= 0) goto done;
			ogg_sync_wrote(&oy, n);
		}
		if (!inited) { ogg_stream_init(&os, ogg_page_serialno(&og)); inited = 1; }
		ogg_stream_pagein(&os, &og);
		while (ogg_stream_packetout(&os, &op) == 1)
		{
			if (headers < 3)
			{
				if (vorbis_synthesis_headerin(&vi, &vc, &op) < 0) { fprintf(stderr, "bad header\n"); return 2; }
				if (++headers == 3)
				{
					if (vi.rate >= 44000 && !(vi.rate & 1)) vorbis_synthesis_halfrate(&vi, 1);
					vorbis_synthesis_init(&vd, &vi); vorbis_block_init(&vd, &vb);
				}
				continue;
			}
			{
				unsigned char *copy = malloc(op.bytes + 8);
				long ob = op.bytes;
				memcpy(copy, op.packet, op.bytes);
				op.packet = copy;
				if (rnd() % 3 == 0 && op.bytes > 4) op.bytes -= (long)(rnd() % (op.bytes - 1)) / (rnd() % 4 + 1);   /* cut the tail */
				if (rnd() % 7 == 0 && op.bytes > 2) copy[rnd() % op.bytes] ^= (unsigned char)(1u << (rnd() % 8));
				if (vorbis_synthesis(&vb, &op) == 0)
				{
					float **pcm;
					int got;
					vorbis_synthesis_blockin(&vd, &vb);
					while ((got = vorbis_synthesis_pcmout(&vd, &pcm)) > 0)
					{
						int i;
						for (i = 0; i < got; i++)
						{
							int16_t s[2];
							int k;
							s[0] = conv(pcm[0][i]); s[1] = conv(vi.channels == 2 ? pcm[1][i] : pcm[0][i]);
							for (k = 0; k < 4; k++) h = (h ^ ((const uint8_t *)s)[k]) * 1099511628211ull;
						}
						vorbis_synthesis_read(&vd, got);
					}
				}
				else errors++;
				(void)ob;
				free(copy);
				packets++;
			}
		}
	}
done:
	printf("%s seed %s packets %d errors %d fnv=%016llx\n", argv[1], argv[2], packets, errors, (unsigned long long)h);
	return 0;
}
