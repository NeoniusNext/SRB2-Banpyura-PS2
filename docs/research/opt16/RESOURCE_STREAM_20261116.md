# SRP2 resource allocations: bounded verification and direct TEXC reads

The resource audit found an avoidable stored-data allocation in `PS2TexC_Fetch`, an unbounded verification buffer, and an allocation-order bug at the existing prefetch item limit. The changes preserve SRP2 versions 1/2, all payload bytes, block sizes, index validation, file alignment, add-on handling and network MD5 behavior. `w_wad.c` and the hardware OOM guards are unchanged.

## Changes and measured bounds

| Path | Before | After |
| --- | --- | --- |
| Nonresident TEXC composite | Allocate `l->disksize`, read stored bytes, decode into the existing destination | `WPack_ReadLump` decodes directly into that destination using the resident reader buffers |
| Resident TEXC composite | `WPack_DecodeMem` from the prefetch arena | Same decoder and bytes; same arena touch |
| `-verifypack` | Largest decoded lump allocated even though reads already used 256 KiB chunks | At most 256 KiB decoded buffer, incremental CRC32; identical read sizes, offsets and codec paths |
| Prefetch item growth | `realloc` occurred before checking the 65,536-item limit; a successful move at item 65,537 could leave a dangling pointer | Check the unchanged limit before `realloc`; allocation failure preserves the old list |
| Reader shutdown | Prefetch item allocation remained allocated | Release the prefetch blob and item allocation; `WPack_PrefetchDrop` still retains the item allocation during normal use |

The actual-source fixture reads raw, one-block LZ4 and indexed mixed LZ4/raw composites through all four medium policies. All 36 nonresident cases succeed with **every new allocation denied**; the old fetch succeeds in none of those denied-allocation cases. A raw 1 MiB composite removes a **1,048,576-byte** temporary allocation. The reader already has a 128 KiB codec/bounce pair and a 32/64 KiB medium window; these sizes are unchanged. The TEXC key path limits eligible composites to 1 MiB, so its block index fits the existing stack array.

Verification's guarded allocator measured **4,194,305 -> 262,144 bytes** for its largest request. Peak extra allocation was **262,452 bytes**, including the fixture's 48-byte CRC table and the 260-byte index for its deliberately oversized 65-block lump. Both versions issued exactly **235 commands / 12,474,368 bytes** for verification. Chaining CRC32 does not change the reader's existing 256 KiB request sequence. A bad final raw byte, malformed block index, damaged CRC table and failed table/buffer allocations all retain error reporting and cleanup.

Stock pack verification buffer reductions are:

| Pack | Largest lump / previous buffer | New buffer | Reduction |
| --- | ---: | ---: | ---: |
| SRB2.PAK | 1,090,568 | 262,144 | 828,424 bytes |
| ZONES.PAK | 3,313,934 | 262,144 | 3,051,790 bytes |
| CHARS.PAK | 9,618 | 9,618 | 0 |
| MUSIC.PAK | 4,122,587 | 262,144 | 3,860,443 bytes |

The CRC table is separate (four bytes per lump). Valid stock compressed lumps have at most 51 blocks, so verification needs no heap block-index allocation for them. An unusually large valid add-on lump still retains the existing heap-index path; no format is disabled to obtain the decoded-buffer bound.

## Current resource footprint and scope

EE compilation measures `sizeof(lumpinfo_t) = 36`, `sizeof(pf_item_t) = 16`, and `sizeof(pkstream_t) = 492`. The four current stock packs contain 15,052 entries: **541,872 bytes** of lump metadata, **509,060 bytes** of name pools and **120,416 bytes** in the two per-lump cache pointer tables, excluding allocator overhead and other engine indexes. Startup head tables occupy **234,688 bytes** and were already released after startup.

Persistent reader payload is still **384 KiB** for four DVD/HDD windows plus codec scratch, or **256 KiB** for four USB/SD windows plus scratch. Six occupied stream slots use 512/320 KiB respectively. The existing optional prefetch index can reach 1 MiB on EE; its capacity is unchanged. The prefetch stored-data blob remains evictable `PU_CACHE`.

No `TEXC.PAK` is present in the current `build/pak` set. The direct-fetch saving applies when that optional pack is installed. Verification savings apply to `-verifypack`; level prefetch is also opt-in. These changes do not claim lower default steady-state reader memory. The separate cooker name-pool work can reduce permanent resource memory. The existing PK3 DEFLATE compressed-input and partial-output-cache allocations were audited but not changed in this scope.

## Validation

`tools/ps2/resource_stream_opt17_hosttest.py --negative-controls` includes the complete production pack reader and extracts the production fetch function without rewriting it. Baseline source is revision `85e35eef5`. The fixture covers exact destination guards and pixel bytes, resident/nonresident raw and compressed reads, allocation denial, device read errors, truncation, invalid block indexes and LZ4 bodies, CRC corruption, failed verification allocations, every prefetch growth failure, a moving allocator at the item bound, normal Drop retention and complete Shutdown cleanup. Normal and ASan/UBSan runs pass **46,266,999 checks** with no live tracked allocation. Seven semantic negative controls fail as intended: resetting chunk CRC, restoring an unbounded buffer, checking the prefetch bound after realloc, retaining the index, restoring the fetch allocation, removing its size check, and skipping resident decoding.

`test_pack_reader.py` passes all **15,052 stock lumps / 300,940 partial reads**, their decoded CRCs and metadata against the original PK3 files and cooked-picture sidecar. It also passes the existing 21 malformed-input cases, versions 1/2 with/without head tables and deduplication/order, all medium policies, prefetch/drop and transient/permanent device-error injection. All nine input packs/archives/sidecars remain unchanged.

Both production files compile to actual EE objects with `-Wall -Wextra -Werror` and no diagnostics. Stack reports: fetch 48 bytes; verifier 160 bytes. No emulator was used for this resource-only work. Source hashes, measured footprints and proof output are in `RESOURCE_STREAM_RESULTS_20261116.json`; full local logs are under `build/nov17-resource`.
