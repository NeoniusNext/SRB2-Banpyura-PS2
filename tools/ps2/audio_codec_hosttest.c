// Uses real host Vorbis/mpg123 DLLs and stock audio fixtures through read_at.
#include "ps2_music.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"codec:%d: %s (%s)\n",__LINE__,#x,path); exit(1); } } while (0)
typedef struct { FILE *file; size_t length, maxread; } source;
static size_t Read(void *user, size_t off, void *dst, size_t n)
{
	source *s = user;
	if (off > s->length || n > s->length-off) return 0;
	if (n > s->maxread) s->maxread = n;
	if (fseek(s->file,(long)off,SEEK_SET)) return 0;
	return fread(dst,1,n,s->file);
}
int main(int argc, char **argv)
{
	unsigned counts[5] = {0}, silent = 0;
	int arg;
	for (arg = 1; arg < argc; arg++)
	{
		const char *path = argv[arg];
		source s;
		ps2_audio_input in;
		ps2_music *m;
		int16_t a[1024], b[1024];
		unsigned nonzero = 0, i, blocks;
		ps2_music_type type;
		s.file = fopen(path,"rb"); CHECK(s.file);
		CHECK(!fseek(s.file,0,SEEK_END)); s.length = (size_t)ftell(s.file); s.maxread = 0;
		in.user = &s; in.size = s.length; in.read_at = Read;
		m = PS2_MusicOpen(&in); CHECK(m);
		type = PS2_MusicType(m); CHECK(type > PS2_MUSIC_NONE && type <= PS2_MUSIC_MIDI);
		counts[type]++;
		CHECK(PS2_MusicPlay(m,0)); CHECK(PS2_MusicRender(m,a,512) > 0);
		CHECK(PS2_MusicSeek(m,0)); CHECK(PS2_MusicRender(m,b,512) > 0);
		CHECK(!memcmp(a,b,sizeof a));
		for (blocks = 0; blocks < 16 && PS2_MusicPlaying(m); blocks++)
		{
			PS2_MusicRender(m,a,512);
			for (i = 0; i < 1024; i++) if (a[i]) nonzero++;
		}
		CHECK(!PS2_MusicError(m));
		if (!nonzero) silent++;
		if (PS2_MusicLength(m) > 2000)
		{
			CHECK(PS2_MusicSeek(m,1000)); CHECK(PS2_MusicPosition(m) >= 999);
			CHECK(PS2_MusicRender(m,a,512) == 512 && !PS2_MusicError(m));
		}
		CHECK(s.maxread <= 65536);
		PS2_MusicClose(m); fclose(s.file);
	}
	printf("Real codecs: WAV=%u Vorbis=%u MP3=%u MIDI=%u, silent intro/fixture=%u, bounded reads/restart/seek PASS\n",
		counts[1],counts[2],counts[3],counts[4],silent);
	return 0;
}
