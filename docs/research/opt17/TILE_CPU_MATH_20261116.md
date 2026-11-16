# Exact CPU reductions for oversized texture tiles

The isolated old scanline-classification change passed strict host and actual EE comparisons. In the standard 32 MiB PCSX2 profile, its complete classification fixture used **7.30% fewer Count ticks**. The origin helper alone improved 2.76 times. These are CPU fixture results; they do not measure rendered frames, GS work, or physical-console performance.

The separate column-map candidate passed full-upload byte comparisons and EE compilation. It removes repeated source-column arithmetic from texture uploads. Its timing evidence is host-only pending the combined native renderer gate.

## Frozen inputs and scope

The math baseline is the private r2 `ps2_hw_tile_draw.inc`, SHA-256 `977e69addc49c5853d4ccd0655d111da60e52792c498e21fdbba66e53462de4a`. The isolated candidate is `6b1683bbb1840ececd91c1873e7e0e9054b34da858640613b71e27bb00c66a63`. No production source was edited by this audit. The earlier private v6 scanline snapshot `e11c7e8187bc11eebd4e14a9425032c5271638a160891a67c0e3b2001b84ffc6` incorporated exactly the candidate origin helper and integer-edge prelude; its float barycentric, Q, UV, and closed-edge comparison block remained byte-identical. That historical binding is preserved separately from the final combined version. The source and section hashes are in [the result record](TILE_CPU_MATH_RESULTS_20261116.json).

The final combined **6-box** freeze has draw-file SHA-256 `27f368345e38cb8687ef047c3ee5955bdbc2956d38a77ab78b6e2d198464c4f8` and manifest `869af1a7a8d9d841e2bbc0ca452506534df63f552d21ade4f3090a13e26c0c5f`. Its signed-origin helper is byte-identical to the proven candidate (helper SHA-256 `4a06a66a4f42974d5e78719de474760db3d69b9b743a9058adab2659b991f44c`). Its entire column-map file is byte-identical to the proven column candidate `04b6fb78e6e551feabe5075e3c904eb4dc0878742b19ac5c3920957ba55d4729`. The box classifier supersedes the old incremental-row recurrence. The 7.30% measurement describes only the isolated old scanline fixture; it is not a measurement of the final box classifier or its whole-game speed. Final combined native sampling and frame timing are recorded by the parent renderer gates.

Frozen, non-shipping [test references](../../../tools/ps2/opt17/tile_cpu_reference/README.md) preserve both input versions so the fixture can run after the renderer evolves. The host and EE builders extract the real functions from those sources. Successful draw/stencil sinks capture resulting rectangles instead of talking to the GS. Complete rectangle arrays are compared in order and byte for byte; timing also checks paired checksums.

## Exact changes

`tile_region_origin` replaces `fmodf` and `floorf` in the common bounded, power-of-two case. With `-2^35 < value < 2^35`, scaling by 1/32 yields a safely representable integer-cell range. Truncation plus a negative correction implements floor. Masking selects the periodic cell, and a second correction preserves the original signed `fmodf` key, including negative fractional values whose masked cell is zero. Huge values and non-power-of-two periods use the original `fmodf` fallback; nonfinite handling and nonwrapping far clamps retain their original behavior. This preserves exact origin integers, not only an equivalent wrapped region.

Inside each scanline, the two 64-bit edge numerators start with the original expression at the first pixel. Advancing by a pixel adds the exact constant numerator difference. Fixed-point inputs are bounded by the existing 16-bit GS coordinates, so these recurrences stay within 64-bit range. All float barycentric, perspective division, UV arithmetic, and inclusive edge comparisons are unchanged.

The math candidate adds 16 bytes of fixed stack in the compiled EE classification wrapper (320 to 336 bytes), with the existing variable row workspace unchanged. It adds no heap allocation, persistent storage, or VRAM.

## Evidence

| Check | Result |
|---|---|
| Host normal | 2,493,561 checks, zero failures |
| ASan and UBSan | 2,493,561 checks, zero failures or diagnostics |
| Actual EE in standard 32 MiB PCSX2 | 592,121 checks, zero failures; paired checksums equal |
| Missing negative-key correction control | Detected: 353,928 failures |
| Broken first-edge recurrence control | Detected: 421 failures |

The host origin tests include boundaries, raw float classes, negative keys, huge and nonfinite values, and non-power-of-two sizes. Geometry compares 800 triangles with 3,999,744 bounding-box pixels as an upper count. The shorter EE correctness suite compares 80 triangles at 64 by 48 and 591,960 origin cases. It uses separate non-LTO baseline and candidate translation units with float contraction disabled.

The actual EE timing uses three paired repetitions. Each origin repetition makes 245,760 calls. Each classification repetition makes 30 calls covering up to 537,600 bounding-box pixels.

| Actual EE fixture | Baseline median Count | Candidate median Count | Reduction |
|---|---:|---:|---:|
| Origin helper | 43,143,280 | 15,646,700 | 63.73% |
| Complete classification with successful draw sink | 500,227,288 | 463,704,371 | 7.30% |

ELF SHA-256: `7ea2ad088b9ada468065e2c8f4f88928cc830a43c458bde59772fbdbaa9aa0bd`. The run used `run_verified` → `opt_run.py --ram 32` → the official `run_pcsx2.py`, one emulator slot, and the existing profile. All frozen inputs, non-UI settings, and the emulator binary were unchanged before/after. The public result embeds the captured checks, timings, assets, input hashes, and settings witness. It does not extend this CPU evidence into a GS pixel or full-game speed claim.

## Upload-local source-column map

The independent `ps2_hw_tile.inc` follow-up uses the original 64 source-X calculations once before a cache-miss upload, stores their results in `tile_fill_t.sx[64]`, and indexes that map in every row. It reduces X-coordinate mapping operations from 4,096 to 64 per complete upload. Y mapping still runs once per row. Palette, alpha, packed AP88 reads, framebuffer pitch, and destination-format branches are byte-identical. Cache hits do not call the preparation helper.

The array costs 256 bytes of upload-local context, with no heap, BSS, or VRAM addition. The EE conversion wrapper's compiled stack grows from 128 to 432 bytes; that 304-byte compiled delta includes compiler spills/alignment and is separate from the array size. Whole-game loader stack size must be assessed in the combined build.

The baseline file SHA-256 is `ebaf768bfe4f42f4fa68849b366dde4824046aa033f00f264def9d636db7a66e`, candidate `04b6fb78e6e551feabe5075e3c904eb4dc0878742b19ac5c3920957ba55d4729`. Normal and ASan/UBSan runs each compared 10,240 complete uploads, 167,772,160 bytes including untouched row padding, with zero failures. Cases cover P8, alpha8, AP88 keyed/unkeyed and packed-accessor paths, RGBA index/color paths, alpha-intensity88, screen16/32, all clamp/wrap combinations, region/nonregion addressing, 1 to 1,024 source widths including NPOT, and negative/large origins. The packed accessor is mocked; the changed column map and format branches are real source.

Shifting each prepared X by one is detected in 6,112 uploads. Both baseline and candidate wrappers compile with the EE compiler and `-Werror`. Host timings including map preparation/zeroing showed approximately 1.19 times P8, 1.79 times AP88/color, and 2.34 times screen32 fill speed in this run. These are local host timings, with no native fill or FPS claim. The combined renderer's native sampling and whole-game gates remain required before shipping.

## Reproduction

Run from the repository root; no emulator is used by the host commands:

```sh
python3 -B tools/ps2/opt17/tile_math_hosttest.py \
  --base tools/ps2/opt17/tile_cpu_reference/math-r2.inc \
  --candidate tools/ps2/opt17/tile_cpu_reference/math-candidate.inc \
  --out build/tile-math-host
# Add --sanitize for the sanitizer comparison.

python3 -B tools/ps2/opt17/tile_columns_hosttest.py \
  --base tools/ps2/opt17/tile_cpu_reference/columns-v6.inc \
  --candidate tools/ps2/opt17/tile_cpu_reference/columns-candidate.inc \
  --out build/tile-columns-host
# Add --sanitize, --negative, or --ee-objects as needed.
```

After sourcing the documented PS2 environment scripts, build the actual EE math fixture with the first command and `--ee-only --out build/tile-math-ee`. With the shared emulator slot granted, run `tile_math_native.py --build build/tile-math-ee --out build/tile-math-native`; it enforces the existing single-slot, standard 32 MiB profile and pins the fixture, sources, assets, runner, and settings. Use a fresh output directory. The tool measures CPU-only fixture behavior and does not draw GS triangles.
