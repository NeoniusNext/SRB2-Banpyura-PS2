"""Prove that the PS2 profile ELF has no PNG/zlib/ZIP/UDMF/remote/addon code (G1 'profile' criteria).

usage: strip_check.py ELF [--log symbols.log] [--baseline ELF0]
Runs nm on the ELF (clean PATH, like build.py), writes the full defined-symbol list to --log and checks every symbol
against the forbidden list. Exit code 0 only if no forbidden symbol is defined. --baseline prints the size/symbol delta.
"""
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

DEV = Path('D:/ps2dev')
NM = DEV / 'ee/bin/mips64r5900el-ps2-elf-nm.exe'
SIZE = DEV / 'ee/bin/mips64r5900el-ps2-elf-size.exe'
ENV = dict(os.environ, PATH=';'.join(str(p) for p in [DEV/'ee/bin', DEV/'bin', Path('C:/Windows/System32'), Path('C:/Windows')]))

# names and prefixes that must not exist in a single-player-only profile
FORBIDDEN_PREFIX = [
    'png_', 'inflate', 'deflate', 'zcalloc', 'zcfree', '_tr_', 'adler32', 'crc32', 'crc_', 'z_errmsg', 'zlibVersion',
    'zlibCompileFlags', 'uncompress', 'compress_block',
    'Picture_PNG', 'M_SavePNG', 'TextmapParse', 'P_WriteTextmap', 'P_LoadTextmap', 'ParseTextmap', 'TextmapCount',
    'ResFindSignature', 'ResGetLumpsZip', 'ResGetLumpsWad', 'ResGetLumpsFolder', 'W_InitFolder', 'W_VerifyPK3',
    'W_VerifyWAD', 'W_VerifyFile', 'W_VerifyFileMD5', 'getdirectoryfiles', 'M_Addons', 'M_DrawAddons', 'M_HandleAddons',
    'M_LocalAddons', 'Command_Addfile', 'Command_Addfolder', 'Command_ListWADS', 'Command_SaveAddons', 'Got_Addfile',
    'Got_Addfolder', 'Got_RequestAddfile', 'Got_RequestAddfolder', 'P_AddFolder', 'preparefilemenu', 'closefilemenu',
    'searchfilemenu', 'Addons_option_Onchange', 'cv_addons_', 'MISC_AddonsDef', 'OP_AddonsOptions', 'zerr',
    'P_LoadAddon', 'P_AddWadFileLocal', 'FindFolder', 'Command_RunSOC', 'Got_RunSOC',
    'Command_Changepassword', 'Command_Clearpassword', 'Command_Login', 'Command_Verify', 'Command_RemoveAdmin',
    'Command_MotD', 'Got_MotD', 'Got_Verification', 'Got_Removal',
    'Command_ServerTeamChange', 'Command_MutePlayer', 'Command_UnmutePlayer', 'MutePlayer', 'Got_MutePlayer',
    'Command_Clearscores', 'Got_Clearscores',
    'Command_connect', 'Command_Kick', 'Command_Ban', 'Command_ClearBans', 'Command_ShowBan', 'Command_ReloadBan',
    'Command_set_http_login', 'Command_list_http_logins', 'Command_ResendGamestate', 'Ban_', 'D_SaveBan',
    'M_Connect', 'M_Rejoin', 'M_RoomMenu', 'M_ChooseRoom', 'M_StartServer', 'M_StartSplitServer', 'M_ServerOptions',
    'M_HandleConnectIP', 'M_HandleServerPage', 'M_DrawConnect', 'M_DrawRejoin', 'M_DrawRoomMenu', 'M_DrawMPMainMenu',
    'MP_MainDef', 'MP_ServerDef', 'MP_ConnectDef', 'MP_RejoinDef', 'MP_RoomDef', 'MP_SplitServerDef', 'OP_ServerOptionsDef',
]
# code-only prefixes (data tables with these names are cvar value lists, not code)
FORBIDDEN_TEXT_PREFIX = ['apng_', 'zlib_']
ALLOWED = set()
# W_VerifyNMUSlumps is kept on purpose: on the profile it only reads the pack header flag (no ZIP/WAD scan)


def nm(elf):
    p = subprocess.run([str(NM), '--defined-only', str(elf)], env=ENV, capture_output=True, text=True, check=True)
    out = p.stdout
    syms = []
    for line in out.splitlines():
        m = re.match(r'^([0-9a-fA-F]+) (\S) (\S+)$', line)
        if m:
            syms.append((m.group(1), m.group(2), m.group(3)))
    if not syms:
        raise RuntimeError(f'nm produced no defined symbols for {elf}: {p.stderr}')
    return syms


def size_line(elf):
    return subprocess.run([str(SIZE), str(elf)], env=ENV, capture_output=True, text=True, check=True).stdout.strip()


def forbidden(syms):
    bad = []
    for addr, typ, name in syms:
        if typ.lower() not in 'tdbrgs' or name in ALLOWED:
            continue
        if any(name.startswith(p) for p in FORBIDDEN_PREFIX) or                 (typ in 'Tt' and any(name.startswith(p) for p in FORBIDDEN_TEXT_PREFIX)):
            bad.append((addr, typ, name))
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('elf')
    ap.add_argument('--log')
    ap.add_argument('--baseline')
    a = ap.parse_args()
    elf = Path(a.elf)
    syms = nm(elf)
    bad = forbidden(syms)
    lines = [f'ELF {elf} {elf.stat().st_size} bytes', size_line(elf), f'defined symbols: {len(syms)}',
             f'forbidden symbols: {len(bad)}']
    lines += [f'  FORBIDDEN {t} {n} @{ad}' for ad, t, n in bad]
    if a.baseline:
        b = Path(a.baseline)
        bs = nm(b)
        names_now = {n for _, _, n in syms}
        gone = sorted(n for _, _, n in bs if n not in names_now)
        lines += [f'baseline {b} {b.stat().st_size} bytes -> {elf.stat().st_size} bytes (delta {elf.stat().st_size - b.stat().st_size})',
                  f'symbols removed vs baseline: {len(gone)}']
        lines += ['  - ' + n for n in gone]
    print('\n'.join(lines[:60]))
    if a.log:
        full = list(lines)
        full.append('--- nm --defined-only (address type name) ---')
        full += [f'{ad} {t} {n}' for ad, t, n in syms]
        Path(a.log).write_text('\n'.join(full) + '\n', encoding='utf-8')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
