# Pack-reader scratch reuse, third pass — 2026-11-14

`src/w_pack.c` no longer reserves a separate 65,536-byte middle-block LZ4
decode buffer. It uses the existing 64-byte-aligned I/O bounce buffer for that
short, synchronous operation. Startup still allocates both required buffers;
no allocation is deferred to a gameplay read. The compressed fallback buffer,
stream windows, prefetch capacity, block sizes and public interfaces are unchanged.

This saves **64 KiB of persistent RAM**, including during decoding, in both
Software and Hardware builds using cooked packs. It does not change VRAM use,
codec selection, compressed content, visual quality or compatibility limits.

## Lifetime argument

The reader's API already specifies that it is not reentrant. `PeekAt` returns
only the stream window or the prefetch blob; when it cannot, `ReadAt` finishes
copying into the separate `cbuf` before LZ4 starts. None of these sources aliases
`iobuf`. The block decoder and the immediately following slice copy make no
allocations, engine callbacks or device reads. The slice is copied to the
caller's buffer before profiling ends or another device read starts. A retry
rereads the compressed source before decoding again. The I/O buffer is private
and is never returned to a caller.

Prefetch invokes its optional pump after copying a complete range from the
bounce buffer. The threaded music reader uses its own handle/buffers and does
not concurrently invoke the shared WPack reader. An independent GS subagent
reviewed these source, retry, callback and music-thread paths without finding
an actionable alias/lifetime problem. This change uses the pure LZ4 decoder;
it does not extend buffer sharing to allocating DEFLATE callbacks.

## Verification

Reference: immediately preceding completed pass, recreated by
`tools/ps2/prepare_next3_baseline.py`, rather than old Git HEAD.

- `test_pack_reader.py`: all **15,052** stock lumps and **300,940** partial reads
  match source PK3 metadata/CRCs (cooked PNG entries use their sidecar CRCs).
  v1/v2/headless fixtures, 65-block heap indexes, 21 malformed-layout cases,
  additional v2 damage/version checks, four device policies and custom windows,
  prefetch/drop and injected I/O failures pass. Nine input files are unchanged.
- `pack_scratch_hosttest.py`: builds actual old/current reader functions with
  guarded, aligned allocation accounting and the system **liblz4.so.1**.
  **5,968 assertions** pass for each normal version; the current version also
  passes ASan/UBSan. Cold-buffer
  allocation failures/retries/shutdown, window and fallback sources, prefix,
  middle, tail, empty/full ranges, random/compressible data up to 65,536 bytes,
  prefetch and 28 callbacks that reuse the I/O buffer are covered.
- Guarded allocations: scratch storage **196,608 → 131,072 B**; peak including
  the test's largest stream window **262,144 → 196,608 B**; zero live bytes
  after shutdown. The full stock corpus also passes through old/current actual
  readers using system liblz4, with byte-identical tables, read counts, retry
  counts and transferred bytes.

Reproduce in an environment with the PS2 SDK headers, C compiler, liblz4 and
cooked packs:

```sh
python3 -B tools/ps2/prepare_next3_baseline.py
python3 -B tools/ps2/pack_scratch_hosttest.py
python3 -B tools/ps2/test_pack_reader.py --pak build/pak --src /path/to/srb2-assets \
  --out build/nov14-next3/pack-corpus
```

Evidence is under `build/nov14-next3/{pack-scratch,pack-corpus}`. The complete
engine builds and game-level memory/frame comparisons are recorded separately
in `PS2_OPTIMIZATION_NEXT3_20261114.md`.
