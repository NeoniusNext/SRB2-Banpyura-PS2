// SRB2 PS2 audio: PCM mixing and music decoding on their own EE threads, bounded streaming to audsrv/SPU2.
// The game thread only posts commands (ps2_athread.c) and keeps shadow state; the mixer thread (M) refills the
// audsrv queue independently of what the game thread is doing (map loads, slow frames, SFX decoding), the
// music thread (D) decodes the current song ahead into a ring. See docs/GATES/g1/opt2-A.md.
#include "../doomdef.h"
#include "../i_sound.h"
#include "../i_system.h"
#include "../i_time.h"
#include "../s_sound.h"
#include "../w_wad.h"
#include "../z_zone.h"
#include "ps2_boot.h"
#include "ps2_audio.h"
#include "ps2_music.h"
#include "ps2_athread.h"
#include <audsrv.h>
#include <stdlib.h>
#include <string.h>
#ifdef _EE
#include "../m_argv.h"
#include "../command.h"
#include "../g_game.h"
#include "../d_main.h"
#include <kernel.h>
#include <timer.h>
#include <delaythread.h>
#include <malloc.h>
#include <stdio.h>
#endif

#define SFX_BUDGET (2u * 1024 * 1024)
#define SFX_MAX SFX_BUDGET
#define ENCODED_MAX (256u * 1024)
#define QUEUE_LEGACY (PS2_AUDIO_BLOCK * 4 * 4) // 93 ms, stereo 16-bit: the old single-thread target
#define BYTES_PER_MS 88                       // 22050 Hz * 4 bytes / 1000, rounded down
#define MIXER_BLOCKS 8                        // at most this many blocks per mixer wake-up
#define RD_WINDOW (32u * 1024)                // private pack reader: one aligned read
#define MIXER_STACK (12u * 1024)     // measured use 4.4 KiB (ASTAT stack_used)
#define DECODER_STACK (32u * 1024)   // measured use 6.4 KiB with Vorbis

#ifdef _EE
#define A_NOW() ((UINT64)GetTimerSystemTime())
#define A_HZ ((UINT64)kBUSCLK)
#else
#define A_NOW() ((UINT64)I_GetPreciseTime())
#define A_HZ ((UINT64)I_GetPrecisePrecision())
#endif

UINT8 sound_started;
static ps2_engine eng;
static ps2_music *song;
static UINT8 music_volume = 31, internal_volume = 100;
static volatile boolean stream_reported;
static boolean stream_printed;
static boolean threaded;
static int16_t output[PS2_AUDIO_BLOCK*2] __attribute__((aligned(64)));
static size_t output_pending, output_offset;
static ps2e_acct acct;
static UINT32 queue_cap, queue_target, queue_ms_arg, mixer_period_us = 5000;
static volatile int audio_failure, failure_status;
static const char *volatile failure_stage;

// game-thread view of the song (the decoder object belongs to D between Open and Close)
static boolean mus_playing, mus_paused;
static char song_name[16];
static UINT32 mus_epoch, mus_loop_ms;
static struct { UINT32 epoch, ms; } pos_override;
static boolean music_error_reported;

typedef struct cached_sample
{
	ps2_sample sample;
	sfxinfo_t *owner;
	struct cached_sample *next;
	size_t bytes;
	UINT64 used;
	UINT32 start_seq;
	uint8_t data[];
} cached_sample;
static cached_sample *samples;
static size_t sample_bytes;
static UINT64 sample_serial;

// Music from a raw pack lump is read through a private handle on the pack: the pack reader of the engine (w_pack.c) has
// static buffers and a shared FILE, so D must never call into it while the game thread loads.
typedef struct
{
	void *fp;
	uint8_t *buf;
	size_t base, size, win_start, win_len;
} pack_reader;
typedef struct { const uint8_t *memory; lumpnum_t lump; pack_reader *rd; } audio_source;
static audio_source song_source;
static pack_reader *song_reader;
static void *song_owned;
lumpnum_t ps2_music_lump = LUMPERROR; // PS2-70
static lumpnum_t pending_lump = LUMPERROR;
static size_t pending_length;

static boolean fading;
static UINT8 fade_source, fade_target;
static UINT32 fade_ms;
static precise_t fade_last;
static UINT64 fade_elapsed;
static void (*fade_callback)(void);

// ---- diagnostics (PS2 only): -astall <ms> -areload <levelframe> -aqueue <ms> -noathread -ahash -adump <frames> ----
#ifdef _EE
static UINT32 diag_stall_ms, diag_reload_at = 0xffffffffu, diag_quit_at = 0xffffffffu, diag_levelframes;
static boolean diag_reloaded, diag_restarted, diag_parsed, diag_hash_flag, diag_nothread;
static UINT32 diag_restart_at = 0xffffffffu;
static int16_t *dump_buf;
static size_t dump_frames, dump_max;
// -atrace <records>: a journal of every audsrv write and every read of the IOP ring (tools/ps2/audio_ring_sim.py replays it)
typedef struct { UINT32 t_us; UINT16 kind, off; INT32 a, b; } trace_rec;
enum { TR_WRITE = 1, TR_OBS = 2, TR_MARK = 3 };
static trace_rec *trace_buf;
static UINT32 trace_n, trace_max, trace_block;
#endif
static UINT32 stat_last_print_ms;
static UINT32 stat_last_underruns, stat_last_blocks;
static UINT64 stat_t0, main_last;

// The PS2 branch of S_LoadMusic() calls I_LoadSong(NULL, W_LumpLength(mlumpnum)) without passing mlumpnum.
// --wrap=W_LumpLength captures the argument evaluated immediately before I_LoadSong.
size_t __real_W_LumpLength(lumpnum_t lump);
size_t __wrap_W_LumpLength(lumpnum_t lump)
{
	size_t len = __real_W_LumpLength(lump);
	pending_lump = lump; pending_length = len; return len;
}

// ---- private pack reader ----------------------------------------------------------------------------------------

#ifdef _EE
static pack_reader *PackOpen(lumpnum_t lump, const lumpinfo_t *info)
{
	FILE *fp;
	pack_reader *r = calloc(1, sizeof *r);
	if (!r) return NULL;
	fp = fopen(wadfiles[WADFILENUM(lump)]->filename, "rb");
	r->buf = fp ? memalign(64, RD_WINDOW) : NULL;
	if (!fp || !r->buf)
	{
		if (fp) fclose(fp);
		free(r->buf); free(r); return NULL;
	}
	setvbuf(fp, NULL, _IONBF, 0); // whole aligned windows are read straight into r->buf
	r->fp = fp; r->base = (size_t)info->position; r->size = info->size;
	return r;
}

static void PackClose(pack_reader *r)
{
	if (!r) return;
	fclose(r->fp); free(r->buf); free(r);
}

static size_t PackRead(pack_reader *r, size_t off, void *dst, size_t n)
{
	uint8_t *out = dst;
	size_t done = 0;
	if (off >= r->size) return 0;
	if (n > r->size - off) n = r->size - off;
	while (done < n)
	{
		size_t abs = r->base + off + done, chunk;
		if (abs < r->win_start || abs >= r->win_start + r->win_len)
		{
			size_t start = abs & ~(size_t)2047, got;
			if (fseek(r->fp, (long)start, SEEK_SET) != 0) break;
			got = fread(r->buf, 1, RD_WINDOW, r->fp);
			if (got <= abs - start) break;
			r->win_start = start; r->win_len = got;
		}
		chunk = min(n - done, r->win_start + r->win_len - abs);
		memcpy(out + done, r->buf + (abs - r->win_start), chunk);
		done += chunk;
	}
	return done;
}
#else
static pack_reader *PackOpen(lumpnum_t lump, const lumpinfo_t *info) { (void)lump; (void)info; return NULL; }
static void PackClose(pack_reader *r) { (void)r; }
static size_t PackRead(pack_reader *r, size_t off, void *dst, size_t n) { (void)r; (void)off; (void)dst; (void)n; return 0; }
#endif

static size_t ReadSource(void *user, size_t off, void *dst, size_t n)
{
	audio_source *s = user;
	if (s->memory) { memcpy(dst, s->memory + off, n); return n; }
	if (s->rd) return PackRead(s->rd, off, dst, n);
	return W_ReadLumpHeader(s->lump, dst, n, off);
}

// ---- thread plumbing ----------------------------------------------------------------------------------------------

#ifdef _EE
static inline UINT32 CopCount(void) { UINT32 v; __asm__ volatile("mfc0 %0,$9" : "=r"(v)); return v; }
extern void *_gp;
static uint8_t mixer_stack[MIXER_STACK] __attribute__((aligned(16)));
static uint8_t decoder_stack[DECODER_STACK] __attribute__((aligned(16)));
static int mixer_tid = -1, decoder_tid = -1;
static volatile int mixer_exited, decoder_exited;
static int prio_main_orig, prio_main, prio_mixer, prio_decoder;

static void YieldSleep(void) { DelayThread(500); }
static void SleepMs(UINT32 ms) { DelayThread((s32)(ms * 1000)); }
#else
static void YieldSleep(void) {}
#endif

static void Fail(const char *stage, int status)
{
	failure_stage = stage; failure_status = status; audio_failure = 1;
}

static void UpdateGain(void)
{
	eng.music_gain = (unsigned)music_volume * internal_volume * 32768 / (31 * 100);
}

#ifdef _EE
static void Trace(UINT16 kind, UINT16 off, INT32 a, INT32 b)
{
	if (trace_buf && trace_n < trace_max)
	{
		trace_rec *r = &trace_buf[trace_n++];
		r->t_us = (UINT32)((A_NOW() - stat_t0) * 1000000 / A_HZ); r->kind = kind; r->off = off; r->a = a; r->b = b;
	}
}
// Diagnostic read of the IOP ring (extra RPCs, only with -atrace): available and queued as the IOP reports them.
static void TraceObserve(void)
{
	int av, q;
	if (!trace_buf) return;
	av = audsrv_available(); q = audsrv_queued();
	Trace(TR_OBS, 0, av, q);
}
#else
#define Trace(k, o, a, b) ((void)0)
#define TraceObserve() ((void)0)
#endif

// One refill of the audsrv queue: executes commands, renders blocks, sends them. Mixer thread, or the game thread in
// single-thread mode. Never waits for playback space.
static void AudioPump(unsigned maxblocks)
{
	unsigned blocks;
	int queued, sent = 0;
	PS2E_Process(&eng);
	if (!threaded) while (PS2E_SlotsFilled(&eng) < 1 && PS2E_DecodeStep(&eng)) {}
	if (!output_pending && !PS2E_Active(&eng))
	{
		acct.active_prev = 0; // idle: the empty queue that follows is not an underrun
#ifdef _EE
		{
			static UINT32 idle_pumps;
			if (trace_buf && (idle_pumps++ & 3) == 0) TraceObserve(); // idle: sample the ring every 4th wake-up
		}
#endif
		return;
	}
	TraceObserve();
	queued = audsrv_queued();
	if (queued < 0) { Fail("queue query", queued); return; }
	PS2E_AcctBegin(&acct, &eng, A_NOW(), A_HZ, queued);
	for (blocks = 0; blocks < maxblocks; blocks++)
	{
		int space = (int)queue_cap - queued, request, written, status;
		if (!output_pending)
		{
			if (queued >= (int)queue_target || space < (int)sizeof output) break;
			if (!threaded) while (PS2E_SlotsFilled(&eng) < 1 && PS2E_DecodeStep(&eng)) {}
			if (!PS2E_Active(&eng)) break;
#ifdef _EE
			{
				UINT32 c0 = CopCount(), c1;
				PS2E_Render(&eng, output);
				c1 = CopCount() - c0;
				eng.st.mix_cycles_total += c1; eng.st.mix_cycles_blocks++;
				if (c1 > eng.st.mix_cycles_max) eng.st.mix_cycles_max = c1;
			}
#else
			PS2E_Render(&eng, output);
#endif
#ifdef _EE
			if (dump_buf && dump_frames + PS2_AUDIO_BLOCK <= dump_max)
			{ memcpy(dump_buf + dump_frames * 2, output, sizeof output); dump_frames += PS2_AUDIO_BLOCK; }
			trace_block = (UINT32)(eng.st.blocks - 1);
#endif
			output_pending = sizeof output; output_offset = 0;
		}
		request = (int)min(output_pending, (size_t)(space & ~3));
		if (request <= 0) break;
		written = audsrv_play_audio((const char *)output + output_offset, request);
		status = audsrv_get_error();
		Trace(TR_WRITE, (UINT16)output_offset, written, (INT32)trace_block);
		if (written < 0 || written > request || (written & 3) || status != AUDSRV_ERR_NOERROR)
		{ Fail("PCM transfer", status ? status : written); return; }
		// A short or zero write is backpressure, not an RPC failure: keep the unsent suffix, do not render again.
		output_offset += (size_t)written; output_pending -= (size_t)written;
		queued += written; sent += written;
		eng.st.bytes_sent += (UINT32)written;
		if (written < request)
		{
			int available = audsrv_available();
			if (available >= 0) queue_cap = (UINT32)(available + queued); // resynchronise the capacity estimate
			if (!written) break;
		}
	}
	PS2E_AcctEnd(&acct, A_NOW(), A_HZ, sent, output_pending != 0 || PS2E_Active(&eng));
	if (sent) stream_reported = true;
}

#ifdef _EE
static void MixerThread(void *arg)
{
	(void)arg;
	while (!eng.quit)
	{
		AudioPump(MIXER_BLOCKS);
		DelayThread((s32)mixer_period_us);
	}
	mixer_exited = 1;
	ExitThread();
}

static void DecoderThread(void *arg)
{
	(void)arg;
	while (!eng.quit)
	{
		UINT64 t0 = GetTimerSystemTime();
		int work = PS2E_DecodeStep(&eng);
		UINT32 us = (UINT32)((GetTimerSystemTime() - t0) * 1000000 / kBUSCLK);
		if (us > eng.st.dec_max_us) eng.st.dec_max_us = us;
		if (!work) DelayThread(4000);
		else if (PS2E_SlotsFilled(&eng) >= 6) DelayThread(600); // leave the game thread some time between blocks
	}
	decoder_exited = 1;
	ExitThread();
}

static int SpawnThread(void (*fn)(void *), void *stack, int size, int priority)
{
	ee_thread_t t;
	int tid;
	memset(stack, 0xA5, (size_t)size); // high-water mark
	memset(&t, 0, sizeof t);
	t.func = (void *)fn; t.stack = stack; t.stack_size = size; t.gp_reg = &_gp;
	t.initial_priority = priority; t.attr = 0; t.option = 0;
	tid = CreateThread(&t);
	if (tid < 0) return tid;
	if (StartThread(tid, NULL) < 0) { DeleteThread(tid); return -1; }
	return tid;
}

static boolean StartThreads(void)
{
	ee_thread_status_t ms;
	if (ReferThreadStatus(GetThreadId(), &ms) < 0) return false;
	prio_main = prio_main_orig = ms.current_priority;
	// Lower number = higher priority. Keep the mixer above the decoder, both above the game thread.
	if (prio_main < 4) { ChangeThreadPriority(GetThreadId(), 8); prio_main = 8; }
	prio_mixer = prio_main - 3; prio_decoder = prio_main - 1;
	eng.quit = 0; mixer_exited = decoder_exited = 0;
	mixer_tid = SpawnThread(MixerThread, mixer_stack, sizeof mixer_stack, prio_mixer);
	if (mixer_tid < 0) return false;
	decoder_tid = SpawnThread(DecoderThread, decoder_stack, sizeof decoder_stack, prio_decoder);
	if (decoder_tid < 0)
	{
		eng.quit = 1;
		TerminateThread(mixer_tid); DeleteThread(mixer_tid); mixer_tid = -1;
		return false;
	}
	return true;
}

static size_t StackUsed(const uint8_t *stack, size_t size)
{
	size_t i = 0;
	while (i < size && stack[i] == 0xA5) i++;
	return size - i;
}

static void StopThreads(void)
{
	int waited;
	eng.quit = 1;
	for (waited = 0; waited < 400 && !(mixer_exited && decoder_exited); waited++) SleepMs(5);
	if (mixer_tid >= 0) { TerminateThread(mixer_tid); DeleteThread(mixer_tid); }
	if (decoder_tid >= 0) { TerminateThread(decoder_tid); DeleteThread(decoder_tid); }
	mixer_tid = decoder_tid = -1;
}
#endif

// Wait (bounded) until M has executed command `seq`. Single-thread mode runs the command here.
static boolean WaitConsumed(uint32_t seq)
{
#ifdef _EE
	if (threaded)
	{
		int i;
		for (i = 0; i < 2000 && !PS2E_Consumed(&eng, seq); i++) SleepMs(1);
		return PS2E_Consumed(&eng, seq);
	}
#endif
	PS2E_Process(&eng);
	return PS2E_Consumed(&eng, seq);
}

static boolean WaitClosed(void)
{
#ifdef _EE
	if (threaded)
	{
		int i;
		for (i = 0; i < 4000 && !PS2E_MusicClosed(&eng); i++) SleepMs(1);
		return PS2E_MusicClosed(&eng);
	}
#endif
	while (!PS2E_MusicClosed(&eng) && PS2E_DecodeStep(&eng)) {}
	return PS2E_MusicClosed(&eng);
}

// ---- statistics --------------------------------------------------------------------------------------------------

static void StatLine(const char *tag)
{
	const ps2e_stats *s = &eng.st;
	I_OutputMsg("ASTAT %s mode=%s cap=%u target=%u underruns=%u emptyobs=%u gapmax_ms=%u gaptotal_ms=%u pumps=%u "
		"maxint_ms=%u minq_ms=%d blocks=%u sent=%u mstarve=%u hmis=%u decslots=%u decmax_us=%u cmdfull=%u dcmdfull=%u "
		"fdec=%u fcons=%u hdec=%08x hcons=%08x herr=%u maingap_ms=%u gaps100=%u mixavg_cyc=%u mixmax_cyc=%u\n",
		tag, threaded ? "thread" : "single", (unsigned)queue_cap, (unsigned)queue_target, s->underruns, s->empty_obs,
		s->gap_max_ms, s->gap_total_ms, s->pumps, s->max_interval_ms, s->min_queue_ms == 0xffffffffu ? -1 : (int)s->min_queue_ms,
		s->blocks, s->bytes_sent, s->music_starved, s->handle_mismatch, s->dec_slots, s->dec_max_us, s->cmd_full,
		s->dcmd_full, s->music_frames_dec, s->music_frames_cons, s->music_hash_dec, s->music_hash_cons, s->hash_errors, s->main_gap_max_ms,
		s->main_gaps_over_100ms, s->mix_cycles_blocks ? (unsigned)(s->mix_cycles_total / s->mix_cycles_blocks) : 0u, s->mix_cycles_max);
#ifdef _EE
	if (threaded)
		I_OutputMsg("ASTAT %s threads prio main=%d (was %d) mixer=%d decoder=%d stack_used mixer=%u/%u decoder=%u/%u\n", tag,
			prio_main, prio_main_orig, prio_mixer, prio_decoder, (unsigned)StackUsed(mixer_stack, sizeof mixer_stack), (unsigned)sizeof mixer_stack,
			(unsigned)StackUsed(decoder_stack, sizeof decoder_stack), (unsigned)sizeof decoder_stack);
#endif
}

static void StatPoll(void)
{
	UINT32 now_ms = (UINT32)((A_NOW() - stat_t0) * 1000 / A_HZ);
	if (eng.st.underruns != stat_last_underruns && now_ms - stat_last_print_ms >= 1000)
	{
		stat_last_underruns = eng.st.underruns; stat_last_print_ms = now_ms;
		StatLine("evt");
	}
	if (now_ms - stat_last_print_ms >= 2000 && eng.st.blocks != stat_last_blocks)
	{
		stat_last_print_ms = now_ms; stat_last_blocks = eng.st.blocks;
		StatLine("tick");
	}
}

// ---- sound effects -----------------------------------------------------------------------------------------------

static void ReleaseSample(cached_sample *s)
{
	cached_sample **link;
	boolean freed = true;
	if (sound_started && PS2E_SampleBusy(&eng, &s->sample, s->start_seq))
	{
		// a voice plays it, or a start of it is still queued: have M drop it, and wait until it did
		uint32_t seq = PS2E_Forget(&eng, &s->sample);
		freed = seq && WaitConsumed(seq);
	}
	for (link = &samples; *link && *link != s; link = &(*link)->next) {}
	if (*link) *link = s->next;
	if (s->owner->data == s) s->owner->data = NULL;
	sample_bytes -= s->bytes;
	if (freed) Z_Free(s);
	else CONS_Printf("PS2 audio: mixer did not release an effect in time; block leaked\n");
}

static boolean MakeSampleRoom(size_t bytes)
{
	cached_sample *s, *oldest;
	boolean waited = false;
	if (bytes > SFX_BUDGET) return false;
	while (sample_bytes + bytes > SFX_BUDGET)
	{
		oldest = NULL;
		for (s = samples; s; s = s->next)
			if (!PS2E_SampleBusy(&eng, &s->sample, s->start_seq) && (!oldest || s->used < oldest->used)) oldest = s;
		if (!oldest)
		{
			// Everything looks busy; part of it may only be commands M has not executed yet (a stop just issued).
			if (waited || !sound_started) return false;
			waited = true;
			WaitConsumed(eng.cmd_head);
			continue;
		}
		ReleaseSample(oldest);
	}
	return true;
}

// PS2-71: called by the zone (z_zone.c, game thread) when an allocation does not fit and no cache is left to evict: the effects cache is
// the largest block of memory the engine can rebuild on demand (the next I_GetSfx decodes the effect again), so idle samples go, least
// recently used first, until `want` bytes are free. Samples a voice plays, with a start still queued, or the one loaded last (its caller has not started it yet) stay.
static size_t ReclaimSamples(size_t want)
{
	size_t freed = 0;
	while (freed < want)
	{
		cached_sample *s, *oldest = NULL;
		for (s = samples; s; s = s->next)
			if (s->used < sample_serial && !PS2E_SampleBusy(&eng, &s->sample, s->start_seq) && (!oldest || s->used < oldest->used)) oldest = s;
		if (!oldest) break;
		freed += oldest->bytes + sizeof *oldest;
		ReleaseSample(oldest);
	}
	return freed;
}

static cached_sample *DecodeEffect(const ps2_audio_input *in)
{
	ps2_music *decoder = PS2_MusicOpen(in);
	cached_sample *s;
	size_t used = 0, capacity, n, i;
	UINT32 length;
	unsigned rate = PS2_AUDIO_RATE, stride = 1;
	// PS2-71: a mono source is cached as mono (2 bytes per frame instead of 4): the mixer copies a mono sample to both channels,
	// so the output is the same bytes as for the duplicated stereo frames the decoder produces.
	const unsigned channels = decoder && PS2_MusicChannels(decoder) == 1 ? 1 : 2, bpf = channels * 2;
	int16_t block[PS2_AUDIO_BLOCK*2];
	if (!decoder) return NULL;
	if (PS2_MusicType(decoder) != PS2_MUSIC_OGG && PS2_MusicType(decoder) != PS2_MUSIC_MP3)
	{ PS2_MusicClose(decoder); return NULL; }
	length = PS2_MusicLength(decoder);
	// Long effects are cached at half rate, preserving the channels. The longest
	// stock 41.8s effect fits in 2 MiB this way. Never allocate beyond the cap.
	if (length > SFX_MAX * 1000ull / (PS2_AUDIO_RATE * bpf)) { rate /= 2; stride = 2; }
	capacity = length ? (size_t)(((UINT64)length * rate / 1000 + PS2_AUDIO_BLOCK) * bpf) : SFX_MAX;
	if (capacity > SFX_MAX) capacity = SFX_MAX;
	if (!MakeSampleRoom(capacity)) { PS2_MusicClose(decoder); return NULL; }
	s = Z_TryMallocAlign(sizeof *s + capacity, PU_SOUND, NULL, 6);
	if (!s || !PS2_MusicPlay(decoder, 0)) { Z_Free(s); PS2_MusicClose(decoder); return NULL; }
	while ((n = PS2_MusicRender(decoder, block, PS2_AUDIO_BLOCK)) != 0)
	{
		for (i = 0; i < n; i += stride)
		{
			if (used + bpf > capacity) { Z_Free(s); PS2_MusicClose(decoder); return NULL; }
			// Write explicitly little-endian; descriptor stays portable.
			s->data[used++] = (uint8_t)block[i*2]; s->data[used++] = (uint8_t)((uint16_t)block[i*2] >> 8);
			if (channels == 2) { s->data[used++] = (uint8_t)block[i*2+1]; s->data[used++] = (uint8_t)((uint16_t)block[i*2+1] >> 8); }
		}
	}
	if (!used || PS2_MusicError(decoder)) { Z_Free(s); s = NULL; }
	PS2_MusicClose(decoder);
	if (s)
	{
		// PS2-71: the estimate from the stream length leaves a tail (and a block of slack): give it back (in place, the block keeps its address)
		if (used < capacity) s = Z_ReallocAlign(s, sizeof *s + used, PU_SOUND, NULL, 6);
		s->sample.bytes = s->data;
		s->sample.pcm.offset = 0; s->sample.pcm.frames = (uint32_t)(used/bpf);
		s->sample.pcm.rate = rate; s->sample.pcm.bits = 16; s->sample.pcm.channels = channels;
		s->bytes = used;
	}
	return s;
}

void *I_GetSfx(sfxinfo_t *sfx)
{
	cached_sample *s;
	audio_source source;
	ps2_audio_input in;
	size_t len;
	if (!sound_started || !sfx) return NULL;
	if (sfx->data) return sfx->data;
	if (sfx->lumpnum == LUMPERROR) sfx->lumpnum = S_GetSfxLumpNum(sfx);
	if (sfx->lumpnum == LUMPERROR) return NULL;
	len = W_LumpLengthPwad(WADFILENUM(sfx->lumpnum), LUMPNUM(sfx->lumpnum));
	if (!len || len > SFX_MAX) return NULL;
	if (!MakeSampleRoom(len)) return NULL;
	s = Z_TryMallocAlign(sizeof *s + len, PU_SOUND, NULL, 6); if (!s) return NULL;
	s->start_seq = 0;
	source.memory = NULL; source.lump = sfx->lumpnum; source.rd = NULL;
	in.user = &source; in.size = len; in.read_at = ReadSource;
	// Read once: LZ4 samples are decoded once by WPack, never during mixing.
	if (PS2_AudioRead(&in, 0, s->data, len) != len) { Z_Free(s); return NULL; }
	source.memory = s->data;
	if (!PS2_ParsePCM(&in, &s->sample.pcm, 1))
	{
		cached_sample *decoded = DecodeEffect(&in);
		Z_Free(s); s = decoded;
		if (!s)
		{ CONS_Printf("PS2 audio: SFX %s invalid, unsupported or exceeds cache budget\n", sfx->name); return NULL; }
		s->start_seq = 0;
	}
	else { s->sample.bytes = s->data; s->bytes = len; }
	s->owner = sfx; s->used = ++sample_serial;
	s->next = samples; samples = s; sample_bytes += s->bytes;
	sfx->length = len; sfx->data = s; return s;
}

void I_FreeSfx(sfxinfo_t *sfx)
{
	if (sfx && sfx->data) ReleaseSample(sfx->data);
	if (sfx) { sfx->data = NULL; sfx->length = 0; sfx->lumpnum = LUMPERROR; }
}

// ---- start-up / shutdown -----------------------------------------------------------------------------------------

#ifdef _EE
// Diagnostic (-aprobe): how much PCM does the audsrv queue take, and how fast does it drain?
static void ProbeQueue(void)
{
	static int16_t silence[512] __attribute__((aligned(64)));
	int total = 0, i, w;
	I_OutputMsg("ASTAT probe avail0=%d queued0=%d\n", audsrv_available(), audsrv_queued());
	for (i = 0; i < 400; i++)
	{
		w = audsrv_play_audio((const char *)silence, (int)sizeof silence);
		if (w <= 0) break;
		total += w;
		if (w < (int)sizeof silence) break;
	}
	I_OutputMsg("ASTAT probe accepted=%d avail=%d queued=%d\n", total, audsrv_available(), audsrv_queued());
	for (i = 0; i < 24; i++)
	{
		DelayThread(25000);
		I_OutputMsg("ASTAT probe t=%dms avail=%d queued=%d\n", (i + 1) * 25, audsrv_available(), audsrv_queued());
	}
}
#endif

void I_StartupSound(void)
{
	audsrv_fmt_t fmt = { PS2_AUDIO_RATE, 16, 2 };
	int available, queued0;
	boolean want_thread = true;
	if (sound_started) return;
	CONS_Printf("PS2 audio: loading IOP sound modules\n");
	if (!PS2Boot_LoadAudio()) { CONS_Printf("PS2 audio: IOP modules unavailable\n"); return; }
	CONS_Printf("PS2 audio: initializing audsrv RPC\n");
	if (audsrv_init() != 0) { CONS_Printf("PS2 audio: audsrv_init failed\n"); return; }
	CONS_Printf("PS2 audio: configuring output\n");
	if (audsrv_set_format(&fmt) != 0 || audsrv_set_volume(100) != 0)
	{ audsrv_quit(); CONS_Printf("PS2 audio: output setup failed\n"); return; }
#ifdef _EE
	if (M_CheckParm("-aprobe")) ProbeQueue();
#endif
	available = audsrv_available(); queued0 = audsrv_queued();
	// The IOP ring of the stock audsrv.irx is 9400 bytes (106 ms) and half of it is pre-filled with silence at start, so
	// available + queued is the capacity. It cannot be enlarged from the EE: the only protection against a blocked game
	// thread is a mixer thread that always runs, never a deeper queue.
	if (available < 0 || queued0 < 0 || available + queued0 < (int)(PS2_AUDIO_BLOCK * 4 * 4))
	{ audsrv_quit(); CONS_Printf("PS2 audio: queue too small (%d + %d)\n", available, queued0); return; }
	PS2E_Init(&eng); PS2E_YieldHook = YieldSleep;
	memset(&acct, 0, sizeof acct);
	queue_cap = (UINT32)(available + queued0);
	stream_reported = stream_printed = false; audio_failure = 0; output_pending = output_offset = 0;
	main_last = 0; stat_t0 = A_NOW(); stat_last_print_ms = 0; stat_last_underruns = 0; stat_last_blocks = 0;
	UpdateGain();
	queue_target = QUEUE_LEGACY; mixer_period_us = 10000;
#ifdef _EE
	if (!diag_parsed)
	{
		diag_parsed = true;
		diag_hash_flag = M_CheckParm("-ahash") != 0;
		diag_nothread = M_CheckParm("-noathread") != 0;
		if (M_CheckParm("-aqueue") && M_IsNextParm()) queue_ms_arg = (UINT32)atoi(M_GetNextParm());
		if (M_CheckParm("-astall") && M_IsNextParm()) diag_stall_ms = (UINT32)atoi(M_GetNextParm());
		if (M_CheckParm("-areload") && M_IsNextParm()) diag_reload_at = (UINT32)atoi(M_GetNextParm());
		if (M_CheckParm("-aquit") && M_IsNextParm()) diag_quit_at = (UINT32)atoi(M_GetNextParm());
		if (M_CheckParm("-arestart") && M_IsNextParm()) diag_restart_at = (UINT32)atoi(M_GetNextParm());
		if (M_CheckParm("-adump") && M_IsNextParm())
		{
			dump_max = (size_t)atoi(M_GetNextParm());
			dump_buf = dump_max ? malloc(dump_max * 4) : NULL; dump_frames = 0;
		}
		if (M_CheckParm("-atrace") && M_IsNextParm())
		{
			trace_max = (UINT32)atoi(M_GetNextParm());
			trace_buf = trace_max ? malloc((size_t)trace_max * sizeof(trace_rec)) : NULL; trace_n = 0;
			if (!trace_buf) trace_max = 0;
		}
	}
	if (diag_nothread) want_thread = false;
	eng.diag_hash = diag_hash_flag;
	if (queue_ms_arg) queue_target = queue_ms_arg * BYTES_PER_MS;   // experiments only
	if (queue_target > queue_cap) queue_target = queue_cap;
	threaded = false;
	if (want_thread)
	{
		mixer_period_us = queue_target / BYTES_PER_MS / 4 * 1000;   // wake up at least four times per queue length
		if (mixer_period_us < 2000) mixer_period_us = 2000;
		if (mixer_period_us > 10000) mixer_period_us = 10000;
		threaded = StartThreads();
		if (!threaded) CONS_Printf("PS2 audio: cannot start threads; single-thread mixing\n");
	}
#else
	(void)want_thread;
	threaded = false;
	if (queue_target > queue_cap) queue_target = queue_cap;
#endif
	sound_started = 1;
	Z_SetReclaimHook(ReclaimSamples);
	CONS_Printf("PS2 audio: %d Hz stereo, %d SFX channels, %s, audsrv queue %u bytes, target %u bytes\n", PS2_AUDIO_RATE,
		PS2_AUDIO_CHANNELS, threaded ? "mixer and decoder threads" : "single-thread mixing", (unsigned)queue_cap, (unsigned)queue_target);
}

void I_ShutdownSound(void)
{
	I_UnloadSong();
	if (sound_started) StatLine("final");
#ifdef _EE
	if (threaded) StopThreads();
	threaded = false;
	if (dump_buf)
	{
		char path[300];
		FILE *f;
		snprintf(path, sizeof path, "%s/apcm.raw", srb2home);
		f = fopen(path, "wb");
		if (f) { fwrite(dump_buf, 4, dump_frames, f); fclose(f); I_OutputMsg("ASTAT dump %u frames -> %s\n", (unsigned)dump_frames, path); }
		free(dump_buf); dump_buf = NULL; dump_frames = dump_max = 0;
	}
	if (trace_buf)
	{
		char path[300];
		FILE *f;
		snprintf(path, sizeof path, "%s/atrace.bin", srb2home);
		f = fopen(path, "wb");
		if (f) { fwrite(trace_buf, sizeof(trace_rec), trace_n, f); fclose(f); I_OutputMsg("ASTAT trace %u records -> %s\n", (unsigned)trace_n, path); }
		free(trace_buf); trace_buf = NULL; trace_n = trace_max = 0;
	}
#endif
	Z_SetReclaimHook(NULL);
	while (samples) ReleaseSample(samples);
	if (sound_started) { audsrv_stop_audio(); audsrv_quit(); }
	sound_started = 0; output_pending = output_offset = 0; PS2E_Init(&eng);
}

INT32 I_StartSound(sfxenum_t id, UINT8 vol, UINT8 sep, UINT8 pitch, UINT8 priority, INT32 channel)
{
	cached_sample *s;
	(void)priority; // SRB2 chooses/steals channels before calling the driver.
	if (!sound_started || id <= sfx_None || id >= (unsigned)LIMIT_NUMSFX) return -1;
	s = S_sfx[id].data;
	if (!s) return -1;
	s->used = ++sample_serial;
	return PS2E_Start(&eng, &s->sample, channel, vol, sep, pitch, &s->start_seq);
}
void I_StopSound(INT32 handle) { PS2E_Stop(&eng, handle); }
boolean I_SoundIsPlaying(INT32 handle) { return PS2E_Playing(&eng, handle) != 0; }
void I_UpdateSoundParams(INT32 h, UINT8 vol, UINT8 sep, UINT8 pitch) { PS2E_Params(&eng, h, vol, sep, pitch); }
void I_SetSfxVolume(UINT8 volume) { eng.sfx_volume = min(volume, 31); }

// ---- music fades (game thread) ----------------------------------------------------------------------------------------

static void SetInternalVolume(UINT8 v) { internal_volume = v; UpdateGain(); }

void I_StopFadingSong(void) { fading = false; fade_callback = NULL; fade_elapsed = 0; }
static void FadeTick(void)
{
	precise_t now = I_GetPreciseTime();
	UINT64 duration;
	void (*callback)(void);
	if (!fading) return;
	if (!I_SongPaused()) fade_elapsed += now - fade_last;
	fade_last = now;
	duration = (UINT64)fade_ms * I_GetPrecisePrecision() / 1000;
	if (fade_elapsed >= duration)
	{
		SetInternalVolume(fade_target); callback = fade_callback;
		I_StopFadingSong(); // callback may unload, play or start another fade
		if (callback) callback();
	}
	else SetInternalVolume((UINT8)((INT32)fade_source +
			((INT64)fade_target - fade_source) * (INT64)fade_elapsed / (INT64)duration));
}

#ifdef _EE
// Diagnostic load generators, run from the frame loop exactly where the old mixer ran.
static void DiagFrame(void)
{
	if (gamestate != GS_LEVEL) return;
	diag_levelframes++;
	if (diag_levelframes >= diag_quit_at) { StatLine("end"); I_Quit(); }
	if (!diag_restarted && diag_levelframes >= diag_restart_at)
	{
		diag_restarted = true;
		I_OutputMsg("ASTAT restartaudio requested at levelframe %u\n", diag_levelframes);
		COM_BufAddText("restartaudio\n");
	}
	// -acmd <levelframe> <console command> (repeatable, up to 8): scripted console commands (stopmusic, tunes, pause, digmusicvolume ...)
	{
		static boolean acmd_done[8];
		int i, n = 0;
		for (i = 1; i + 2 < myargc && n < 8; i++)
			if (!strcmp(myargv[i], "-acmd"))
			{
				if (!acmd_done[n] && diag_levelframes >= (UINT32)atoi(myargv[i + 1]))
				{
					acmd_done[n] = true;
					I_OutputMsg("ASTAT acmd at levelframe %u: %s\n", diag_levelframes, myargv[i + 2]);
					COM_BufAddText(va("%s\n", myargv[i + 2]));
				}
				n++; i += 2;
			}
	}
	if (!diag_reloaded && diag_levelframes >= diag_reload_at)
	{
		diag_reloaded = true;
		I_OutputMsg("ASTAT reload requested at levelframe %u\n", diag_levelframes);
		COM_BufAddText("map MAP02 -force\n");
	}
	if (diag_stall_ms)
	{
		UINT64 until = GetTimerSystemTime() + (UINT64)diag_stall_ms * (kBUSCLK / 1000);
		while ((INT64)(until - GetTimerSystemTime()) > 0) {}
	}
}
#endif

void I_UpdateSound(void)
{
	FadeTick();
	if (!sound_started) return;
	{
		// how long the game thread stayed away since the previous call (map loads, slow frames, the -astall generator)
		UINT64 now = A_NOW();
		if (main_last)
		{
			UINT32 gap = (UINT32)((now - main_last) * 1000 / A_HZ);
			if (gap > eng.st.main_gap_max_ms) eng.st.main_gap_max_ms = gap;
			if (gap > 100) eng.st.main_gaps_over_100ms++;
		}
		main_last = now;
	}
#ifdef _EE
	DiagFrame();
#endif
	if (!threaded && !audio_failure)
	{
		while (PS2E_SlotsFilled(&eng) < 2 && PS2E_DecodeStep(&eng)) {} // keep the decoder one block ahead
		AudioPump(4);
	}
	if (audio_failure)
	{
		CONS_Printf("PS2 audio: %s failed (status=%d); audio stopped\n", failure_stage ? failure_stage : "?", failure_status);
		I_ShutdownSound();
		return;
	}
	if (eng.music_err_epoch == mus_epoch && mus_epoch && !music_error_reported)
	{ CONS_Printf("PS2 audio: music decoder error; playback stopped\n"); music_error_reported = true; }
	if (stream_reported && !stream_printed) { stream_printed = true; I_OutputMsg("PS2 audio: PCM stream active\n"); }
	StatPoll();
}

// ---- music (game thread side) -------------------------------------------------------------------------------------

void I_InitMusic(void) { if (!sound_started) I_StartupSound(); }
void I_ShutdownMusic(void) { I_UnloadSong(); }
musictype_t I_SongType(void)
{
	switch (PS2_MusicType(song))
	{
		case PS2_MUSIC_WAV: return MU_WAV;
		case PS2_MUSIC_OGG: return MU_OGG;
		case PS2_MUSIC_MP3: return MU_MP3;
		case PS2_MUSIC_MIDI: return MU_MID;
		default: return MU_NONE;
	}
}
boolean I_SongPlaying(void) { return song && PS2E_MusicPlaying(&eng) && mus_playing; }
boolean I_SongPaused(void) { return song && mus_paused; }
boolean I_SetSongSpeed(float speed)
{
	UINT32 position, epoch;
	if (!song || !(speed >= 0.25f && speed <= 4.0f)) return false;
	position = I_GetSongPosition();
	epoch = PS2E_MusicSpeed(&eng, speed, position);
	if (!epoch) return false;
	mus_epoch = epoch; pos_override.epoch = epoch; pos_override.ms = position;
	return true;
}
UINT32 I_GetSongLength(void) { return PS2_MusicLength(song); }
boolean I_SetSongLoopPoint(UINT32 ms)
{
	UINT32 length = PS2_MusicLength(song);
	if (!song || (length && ms >= length)) return false;
	mus_loop_ms = ms; PS2E_MusicLoopPoint(&eng, ms); return true;
}
UINT32 I_GetSongLoopPoint(void) { return song ? mus_loop_ms : 0; }
boolean I_SetSongPosition(UINT32 ms)
{
	UINT32 length = PS2_MusicLength(song), loop = song ? mus_loop_ms : 0, epoch;
	if (!song) return false;
	if (length && ms >= length && loop < length) ms = loop + (ms - loop) % (length - loop);
	epoch = PS2E_MusicSeek(&eng, ms);
	if (!epoch) return false;
	mus_epoch = epoch; pos_override.epoch = epoch; pos_override.ms = ms;
	return true;
}
UINT32 I_GetSongPosition(void)
{
	if (!song) return 0;
	if (eng.music_pos_epoch == mus_epoch) return eng.music_pos_ms;
	return pos_override.epoch == mus_epoch ? pos_override.ms : 0;
}

boolean I_LoadSong(char *data, size_t len)
{
	lumpnum_t lump = ps2_music_lump; // PS2-70: set by S_LoadMusic (the --wrap capture below does not work inside the LTO-merged engine object)
	size_t captured_length = lump == LUMPERROR ? 0 : W_LumpLength(lump);
	ps2_audio_input in;
	ps2_music_lump = LUMPERROR; pending_lump = LUMPERROR; pending_length = 0;
	I_UnloadSong();
	if (!sound_started || !len) return false;
	song_source.memory = (const uint8_t *)data; song_source.lump = lump; song_source.rd = NULL; song_name[0] = 0;
	if (!data)
	{
		const char *name;
		lumpinfo_t *info;
		if (lump == LUMPERROR || captured_length != len || WADFILENUM(lump) >= numwadfiles
			|| LUMPNUM(lump) >= wadfiles[WADFILENUM(lump)]->numlumps) return false;
		name = W_CheckNameForNum(lump);
		if (!name || (strncasecmp(name, "O_", 2) && strncasecmp(name, "D_", 2))) return false;
		strncpy(song_name, name, sizeof song_name - 1); song_name[sizeof song_name - 1] = 0;
		info = &wadfiles[WADFILENUM(lump)]->lumpinfo[LUMPNUM(lump)];
		if (info->compression != CM_NOCOMPRESSION)
		{
			// WPack decompresses entire compressed lumps for partial reads. Bound
			// that cost once; large tracks must be stored raw in the cooked pack.
			if (len > ENCODED_MAX)
			{ CONS_Printf("PS2 audio: compressed music exceeds 256 KiB; cook music as raw lumps\n"); return false; }
			song_owned = Z_TryMallocAlign(len, PU_MUSIC, NULL, 6); if (!song_owned) return false;
			W_ReadLump(lump, song_owned); song_source.memory = song_owned;
		}
		else
		{
			song_reader = PackOpen(lump, info);
			if (song_reader) song_source.rd = song_reader;
			else if (threaded)
			{ CONS_Printf("PS2 audio: cannot open a private handle for the music pack\n"); return false; }
		}
	}
	in.user = &song_source; in.size = len; in.read_at = ReadSource;
	song = PS2_MusicOpen(&in);
	if (!song)
	{
		CONS_Printf("PS2 audio: invalid or unsupported music stream\n");
		Z_Free(song_owned); song_owned = NULL; PackClose(song_reader); song_reader = NULL; return false;
	}
	mus_loop_ms = PS2_MusicLoop(song); mus_playing = mus_paused = false; music_error_reported = false;
	PS2E_MusicOpen(&eng, song);
	mus_epoch = eng.want_epoch; pos_override.epoch = mus_epoch; pos_override.ms = 0;
	return true;
}
void I_UnloadSong(void)
{
	if (song)
	{
		boolean released = true;
		if (sound_started)
		{
			PS2E_MusicClose(&eng);
			released = WaitClosed();
			mus_epoch = eng.want_epoch;
		}
		if (released) { PS2_MusicClose(song); Z_Free(song_owned); PackClose(song_reader); }
		else CONS_Printf("PS2 audio: decoder thread did not release the song in time; song leaked\n");
	}
	else { Z_Free(song_owned); PackClose(song_reader); }
	song = NULL; song_owned = NULL; song_reader = NULL;
	memset(&song_source, 0, sizeof song_source);
	mus_playing = mus_paused = false; music_error_reported = false;
	I_StopFadingSong(); SetInternalVolume(100);
}
boolean I_PlaySong(boolean loop)
{
	if (!sound_started || !song) return false;
	mus_epoch = PS2E_MusicPlay(&eng, loop ? 1 : 0);
	mus_playing = true; mus_paused = false;
	pos_override.epoch = mus_epoch; pos_override.ms = 0;
	I_OutputMsg("PS2 audio: music playback type=%d looping=%d name=%s\n", (int)I_SongType(), (int)loop, song_name);
	return true;
}
void I_StopSong(void)
{
	if (song)
	{
		mus_epoch = PS2E_MusicStop(&eng);
		pos_override.epoch = mus_epoch; pos_override.ms = 0;
	}
	mus_playing = mus_paused = false;
	I_StopFadingSong(); SetInternalVolume(100);
}
void I_PauseSong(void)
{
	FadeTick();
	if (song && I_SongPlaying()) { mus_paused = true; eng.music_paused = 1; }
}
void I_ResumeSong(void)
{
	FadeTick();
	if (song && mus_paused) { mus_paused = false; eng.music_paused = 0; }
}
void I_SetMusicVolume(UINT8 volume) { music_volume = min(volume, 31); UpdateGain(); }
boolean I_SetSongTrack(INT32 track) { return song && track == 0; }
void I_SetInternalMusicVolume(UINT8 volume) { SetInternalVolume(min(volume, 100)); }

boolean I_FadeSongFromVolume(UINT8 target, UINT8 source, UINT32 ms, void (*callback)(void))
{
	I_StopFadingSong();
	target = min(target, 100); source = min(source, 100);
	SetInternalVolume(source);
	if (!ms || target == source)
	{
		SetInternalVolume(target); if (callback) callback(); return true;
	}
	fading = true; fade_source = source; fade_target = target; fade_ms = ms;
	fade_last = I_GetPreciseTime(); fade_callback = callback; return true;
}
boolean I_FadeSong(UINT8 target, UINT32 ms, void (*callback)(void))
{ return I_FadeSongFromVolume(target, internal_volume, ms, callback); }
boolean I_FadeOutStopSong(UINT32 ms) { return I_FadeSong(0, ms, I_StopSong); }
boolean I_FadeInPlaySong(UINT32 ms, boolean loop)
{ return I_PlaySong(loop) && I_FadeSongFromVolume(100, 0, ms, NULL); }
