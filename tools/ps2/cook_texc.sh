#!/bin/bash
# OPT13 IZ (PS2-602, R2): TEXC.PAK, the composite textures of the game made once on the host (docs/PACK_FORMAT.md, "TEXC.PAK").
#   1. the host engine (the PS2 profile on x86: the same r_textures.c, w_pack.c, patch code and the key function src/ps2/ps2_texc.c as the console) loads the cooked packs of PAKDIR,
#      builds its texture list and composes every texture that can have a prebuilt composite (-texcdump FILE; about 1 s, 100 MB of pixels)
#   2. tools/ps2/cook.py --texc packs the dump (LZ4HC, SRP2 v2) into OUTDIR/TEXC.PAK (about 40 s)
# usage: cook_texc.sh PAKDIR OUTDIR     PAKDIR: SRB2.PAK ZONES.PAK CHARS.PAK MUSIC.PAK as the console loads them; OUTDIR/TEXC.PAK goes next to them (or beside the ELF)
# The key of a texture holds the content identity of the packs its patches come from (the checksums of their tables): TEXC.PAK is for exactly the packs that were in PAKDIR; cook the game
# packs again and this again. Add-ons and anything not from the cooked packs are not in it and are composed from their patches at run time as before.
# Needs the host profile to build (cmake, ninja, SDL2, xvfb-run); the build directory is build/host-texc.
set -e
PAK=$(realpath "$1"); OUT=$(realpath -m "$2")
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
B=build/host-texc
if [ ! -f $B/b/build.ninja ]; then
  mkdir -p $B
  cmake -S . -B $B/b -G Ninja -DCMAKE_BUILD_TYPE=Release -DSRB2_CONFIG_STATIC_STDLIB=OFF -DSRB2_CONFIG_USE_GME=OFF -DSRB2_CONFIG_HWRENDER=OFF -DSRB2_CONFIG_PS2REF=ON \
    -DSRB2_CONFIG_PS2PROFILE=ON "-DSRB2_PS2_NO=lua;udmf;addons;limits;zippng" -DSRB2_HOST_PROFILE_LZ4_SOURCE=$ROOT/tools/ps2/host_lz4_shim.c \
    -DSRB2_HOST_PROFILE_LZ4_INCLUDE_DIR=/opt/ps2dev-x/ps2dev/ps2sdk/ports/include "-DCMAKE_C_FLAGS=-DPS2_NOOPT_SLOPE -DPS2_NOOPT_SEGS -fwrapv" > /dev/null
fi
ninja -C $B/b -j${JOBS:-2} | tail -1
EXE=$(realpath "$(ls -t $B/b/bin/* | head -1)")
W=$ROOT/$B/work
rm -rf "$W"; mkdir -p "$W/home/.srb2" "$OUT"
(cd "$W" && SRB2WADDIR=$PAK SDL_AUDIODRIVER=dummy xvfb-run -a -s "-screen 0 800x600x24" timeout 600 "$EXE" -home "$W/home" -nolog -noendtxt -win -width 320 -height 200 -texcdump "$W/texc.dump" > stdout.log 2>&1) || { tail -5 "$W/stdout.log"; exit 1; }
grep TEXCDUMP "$W/stdout.log"
python3 tools/ps2/cook.py --texc "$W/texc.dump" --out "$OUT"
