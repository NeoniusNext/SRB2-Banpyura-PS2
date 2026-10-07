#!/bin/bash
# A host profile variant (extra compile flags) against the baseline host run (build/host-base/out), see host_demo.sh/host_ab.sh.
#  negative control: expect=differ, e.g.  host_variant.sh neg7 "-DPS2_NEGCTL=7" differ DEMO_003 DEMO_004
#     (a deliberately wrong variant of a fast path, the #if PS2_NEGCTL == N blocks of the sources: the harness must see different frames)
#  stress variant:   expect=same,   e.g.  host_variant.sh stress1 "-DPS2_NODE_ORDER_MAX=4096" same
#     (a correct implementation in a mode that exercises rare paths, e.g. constant relabelling of the draw node list: frames must be identical)
# usage: host_variant.sh NAME "FLAGS" same|differ [DEMO_00n ...]
set -e
NAME=$1; FLAGS=$2; EXPECT=$3; shift 3
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
B=build/host-$NAME
# (re)configure when the build directory is new or was made for other flags
if [ ! -f $B/b/build.ninja ] || [ "$(cat $B/flags.stamp 2>/dev/null)" != "$FLAGS" ]; then
  mkdir -p $B
  cmake -S . -B $B/b -G Ninja -DCMAKE_BUILD_TYPE=Release -DSRB2_CONFIG_STATIC_STDLIB=OFF -DSRB2_CONFIG_USE_GME=OFF -DSRB2_CONFIG_HWRENDER=OFF -DSRB2_CONFIG_PS2REF=ON \
    -DSRB2_CONFIG_PS2PROFILE=ON "-DSRB2_PS2_NO=lua;udmf;addons;limits;zippng" -DSRB2_HOST_PROFILE_LZ4_SOURCE=$ROOT/tools/ps2/host_lz4_shim.c \
    -DSRB2_HOST_PROFILE_LZ4_INCLUDE_DIR=/opt/ps2dev-x/ps2dev/ps2sdk/ports/include "-DCMAKE_C_FLAGS=-DPS2_NOOPT_SLOPE -DPS2_NOOPT_SEGS -fwrapv $FLAGS" > /dev/null
  printf "%s" "$FLAGS" > $B/flags.stamp
fi
ninja -C $B/b -j${JOBS:-3} 2>&1 | tail -1
EXE=$(ls $B/b/bin/*)
DEMOS=${@:-DEMO_001 DEMO_002 DEMO_003 DEMO_004}
tools/ps2/host_demo.sh "$EXE" $B/out $DEMOS
rc=0
for D in $DEMOS; do
  if cmp -s build/host-base/out/$D/tics.csv $B/out/$D/tics.csv && cmp -s build/host-base/out/$D/allhash.csv $B/out/$D/allhash.csv; then
    echo "VARIANT $NAME $D: tics and all frame hashes identical"; [ $EXPECT = differ ] && rc=1
  else
    echo "VARIANT $NAME $D: differs from the baseline"; [ $EXPECT = same ] && rc=1
  fi
done
[ $rc = 0 ] && echo "VARIANT $NAME ($FLAGS): as expected ($EXPECT)" || echo "VARIANT $NAME ($FLAGS): NOT AS EXPECTED (expected $EXPECT)"
exit $rc
