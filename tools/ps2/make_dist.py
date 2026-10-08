"""Lays out the distribution folder of the PS2 port: dist/SRB2-PS2/ (OPT10-X).

usage: python3 tools/ps2/make_dist.py [--elf build/out/SRB2.ELF] [--pak build/pakx] [--out dist/SRB2-PS2] [--modules DIR] [--zip]
Result (everything the engine opens at run time, nothing else):
  SRB2.ELF            the engine (full configuration: Lua, UDMF, add-ons, limits, network, master server, software + GS hardware renderer)
  SRB2.PAK ZONES.PAK CHARS.PAK MUSIC.PAK   cooked game data (tools/ps2/cook.py from the user's own SRB2 2.2.15 files; not part of the repository)
  MODELS.PAK          optional, cooked 3D models (tools/ps2/cook_models.py from the user's own models/ folder and models.dat): taken from --pak when it is there
  FINEACON.DAT        arccos table of Lua's acos/asin (tools/ps2/gen_fineacon.py; without it acos is computed, slower)
  modules/*.irx       IOP drivers loaded on first use from <data>/modules (src/ps2/ps2_addons.c): memory card (mcman, mcserv) and USB mass storage (bdm,
                      bdmfs_fatfs, usbmass_bd); the other drivers (sio2man, padman, usbd, keyboard/mouse, audsrv, network) are embedded in the ELF
  autoload/           add-ons put here are loaded at every start (README.txt inside; an empty folder would not survive copying)
  ps2args.example     the start-up arguments file (rename to ps2args)
  README.txt          how to run on a console and in PCSX2 (Russian and English)
The pack files and the ELF are copied (hard-linked when the output is on the same disk). The folder is gitignored (/dist).
"""
import argparse
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SDK_IRX = Path(os.environ.get('PS2DEV') or '/opt/ps2dev-x/ps2dev') / 'ps2sdk/iop/irx'
MODULES = ['mcman.irx', 'mcserv.irx', 'bdm.irx', 'bdmfs_fatfs.irx', 'usbmass_bd.irx']  # ps2_addons.c: PrepareMC / PrepareUSB

README = r"""SRB2 for PlayStation 2 (Sonic Robo Blast 2 2.2.15, "Banpyura" port)
====================================================================

Содержимое / Contents
---------------------
SRB2.ELF                    движок / the engine
SRB2.PAK ZONES.PAK CHARS.PAK MUSIC.PAK   данные игры (упакованные файлы SRB2 2.2.15) / the game data (cooked from SRB2 2.2.15)
MODELS.PAK                  необязательно: 3D-модели (MD3) для gr_models On / optional: the 3D models for gr_models On (the game runs without it, with sprites)
FINEACON.DAT                таблица арккосинуса для Lua / arccos table for Lua
modules/*.irx               драйверы IOP: карта памяти и USB-накопитель, грузятся при первом обращении / IOP drivers for the memory card and USB storage, loaded on first use
autoload/                   сюда класть аддоны (.pk3 .wad .soc .lua): грузятся при каждом старте / add-ons put here load at every start
ps2args.example             файл аргументов запуска (переименуйте в ps2args) / start-up arguments (rename to ps2args)

Запуск на консоли / On a console
--------------------------------
1. Скопируйте всю папку SRB2-PS2 на USB-накопитель (FAT32), на жёсткий диск или карту памяти (нужно ~190 МБ; на карте памяти не поместится -
   ELF и данные держите на USB/HDD, а на карте памяти будут настройки и сохранения).
   Copy the whole SRB2-PS2 folder to a FAT32 USB stick or a hard disk (about 190 MB; a memory card is too small for the data: keep the ELF and the data
   on USB/HDD, the memory card holds settings and saves).
2. Запустите SRB2.ELF загрузчиком (uLaunchELF/wLaunchELF, OPL, FreeMcBoot): данные ищутся в папке, откуда запущен ELF.
   Start SRB2.ELF with a loader (uLaunchELF/wLaunchELF, OPL, FreeMcBoot); the data is looked up in the folder the ELF was started from.
3. Настройки и сохранения пишутся в папку .srb2 рядом с ELF (если устройство доступно для записи) - при запуске с диска/дисковода они идут на карту памяти mc0:.
   Settings and saves are written to the .srb2 folder next to the ELF (when that device is writable); when started from a disc they go to the memory card (mc0:).
4. Управление / Controls: DualShock 2 (порт 1 и 2: сплитскрин), крестик = прыжок/ввод, круг = отмена/Esc, Start = меню; клавиатура и мышь USB поддерживаются
   (клавиатура вводит адреса и чат; без клавиатуры есть экранная клавиатура: в меню ввода адреса нажмите Треугольник).
   DualShock 2 (ports 1 and 2: split screen), Cross = jump/Enter, Circle = Esc, Start = menu; a USB keyboard and mouse work; without a keyboard use the
   on-screen keyboard (Triangle in the address entry menu).

Запуск в PCSX2 / In PCSX2
-------------------------
File > Boot ELF > SRB2.ELF. Нужен BIOS PS2. Включите Settings > Advanced > "Enable Host Filesystem" (HostFs): игра читает данные из папки ELF через host:.
A PS2 BIOS is required. Enable Settings > Advanced > "Enable Host Filesystem": the data is read from the ELF folder through host:.
Рекомендуется / Recommended: 32 MB RAM (не включать "128 MB RAM"), программный или аппаратный GS.
Сеть в PCSX2 / Network in PCSX2: Settings > Network & HDD > Ethernet Device Enabled, Ethernet Device Type = Sockets (или PCAP), выберите сетевой адаптер;
гость получит адрес по DHCP. В режиме Sockets входящие датагарммы принимаются только от адресов, которым гость уже отправлял данные: для сервера на PS2
введите в консоли "punch <адрес клиента> <порт клиента>" (клиент: -clientport N) / in Sockets mode inbound datagrams pass only from addresses the guest has sent to:
on a PS2 server type "punch <client address> <client port>" in the console (client: -clientport N).

Аргументы запуска / Start-up arguments (ps2args, по одному на строку / one per line)
------------------------------------------------------------------------------------
-renderer Hardware       аппаратный рендерер GS (-zreserve не нужен). Не хватило памяти (большая карта, много текстур): игра сама переходит на software до конца карты
                         и пробует Hardware снова на следующей / GS hardware renderer (no -zreserve needed). When memory runs out (a very big map, many textures) the game
                         switches to the software renderer for the rest of that map and tries Hardware again on the next one
-ntsc | -pal | -480p     формат вывода / video output
-connect <адрес>         сразу подключиться к серверу / join a server at start-up
-server                  запустить сервер / start a server
-nousb | -nokbd | -nomouse | -noautoload
-skipintro -warp MAP01 -file addon.pk3 ...   обычные параметры SRB2 / usual SRB2 parameters
-ip A.B.C.D -netmask M -gateway G -dns D   фиксированный адрес вместо DHCP / a fixed address instead of DHCP

Аддоны / Add-ons
----------------
* меню Add-ons (главное меню) показывает папку .srb2/addons рядом с ELF; в Options > Data Options > Add-on Options... > Location выбираются карта памяти mc0:/SRB2,
  mc1:/SRB2 и USB mass:/SRB2 (драйверы загружаются при первом обращении) / the Add-ons menu lists .srb2/addons next to the ELF; Options > Data Options > Add-on Options... > Location chooses
  mc0:/SRB2, mc1:/SRB2 or mass:/SRB2;
* консоль: addfile <файл>; аргумент -file <файл>; папка autoload/ (и <home>/autoload, и -autoload <папка>) / console: addfile; -file; the autoload folder;
* поддерживаются pk3, wad, soc, lua; скины, Lua-скрипты (в т.ч. Lua-HUD), SOC, карты в UDMF; лимиты слотов (состояния, типы, звуки, цвета) как в PC-версии;
  pk3, wad, soc, lua are supported: skins, Lua scripts (including Lua HUD), SOC, UDMF maps; the slot limits (states, object types, sounds, colours) are those of the PC game;
* при подключении к серверу с аддонами файлы скачиваются автоматически (по игровому соединению или с HTTP-источника сервера) в .srb2/DOWNLOAD.
  when you join a server with add-ons the files are downloaded automatically (over the game connection or from the server's HTTP source) into .srb2/DOWNLOAD.

Сетевая игра и мастер-сервер / Network play and the master server
------------------------------------------------------------------
Multiplayer > Internet/LAN: список серверов мастер-сервера (по умолчанию http://ds.ms.srb2.org/MS/0, cvar masterserver) и вход в сервер ПК или PS2 прямо из списка;
Multiplayer > Specify server address: ввод адреса (экранная клавиатура или USB-клавиатура). Хост: Multiplayer > Internet/LAN... > Start регистрирует сервер в выбранной комнате.
Multiplayer > Internet/LAN: the master server list and joining a PC or PS2 server straight from it; "Specify server address" enters an address (on-screen or USB keyboard);
hosting from that menu registers the server in the chosen room.
PS2 <-> ПК и PS2 <-> PS2 совместимы по сети (протокол 2.2.15, режимы Co-op, Match, CTF, Race, Tag) / PS2 <-> PC and PS2 <-> PS2 play together (2.2.15 protocol).
Нужен сетевой адаптер PS2 (Ethernet) с DHCP; без него игра работает без сети / a PS2 Ethernet adapter and DHCP are needed; without them the game runs offline.

Лицензия / Licence
------------------
SRB2 - GNU GPL v2 (см. LICENSE.txt проекта). Игровые данные SRB2 принадлежат Sonic Team Junior; в архив они не входят - упакуйте свои файлы SRB2 2.2.15 (tools/ps2/cook.py).
SRB2 is GPL v2 software; the SRB2 2.2.15 game data belongs to Sonic Team Junior and must come from your own copy (tools/ps2/cook.py).
"""

AUTOLOAD_README = """Put add-ons here (.pk3 .wad .soc .lua): every file of this folder is loaded at every start, in name order.
Кладите сюда аддоны (.pk3 .wad .soc .lua): все файлы папки грузятся при каждом старте по порядку имён.
(-noautoload switches it off / отключается параметром -noautoload)
"""

PS2ARGS = """# Start-up arguments, one per line ('#' starts a comment). Rename this file to ps2args.
# -renderer Hardware
# +name "PS2 player test"     (player name; "+command" lines go to the console; or type: name "PS2 player test")
# -connect 192.168.1.10
# -ntsc
"""


def link_or_copy(src, dst):
    if dst.exists():
        dst.unlink()
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', default=str(ROOT / 'build/out/SRB2.ELF'))
    ap.add_argument('--pak', default=str(ROOT / 'build/pakx'), help='folder with the cooked packs (python3 tools/ps2/net_env.py makes build/pakx: links + FINEACON.DAT)')
    ap.add_argument('--out', default=str(ROOT / 'dist/SRB2-PS2'))
    ap.add_argument('--modules', default=str(SDK_IRX))
    ap.add_argument('--zip', action='store_true', help='also write <out>.zip')
    a = ap.parse_args()
    out, pak, elf = Path(a.out), Path(a.pak), Path(a.elf)
    if not elf.is_file() or elf.stat().st_size < 1_000_000:
        sys.exit(f'no engine ELF: {elf}')
    shutil.rmtree(out, ignore_errors=True)
    (out / 'modules').mkdir(parents=True)
    (out / 'autoload').mkdir()
    link_or_copy(elf, out / 'SRB2.ELF')
    packs = sorted(pak.glob('*.PAK'))
    if len(packs) < 4:
        sys.exit(f'{pak}: expected SRB2.PAK ZONES.PAK CHARS.PAK MUSIC.PAK (found {[p.name for p in packs]})')
    for p in packs:
        link_or_copy(p, out / p.name)
    if (pak / 'FINEACON.DAT').is_file():
        link_or_copy(pak / 'FINEACON.DAT', out / 'FINEACON.DAT')
    else:
        subprocess.run([sys.executable, str(ROOT / 'tools/ps2/gen_fineacon.py'), str(out)], check=True)
    for m in MODULES:
        src = Path(a.modules) / m
        if not src.is_file():
            sys.exit(f'missing IOP module {src}')
        shutil.copy2(src, out / 'modules' / m)
    (out / 'autoload/README.txt').write_text(AUTOLOAD_README, encoding='utf-8')
    (out / 'ps2args.example').write_text(PS2ARGS, encoding='utf-8')
    (out / 'README.txt').write_text(README, encoding='utf-8')
    total = 0
    for f in sorted(out.rglob('*')):
        if f.is_file():
            total += f.stat().st_size
            print(f'{f.relative_to(out).as_posix():34s} {f.stat().st_size:>12,d}')
    print(f'dist: {out}  ({total / 1048576:.1f} MiB)')
    if a.zip:
        zp = Path(str(out) + '.zip')
        with zipfile.ZipFile(zp, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
            for f in sorted(out.rglob('*')):
                if f.is_file():
                    z.write(f, Path(out.name) / f.relative_to(out))
        print('zip:', zp, f'{zp.stat().st_size / 1048576:.1f} MiB')


if __name__ == '__main__':
    main()
