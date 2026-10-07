#!/bin/bash
# OPT10-S: native Linux build of the PS2 profile (software renderer, PS2_PROFILE, --ps2ref hooks) with AddressSanitizer and the UBSan checks that matter on the EE
# (bounds, alignment, null, vla-bound), all recoverable so that one run lists every distinct finding; then the four golden demos under it.
# The emulator forgives what the console does not: out-of-bounds reads/writes, use after free, NULL-based stores, unaligned accesses.
#
# usage: [ASAN_DIR=build/host-asan] tools/ps2/host_asan.sh build    configure + build into build/host-asan   (a few minutes with -j2)
#        tools/ps2/host_asan.sh demos    DEMO_001..004 timedemos under it: build/runs/asan-<dir>-DEMO_00n/stdout.log, findings listed per demo
# Needs: cmake, ninja, gcc, SDL2/openmpt/png/zlib dev packages (the CMake host profile), the PS2 packs in $PAKDIR (default build/pak), the demos in golden/phase0-v2.
# Notes: the CMake host profile (src/CMakeLists.txt) does not compile d_netfil.c, mserv.c and http-mserv.c, which the EE build has: they are compiled here by hand with the
# same flags and the stubs of tools/ps2/host_asan_shim.c are linked; lzf.c gets STRICT_ALIGN=1 as on MIPS (x86 would use unaligned u16 loads that UBSan flags).
# Not covered: Lua, UDMF, add-ons, the hardware renderer, the audio/USB/network code (host profile features off: SRB2_PS2_NO=lua;udmf;addons;limits).
set -e
W=$(cd "$(dirname "$0")/../.." && pwd)
cd "$W"
B=${ASAN_DIR:-build/host-asan}
S=$B/shim
PAKDIR=${PAKDIR:-$W/build/pak}
SAN="-fsanitize=address,bounds,alignment,null,vla-bound,nonnull-attribute,returns-nonnull-attribute -fsanitize-recover=all"

if [ "$1" = build ]; then
  mkdir -p $S
  # the host CMake profile turns -Werror=vla on: the engine uses VLAs in a few places
  printf '#!/bin/bash\nexec /usr/bin/gcc "$@" -Wno-error=vla -Wno-vla\n' > $S/cc.sh
  chmod +x $S/cc.sh
  gcc -c -O1 -g $SAN -o $S/shim.o tools/ps2/host_asan_shim.c
  cmake -S . -B $B -G Ninja -DCMAKE_C_COMPILER=$W/$S/cc.sh -DCMAKE_BUILD_TYPE=Debug \
    -DSRB2_CONFIG_HWRENDER=OFF -DSRB2_CONFIG_USE_GME=OFF -DSRB2_CONFIG_STATIC_STDLIB=OFF \
    -DSRB2_CONFIG_PS2REF=ON -DSRB2_CONFIG_PS2PROFILE=ON -DSRB2_PS2_NO="lua;udmf;addons;limits" \
    -DSRB2_HOST_PROFILE_LZ4_SOURCE=$W/tools/ps2/host_lz4_shim.c \
    -DSRB2_HOST_PROFILE_LZ4_INCLUDE_DIR=/opt/ps2dev-x/ps2dev/ps2sdk/ports/include \
    -DCMAKE_C_FLAGS="-O1 -g -fno-omit-frame-pointer $SAN -DSTRICT_ALIGN=1 -Wno-error" \
    -DCMAKE_EXE_LINKER_FLAGS="$SAN" > $B/configure.log 2>&1
  # the compile command of an engine unit (flags of the profile) for the hand-built ones
  CMD=$(ninja -C $B -t commands src/CMakeFiles/SRB2SDL2.dir/netcode/d_clisrv.c.o | tail -1 | sed 's/ -MD -MT [^ ]* -MF [^ ]*//; s/ -o src.*$//')
  for f in d_netfil mserv http-mserv; do
    (cd $B && eval "$CMD -c $W/src/netcode/$f.c -o $W/$S/$f.o") > /dev/null 2>&1
  done
  cmake -S . -B $B -DCMAKE_EXE_LINKER_FLAGS="$SAN $W/$S/d_netfil.o $W/$S/mserv.o $W/$S/http-mserv.o $W/$S/shim.o" >> $B/configure.log 2>&1
  ninja -C $B -j2
  ls -la $B/bin/
  exit 0
fi

if [ "$1" = demos ]; then
  export ASAN_OPTIONS=detect_leaks=0:abort_on_error=0:halt_on_error=0:print_stacktrace=1:symbolize=1
  export UBSAN_OPTIONS=print_stacktrace=0
  EXE=$(ls $W/$B/bin/lsdlsrb2_* | grep -v '\.debug$' | head -1)
  for D in DEMO_001 DEMO_002 DEMO_003 DEMO_004; do
    OUT=$W/build/runs/asan-$(basename $B)-$D
    rm -rf "$OUT"; mkdir -p "$OUT/home/.srb2"
    cp golden/phase0-v2/$D.lmp "$OUT/home/.srb2/$D.lmp"
    printf 'fpscap "35"\nfullscreen "Off"\nshowfps "Off"\nshowping "Off"\n' > "$OUT/home/.srb2/reference.cfg"
    (cd "$OUT" && SRB2WADDIR=$PAKDIR SDL_AUDIODRIVER=dummy xvfb-run -a -s "-screen 0 800x600x24" timeout 900 "$EXE" -ps2ref "$OUT" -home "$OUT/home" \
      -config reference.cfg -nolog -noendtxt -win -width 320 -height 200 -timedemo $D.lmp > stdout.log 2>&1) || true
    echo "=== $D: $(wc -l < $OUT/tics.csv) tics.csv rows; distinct findings (count):"
    grep -h "runtime error\|ERROR: AddressSanitizer" $OUT/stdout.log | sed "s#$W/##" | sort | uniq -c | sort -rn | head -20 | cut -c1-240
  done
  exit 0
fi

echo "usage: $0 build|demos" >&2
exit 2
