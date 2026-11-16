# Native high-resolution texture and capture witness

This standalone EE ELF links the real GS renderer. Synthetic P8/AP88/RGBA sources are rendered through both a roomy resident pool and a deliberately small pool, then complete GS frame readbacks are compared. It also exercises 640×400/480/512 captures, concurrent slots, real EE spills, wipes, post-image redraw, colored/model-light fog, strong minification and source-coordinate byte planes. Engine allocation, regeneration and zone-lock hooks are fixture services. This does not run the full game or test physical PS2 hardware.

The builder copies source and fixture inputs, pins generated headers, records compiler dependencies and uses the SDK two-stage LTO link. The runner uses `next12_runtime.run_verified` and the official `run_pcsx2.py`, stages every boot separately, and requires the unchanged 32 MiB single-slot profile. It does not change settings or include assets or BIOS data. Run from the repository root with the repository PS2 SDK environment configured; on the managed workspace:

```sh
source /workspace/.onboarding/environment.sh
source /workspace/.onboarding/emulator-env.sh
python tools/ps2/opt17/highres_tiles_native/build_native.py --source . --out build/native-check
python tools/ps2/opt17/highres_tiles_native/build_native.py --source . --out build/native-check-no-halo --no-halo
python tools/ps2/opt17/highres_tiles_native/run_native.py --elf build/native-check/TILES17.ELF --out build/native-check-full-run
python tools/ps2/opt17/highres_tiles_native/run_native.py --elf build/native-check/TILES17.ELF --out build/native-check-common-run --args 'commonpath source-only'
python tools/ps2/opt17/highres_tiles_native/run_native.py --elf build/native-check/TILES17.ELF --out build/native-check-attributes-run --args attributes
python tools/ps2/opt17/highres_tiles_native/run_native.py --elf build/native-check/TILES17.ELF --out build/native-check-coordinate-run --args coordinates
python tools/ps2/opt17/highres_tiles_native/run_native.py --elf build/native-check-no-halo/TILES17.ELF --out build/native-check-no-halo-run --args halo-only
python tools/ps2/opt17/highres_tiles_native/run_native.py --elf build/native-check/TILES17.ELF --out build/native-check-onecore-run --args onecore
```

Use fresh output paths. The original default suite deliberately retains its strict one-pixel, one-RGB-level palette-lighting fast-reference discrepancy; its process returns 1. The matching common EE reference is exact. The halo-removal run also returns 1, with a much larger expected failure. No assertion or tolerance is waived by the runner. `triangles source-only` is an additional symmetric primitive control; `stencil` runs the isolated alias, varying/max-depth and poisoned-fog-latch mechanism probes.

The `onecore` mode requires the V6 counter API. It compares complete RGBA and low-24-bit depth for narrow, inclusive 0..32 and nonaligned 61-texel source windows, with colored/model-light fog, perspective and both depth-write settings. The 12 positive pairs must increment the fast counter with zero stencil masks or classifier workspace. A raw 512-texel repeat control must use the bounded fallback. Sources without the counter API report an explicit failure for this mode.

For V8, use `--args 'onecore widewindow'` to add four 125-texel window pairs, and `--args 'halo-only indexed-halo'` for the negative boot. The latter renders both indexed and direct-color sources and checks that the actual cache uses 128/64 texels respectively in the same 64-block scratch. The builder clamps halo samples using the candidate's dynamic extent while retaining the rolled physical address. Both negative cases must fail visibly. Pass `--onecore --widewindow --indexed-halo` to the summary command to bind these controls; the original rendering tolerances remain unchanged.

V9 adds `--args borrowwide`, stored in a separate `PREFIX-borrow-run` boot, and summary option `--borrowwide`. It compares borrowed P8 128×256 and captured CT32 128×64 sources against resident rendering, with both colored/model-light fog and depth-write settings. A live adjacent allocation must prevent borrowing and use the V8 fallback. A queued wide draw is followed by a real ordinary allocation that reclaims the prefix, then a GS upload overwrites that complete returned range before the first frame readback. Counters, cache ownership, 32 KiB upload sizes, pending packet words, all RGBA bits and low-24-bit depth are checked. The fixture introduces no flush, fence or read between the queued draw and ordinary allocation.

The original CT32 span125×61 is retained as four exact-output fallback controls; actual EE division places its lower V just below70, so the strict neighbor guard rejects it. Separate interior124×60 inputs prove the wide branch. Both recipes log native eligibility and raw UV bits. `--initial-borrow-report PATH` optionally binds the unchanged initial262-check/20-branch-failure report alongside the refreshed318-check acceptance; all original RGBA comparisons in that initial run were exact.

`verify_capture_frames.py` and `verify_coordinate_frames.py` independently verify saved native readbacks. `frame_diffs.py` exports complete, unscaled RGB reference, candidate and absolute-difference PNGs. These analysis helpers require NumPy; PNG export also requires Pillow. Reports distinguish native execution from host analysis of actual GS readbacks. The committed final-r2 result is `docs/research/opt17/highres-native-final-r2.json`; historical red evidence remains under `build/nov17-tiles/native/` in the original workspace.

```sh
python tools/ps2/opt17/highres_tiles_native/verify_capture_frames.py --frames build/native-check-full-run-host --out build/native-check-capture-frame-proof.json
python tools/ps2/opt17/highres_tiles_native/verify_coordinate_frames.py --frames build/native-check-coordinate-run-host --out build/native-check-coordinate-frame-proof.json
python tools/ps2/opt17/highres_tiles_native/verify_texture_frames.py --frames build/native-check-full-run-host --out build/native-check-texture-RGBA-proof.json
python tools/ps2/opt17/highres_tiles_native/summarize.py --manifest docs/research/opt17/highres-native-final-r2.json --prefix build/native-check --out build/native-check-result.json
```

The summary command checks every guarded source hash and all runtime invariants. It refuses a changed default failure, a failed positive control, or a missing halo-negative failure; it preserves the original strict report as red. Add `--onecore` to require and bind the dedicated V6 counter and complete-frame controls. Published native results include `highres-native-final-r2.json`, `highres-native-grouped-v5.json`, `highres-native-combined-v6.json`, `highres-native-measured-v7.json`, `highres-native-indexed-v8.json` and `highres-native-borrowed-v9.json` in `docs/research/opt17/`; game performance remains a separate gate.
