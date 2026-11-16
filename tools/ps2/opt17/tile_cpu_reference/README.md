# OPT17 frozen CPU test inputs

These source snapshots are test inputs, never shipping translation units.

- `math-r2.inc`: private tiled-renderer r2 classification before CPU changes.
- `math-candidate.inc`: same source with exact origin/edge arithmetic changes.
- `columns-v6.inc`: private v6 upload conversion before a column map.
- `columns-candidate.inc`: same source with upload-local source-X lookup.
- `i64f-box-base.inc`: final combined6-box source before exact conversion changes.
- `i64f-box-candidate.inc`: same source with the exact signed32 converter at two numerator casts.

The fixtures extract actual functions into separate translation units. Keeping the inputs here makes the paired checks reproducible after production rendering code changes. Full source and section hashes, the actual EE native math run, and the limits of that evidence are recorded in [the report](../../../../docs/research/opt17/TILE_CPU_MATH_20261116.md).

The column fixture mocks the packed AP88 accessor. Palette and alpha branches are the original source; the optimization changes only source-X preparation/reuse. Its timing is host-only until the combined renderer gate.

The converter has a separate [actual EE bit/Count report](../../../../docs/research/opt17/TILE_I64_TO_FLOAT_20261116.md). Hardware int32 casts differed from software int64 casts on the EE; the exact constructor preserves the latter bits.
