"""Linux locations shared by the network test tools (net_specs*.py, net_session.py): OPT10-X.

 * emulators: /opt/pcsx2/net1, /opt/pcsx2/net2 (DEV9 Ethernet, Sockets API on eth0), names are what net_session.py "emu" takes;
 * PC SRB2 for the network tests: build/pc-net (this source tree with -DNETSYNC_DIAG so that "-netsync" prints NETSYNC lines):
     cmake -S . -B build/pc-net -G Ninja -DCMAKE_BUILD_TYPE=Release -DSRB2_CONFIG_STATIC_STDLIB=OFF -DSRB2_CONFIG_USE_GME=OFF \
           -DSRB2_CONFIG_HWRENDER=OFF -DCMAKE_C_FLAGS=-DNETSYNC_DIAG && ninja -C build/pc-net -j2 SRB2SDL2
   (the dedicated server of build/pc-ref has no -netsync hook; it serves for the plain checks);
 * packs: build/pak (the cooked packs of the main tree, see tools/ps2/wt_setup.sh).
"""
import glob
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EMU1 = 'net1'
EMU2 = 'net2'
PAK = 'build/pakx'  # links of the cooked packs of the main tree + FINEACON.DAT (python3 tools/ps2/net_env.py makes it)
BASE = 'build/opt10-x'


def pc_exe(kind='pc-net'):
    """the PC engine binary of build/<kind>/bin (its name carries the branch name)"""
    hits = sorted(x for x in glob.glob(str(ROOT / 'build' / kind / 'bin' / '*')) if Path(x).is_file())
    if not hits:
        raise SystemExit(f'no PC engine in build/{kind}/bin: see tools/ps2/net_env.py')
    return Path(hits[0]).relative_to(ROOT).as_posix() if Path(hits[0]).is_relative_to(ROOT) else hits[0]


def make_pak(src='/home/user/SRB2-Banpyura-PS2/build/pak'):
    """build/pakx = hard links of the cooked packs of the main tree (read-only there) + FINEACON.DAT (the arccos table Lua's acos/asin read, tools/ps2/gen_fineacon.py)"""
    import os
    import shutil
    import subprocess
    import sys
    dst = ROOT / PAK
    dst.mkdir(parents=True, exist_ok=True)
    for f in Path(src).iterdir():
        if not (dst / f.name).exists():
            try:
                os.link(f, dst / f.name)
            except OSError:
                shutil.copy2(f, dst / f.name)
    subprocess.run([sys.executable, str(ROOT / 'tools/ps2/gen_fineacon.py'), str(dst)], check=True)


if __name__ == '__main__':
    make_pak()
