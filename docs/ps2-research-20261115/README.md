# PS2 research verification metadata — 2026-11-15

This directory makes the [47-entry source catalog](../PS2_ARCHITECTURE_AND_OPTIMIZATION_SOURCES_20261115.md)
reviewable from a repository checkout or GitHub. [evidence.json](evidence.json)
contains compact checksums, page counts, access results, pinned source paths and
the read-only repository source map. It contains no manuals, PDF bytes, source
file bodies, assets, BIOS files or runtime configuration.

The research inspected revision
`85bd9716b1ed114d46f95831c64a87623038eeaa`; this is a historical source binding,
not a claim about subsequent commits or integrated game performance. No
production code, compiler, emulator or game runtime was changed by this
documentation publication. All optimization suggestions remain unmeasured.

## Manuals

The JSON `manuals` array records original direct-GET status, byte size, SHA256
and actual PDF page count for eight SCE v6 manuals and the BigBoss reference.
Toshiba TX79 is in `papers`, matching its original download record. SCE copies
identify Version 6.0, April 2002; the catalog gives chapter/page locators.
The archival hosting location is not a claim of current Sony distribution or
permission to redistribute the documents. Only metadata is published here.

## Papers

The `papers` array preserves the checked developer-paper/vendor-paper downloads,
including compressed and scanned files. For the Assarsson/Möller gzip, SHA256
identifies the compressed response; `expanded_pdf_pages` describes its decoded
PDF. Additional direct checks, and original successful web-reader inspections
where direct access differs, appear in `additional_pdf_checks` and
`web_pdf_inspections`. An unavailable checksum is explicit rather than invented.

The Teller thesis was scanned without usable extracted text; only its
institutional abstract supports detailed catalog claims. The DATE 2001 mirror
contains a one-page abstract rather than a full paper. The Toronto architecture
paper parsed as eight pages through the web reader, although a direct request
through the execution proxy was denied. These limits remain part of the record.

## Maintainers

The `maintainer_sources` array records pinned commits, source paths, available
Git blob IDs and successful connector inspection. A Git blob SHA1 and a PDF
SHA256 identify different objects. Pinned upstream versions are research
references, not an assertion that the port links those SDK/library versions.
The PCSX2 implementation is corroborating emulator source, not a hardware
specification or proof of physical GS behavior.

## Repository source map

The `repository_source_map` object preserves the original baseline revision,
relative source/report paths, byte sizes and SHA256 values. These hashes bind
the read-only inspection; they do not certify later revisions. No source bodies
or build artifacts are embedded.

## Link checks

The `link_verification` object retains the 55 distinct external URLs and their
original HEAD results: 54 succeeded, and the Toronto proxy request was denied.
HEAD reachability is separate from the body inspection described above. URLs
and the catalog's 47 primary entries were preserved during publication.

The original catalog version and its workspace-local checks remain identified
in `original_catalog_verification`. Publication replaces ignored-build links
with tracked documentation links. The publication's own document/hash/link
proof is archived separately under the build research directory; it is not
embedded in a document that would then need to hash itself.

## CLUT source rationale

The [GS manual](https://raw.githubusercontent.com/ninjadynamics/PS2Docs/main/GS_Users_Manual.pdf)
§3.4.7 pp55–57 distinguishes local-memory source CBP from the internal CLUT
temporary-buffer position selected by CSA. The [pinned PCSX2 P4 loader](https://github.com/PCSX2/pcsx2/blob/0ab00c49b157be667a8a2f0bf4cda83f75a9198e/pcsx2/GS/GSClut.cpp#L235)
corroborates that distinction. P4 CLD1 with an unchanged CBP does not select an
arbitrary source palette band merely by changing CSA.

A full T8 CLD1 load at CSA0 followed by T4 CLD0 at the intended bank is a
different candidate protocol, not a validated optimization. Its review must
cover the one shared internal CLUT across contexts; every TEX0/TEX2 writer;
palette-content generations and replacement; CBP0/CBP1 conditional-load state;
and actual DMA/GIF/VIF execution order. CLD1 does not update CBP0/CBP1; CLD2/3
do. Cached bank selection must not survive a conflicting later load.

Transfer legality is separate: indexed texture buffer width uses 128-texel
multiples (p27); P4 host-to-local start X and transfer-width restrictions are on
p76; packed nibbles on p79; layout on pp162,168–170. Logical dimensions,
transfer padding, TBW, sampling/clamp behavior and occupied blocks must all be
proved. No P4 protocol or hardware performance was validated by this research.
