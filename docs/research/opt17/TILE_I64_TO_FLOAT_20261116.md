# Exact EE conversion without software-double work

The private conversion candidate preserves the original cast bits while reducing the signed32 conversion fixture from **125,774,508 to 11,389,047 median Count ticks**: 90.94% less work, or 11.04 times faster. This measures the isolated converter in standard 32 MiB PCSX2, not frame rate or physical-console speed.

The V12 PC sampler attributed 54.1% of sampled work to libgcc software-double/64-bit helpers. Disassembly showed that each `(float)int64_t` numerator cast calls `__floatdisf`, which uses software double intermediates and conversion routines. The private patch replaces exactly the two `n1`/`n2` casts in `tile_plane_uv`. The determinant cast and all subsequent floating-point operations retain their original source.

## Why a plain 32-bit cast is unsafe

Host comparisons showed zero differences for a direct `(float)(int32_t)` shortcut. Actual EE code produced **150,396 differences in 485,782 cases**. For example, integer 16,777,219 becomes float bits `4b800002` through the original software int64 cast and `4b800001` through EE `cvt.s.w`. The negative counterpart differs identically in magnitude. The direct shortcut is retained only as a diagnostic negative control in the test fixture.

The alternative that uses a hardware cast only for exactly representable inputs also passed, but took 127,325,543 ticks on the broad signed32 benchmark. It did not address enough of the measured work.

## Exact helper

The candidate first checks whether sign-extending the low signed32 value reproduces the complete input. Wider inputs use the original `(float)value` expression. Signed32 inputs from -2^24 through 2^24 use hardware conversion because every such integer is exactly representable.

For the remaining signed32 values, unsigned subtraction obtains the magnitude safely, including `INT32_MIN`. The magnitude's leading exponent is 24 through 31. Three bounded integer comparisons determine the one to eight low bits that a float cannot retain, without `__builtin_clz` (which itself called a libgcc helper in the tested toolchain). The retained mantissa rounds up when the remainder exceeds half, or equals half and the retained mantissa is odd. Combining sign, biased exponent, and mantissa with addition handles a carry into the next exponent.

This exhausts the mathematical signed32 cases: zero and exact small integers; both signs; all eight possible discard widths; remainders below, equal to, or above half; even/odd ties; mantissa carry; and `INT32_MIN`. Signed32 integers are exact in the software double intermediate, so the constructor matches the original round-to-nearest-even float result. Native tests check that contract against the actual linked compiler routines. All wider input behavior remains the compiler's original behavior.

The EE object has a software conversion call only on the wide fallback. The common integer constructor has no soft-double, leading-zero, division, or allocation calls. The hardware path is restricted to exact integers. The patch adds no heap, persistent storage, or VRAM.

## Evidence and source binding

| Check | Result |
|---|---|
| Host conversion differential | 2,185,782 cases, zero constructor/safe-path mismatches |
| ASan/UBSan conversion differential | Same count, zero mismatches or diagnostics |
| Actual EE conversion differential | 485,782 cases, zero constructor/safe-path mismatches |
| EE direct32 diagnostic | 150,396 differences, correctly rejected |
| Actual `tile_plane_uv`, host | 1,000,000 calls; all 6,000,000 float outputs bit-identical |
| Actual `tile_plane_uv`, ASan/UBSan | Same count, zero differences or diagnostics |

The conversion tests include all integers from -65,536 through 65,536; signed power-of-two boundaries; every discarded-bit pattern for each signed32 exponent at even/odd and carry-sensitive mantissas; values around ±2^31 and ±2^32; randomized signed32 inputs; and wide signed64 fallbacks including extrema. They are stratified tests with a case-based proof, not an exhaustive sweep of all 2^32 inputs.

Three paired EE timing repetitions convert 163,840 deterministic broad signed32 values per path. Reference, exact-constructor, and safe-only checksums match. The direct32 diagnostic checksum differs.

| Path | Median Count |
|---|---:|
| Original software int64 cast | 125,774,508 |
| Exact signed32 constructor + wide fallback | 11,389,047 |
| Exactly representable-only shortcut + fallback | 127,325,543 |
| Direct signed32 diagnostic, rejected | 2,130,245 |

The baseline draw file is `27f368345e38cb8687ef047c3ee5955bdbc2956d38a77ab78b6e2d198464c4f8`; the private candidate is `7563b662e0b2a8742a32622e78e06c1b4e280f51f30b2f6d21b67ab9b2d8af52`. Replacing only the two candidate call expressions with the old casts makes the complete UV function byte-identical to the baseline. The candidate helper body matches the natively tested helper's tokens exactly; whitespace/comments and the function name are the only differences. Body token SHA-256: `5da4354cc823c94f3b95c4101fb74aeffc5935944efce7fb73fd0f2024972abb`.

The private combined `review-v7` draw file is byte-identical to that candidate (`7563b662e0b2a8742a32622e78e06c1b4e280f51f30b2f6d21b67ab9b2d8af52`). This binds the conversion component; the separately changed texture row-copy path and combined renderer gates have their own evidence.

The native ELF is `85180b5d0f1f34bef48a884db322046e6b4c8d45be7314e8494ca13a74c0e474`. It ran through `run_verified` → `opt_run.py --ram 32` → the official runner, with one granted emulator slot. The inputs, asset hashes, emulator binary, and non-UI settings remained unchanged before/after. [The result record](TILE_I64_TO_FLOAT_RESULTS_20261116.json) preserves the full timing, bit checks, and runner witness. Parent-owned combined native rendering and whole-game Count checks supply integration evidence.

## Reproduction

Run from the repository root:

```sh
python3 -B tools/ps2/opt17/tile_i64f_hosttest.py --out build/i64f-host
# Add --sanitize for ASan/UBSan.
python3 -B tools/ps2/opt17/tile_i64f_plane_hosttest.py \
  --base tools/ps2/opt17/tile_cpu_reference/i64f-box-base.inc \
  --candidate tools/ps2/opt17/tile_cpu_reference/i64f-box-candidate.inc \
  --out build/i64f-plane-host
# Add --sanitize for ASan/UBSan.
```

After sourcing the PS2 environment scripts, add `--ee-only --out build/i64f-ee` to the conversion command to build the EE fixture. With the shared emulator slot granted, run `tile_i64f_native.py --build build/i64f-ee --out build/i64f-native`. The native tool requires a fresh output directory and the existing standard single-slot profile. The standalone test code is not part of the game build.
