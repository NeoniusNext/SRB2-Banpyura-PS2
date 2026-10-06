"""Isolated build tree for the strip checks: HEAD sources + a chosen set of working-tree files overlaid.

usage: strip_snapshot.py DEST [--overlay FILE ...]
The working tree is shared by several agents and may hold half-finished changes of other files (zone arena, GS renderer...).
This copies `git archive HEAD` of src/ and tools/ps2/ into DEST and then copies the listed working-tree files on top, so a
build in DEST (python DEST/tools/ps2/build.py with SRB2_PS2_OUT set) contains exactly HEAD + those files. Nothing in the
repository is touched. Default overlay: the files of the PS2-20 strip and the PS2-21 zone arena (see OVERLAY).
"""
import argparse
import io
import shutil
import subprocess
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OVERLAY = [
    'src/w_wad.c', 'src/w_wad.h', 'src/w_pack.c', 'src/w_pack.h', 'src/r_picformats.c', 'src/r_picformats.h',
    'src/filesrch.c', 'src/filesrch.h', 'src/p_setup.c', 'src/p_setup.h', 'src/m_menu.c', 'src/m_misc.c',
    'src/netcode/d_netcmd.c', 'src/netcode/commands.c', 'src/d_main.c', 'tools/ps2/build.py',
    'src/z_zone.c', 'src/z_zone.h', 'src/ps2/ps2_mem.c', 'src/ps2/ps2_mem.h', 'src/ps2/i_system.c', 'tools/ps2/sources.txt',  # PS2-21 arena
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dest')
    ap.add_argument('--overlay', nargs='*', default=OVERLAY)
    ap.add_argument('--full', action='store_true', help='all of HEAD (for the CMake host profile build), not just src/tools/CMakeLists')
    a = ap.parse_args()
    dest = Path(a.dest).resolve()
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    tar = subprocess.run(['git', 'archive', 'HEAD'] + ([] if a.full else ['src', 'tools/ps2', 'CMakeLists.txt']), cwd=ROOT, capture_output=True, check=True).stdout
    with tarfile.open(fileobj=io.BytesIO(tar)) as t:
        t.extractall(dest)
    for rel in a.overlay:
        (dest / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / rel, dest / rel)
    print(f'snapshot {dest}: HEAD + {len(a.overlay)} overlaid files')


if __name__ == '__main__':
    main()
