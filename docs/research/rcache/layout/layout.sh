#!/bin/bash
# usage: layout.sh OUTDIR   -> OUTDIR/<struct>.txt  (gdb ptype /o of every hot struct, EE ABI). Needs PS2DEV.
set -e
R=$(cd "$(dirname "$0")/../../../.." && pwd); D=$R/docs/research/rcache/layout
PS2DEV=${PS2DEV:-/opt/ps2dev-x/ps2dev}; SDK=$PS2DEV/ps2sdk; OUT=${1:-$R/build/layout}; mkdir -p $OUT
CC=$PS2DEV/ee/bin/mips64r5900el-ps2-elf-gcc; GDB=$PS2DEV/ee/bin/mips64r5900el-ps2-elf-gdb
GEN=${GEN:-$R/build/out-prof/gen}
$CC -c -g3 -O0 -G0 -std=gnu23 -D_EE -DPS2 -DPS2_PROFILE -DHWRENDER -DNDEBUG -DPS2_AUDIO_VORBIS -DNOMUMBLE -DNO_IPV6 -DNOUPNP -DCMAKECONFIG -DNOEXECINFO -DUNIXCOMMON -fwrapv -Wno-trigraphs \
  -I$R/src -I$R/src/ps2 -I$GEN -I$SDK/ee/include -I$SDK/common/include -I$PS2DEV/gsKit/include -I$SDK/ports/include $D/probe.c -o $OUT/probe.o
for t in mobj_t precipmobj_t sector_t line_t side_t seg_t subsector_t node_t vertex_t vissprite_t drawseg_t visplane_t player_t msecnode_t ffloor_t mapthing_t thinker_t spriteframe_t spritedef_t state_t mobjinfo_t \
         polyvertex_t poly_t extrasubsector_t gl_vissprite_t FOutVector FSurfaceInfo PolygonArrayEntry GLMipmap_t GLPatch_t patch_t texture_t lumpinfo_t pslope_t; do
  $GDB -batch -ex "set print type hex off" -ex "ptype /o $t" $OUT/probe.o > $OUT/$t.txt 2>&1 || true
done
ls $OUT | head -50
