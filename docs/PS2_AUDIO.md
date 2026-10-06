# PS2 audio backend

`src/ps2/i_sound.c` now submits actual signed PCM through audsrv to SPU2.
Playback state follows real mixer cursors and decoder results. Invalid resources,
unavailable modules, allocation failures and stream failures do not return a
dummy loaded/playing marker.

## Build integration

The production `tools/ps2/sources.txt` now includes these compilation units,
alongside the existing `i_sound.c` and `ps2_boot.c`:

```text
src/ps2/ps2_audio.c
src/ps2/ps2_music.c
src/ps2/ps2_midi.c
```

`tools/ps2/build.py` enables the installed compressed-audio decoders:

```text
-DPS2_AUDIO_VORBIS -DPS2_AUDIO_MP3
```

With the existing PS2SDK ports include/library directories, the build links these
libraries after objects and before `-lm`:

```text
-lvorbisfile -lvorbis -logg -lmpg123
```

Keep the existing `-lps2_drivers -laudsrv -lpatches` dependencies. The installed
`D:/ps2dev/ps2sdk/ports/lib/libps2_drivers.a` already contains `audsrv_irx` and
`size_audsrv_irx`; no generated IRX object is needed with this archive.
`PS2Boot_LoadAudio()` lazily loads `rom0:LIBSD` and the embedded audsrv IRX once
per process, after the boot IOP reset. audsrv initialization/format errors leave
`sound_started` false.

**The current engine also requires this linker flag:**

```text
-Wl,--wrap=W_LumpLength
```

The PS2 branch of `S_LoadMusic()` calls
`I_LoadSong(NULL, W_LumpLength(mlumpnum))`, without passing `mlumpnum` to the
backend. `__wrap_W_LumpLength` forwards to the real function and captures that
immediately preceding lookup. `I_LoadSong` consumes the captured argument,
checks its length, bounds and `O_`/`D_` name, and builds a random-access lump
reader. This relies on the existing single-threaded engine calls and ordinary
non-LTO linking. There is no filename guess or length-based search for music.

The sample/encoded-music allocations use the existing PS2
`Z_TryMallocAlign(..., PU_SOUND/PU_MUSIC, ..., 6)` arena API. This avoids spending
the port's small libc heap reserve on the effects cache. Codec allocations use
the normal libc heap and are released when their decoder is closed.

### Cooked music storage

Ogg music in the current cooker is already stored raw and streams directly via
`W_ReadLumpHeader`. No complete compressed track or decoded track is retained.

The current cooker may LZ4-compress WAV, MP3 and MIDI music. A partial read of an
LZ4 lump decompresses the whole lump in `WPack`, so the backend loads such an
encoded lump **once**, only when its decoded pack-lump size is at most 256 KiB.
Larger compressed music is rejected with a diagnostic. Store larger music lumps
as `CM_NOCOMPRESSION` in the cooker to enable bounded streaming; the cooker was
not edited in this change. The 256 KiB exception holds encoded file data, never
full-track PCM.

## Formats and behavior

| Resource | Implemented support |
| --- | --- |
| Effects | Doom DMX v3 unsigned 8-bit mono; PCM WAV unsigned 8-bit or signed little-endian 16-bit, mono/stereo; Ogg Vorbis; MP3 |
| Music | PCM WAV; single-logical-stream Ogg Vorbis; MP3 through mpg123; Standard MIDI File types 0 and 1 |
| WAV chunks | Arbitrary chunk order, unknown chunks, normal odd-byte padding; bounded compatibility for stock WAVs omitting the data pad before `LIST` |
| MIDI timing | PPQN division, up to 32 merged tracks, running status, tempo events, end-of-track, bounded meta/SysEx skipping |
| MIDI synth | 24 voices, note on/off, velocity, program-derived waveforms, volume/expression/pan, sustain, ±2-semitone pitch bend, controller reset, all notes/sounds off, procedural percussion |
| Playback controls | Start/stop, pause/resume, volume, pitch/pan updates, music speed 0.25–4×, position, length where known, loop points and looping |
| Loop metadata | Vorbis `LOOPMS` or `LOOPPOINT` (original source-rate sample units); MUSICDEF loop points can override these |
| Fades | Linear internal-volume fades; pause freezes them; callbacks run once on the main thread and can start another fade/song |

MIDI produces real audio without an external bank. Its sine/triangle/pulse/saw
palette and noise percussion are **approximate instruments**, not a full General
MIDI/SoundFont bank. Bank selection, modulation, aftertouch, effects processors
and vendor SysEx sound generation are not implemented. Type-2 and SMPTE-timed
MIDI, Doom MUS, chained/multichannel Vorbis, compressed/float/extensible WAV,
FLAC, trackers and GME formats are rejected. MP3 length may be unknown (zero)
when the file lacks useful length metadata; there is no full-track scan at load.

## CPU and memory design

- Output: **22,050 Hz, signed 16-bit stereo**.
- Mixer: 64 bounded engine channels with original-rate DMX/WAV data,
  fixed-point sample cursors, linear pan, pitch and saturating accumulation.
  Generation-tagged handles cannot stop a replacement voice through an old handle.
- Effects cache: **2 MiB payload/allocation-capacity budget**, plus small sample
  descriptors. Inactive samples are evicted least-recently-used and their engine
  `sfx->data` is cleared. Active samples remain pinned. If the active set prevents
  an allocation, the new effect fails instead of evicting active voices.
- Ogg/MP3 effects are decoded once on a cache miss to bounded PCM. Ordinary
  effects are cached at 22,050 Hz stereo. Effects longer than approximately
  23.8 seconds use 11,025 Hz stereo; two stock Ogg effects need that path.
  An effect whose encoded or decoded cache allocation exceeds 2 MiB is rejected.
  The temporary encoded effects buffer can add up to another 2 MiB during a
  cache miss. Both buffers are arena allocations; decoder working memory is extra.
- Music decoder scratch: 1,024 native-rate stereo frames plus a 4 KiB raw buffer.
  The MIDI sequencer has fixed track caches and voice arrays, with no event list
  or SoundFont. Validation is capped at one million events and 24 hours. MIDI
  seeking replays events and advances note state without rendering an entire
  discarded track; percussion/release-state advancement is bounded per voice.
- Vorbis uses the installed float library, with native half-rate synthesis for
  even-rate sources at or above 44 kHz. The SDK has no installed Tremor or MIDI
  synth library. Library-private allocation size still depends on codec headers;
  this is a bounded application-buffer design, not a fixed codec-heap allocator.
- The PCM mixer and procedural MIDI sample loop allocate no memory and use no
  per-sample floating-point math. MIDI note/bend setup uses floating point
  outside that loop; Vorbis itself uses floating-point decoding and can allocate
  private working state. There are no decoder threads.
- `I_UpdateSound` makes at most four PCM submissions per call, targeting 8,192 queued
  bytes (about **93 ms**). It checks audsrv queue/space before rendering and does
  not call `audsrv_wait_audio`. RPC itself remains synchronous. Mixer/decoder
  cursors do not advance when the output queue has no space. Short/zero writes
  retain the pending PCM and retry its exact unsent suffix; the decoder is not
  advanced again until that block has been submitted.

Nearest-neighbor resampling and the 22 kHz output trade fidelity for lower mixer
cost. Initial compressed-effect decoding can stall the main thread, especially
for a long cache miss. Music decoding also runs on the main thread. Game/IO
stalls longer than the queue reserve can underrun; CPU percentages and sustainable
worst-case polyphony have not been measured on EE hardware.

Playback position and playing-state describe generated PCM, up to one queue
reserve ahead of audible output. Already submitted shared music/effect PCM is
not individually retractable: stop/pause/seek/volume changes can take up to the
queue latency to be heard. Complete audio shutdown stops the audsrv stream.

## Tests and artifacts

Run from the repository root:

```powershell
python tools/ps2/audio_check.py
python tools/ps2/audio_codec_check.py
```

Both scripts check the output parent before creating files, and write only under
`build/ps2-audio-check`. `audio_check.py` also accepts `--host-only` or `--ee-only`.
MSVC host tests use explicit checks that stay enabled in release builds.

Verified on this working tree:

1. Portable host PCM/music/MIDI tests: real PCM values, malformed/truncated input,
   chunk layouts, pan/pitch/clipping, stale handles, pause, seek, speed, EOF and
   looping. MIDI seek output matches uninterrupted synthesis at the same sample.
2. Production `i_sound.c` host boundary tests: only dependency includes are
   substituted; fake engine clock/zone/resource and audsrv endpoints test PCM
   submission, queue limits, the NULL-data lump bridge, cache eviction/pinning,
   immediate and reentrant fade callbacks, allocation failure and RPC cleanup.
3. Real bundled x64 Vorbis/mpg123 DLL tests: **109 stock Ogg music tracks**,
   **105 stock MIDI tracks**, and a valid MPEG-1 layer-III silent-frame fixture;
   incremental reads, restart determinism and seeking.
4. Real decoder/backend tests for **all 737 stock effects** in `srb2.pk3`
   (**153 DMX, 70 WAV, 514 Ogg**): complete bounded decoding/caching, mixer start,
   PCM submission, free/stop and allocation cleanup. The effects cache stayed
   within 2 MiB. These tests run against raw extracted assets, not a PS2 pack.
5. **12 strict EE syntax checks** (`-Wall -Wextra -Werror`): all new core sources,
   `i_sound.c`, `ps2_boot.c` and the smoke program, with codec flags both disabled
   and enabled.
6. Real EE static-library link of
   `build/ps2-audio-check/ee/AUDIO.ELF`, including audsrv, Vorbis/Ogg, mpg123 and MIDI.

The real-codec test creates host import libraries from the bundled DLL exports;
it downloads nothing. Its MP3 fixture checks actual decoding and cursor/seek
behavior, but is silent and is not an MP3 music fidelity test.

### Hardware verification still required

`tools/ps2/audio_smoke.c` plays a two-second PCM tone without arguments, or accepts
a resource path, for example `host:track.ogg`, `host:track.mp3`, `host:track.wav` or
`host:track.mid`. Use the produced `AUDIO.ELF` on PCSX2 or PS2. Its PASS log proves
submission/completion; audible output must be confirmed separately.

The standalone smoke ELF has not been run. Integrated game runtime evidence and
the startup/streaming fixes are recorded below. Actual audible stereo output,
physical PS2 behavior, stock music transitions, pause/fades, long-effect cache
misses, underruns and EE CPU/heap budgets still need target validation.

## Startup hang and short-write fixes — 2026-10-02

The previous integrated ELF hung immediately after
`S_InitSfxChannels(): Setting up sound channels.` The hang was reproduced under
the real-32-MiB PCSX2 profile in `build/ps2-audio-runtime/before-fix`.

`SifSearchModuleByName` is not reliable with the tested ROM LOADFILE server: the
LIBSD search returned **29335136**, an unrelated positive value, and the code
also accepted the audsrv search without actually loading its IRX. Consequently,
SDK `audsrv_init()` waited forever for an absent RPC server. The process now
loads its required modules explicitly, once, and checks positive loader IDs.
No unsupported module-search RPC is used. On the fixed run LIBSD/audsrv actually
loaded as IDs **30/31**, and audio initialization reached the main game loop.

Runtime testing then exposed normal short PCM writes being treated as fatal
RPC errors. Queue-space queries and writes are separate RPCs; the IOP read head
can change between them. The backend now preserves and retries unsent bytes,
including after the last active voice ends. Invalid counts and actual audsrv
errors remain failures with a stage/status diagnostic. This adds no PCM buffer.

Verification on the final ELF:

| Scenario | Evidence |
| --- | --- |
| Host PCM/music/MIDI and backend | PASS, including zero-write/short-write byte-pointer continuity and no repeated decoder advancement |
| GS HW title, Ogg | 350 frames, music type 4, PCM submission active, normal exit, no audio failures |
| Software MAP01, looping Ogg | 350 frames, music type 4 with looping, PCM submission active, normal exit, no audio failures |
| GS HW title, MIDI (`-nodigmusic`) | 350 frames, music type 3, PCM submission active, normal exit, no audio failures |

Runtime logs: `build/ps2-audio-runtime/{hw-stream-fixed,software-map-ogg,hw-title-midi}/`.
All emulator runs used `tools/ps2/run_pcsx2.py` through `opt_run.py` and its lock.
The HW runs have an unrelated missing `models.dat` diagnostic; they are audio
startup/stream tests, not full HW visual-parity validation. Successful decoder
start and PCM submission do not by themselves confirm audible output.

Reproduction:

```powershell
python -B tools/ps2/audio_check.py --host-only
python -B tools/ps2/opt_run.py --name hw-stream-fixed --elf build/ps2/SRB2.ELF --out build/ps2-audio-runtime --no-ref --timeout 180 -- -skipintro -renderer Hardware -hwexit 350
python -B tools/ps2/opt_run.py --name software-map-ogg --elf build/ps2/SRB2.ELF --out build/ps2-audio-runtime --no-ref --map MAP01 --timeout 160 -- -renderer Software -hwexit 350
python -B tools/ps2/opt_run.py --name hw-title-midi --elf build/ps2/SRB2.ELF --out build/ps2-audio-runtime --no-ref --timeout 160 -- -skipintro -renderer Hardware -nodigmusic -hwexit 350
```
