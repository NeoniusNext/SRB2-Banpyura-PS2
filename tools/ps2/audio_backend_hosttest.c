// Includes the production implementation with engine and RPC boundaries mocked.
// Only dependency includes are substituted by audio_check.py.
#include "audio_backend_stubs.h"
#include <stdarg.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"backend:%d: %s\n",__LINE__,#x); exit(1); } } while (0)
#include "i_sound_host.c"

sfxinfo_t S_sfx[NUMSFX];
uint16_t numwadfiles = 1;
static lumpinfo_t lumps[8];
static wadfile_t wad = { 8, lumps };
static wadfile_t *wadlist[] = { &wad };
wadfile_t **wadfiles = wadlist;
static const uint8_t *lumpdata[8];
static size_t lengths[8], allocated, zone_limit;
static unsigned reclaim_calls;
static z_reclaim_fn reclaim_hook;
static precise_t clock_ticks;
static int module_ok = 1, rpc_init_ok = 1, queue_bytes, sent, fail_transfer, fail_alloc;
static int write_limit = -1;
static const char *expected_chunk;
static int16_t first_pcm;
static unsigned callback_a, callback_b;

void CONS_Printf(const char *fmt, ...) { (void)fmt; }
void I_OutputMsg(const char *fmt, ...) { (void)fmt; }
boolean PS2Boot_LoadAudio(void) { return module_ok; }
precise_t I_GetPreciseTime(void) { return clock_ticks; }
precise_t I_GetPrecisePrecision(void) { return 1000000; }
size_t __real_W_LumpLength(lumpnum_t lump) { return lengths[lump]; }
size_t W_LumpLengthPwad(uint16_t w, uint16_t l) { CHECK(w == 0); return lengths[l]; }
size_t W_ReadLumpHeader(lumpnum_t l, void *dst, size_t n, size_t off)
{ CHECK(l < 8 && off <= lengths[l] && n <= lengths[l]-off); memcpy(dst,lumpdata[l]+off,n); return n; }
void W_ReadLump(lumpnum_t l, void *dst) { W_ReadLumpHeader(l,dst,lengths[l],0); }
const char *W_CheckNameForNum(lumpnum_t l) { return l == 2 ? "O_TEST" : "DS_TEST"; }
lumpnum_t S_GetSfxLumpNum(sfxinfo_t *sfx) { (void)sfx; return 1; }
void *Z_TryMallocAlign(size_t n, int tag, void *user, int alignment)
{
	size_t *p;
	(void)tag; (void)user; (void)alignment;
	if (fail_alloc) return NULL;
	if (zone_limit && allocated + n > zone_limit && reclaim_hook)
	{
		reclaim_calls++;
		reclaim_hook(allocated + n - zone_limit);
	}
	if (zone_limit && allocated + n > zone_limit) return NULL;
	p = malloc(n+sizeof *p); CHECK(p); *p = n; allocated += n; return p+1;
}
void Z_Free(void *p) { if (p) { size_t *n = (size_t *)p-1; allocated -= *n; free(n); } }
void *Z_ReallocAlign(void *p, size_t n, int tag, void *user, int alignment)
{
	size_t *old = (size_t *)p-1, *q;
	(void)tag; (void)user; (void)alignment;
	CHECK(p && n <= *old); /* the backend only ever shrinks (in place on the real zone) */
	allocated -= *old; q = realloc(old, n+sizeof *q); CHECK(q); *q = n; allocated += n; return q+1;
}
void Z_SetReclaimHook(z_reclaim_fn fn) { reclaim_hook = fn; }
int audsrv_init(void) { return rpc_init_ok ? 0 : 1; }
int audsrv_quit(void) { queue_bytes = 0; return 0; }
int audsrv_set_format(audsrv_fmt_t *fmt) { CHECK(fmt->freq == 22050 && fmt->bits == 16 && fmt->channels == 2); return 0; }
int audsrv_set_volume(int volume) { CHECK(volume == 100); return 0; }
int audsrv_stop_audio(void) { queue_bytes = 0; return 0; }
int audsrv_available(void) { return 20480 - queue_bytes; }
int audsrv_queued(void) { return queue_bytes; }
int audsrv_get_error(void) { return fail_transfer ? AUDSRV_ERR_RPC_FAILED : AUDSRV_ERR_NOERROR; }
int audsrv_play_audio(const char *p, int n)
{
	CHECK(n > 0 && n <= 2048 && !(n & 3) && n <= audsrv_available());
	if (fail_transfer) return -1;
	if (expected_chunk) CHECK(p == expected_chunk);
	if (write_limit >= 0 && n > write_limit) n = write_limit;
	if (!n) return 0;
	if (expected_chunk) expected_chunk += n;
	memcpy(&first_pcm,p,sizeof first_pcm); sent += n; queue_bytes += n; return n;
}
static void CallbackB(void) { callback_b++; }
static void CallbackA(void) { callback_a++; CHECK(I_FadeSong(100,100,CallbackB)); }

static void LE16(uint8_t *p, unsigned n) { p[0] = (uint8_t)n; p[1] = (uint8_t)(n>>8); }
// PS2-71: a mono effect is cached as mono. The mixer must produce the same bytes as for the duplicated stereo frames (what the decoder returns).
static unsigned mono_checked;
static void CheckMonoAsStereo(const cached_sample *s)
{
	static const struct { unsigned vol, pan, pitch; } cfg[] = { {31,128,128}, {20,0,128}, {31,255,128}, {7,64,200}, {31,128,64}, {15,200,255}, {1,90,128} };
	ps2_sample ref = s->sample;
	uint8_t *dup;
	unsigned c, f;
	static ps2_mixer ma, mb;
	int16_t oa[PS2_AUDIO_BLOCK*2], ob[PS2_AUDIO_BLOCK*2];
	if (s->sample.pcm.channels != 1 || s->sample.pcm.bits != 16) return;
	dup = malloc((size_t)s->sample.pcm.frames*4); CHECK(dup);
	for (f = 0; f < s->sample.pcm.frames; f++)
	{ memcpy(dup+f*4, s->sample.bytes+s->sample.pcm.offset+f*2, 2); memcpy(dup+f*4+2, s->sample.bytes+s->sample.pcm.offset+f*2, 2); }
	ref.bytes = dup; ref.pcm.channels = 2; ref.pcm.offset = 0;
	for (c = 0; c < sizeof cfg/sizeof *cfg; c++)
	{
		unsigned blocks = 0;
		PS2_MixerInit(&ma); PS2_MixerInit(&mb); ma.volume = mb.volume = 31;
		CHECK(PS2_MixerStart(&ma,&s->sample,0,cfg[c].vol,cfg[c].pan,cfg[c].pitch) >= 0);
		CHECK(PS2_MixerStart(&mb,&ref,0,cfg[c].vol,cfg[c].pan,cfg[c].pitch) >= 0);
		while (ma.voices[0].sample && blocks < 200)
		{
			PS2_MixerRender(&ma,oa,NULL,PS2_AUDIO_BLOCK,32768); PS2_MixerRender(&mb,ob,NULL,PS2_AUDIO_BLOCK,32768);
			CHECK(!memcmp(oa,ob,sizeof oa) && (!ma.voices[0].sample) == (!mb.voices[0].sample));
			blocks++;
		}
	}
	free(dup); mono_checked++;
}
static void LE32(uint8_t *p, unsigned n) { LE16(p,n); LE16(p+2,n>>16); }
int main(int argc, char **argv)
{
	static uint8_t effect[] = {3,0,0x22,0x56,4,0,0,0,255,128,0,128};
	uint8_t wav[44+22050*2];
	uint8_t *large;
	unsigned i;
	int h, old_sent;
	if (argc > 1)
	{
		unsigned count = 0, half_rate = 0;
		int arg;
		I_StartupSound(); CHECK(sound_started);
		for (arg = 1; arg < argc; arg++)
		{
			FILE *f = fopen(argv[arg],"rb");
			uint8_t *data;
			size_t n;
			cached_sample *s;
			CHECK(f); CHECK(!fseek(f,0,SEEK_END)); n = (size_t)ftell(f); CHECK(n);
			data = malloc(n); CHECK(data); rewind(f); CHECK(fread(data,1,n,f) == n); fclose(f);
			lumpdata[6] = data; lengths[6] = n;
			S_sfx[6].name = argv[arg]; S_sfx[6].lumpnum = 6;
			s = I_GetSfx(&S_sfx[6]);
			if (!s) {
				fprintf(stderr,"stock effect failed: %s (%zu bytes, magic=%02x%02x%02x%02x)\n",argv[arg],n,data[0],data[1],data[2],data[3]);
				if (!memcmp(data,"RIFF",4)) {
					size_t off = 12;
					while (off + 8 <= n) {
						uint32_t bytes = data[off+4] | (uint32_t)data[off+5]<<8 | (uint32_t)data[off+6]<<16 | (uint32_t)data[off+7]<<24;
						fprintf(stderr,"  WAV %.4s at %zu bytes=%u\n",data+off,off,bytes);
						if (!memcmp(data+off,"fmt ",4) && bytes >= 16 && bytes <= n-off-8)
							fprintf(stderr,"  format=%u channels=%u align=%u bits=%u\n",data[off+8],data[off+10],data[off+20],data[off+22]);
						if (bytes > n-off-8) break;
						off += 8 + bytes + (bytes&1);
					}
				}
				return 1;
			}
			CHECK(s->sample.pcm.frames > 0 && s->sample.bytes && sample_bytes <= SFX_BUDGET);
			CheckMonoAsStereo(s);
			if (s->sample.pcm.rate == 11025 && memcmp(data,"OggS",4) == 0) half_rate++;
			h = I_StartSound(6,255,128,128,0,0); CHECK(h >= 0 && I_SoundIsPlaying(h));
			queue_bytes = 0; I_UpdateSound(); CHECK(sound_started);
			I_FreeSfx(&S_sfx[6]); CHECK(!I_SoundIsPlaying(h) && !allocated);
			free(data); count++;
		}
		I_ShutdownSound(); CHECK(!allocated);
		printf("Stock effects: %u decoded/mixed/freed, %u long Ogg effects half-rate, 2 MiB cache bound PASS\n",count,half_rate);
		printf("Mono effects cached as mono: %u, mixer output identical to the duplicated stereo frames at 7 gain/pan/pitch settings PASS\n",mono_checked);
		return 0;
	}
	module_ok = 0; I_StartupSound(); CHECK(!sound_started);
	module_ok = 1; rpc_init_ok = 0; I_StartupSound(); CHECK(!sound_started);
	rpc_init_ok = 1; I_StartupSound(); CHECK(sound_started);
	lumpdata[1] = effect; lengths[1] = sizeof effect;
	S_sfx[1].name = "test"; S_sfx[1].lumpnum = LUMPERROR;
	CHECK(I_GetSfx(&S_sfx[1]) && allocated > 0);
	h = I_StartSound(1,255,128,128,0,0); CHECK(h >= 0 && I_SoundIsPlaying(h));
	I_UpdateSound(); CHECK(sent == 2048 && first_pcm == 32512 && !I_SoundIsPlaying(h));
	h = I_StartSound(1,255,128,128,0,0); I_FreeSfx(&S_sfx[1]); CHECK(!I_SoundIsPlaying(h) && !allocated);
	CHECK(I_StartSound(1,255,128,128,0,0) == -1);
	fail_alloc = 1; CHECK(!I_GetSfx(&S_sfx[1])); fail_alloc = 0;
	// The engine's NULL-data music path: exact lookup capture, type/length,
	// pause preserving SFX, queue limits, and fades on the main thread.
	memset(wav,0,sizeof wav); memcpy(wav,"RIFF",4); LE32(wav+4,sizeof wav-8);
	memcpy(wav+8,"WAVEfmt ",8); LE32(wav+16,16); LE16(wav+20,1); LE16(wav+22,1);
	LE32(wav+24,22050); LE32(wav+28,44100); LE16(wav+32,2); LE16(wav+34,16);
	memcpy(wav+36,"data",4); LE32(wav+40,44100);
	for (i = 44; i < sizeof wav; i += 2) LE16(wav+i,10000);
	lumpdata[2] = wav; lengths[2] = sizeof wav;
	CHECK(!I_LoadSong(NULL,lengths[2]));
	CHECK(__wrap_W_LumpLength(2) == lengths[2]); CHECK(I_LoadSong(NULL,lengths[2]));
	CHECK(I_SongType() == MU_WAV && I_GetSongLength() == 1000 && I_PlaySong(true));
	// The IOP can return short/zero writes after the space query. Preserve
	// every unsent byte and do not advance the decoder again during retries.
	queue_bytes = 0; write_limit = 0; expected_chunk = (const char *)output;
	old_sent = sent; I_UpdateSound();
	CHECK(sound_started && sent == old_sent && output_pending == sizeof output && !output_offset);
	{
		UINT32 position = I_GetSongPosition();
		I_UpdateSound(); CHECK(I_GetSongPosition() == position && sent == old_sent);
		write_limit = 512; I_UpdateSound();
		CHECK(sound_started && sent == old_sent + 2048 && !output_pending);
		CHECK(expected_chunk == (const char *)output + sizeof output && I_GetSongPosition() == position);
	}
	expected_chunk = NULL; write_limit = -1;
	queue_bytes = 0; I_UpdateSound(); CHECK(first_pcm == 10000 && queue_bytes == 8192);
	old_sent = sent; I_UpdateSound(); CHECK(sent == old_sent); // bounded, no fake progress
	CHECK(I_FadeSongFromVolume(0,100,1000,CallbackA));
	clock_ticks = 500000; queue_bytes = 0; I_UpdateSound(); CHECK(first_pcm == 5000 && !callback_a);
	I_PauseSong(); clock_ticks += 1000000; I_UpdateSound(); CHECK(I_SongPaused() && !callback_a);
	I_ResumeSong(); clock_ticks += 500000; queue_bytes = 0; I_UpdateSound(); CHECK(callback_a == 1 && !callback_b);
	clock_ticks += 100000; I_UpdateSound(); CHECK(callback_b == 1);
	I_UpdateSound(); CHECK(callback_a == 1 && callback_b == 1);
	CHECK(I_FadeSongFromVolume(50,0,0,CallbackB)); CHECK(callback_b == 2);
	CHECK(I_SetSongPosition(1500) && I_GetSongPosition() == 500);
	I_StopSong(); CHECK(!I_SongPlaying() && I_SongType() == MU_WAV);
	I_UnloadSong(); CHECK(I_SongType() == MU_NONE && !I_PlaySong(false));
	CHECK(!I_LoadSong("not a song!",11) && !I_SongPlaying());
	// LRU eviction clears the engine's cached pointer; active sounds pin it.
	large = malloc(1200000); CHECK(large); memset(large,128,1200000);
	LE16(large,3); LE16(large+2,22050); LE32(large+4,1200000-8);
	lumpdata[3] = large; lengths[3] = 1200000;
	S_sfx[3].name = "large"; S_sfx[3].lumpnum = 3;
	S_sfx[4].name = "large2"; S_sfx[4].lumpnum = 3;
	CHECK(I_GetSfx(&S_sfx[3])); CHECK(I_GetSfx(&S_sfx[4]) && !S_sfx[3].data);
	h = I_StartSound(4,255,128,128,0,0); CHECK(h >= 0);
	CHECK(!I_GetSfx(&S_sfx[3]) && I_SoundIsPlaying(h));
	I_StopSound(h); CHECK(I_GetSfx(&S_sfx[3]) && !S_sfx[4].data);
	// PS2-71: the zone asks the effects cache for memory when nothing else can go: idle samples are released least recently used first,
	// a playing one and the one loaded last stay, and the owner pointer of a released effect is cleared.
	{
		uint8_t *mid = malloc(300000);
		static const int ids[] = {1,2,5,7};
		unsigned k;
		int h2, h3;
		I_FreeSfx(&S_sfx[3]); CHECK(!S_sfx[3].data); /* the 2 MiB budget must not be what evicts in this test */
		CHECK(mid); memset(mid,128,300000); LE16(mid,3); LE16(mid+2,22050); LE32(mid+4,300000-8);
		lumpdata[5] = mid; lengths[5] = 300000;
		for (k = 0; k < 4; k++) { S_sfx[ids[k]].name = "mid"; S_sfx[ids[k]].lumpnum = 5; S_sfx[ids[k]].data = NULL; }
		zone_limit = allocated + 3*(300000+sizeof(cached_sample)) + 4096;
		CHECK(I_GetSfx(&S_sfx[ids[0]])); h = I_StartSound(ids[0],255,128,128,0,0); CHECK(h >= 0 && I_SoundIsPlaying(h));
		CHECK(I_GetSfx(&S_sfx[ids[1]])); CHECK(I_GetSfx(&S_sfx[ids[2]]));
		reclaim_calls = 0;
		CHECK(I_GetSfx(&S_sfx[ids[3]]) && reclaim_calls >= 1);
		CHECK(!S_sfx[ids[1]].data && S_sfx[ids[0]].data && S_sfx[ids[2]].data && S_sfx[ids[3]].data && I_SoundIsPlaying(h));
		CHECK(allocated <= zone_limit);
		// everything left plays, and the budget is not what refuses: the allocation fails cleanly, nothing is torn down
		h2 = I_StartSound(ids[2],255,128,128,0,1); h3 = I_StartSound(ids[3],255,128,128,0,2); CHECK(h2 >= 0 && h3 >= 0);
		zone_limit = allocated + 1000; S_sfx[ids[1]].data = NULL;
		CHECK(!I_GetSfx(&S_sfx[ids[1]]) && I_SoundIsPlaying(h) && S_sfx[ids[0]].data);
		CHECK(S_sfx[ids[2]].data && S_sfx[ids[3]].data && I_SoundIsPlaying(h2) && I_SoundIsPlaying(h3));
		zone_limit = 0; I_StopSound(h); I_StopSound(h2); I_StopSound(h3);
		I_FreeSfx(&S_sfx[ids[0]]); I_FreeSfx(&S_sfx[ids[2]]); I_FreeSfx(&S_sfx[ids[3]]);
		free(mid); S_sfx[3].lumpnum = 3; CHECK(I_GetSfx(&S_sfx[3]));
		puts("Backend: zone reclaim hook frees idle effects LRU first, keeps playing and newest effects, fails cleanly when nothing can go PASS");
	}
	// Transfer failure invalidates actual playback, samples, and started state.
	h = I_StartSound(3,255,128,128,0,0); queue_bytes = 0; fail_transfer = 1;
	I_UpdateSound(); CHECK(!sound_started && !I_SoundIsPlaying(h) && !allocated);
	I_ShutdownSound(); CHECK(!allocated); free(large);
	puts("Backend: real PCM submission, NULL lump bridge, bounded queue/cache, active pinning, pause, reentrant fades, failure cleanup PASS");
	return 0;
}
