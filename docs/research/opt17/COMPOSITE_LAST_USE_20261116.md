# Release temporary composition patches after their final use

`HWR_GenerateTexture` retains a newly loaded small canonical patch through its
last placement in that texture, then frees it after the synchronous CPU copy.
It previously kept every such patch until the entire texture was complete.
Repeated placements still use the same canonical source. Existing cached
patches belong to their caller and are never freed by the new path.

The 32-entry temporary table retains its original numbering. A released entry
becomes NULL and is not reused; the original overflow path, per-source build
counts, 64-KiB try threshold and old composition mode therefore remain intact.
The temporary last-placement fields add 128 EE stack bytes, with no persistent
metadata or new allocation. Drawing, patch conversion, output format, logical
dimensions, colormap and blend policy are unchanged.

The [portable runner](../../../tools/ps2/opt17/composite_lastuse_hosttest.py)
and [C fixture](../../../tools/ps2/opt17/composite_lastuse_hosttest.c) extract the
complete actual baseline/candidate generator, column drawers, blend functions,
`MakeBlockEx`, canonical patch creation/free, and TEXTURES/token parsers. The
reference generation body is literally bound to commit
`ce622181b6adcac285432ede2e705f5aab4c0b1a`. The selected source can precede or
contain the exact reviewed correction; only that delta is applied or reversed
in the isolated proof. The runner also guards the existing 32-slot and 64-KiB
policies and preserves unrelated source changes.

## Executed results

Both ordinary GCC and ASan/UBSan pass **13,878 cases / 25,085,665 checks**.
They compare **631,152,467 complete output bytes** directly, with identical
digest `fbb307dadefc6ae5`. Both variants perform **16,851 canonical builds**;
the complete per-resource build vectors and allocation/try counts match.

The corpus is rebuilt directly from the four external cooked game packs,
without a retained census or research build. It contains all 12,443 decoded
stock patch inputs and 22 TEXTURES lumps. The actual parsers read 506 texture
definitions and 8,073 placements. Resolved definitions execute P8, mapped AP88
and RGBA paths; standalone patches execute P8. Thirty definitions reference
missing stock sources. Their names exactly match the old exporter's same
rejection class; no source is substituted or newly suppressed in production.

Focused cases cover later repeated placements, more than 32 source identities,
mixed caller-owned hardware patches, old/slow composition, TEXC early return,
output-try failure and the existing large-source try failure. Every owned
canonical source is freed at return, nonheld sources survive, actual frees
clear their owners, and no tracked storage leaks.

Three compiled corruptions exit 101 at their specific assertions:

* Release before a future repeat changes the per-resource canonical build count.
* Release a nonheld source violates its existing hardware lifetime.
* Retain every held source until texture end loses the measured peak reduction.

The generic and PS2-without-profile generation bodies preprocess identically
to the reference. Archive lookup/reads, zone placement, nearest-palette and
palette/table access, optional TEXC/PNG/flat services and GPU services are
controlled host boundaries. Actual AST blend code and the original TRANS
tables are retained. This is a compositor/lifetime proof, not a full zone,
engine, GS drawing or physical-console test.

Actual EE GCC 15.2 compilation of both complete cache sources passes with
warnings fatal. Diagnostic non-LTO `HWR_GenerateTexture` stack is **560 → 688
bytes**; object size is 49,708 → 50,164 bytes. These are not shipping LTO sizes.
The [compact record](COMPOSITE_LAST_USE_RESULTS_20261116.json) preserves full
source, tool, pack, corpus, generated-C, binary, report and target-object hashes,
exact executed control diagnostics and both supported source-entry bindings.

## Component peak and MAP11 limit

For the actual stock SKY11 recipe, 48 placements still perform the same 17
canonical builds. The fixture's normalized EE payload peak, including its
524,288-byte P8 output, falls **748,784 → 672,312 bytes**, saving **76,472**.
Normalization uses actual post/pixel counts and independently measured EE
patch/column/post sizes of 48/12/12 bytes. It excludes zone headers/alignment,
fragmentation and other game allocations; it is not measured game RAM.

MAP11 Hardware admission remains unproven. Its v9 log has 627,456 free bytes at
precache, a missing SKY11 source, and a later recoverable 19,504-byte PU_PATCH
failure with total free 24,304 but largest hole only 7,392. The request needs
at least 19,520 contiguous bytes before alignment; the immediate hole deficit
is at least 12,128. Even the improved cold SKY11 recipe exceeds that load-end
free space. Removing the particular OOM would not prove that the complete
frame or later working set fits. The guard, geometry, feature limits and
physics are unchanged; the coordinator's game memory/images remain the gate.

## Reproduction from a checkout

Requires Python 3 with `lz4`, GCC with ASan/UBSan, Git history containing the
pinned commit, and the user's four original cooked packs. No asset, ELF or
generated corpus is committed. Use a fresh output directory:

```sh
python3 -B tools/ps2/opt17/composite_lastuse_hosttest.py \
  --packs /path/to/paks --out build/composite-lastuse-proof --negative-controls
```

`--source-root PATH` selects a root containing `src/`; the current checkout is
the default. In this workspace the complete portable execution is retained at
`build/nov17-composite-lastuse/public-v2/report.json`. It reproduces the private
proof's corpus SHA256 and every result. Commands write only under `--out` and
never start an emulator or alter production source.
