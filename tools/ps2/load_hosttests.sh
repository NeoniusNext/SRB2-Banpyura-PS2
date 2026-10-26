#!/bin/sh
# OPT12-LOAD: every host-side proof of the loading work, one line each (exit code != 0 when one fails).
# usage: tools/ps2/load_hosttests.sh [--quick]     (run in the worktree; needs build/hosttest/TEXTMAP.txt = the TEXTMAP of build/addons/UM.pk3 and build/hosttest/PLAYPAL.lmp)
cd "$(dirname "$0")/../.." || exit 1
mkdir -p build/hosttest
rc=0
check() { name=$1; shift; if "$@" > build/hosttest/last.txt 2>&1; then echo "OK    $name: $(tail -1 build/hosttest/last.txt)"; else echo "FAIL  $name"; tail -5 build/hosttest/last.txt; rc=1; fi; }
if [ ! -f build/hosttest/TEXTMAP.txt ] && [ -f build/addons/UM.pk3 ]; then unzip -p build/addons/UM.pk3 Maps/MAP99.wad > build/hosttest/UM_MAP99.wad; python3 - <<'PY'
import struct
d = open('build/hosttest/UM_MAP99.wad', 'rb').read()
n, ofs = struct.unpack('<II', d[4:12])
for i in range(n):
    p, s, name = struct.unpack('<II8s', d[ofs + i * 16: ofs + i * 16 + 16])
    if name.rstrip(b'\0') == b'TEXTMAP':
        open('build/hosttest/TEXTMAP.txt', 'wb').write(d[p:p + s])
PY
fi
check "tokenizer / pair reader / block scan (original vs fast)" python3 tools/ps2/tokenizer_hosttest.py build/hosttest/TEXTMAP.txt
check "M_GetToken (original vs fast)" python3 tools/ps2/gettoken_hosttest.py
cc -O2 -Wall -I src/ps2 -o build/hosttest/dbl_hosttest tools/ps2/dbl_hosttest.c -lm && check "exact double steps of the light table" build/hosttest/dbl_hosttest build/hosttest/PLAYPAL.lmp $([ "$1" = "--quick" ] && echo 2000000 || echo 20000000)
cc -O2 -DPS2_NEAREST_HOST -I src/ps2 -o build/hosttest/nearest_hosttest tools/ps2/nearest_hosttest.c src/ps2/ps2_nearest.c && check "nearest palette colour, all 2^24 colours x 9 palettes" build/hosttest/nearest_hosttest build/hosttest/PLAYPAL.lmp
check "SRP2 packs against the pk3 (verify_pack.py)" python3 tools/ps2/verify_pack.py --src /opt/srb2-assets --pak build/pak2
check "SRP2 reader tests (test_pack_reader.py)" python3 tools/ps2/test_pack_reader.py
exit $rc
