#!/bin/bash
# OPT13 IZ (PS2-602, R2): TEXC.PAK, the composite textures of the game made once on the host (docs/PACK_FORMAT.md, "TEXC.PAK").
#   1. the host engine (the PS2 profile on x86: the same r_textures.c, w_pack.c, patch code and the key function src/ps2/ps2_texc.c as the console) loads the cooked packs of PAKDIR,
#      builds its texture list and composes every texture that can have a prebuilt composite (-texcdump FILE; about 1 s, 100 MB of pixels)
#   2. tools/ps2/cook.py --texc packs the dump (LZ4HC, SRP2 v2) into OUTDIR/TEXC.PAK (about 40 s)
# usage: cook_texc.sh PAKDIR OUTDIR [HOST_BUILD_ROOT]
# PAKDIR: SRB2.PAK ZONES.PAK CHARS.PAK MUSIC.PAK as the console loads them; OUTDIR/TEXC.PAK goes next to them (or beside the ELF)
# The key of a texture holds the content identity of the packs its patches come from (the checksums of their tables): TEXC.PAK is for exactly the packs that were in PAKDIR; cook the game
# packs again and this again. Add-ons and anything not from the cooked packs are not in it and are composed from their patches at run time as before.
# Needs cmake, ninja, SDL2 development files and Python with lz4. LZ4 headers come from PS2SDK/PS2DEV or SRB2_HOST_PROFILE_LZ4_INCLUDE_DIR.
# This is a content-export tool profile, not a full host game build. SDL's dummy drivers keep export headless; SDL_VIDEODRIVER can override that choice.
set -euo pipefail
if [[ $# -lt 2 || $# -gt 3 ]]; then
  echo "usage: $0 PAKDIR OUTDIR [HOST_BUILD_ROOT]" >&2
  exit 2
fi
PAK=$(realpath "$1"); OUT=$(realpath -m "$2")
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
B=$(realpath -m "${3:-${SRB2_TEXC_BUILD_ROOT:-$ROOT/build/host-texc}}")
CMAKE=${CMAKE:-cmake}
PYTHON=${PYTHON:-python3}
LZ4_INCLUDE=${SRB2_HOST_PROFILE_LZ4_INCLUDE_DIR:-}
if [[ -z "$LZ4_INCLUDE" ]]; then
  if [[ -n "${PS2SDK:-}" ]]; then
    LZ4_INCLUDE=$PS2SDK/ports/include
  elif [[ -n "${PS2DEV:-}" ]]; then
    LZ4_INCLUDE=$PS2DEV/ps2sdk/ports/include
  elif [[ -r /usr/include/lz4.h ]]; then
    LZ4_INCLUDE=/usr/include
  fi
fi
if [[ ! -r "$LZ4_INCLUDE/lz4.h" ]]; then
  echo "Set PS2SDK, PS2DEV or SRB2_HOST_PROFILE_LZ4_INCLUDE_DIR to the directory containing lz4.h" >&2
  exit 2
fi
for PACK in SRB2.PAK ZONES.PAK CHARS.PAK MUSIC.PAK; do
  [[ -r "$PAK/$PACK" ]] || { echo "Missing $PAK/$PACK" >&2; exit 2; }
done
cd "$ROOT"
mkdir -p "$B" "$OUT"
if ! "$CMAKE" -S . -B "$B/b" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSRB2_CONFIG_STATIC_STDLIB=OFF -DSRB2_CONFIG_USE_GME=OFF -DSRB2_CONFIG_HWRENDER=OFF -DSRB2_CONFIG_PS2REF=ON \
    -DSRB2_CONFIG_PS2PROFILE=ON "-DSRB2_PS2_NO=lua;udmf;addons;limits;zippng" "-DSRB2_HOST_PROFILE_LZ4_SOURCE=$ROOT/tools/ps2/host_lz4_shim.c" \
    "-DSRB2_HOST_PROFILE_LZ4_INCLUDE_DIR=$LZ4_INCLUDE" -DSRB2_SDL2_EXE_NAME=texc-exporter -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    "-DCMAKE_C_FLAGS=-DPS2_NOOPT_SLOPE -DPS2_NOOPT_SEGS -fwrapv ${SRB2_TEXC_C_FLAGS:-}" > "$B/configure.log" 2>&1; then
  tail -40 "$B/configure.log" >&2; exit 1
fi
if ! "$CMAKE" --build "$B/b" --target SRB2SDL2 --parallel "${JOBS:-2}" > "$B/build.log" 2>&1; then
  tail -40 "$B/build.log" >&2; exit 1
fi
tail -1 "$B/build.log"
EXE=$B/b/bin/texc-exporter
[[ -x "$EXE" ]] || { echo "Missing host exporter $EXE" >&2; exit 1; }
W=$(mktemp -d "$B/work.XXXXXXXX")
mkdir -p "$W/home/.srb2"
printf '%s\n' "$W" > "$B/last-work"
(cd "$W" && SRB2WADDIR="$PAK" SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-dummy}" timeout 600 "$EXE" -home "$W/home" -nolog -noendtxt -win -width 320 -height 200 -texcdump "$W/texc.dump" > stdout.log 2>&1) || { tail -20 "$W/stdout.log" >&2; exit 1; }
grep '^TEXCDUMP [0-9]' "$W/stdout.log"
"$PYTHON" tools/ps2/cook.py --texc "$W/texc.dump" --out "$OUT" --jobs "${JOBS:-2}"
printf 'TEXC dump and exporter log: %s\n' "$W"
