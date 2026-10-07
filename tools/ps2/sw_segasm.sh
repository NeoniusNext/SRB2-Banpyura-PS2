#!/bin/bash
# usage: sw_segasm.sh OUT.s [extra gcc flags]  -- compile src/r_segs.c to EE assembly (8 s, no LTO) with the flags of a finished build, to read the
# code of the column loops without a full build (spills show as "(sp)" loads). BUILD_REPORT=<out dir>/build-report.json selects the build.
cd "$(dirname "$0")/../.."
OUT=$1; shift
python3 - "${BUILD_REPORT:-build/out-p16/build-report.json}" "$OUT" "$@" <<'PY'
import json, subprocess, sys
d = json.load(open(sys.argv[1]))
fl = [f for f in d['flags'] if f not in ('-flto', '-MMD', '-MP')]
cmd = ['/opt/ps2dev-x/ps2dev/ee/bin/mips64r5900el-ps2-elf-gcc'] + fl + sys.argv[3:] + ['-g1', '-S', '-o', sys.argv[2], 'src/r_segs.c']
print(subprocess.run(cmd, capture_output=True, text=True).stderr[-2000:])
PY
