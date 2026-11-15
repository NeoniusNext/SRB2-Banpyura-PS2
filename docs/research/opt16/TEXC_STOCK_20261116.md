# Stock TEXC pack: original archive identities

`build/pak-texc17/TEXC.PAK` is a fresh pack produced by the repository's actual host compositor and cooker. Its directory contains hard links to the **original fixed** `SRB2.PAK`, `ZONES.PAK`, `CHARS.PAK`, `MUSIC.PAK`, cooked-picture sidecar and `FINEACON.DAT`. All original SHA256/MD5 values, file bytes and content identities remain unchanged. `build/pak` was not modified.

The compositor exported **3,318 records / 3,309 distinct texture keys**, with **zero failures**. The pack holds **129,686,189 decoded bytes** including its 16-byte information lump, in **15,738,880 bytes (12.1%)**. No image or audio data was quantized or recompressed with a lossy codec. Ineligible textures and future add-ons retain the existing runtime composition path.

This TEXC pack belongs to the original archive set only. A layout change to a source pack changes its table checksum and therefore its TEXC keys, even when its decoded content is identical. Export again for the optimized archive set before shipping those archives together. The reader's existing identity check prevents a stale composite from being used.

## Portable exporter and host build

`tools/ps2/cook_texc.sh PAKDIR OUTDIR [HOST_BUILD_ROOT]` now honors `PS2SDK`, `PS2DEV` or `SRB2_HOST_PROFILE_LZ4_INCLUDE_DIR`, instead of a hardcoded `/opt` SDK path. It accepts an isolated build root, refreshes CMake configuration, builds a consistently named exporter, retains logs in separate work directories and uses SDL dummy drivers for headless export. `CMAKE`, `PYTHON`, `JOBS`, `SRB2_TEXC_BUILD_ROOT`, `SRB2_TEXC_C_FLAGS` and `SDL_VIDEODRIVER` remain available as explicit overrides.

The missing CMake/Ninja/SDL2 development dependencies were downloaded from the authenticated Debian package index and extracted entirely under `/workspace/.onboarding/host-texc-deps` (about 17 MB of packages). System packages were not changed. `/workspace/.onboarding/host-texc-env.sh` selects these dependencies and the workspace SDK/Python environment.

GCC 14 exposed one existing host-profile build error: `R_TexturesHash` used `M_CheckParm` under `PS2_PROFILE`, while `m_argv.h` was included only under `PS2 && PS2_PROFILE`. The approved fix widens that include guard to `PS2_PROFILE`. Before/after **EE preprocessed output is byte-identical**, and the native EE object compiles with `-Werror` without diagnostics. The final host build uses the proper include and no forced-header flags.

This is the existing **content-export tool profile**, with Lua/UDMF/add-ons/limits/ZIP-PNG disabled for the exporter only. It is not evidence of a full host game build and does not change the console build's features.

## Exact-byte and allocation proof

Two complete exports, before and after the declaration fix/rebuild, produced byte-identical dumps. `tools/ps2/texc_stock_verify.py` checks every stored composite against the actual compositor's output using an independent Python decoder and the complete production C pack reader. Plain and ASan/UBSan C runs, both with and without head tables, each verify **129,686,189 literal bytes and 66,200 partial reads** without differences.

The same test runs the actual production `PS2TexC_Fetch` and entire production pack reader for every exported record: **129,989,949 pixel bytes in each of the resident and nonresident paths**, including duplicate definitions. Both paths succeed with **all new allocations denied**, exact bytes and destination guards, and no tracked allocation left behind. The largest removed stored-data temporary for this actual stock pack is **287,661 bytes**; the new fetch needs **zero extra allocation**. Negative controls reject both a changed compositor pixel and the old allocating fetch implementation. The fixture's key selection and allocator are explicit boundaries; the exporter computes the real keys, and the complete C reader loads the actual pack index.

## Native memory costs and prefetch budgets

Installing TEXC also has a fixed metadata cost, so its frame allocation saving must not be described as an unconditional reduction in total resident memory. Native payloads for this pack and the 4,740-texture stock list are:

| Allocation | Bytes |
| --- | ---: |
| 3,310 lump metadata records | 119,160 |
| Name pool | 56,262 |
| Key allocation, 16 bytes per entry | 52,960 |
| Texture-to-lump lookup | 9,480 |
| Texture-to-prefetch-offset lookup | 18,960 |
| Additional DVD/HDD stream window | 65,536 |
| **Fixed payload total** | **322,358** |

The 128 KiB codec/bounce pair is shared with the existing reader. TEXC opening passes `packp=NULL`, so its on-disk head table is not retained. Figures exclude allocator overhead and existing engine texture/level structures.

An actual-source `PS2TexC_PrefetchLevel` fixture marks every eligible stock texture and puts the largest stored composite in the sky-priority slot. It measures the following additional payloads above fixed metadata:

| Budget | Stored arena retained | Peak during fill | Temporary work at peak | Stored composites |
| --- | ---: | ---: | ---: | ---: |
| 0 | 0 | 0 | 0 | 0 |
| 256 KiB | 262,112 | 615,604 | 353,492 | 35 |
| 512 KiB | 524,288 | 914,644 | 390,356 | 31 |
| 1 MiB (existing default) | 1,048,576 | 1,438,932 | 390,356 | 132 |
| 2 MiB | 2,089,360 | 2,479,716 | 390,356 | 234 |

These are controlled stress sets, not measured map working sets. The temporary work is the actual 4,740-byte marking array, 94,800-byte item array and largest simultaneous read-run allocation. The pack's largest stored lump is 287,661 bytes, so a single run rounds to **290,816 bytes**, exceeding the usual 256 KiB merged-run bound. Every retained stored byte is compared with its actual pack bytes. Denying both optional work allocations releases the arena and leaves the direct-read path available, without a leak. The host allocator does not simulate the console's existing headroom guard or zone eviction policy; root's console runtime validation covers those behaviors.

## Reproduction and artifacts

```sh
source /workspace/.onboarding/host-texc-env.sh
tools/ps2/cook_texc.sh build/pak-texc17 build/pak-texc17 build/nov17-resource/host-texc
python tools/ps2/texc_stock_verify.py \
  --pak build/pak-texc17/TEXC.PAK \
  --dump "$(cat build/nov17-resource/host-texc/last-work)/texc.dump" \
  --out build/nov17-resource/texc-verification --negative-controls
```

`TEXC_STOCK_RESULTS_20261116.json` records all source/archive hashes, native sizes, dependency package digests, literal-byte tests and measured budgets. The actual artifact SHA256 is `6f408ef03180291f098e26dd1049e09018e71c44ffcfbd1231779a9fbef3b485`. The final exporter, dump and complete logs remain under `build/nov17-resource/host-texc`; verification logs are under `build/nov17-resource/texc-verify-final`. No emulator was used for this work.
