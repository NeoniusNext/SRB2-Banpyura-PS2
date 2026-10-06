"""OPT9-M: build the standalone USB HID probe (tools/ps2/usb_probe.c) next to copies of the IRX files it loads from host:.
usage: build_usb_probe.py [OUTDIR]   (default build/opt9-m/probe) -> OUTDIR/USBPROBE.ELF"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEV = Path('D:/ps2dev')
SDK = DEV / 'ps2sdk'
OUT = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'build/opt9-m/probe'
OUT.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, PS2DEV=str(DEV), PS2SDK=str(SDK))
env['PATH'] = ';'.join(str(p) for p in [DEV/'ee/bin', DEV/'iop/bin', DEV/'bin', Path('C:/Windows/System32'), Path('C:/Windows')])
cc = DEV/'ee/bin/mips64r5900el-ps2-elf-gcc.exe'
cmd = [str(cc), '-D_EE', '-G0', '-O2', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
       '-I'+str(SDK/'ee/include'), '-I'+str(SDK/'common/include'),
       '-T'+str(SDK/'ee/startup/linkfile'), '-L'+str(SDK/'ee/lib'),
       '-Wl,-zmax-page-size=128', str(ROOT/'tools/ps2/usb_probe.c'), '-o', str(OUT/'USBPROBE.ELF'),
       '-lkbd', '-lmouse', '-ldebug', '-lpatches', '-lm']
p = subprocess.run(cmd, env=env, capture_output=True, text=True)
if p.returncode:
    print(p.stdout + p.stderr)
    raise SystemExit(p.returncode)
for name in ['usbd', 'ps2kbd', 'ps2mouse']:
    shutil.copy2(SDK/'iop/irx'/f'{name}.irx', OUT)
print('built', OUT/'USBPROBE.ELF', (OUT/'USBPROBE.ELF').stat().st_size)
