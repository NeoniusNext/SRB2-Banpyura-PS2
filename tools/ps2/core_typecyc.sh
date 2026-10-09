#!/bin/bash
# OPT12-CORE: cycles of P_MobjThinker per mobj type on the native host profile (x86 rdtsc; ranking, not EE cycles). usage: core_typecyc.sh DEMO_00n [FLAGS]
# builds build/host-typecyc with -DPS2_TYPECYC (host_variant.sh does the configure/build/run; the comparison against the base is ignored here)
set -e
cd "$(dirname "$0")/../.."
D=${1:-DEMO_003}; shift || true
JOBS=${JOBS:-2} tools/ps2/host_variant.sh typecyc "-DPS2_TYPECYC $*" same $D > /dev/null 2>&1 || true
grep -h "TYPECYC" build/host-typecyc/out/$D/stdout.log | head -60
