# Native SKY pressure ordering witness

This separate fixture exercises actual pressure eviction. It keeps the renderer's permanent 16 KiB reservation and uses the real allocator, converter, copied GIF uploads, original primitive packing, GS rasterization and full-frame readback. The ordinary allocator and original native suites are unchanged.

Each nearest/bilinear pair compares a resident control with a 512-block pool:

1. Reserve the ordinary 16 KiB cache and upload a 448-block 128×896 P8 victim. Its last 128 source rows occupy the exact adjacent 64-block prefix.
2. Queue a visible left triangle sampling those rows. There is no fixture fence, flush or readback afterward.
3. Enter the actual SKY permission helper and bind a 1024×512 P8 proxy. A visible right triangle spans raw U16×V128, which requires the optional 128×256 image. Pressure admission evicts the old physical owner and claims the prefix.
4. Leave SKY, returning the prefix and restoring 448 ordinary blocks. A real 448-block upload overwrites the entire returned range.
5. Perform the first RGBA and raw Z32 readbacks. Compare every RGBA bit, every low 24 depth bit, every actual packed STQ/RGBA/XY/Zlow24 word, and the untouched input vertices. Check full canonical CPU source hashes.

Both visible regions must exceed 3,000 pixels and have nonzero depth. The two runtime negative modes deliberately corrupt either the victim prefix before its reader or the wide image between its real upload and reader. The independent checker requires changes only in the intended half of the frame, exact depth/packed words, and no unrelated assertion failures. Zero tolerance is retained everywhere.

Build only:

```sh
PS2DEV=/workspace/.onboarding/ps2dev-v2/ps2dev python -B tools/ps2/opt18_sky_pressure_native/build_native.py \
  --source build/opt18-sky-pressure/source-r3 --out build/opt18-sky-pressure/native-pressure-new
```

Run only with the coordinator's exclusive emulator slot. Use the unchanged standard 32 MiB wrapper:

```sh
. /workspace/.onboarding/emulator-env.sh
/workspace/.onboarding/assets-venv/bin/python -B tools/ps2/opt17/highres_tiles_native/run_native.py \
  --elf build/opt18-sky-pressure/native-pressure-new/TILES17.ELF --out build/opt18-sky-pressure/positive-new-run
python -B tools/ps2/opt18_sky_pressure_native/report_native.py --run build/opt18-sky-pressure/positive-new-run
```

Use separate fresh output paths and `--args victim-order-negative` / `--args wide-order-negative` for the controls. The native runner must return a failed native comparison; run the independent checker afterward with `--negative victim` / `--negative wide`, respectively.

The historical r5 wide control is deliberately retained as red: it detected the intended right-only pixel corruption but also expected a direct raw `upload_bands` call to increment texture-caller statistics. The corrected r6 changed only that fixture CHECK; its fresh wide control has exactly the intended RGBA failure. The r5 positive and victim control remain unchanged. See `build/opt18-sky-pressure/native-proof-summary.json` for all bindings.

These are component proofs in PCSX2. Actual game images and performance require separate admission; no physical PS2 is claimed.
