#!/bin/bash
# The host A/B set of the OPT10-SW work in one go (about 10 minutes): the tree as it is against the baseline (build/host-base/out), with the
# shadow checks of the exact-by-construction fast paths, and the negative controls that must be seen (frames differ or the shadow check stops
# the game). The host build uses the original (not PS2_OPT_SLOPE) slope code, so the double slope vectors do not reach the picture there:
# PS2-166 is covered by its shadow check (neg9) instead of by the frames.
#   usage: sw_hostall.sh [demos...]      -> build/hostall.log
cd "$(dirname "$0")/../.."
DEMOS=${@:-DEMO_001 DEMO_002 DEMO_003 DEMO_004}
export JOBS=${JOBS:-2}
rc=0
run() { tools/ps2/host_variant.sh "$@" || rc=1; }
run cand "-DPS2_BSPCHECK -DPS2_NODECHECK" same $DEMOS            # tree as is, with the shadow checks (R_PointInSubsector start and memo, slope lerp values)
run neg9 "-DPS2_NODECHECK -DPS2_NEGCTL=9" differ DEMO_003       # PS2-166: moved dynamic slope values are not followed -> shadow check stops the game
run neg10 "-DPS2_NEGCTL=10" differ DEMO_003                     # PS2-167: the subsector memo compares x only
run neg11 "-DPS2_NEGCTL=11" differ $DEMOS                       # PS2-170: the sprite clip scan stops one column early
run neg13 "-DPS2_NEGCTL=13" differ DEMO_003                     # PS2-172: the sector point memo compares x only
run neg14 "-DPS2_NEGCTL=14" differ DEMO_001 DEMO_002 DEMO_003              # PS2-175: P_LookForPlayers does not set the target
run neg15 "-DPS2_NEGCTL=15" differ DEMO_001 DEMO_003                  # PS2-176: PIT_DoCheckThing drops things that still overlap
[ $rc = 0 ] && echo "SW HOST ALL: OK" || echo "SW HOST ALL: FAILED"
exit $rc
