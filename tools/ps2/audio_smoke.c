// Hardware smoke: pass a WAV/OGG/MP3/MID path, e.g. host:audio-test.ogg.
// Without an argument, plays a real PCM test tone for two seconds.
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <sbv_patches.h>
#include <audsrv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ps2_audio.h"
#include "ps2_music.h"

extern unsigned char audsrv_irx[];
extern unsigned int size_audsrv_irx;
static int16_t pcm[PS2_AUDIO_BLOCK * 2] __attribute__((aligned(64)));
static size_t FileRead(void *f, size_t off, void *dst, size_t n)
{ if (fseek(f, (long)off, SEEK_SET)) return 0; return fread(dst,1,n,f); }

int main(int argc, char **argv)
{
	audsrv_fmt_t format = { PS2_AUDIO_RATE, 16, 2 };
	ps2_music *music = NULL;
	FILE *file = NULL;
	unsigned frames = 0, phase = 0;
	int ret = 0;
	setvbuf(stdout,NULL,_IONBF,0);
	sceSifInitRpc(0); sbv_patch_enable_lmb();
	if (SifLoadModule("rom0:LIBSD",0,NULL) < 0
		|| SifExecModuleBuffer(audsrv_irx,size_audsrv_irx,0,NULL,&ret) < 0 || ret == 1
		|| audsrv_init() || audsrv_set_format(&format) || audsrv_set_volume(100))
	{ puts("AUDIO FAIL init"); return 1; }
	if (argc > 1)
	{
		ps2_audio_input in;
		long length;
		file = fopen(argv[1],"rb"); if (!file) { puts("AUDIO FAIL open"); return 1; }
		fseek(file,0,SEEK_END); length = ftell(file);
		if (length <= 0) return 1;
		in.user = file; in.size = (size_t)length; in.read_at = FileRead;
		music = PS2_MusicOpen(&in);
		if (!music || !PS2_MusicPlay(music,0)) { puts("AUDIO FAIL decoder"); return 1; }
		printf("AUDIO type=%d length=%lu\n", (int)PS2_MusicType(music), (unsigned long)PS2_MusicLength(music));
	}
	while (music ? PS2_MusicPlaying(music) : frames < PS2_AUDIO_RATE*2)
	{
		unsigned i;
		if (music) PS2_MusicRender(music,pcm,PS2_AUDIO_BLOCK);
		else for (i = 0; i < PS2_AUDIO_BLOCK; i++)
		{
			phase += 440; if (phase >= PS2_AUDIO_RATE) phase -= PS2_AUDIO_RATE;
			pcm[i*2] = pcm[i*2+1] = phase < PS2_AUDIO_RATE/2 ? 4000 : -4000;
		}
		if (audsrv_wait_audio(sizeof pcm) || audsrv_play_audio((char *)pcm,sizeof pcm) != sizeof pcm)
		{ puts("AUDIO FAIL transfer"); return 1; }
		frames += PS2_AUDIO_BLOCK;
	}
	while (audsrv_queued() > 0) usleep(10000);
	printf("AUDIO PASS submitted=%u frames (confirm audible output manually)\n",frames);
	PS2_MusicClose(music); if (file) fclose(file);
	audsrv_stop_audio(); audsrv_quit(); return 0;
}
