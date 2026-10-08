// The production i_sound.c (PCM pump) against an exact model of the IOP side of the stock audsrv.irx (PS2-300/301, docs/GATES/g1/opt11-AUDIO.md).
// The model is the code of iop/sound/audsrv/src/audsrv.c version 0.93 at 22050 Hz / 16 bit / stereo:
//   * ring of 9400 bytes, initial readpos 4700 / writepos 0 (audsrv_set_format);
//   * play_thread reads 940 bytes at readpos every 512 SPU2 samples (10.667 ms) once audsrv_play_audio set playing = 1, whether or not
//     anything was written (readpos overtakes writepos and the old content is played again);
//   * audsrv_available() = readpos - writepos (writepos <= readpos) else ring - (writepos - readpos); audsrv_queued() the mirror image,
//     so readpos == writepos reads as "available 0, queued 0" whether the ring is empty or full;
//   * audsrv_play_audio writes min(bytes, available).
// The test replays whole scenes (SFX bursts with music off, music end/stop/pause, stalls, every start phase) and judges what the model's SPU2
// would play: bytes read that were not written since their previous read ("stale") must be silence; every written sound byte must be played
// exactly once and in order; the stream must never stop writing (the old queue_cap estimate collapsed to 0/940 at readpos == writepos).
#include "audio_backend_stubs.h"
#include <stdarg.h>
#include <math.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"ring:%d: %s\n",__LINE__,#x); exit(1); } } while (0)
#include "i_sound_host.c"

sfxinfo_t S_sfx[NUMSFX];
uint16_t numwadfiles = 1;
static lumpinfo_t lumps[8];
static wadfile_t wad = { 8, lumps };
static wadfile_t *wadlist[] = { &wad };
wadfile_t **wadfiles = wadlist;
static const uint8_t *lumpdata[8];
static size_t lengths[8];
static precise_t clock_ticks;      // microseconds of simulated time

void CONS_Printf(const char *fmt, ...) { (void)fmt; }
void I_OutputMsg(const char *fmt, ...) { (void)fmt; }
boolean PS2Boot_LoadAudio(void) { return 1; }
precise_t I_GetPreciseTime(void) { return clock_ticks; }
precise_t I_GetPrecisePrecision(void) { return 1000000; }
size_t __real_W_LumpLength(lumpnum_t lump) { return lengths[lump]; }
size_t W_LumpLength(lumpnum_t lump) { return lengths[lump]; }
size_t W_LumpLengthPwad(uint16_t w, uint16_t l) { CHECK(w == 0); return lengths[l]; }
size_t W_ReadLumpHeader(lumpnum_t l, void *dst, size_t n, size_t off)
{ CHECK(l < 8 && off <= lengths[l] && n <= lengths[l]-off); memcpy(dst,lumpdata[l]+off,n); return n; }
void W_ReadLump(lumpnum_t l, void *dst) { W_ReadLumpHeader(l,dst,lengths[l],0); }
const char *W_CheckNameForNum(lumpnum_t l) { return l == 2 ? "O_TEST" : "DS_TEST"; }
lumpnum_t S_GetSfxLumpNum(sfxinfo_t *sfx) { (void)sfx; return 1; }
void *Z_TryMallocAlign(size_t n, int tag, void *user, int alignment) { (void)tag; (void)user; (void)alignment; return malloc(n); }
void Z_Free(void *p) { free(p); }
void *Z_ReallocAlign(void *p, size_t n, int tag, void *user, int alignment) { (void)tag; (void)user; (void)alignment; return realloc(p, n); }
void Z_SetReclaimHook(z_reclaim_fn fn) { (void)fn; }

// ---- the IOP model -------------------------------------------------------------------------------------------------------------
#define RING 9400
#define STEP 940
#define STEP_US (512.0 * 1e6 / 48000.0)
static uint8_t ringbuf[RING];
static uint8_t fresh[RING];                  // written since the last read of this position
static int readpos, writepos, playing, initialized_ring;
static double next_step_us;
static unsigned long stale_bytes, stale_nonzero, overrun_bytes, equality_pumps, full_events, played_nonzero;
static unsigned long writes_total;
static int no_write_check = 1;
// the data the engine wrote, in order, and the data the SPU2 played in order (non-silent bytes only are compared)
static uint8_t *wrote_log, *heard_log;
static size_t wrote_n, heard_n, log_max;
static int stop_on_stale;

static void ModelReset(void)
{
	memset(ringbuf, 0, sizeof ringbuf); memset(fresh, 0, sizeof fresh);
	readpos = 4700; writepos = 0; playing = 0; next_step_us = 0;
}

static void ModelAdvance(double to_us)
{
	while (next_step_us <= to_us)
	{
		if (playing)
		{
			int i;
			for (i = 0; i < STEP; i++)
			{
				int p = (readpos + i) % RING;
				uint8_t v = ringbuf[p];
				if (!fresh[p]) { stale_bytes++; if (v) stale_nonzero++; }
				else played_nonzero += v != 0;
				fresh[p] = 0;
				if (heard_log && heard_n < log_max) heard_log[heard_n++] = v;
			}
			readpos += STEP; if (readpos >= RING) readpos = 0;
			if (stop_on_stale && stale_nonzero) { fprintf(stderr, "stale audio played at %.1f ms\n", next_step_us / 1000.0); exit(1); }
		}
		next_step_us += STEP_US;
	}
}

int audsrv_init(void) { return 0; }
int audsrv_quit(void) { return 0; }
int audsrv_set_format(audsrv_fmt_t *fmt)
{
	CHECK(fmt->freq == 22050 && fmt->bits == 16 && fmt->channels == 2);
	writepos = 0; readpos = (STEP * 5) & ~3;
	return 0;
}
int audsrv_set_volume(int volume) { (void)volume; return 0; }
int audsrv_stop_audio(void) { playing = 0; return 0; }
int audsrv_available(void) { return writepos <= readpos ? readpos - writepos : RING - (writepos - readpos); }
int audsrv_queued(void) { return writepos < readpos ? RING - (readpos - writepos) : writepos - readpos; }
int audsrv_get_error(void) { return AUDSRV_ERR_NOERROR; }
int audsrv_play_audio(const char *buf, int buflen)
{
	int sent = 0;
	CHECK(buflen >= 0);
	if (!playing) playing = 1;
	buflen = buflen < audsrv_available() ? buflen : audsrv_available();
	while (buflen > 0)
	{
		int copy = buflen;
		if (writepos >= readpos) copy = RING - writepos < buflen ? RING - writepos : buflen;
		if (copy > RING - writepos) copy = RING - writepos;
		for (int i = 0; i < copy; i++)
		{
			if (fresh[writepos + i]) overrun_bytes++;
			fresh[writepos + i] = 1;
		}
		memcpy(ringbuf + writepos, buf, copy);
		if (wrote_log && wrote_n + copy <= log_max) memcpy(wrote_log + wrote_n, buf, copy);
		wrote_n += copy;
		buf += copy; buflen -= copy; sent += copy; writepos += copy;
		if (writepos >= RING) writepos = 0;
	}
	if (sent && writepos == readpos) full_events++;       // the ring was filled completely: indistinguishable from empty
	writes_total += sent;
	return sent;
}

// ---- helpers --------------------------------------------------------------------------------------------------------------------
static void LE16(uint8_t *p, unsigned n) { p[0] = (uint8_t)n; p[1] = (uint8_t)(n>>8); }
static void LE32(uint8_t *p, unsigned n) { LE16(p,n); LE16(p+2,n>>16); }
static uint8_t effect[8 + 6000];
static uint8_t wav[44 + 22050 * 2];
static double sim_us;
static unsigned long rng = 12345;
static unsigned Rnd(unsigned n) { rng = rng * 6364136223846793005ull + 1442695040888963407ull; return (unsigned)(rng >> 33) % n; }

// one wake-up of the mixer at the simulated time: the IOP runs up to now, then the engine pumps
static void Wake(double dt_us)
{
	sim_us += dt_us;
	ModelAdvance(sim_us);
	clock_ticks = (precise_t)sim_us;
	I_UpdateSound();
	CHECK(sound_started);
}
static void Run(double ms, int jitter)
{
	double end = sim_us + ms * 1000.0;
	while (sim_us < end) Wake(10000.0 + (jitter ? (double)Rnd(jitter * 1000) - jitter * 500.0 : 0));
}

static void Init(void)
{
	unsigned i;
	ModelReset(); sim_us = 0; clock_ticks = 0;
	stale_bytes = stale_nonzero = overrun_bytes = equality_pumps = full_events = played_nonzero = writes_total = 0;
	wrote_n = heard_n = 0;
	CHECK(!sound_started); I_StartupSound(); CHECK(sound_started);
	CHECK(ring_bytes == RING);
	memset(effect, 0, sizeof effect); LE16(effect, 3); LE16(effect + 2, 22050); LE32(effect + 4, 6000);
	for (i = 0; i < 6000; i++) effect[8 + i] = (i / 20) & 1 ? 230 : 26;       // loud square wave
	lumpdata[1] = effect; lengths[1] = sizeof effect;
	S_sfx[1].name = "sfx"; S_sfx[1].lumpnum = LUMPERROR; S_sfx[1].data = NULL;
	CHECK(I_GetSfx(&S_sfx[1]));
	memset(wav, 0, sizeof wav); memcpy(wav, "RIFF", 4); LE32(wav + 4, sizeof wav - 8);
	memcpy(wav + 8, "WAVEfmt ", 8); LE32(wav + 16, 16); LE16(wav + 20, 1); LE16(wav + 22, 1);
	LE32(wav + 24, 22050); LE32(wav + 28, 44100); LE16(wav + 32, 2); LE16(wav + 34, 16);
	memcpy(wav + 36, "data", 4); LE32(wav + 40, 44100);
	for (i = 44; i < sizeof wav; i += 2) LE16(wav + i, (i / 40) & 1 ? 9000 : (unsigned)(65536 - 9000));
	lumpdata[2] = wav; lengths[2] = sizeof wav;
}

static void Done(void)
{
	I_ShutdownSound();
	CHECK(!sound_started);
}

static void Report(const char *name)
{
	printf("  %-34s heard stale %lu B (non-silent %lu B), overrun %lu B, ring filled completely %lu x, written %lu B, flush blocks %u\n",
		name, stale_bytes, stale_nonzero, overrun_bytes, full_events, writes_total, eng.st.flush_blocks);
}

int main(int argc, char **argv)
{
	int round, h, scene;
	unsigned phase;
	int expect_stale = argc > 1 && !strcmp(argv[1], "--expect-stale");   // negative control: the old pump must be caught
	log_max = 4u << 20; wrote_log = malloc(log_max); heard_log = malloc(log_max); CHECK(wrote_log && heard_log);

	// 1. SFX only, music off: bursts of a loud effect with silence between them (the user's complaint)
	Init();
	for (round = 0; round < 12; round++)
	{
		h = I_StartSound(1, 255, 128, 128, 0, round & 7); CHECK(h >= 0);
		Run(200 + Rnd(500), 0);
	}
	Run(1500, 0);
	Report("sfx bursts, no music");
	if (expect_stale) { CHECK(stale_nonzero > 0); puts("negative control: stale audio detected as required"); return 0; }
	CHECK(stale_nonzero == 0 && overrun_bytes == 0);
	Done();

	// 2. every start phase: an effect starts at each offset of the 940-byte step and each write position after idle
	for (phase = 0; phase < 64; phase++)
	{
		Init();
		Run(300 + phase * 777 % 400, 0);
		for (round = 0; round < 3; round++)
		{
			Run(phase * 2.5, 0);
			h = I_StartSound(1, 200, 128, 128, 0, 0); CHECK(h >= 0);
			Run(250, 0);
			Run(90 + (phase * 37) % 200, 0);
		}
		Run(1200, 0);
		CHECK(stale_nonzero == 0 && overrun_bytes == 0 && full_events == 0);
		CHECK(eng.st.short_writes == 0);
		CHECK(wrote_n > 0);
		Done();
	}
	puts("  64 start phases: no stale audio, no overrun, the ring is never filled completely");

	// 3. the same effects arrive in the order they were mixed, and exactly once: the non-silent bytes written equal the non-silent bytes played
	Init();
	for (round = 0; round < 8; round++) { I_StartSound(1, 255, 40 + round * 20, 128, 0, round); Run(120 + 30 * round, 3); }
	Run(1500, 0);
	{
		size_t i, j = 0, nw = 0, nh = 0, mism = 0;
		static uint8_t a[1 << 22], b[1 << 22];
		for (i = 0; i < wrote_n && i < log_max; i++) if (wrote_log[i]) a[nw++] = wrote_log[i];
		for (i = 0; i < heard_n && i < log_max; i++) if (heard_log[i]) b[nh++] = heard_log[i];
		for (j = 0; j < nw && j < nh; j++) mism += a[j] != b[j];
		printf("  played sound bytes %zu of %zu written, %zu differ\n", nh, nw, mism);
		CHECK(nw == nh && mism == 0 && nw > 100000);
	}
	CHECK(stale_nonzero == 0);
	Done();

	// 4. music: plays, stops (end of track, stopped, paused, volume 0), SFX go on; no replay of music after it
	for (scene = 0; scene < 5; scene++)
	{
		Init();
		ps2_music_lump = 2; CHECK(I_LoadSong(NULL, lengths[2]));
		CHECK(I_PlaySong(scene == 0));                 // scene 0: looping, stopped by the scene; others: plays to the end of the 1 s track
		Run(400, 0);
		if (scene == 0) { I_StopSong(); }
		if (scene == 1) { I_PauseSong(); }
		if (scene == 2) { I_SetMusicVolume(0); }
		Run(300, 0);
		for (round = 0; round < 4; round++) { I_StartSound(1, 255, 128, 128, 0, round); Run(350, 2); }
		if (scene == 1) { I_ResumeSong(); Run(300, 0); }
		Run(1500, 0);
		printf("  music scene %d: stale non-silent %lu B overrun %lu B\n", scene, stale_nonzero, overrun_bytes);
		CHECK(stale_nonzero == 0 && overrun_bytes == 0);
		Done();
	}

	// 5. stalls of the mixer thread shorter than the queue: still no stale audio; the stream recovers from a long one
	Init();
	I_StartSound(1, 255, 128, 128, 0, 0);
	Run(30, 0); Wake(50000); Run(300, 0);                   // 50 ms stall with 4096..9000 queued
	CHECK(stale_nonzero == 0);
	I_StartSound(1, 255, 128, 128, 0, 1);
	Run(100, 0); Wake(300000); Run(300, 0);                 // 300 ms stall: the ring runs dry (stale audio is unavoidable here) ...
	{
		unsigned long before = writes_total;
		I_StartSound(1, 255, 128, 128, 0, 2);
		Run(600, 0);
		CHECK(writes_total > before + 4096);                // ... but the stream resumes and the next sound is written and played
		Run(1500, 0);
		CHECK(eng.st.short_writes == 0);
	}
	Done();

	// 6. the ring exactly empty (readpos == writepos, reported as available 0 / queued 0) when the pump looks: the old pump collapsed its
	//    capacity estimate to 0 or 940 there and never wrote again. 235 trials at different step phases; every trial must resume.
	Init();
	I_StartSound(1, 255, 128, 128, 0, 0); Run(600, 0);             // playing = 1 on the IOP, as in any real session
	for (phase = 0; phase < 235; phase++)
	{
		unsigned long before = writes_total;
		Run(400 + Rnd(500), 0);                               // idle (ring flushed with silence), IOP step phase decorrelated
		for (int i = 0; i < RING; i++) fresh[i] = 0;
		writepos = readpos;                                     // the pointers coincide: available 0, queued 0
		CHECK(audsrv_available() == 0 && audsrv_queued() == 0);
		equality_pumps++;
		I_StartSound(1, 255, 128, 128, 0, (int)(phase & 7));
		I_UpdateSound();                                        // the pump looks at the ambiguous state
		CHECK(ring_bytes == RING && sound_started);
		Run(600, 0);
		if (writes_total <= before + 4096) fprintf(stderr, "phase %u: wrote %lu since, readpos %d writepos %d avail %d output_pending %zu flush_left %u active %d\n", phase, writes_total - before, readpos, writepos, audsrv_available(), output_pending, (unsigned)flush_left, PS2E_Active(&eng));
		CHECK(writes_total > before + 4096);                    // the stream resumed and played the sound
		CHECK(ring_bytes == RING);
	}
	CHECK(eng.st.short_writes == 0);
	Done();
	printf("  %lu pumps at readpos == writepos: the stream always resumes, ring size stays %d\n", equality_pumps, RING);
	// 7. exactly one block of free space (avail 2048): writing it would make writepos == readpos, a ring that looks empty; the pump must leave 4 bytes
	Init();
	I_StartSound(1, 255, 128, 128, 0, 0); Run(600, 0);
	for (phase = 0; phase < 8; phase++)
	{
		unsigned long before = writes_total;
		Run(300 + phase * 37, 0);
		for (int i = 0; i < RING; i++) fresh[i] = 0;
		writepos = (readpos + RING - 2048 - (int)phase * 4) % RING;         // available = 2048, 2044, ... (down to 2020)
		I_StartSound(1, 255, 128, 128, 0, (int)phase);
		I_UpdateSound();
		CHECK(full_events == 0);
		Run(600, 0);
		CHECK(writes_total > before + 4096 && full_events == 0);
	}
	Done();
	puts("  free space of exactly one block: the pump never fills the ring completely");
	puts("Ring: stock-audsrv IOP model (unconditional readpos, 9400 B ring, 940 B steps): no stale audio after sounds/music, sound bytes played once in order, no stall PASS");
	return 0;
}
