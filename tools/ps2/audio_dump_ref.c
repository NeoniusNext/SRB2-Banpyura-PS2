// Host reference decoder for the PCM-continuity check (tools/ps2/audio_pcm_check.py): decodes a music file with the engine's
// own code (ps2_music.c: Vorbis/WAV/MIDI/MP3 through the host libraries) exactly like the audio engine does, in 512-frame blocks,
// looping, and writes raw signed 16-bit stereo 22050 Hz to a file.
// usage: audio_dump_ref <music file> <frames> <out.raw> [loop_ms]
#include "ps2_music.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { FILE *file; size_t length; } source;
static size_t Read(void *user, size_t off, void *dst, size_t n)
{
	source *s = user;
	if (off > s->length || n > s->length - off) return 0;
	if (fseek(s->file, (long)off, SEEK_SET)) return 0;
	return fread(dst, 1, n, s->file);
}

int main(int argc, char **argv)
{
	source s;
	ps2_audio_input in;
	ps2_music *m;
	FILE *out;
	long frames, done = 0;
	int16_t block[PS2_AUDIO_BLOCK * 2];
	if (argc < 4) { fprintf(stderr, "usage: audio_dump_ref <music file> <frames> <out.raw> [loop_ms]\n"); return 2; }
	s.file = fopen(argv[1], "rb");
	if (!s.file) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
	fseek(s.file, 0, SEEK_END); s.length = (size_t)ftell(s.file);
	in.user = &s; in.size = s.length; in.read_at = Read;
	m = PS2_MusicOpen(&in);
	if (!m) { fprintf(stderr, "cannot open the music stream\n"); return 1; }
	if (argc > 4) PS2_MusicSetLoop(m, (uint32_t)atol(argv[4]));
	frames = atol(argv[2]);
	out = fopen(argv[3], "wb");
	if (!out || !PS2_MusicPlay(m, 1)) { fprintf(stderr, "cannot start\n"); return 1; }
	while (done < frames)
	{
		PS2_MusicRender(m, block, PS2_AUDIO_BLOCK);
		fwrite(block, sizeof block, 1, out);
		done += PS2_AUDIO_BLOCK;
	}
	printf("decoded %ld frames of %s (length %u ms, loop %u ms)\n", done, argv[1], PS2_MusicLength(m), PS2_MusicLoop(m));
	fclose(out); PS2_MusicClose(m); fclose(s.file);
	return 0;
}
