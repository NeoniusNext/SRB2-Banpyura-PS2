# Bounded recovery after 64 VRAM evictions

The allocator keeps its original first 64 attempts unchanged. After they fail,
it now retries allocation once, then asks the existing `best_window` policy
for one feasible aligned window, drops its existing owners and retries once.
There is no new cap, texture level, format conversion, allocator metadata,
physical relocation or unbounded retry loop. Whole-game v7 validation is
pending with the coordinator.

Two legal states demonstrate the old false failure using the actual source:

| State | Original result | Repaired result |
|---|---|---|
| 64-block pool, 64 one-block owners, request 64 | Evicts all 64, leaves the entire pool free, returns zero | Allocates the complete pool after the final victim |
| 256-block pool, alternating addresses oldest, request 128 | Evicts 64 scattered owners, returns zero despite a feasible window | Uses one exact window and succeeds after 128 total evictions |

The original loop tries allocation before dropping its next victim. Its 64th
drop is therefore never followed by another allocation attempt. For a small
request, `small_victim` may also choose 64 individually cheap victims without
ever trying the aligned-window path already used for larger requests.

The recovery uses the same `best_window` and `tex_drop` operations as the
original allocator. Existing weights protect the selected texture's owning
image, its real shared view, pinned images and live captures. If no feasible
window exists it returns zero. The window walk advances by one free block or
to the end of the live owning span; it is bounded by the pool's block range.
After that walk there is one final allocation call. Successful ordinary paths
and the first 64 eviction decisions retain their original code.

## Executed source checks

[`vram_recovery_opt16_hosttest.py`](../../../tools/ps2/vram_recovery_opt16_hosttest.py)
compiles the complete actual texture source and real headers with host engine
and DMA services. It obtains the original allocator literally from pinned
604cfd source, requires its prefix to match, and recognizes either that exact
old function or the exact repaired tail. It can replay an already adopted
source tree without appending recovery twice. A fresh checkout needs no old
build directory: the default runner exports its complete pinned baseline.

Both old and repaired variants pass ordinary GCC and ASan/UBSan. Six cases per
variant cover immediate success, both false failures, protected pin/current
image/shared view/capture, an entirely protected pool, and impossible alignment.
The captured normalized state hashes at the 64-attempt boundary agree in each
old/repaired pair. An independent walk checks every owner/free block, sorted
coalesced free ranges and exact `H.used_blocks`, including every successful
new reservation. No block is lost or counted twice.

Two executed controls fail: removing recovery recreates the final-victim false
failure; selecting an arbitrary window destroys protected records and fails
the protection assertion. Evidence is
`build/nov16-hwopt/vram-recovery-v3/report.json`; its compact checked record is
[`VRAM_RECOVERY_RESULTS_20261116.json`](VRAM_RECOVERY_RESULTS_20261116.json).
The first preparation attempt lacked the existing `lru_touch` extraction and
failed compilation; it remains in `vram-recovery-v1`. Subsequent stages compile
that literal helper too.

Actual EE GCC compilation of the complete hardware driver with `-Werror`
passes (`vram-recovery-target-syntax/build.log`). The final production P4
component run at `p4-production-recovery-host-v1/report.json` also passes
101,434,367 plain/sanitized checks, 101,369,320 decoded texels, 2,232 uploads,
all six corruptions, the mandatory VU packet-memory proof and the unchanged
140-byte target record ABI. The MMI packing and band-scan function bodies are
byte-for-byte unchanged by this allocator repair.

These are source/ownership proofs. The three v6 fog/NPOT game allocation
failures motivated this repair; their disappearance requires the separate v7
game run, rather than inference from passing sampled screenshots or this host
witness. No physical-console timing or FPS improvement is claimed.

```sh
python3 tools/ps2/vram_recovery_opt16_hosttest.py \
  --out build/vram-recovery-replay --negative-controls
```
