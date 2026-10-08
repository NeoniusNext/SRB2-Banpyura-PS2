#!/bin/bash
# Prepare the Linux cloud container for the PS2 port: ps2dev toolchain, PCSX2 AppImage (4 identical portable slots), SRB2 2.2.15 assets.
# usage: tools/ps2/setup_linux_env.sh <dir with the user's BIOS: *.bin [+ .mec .nvm]>
# The BIOS is copied ONLY into /opt/pcsx2/slot*/usr/bin/bios (outside the repository); never commit it.
# Network: needs github.com release downloads (ps2dev, PCSX2, STJr/SRB2 assets).
set -e
BIOSSRC=${1:?usage: setup_linux_env.sh <bios dir>}
PCSX2_VER=${PCSX2_VER:-v2.6.3}
mkdir -p /opt/dl-ps2dev /opt/dl-pcsx2 /opt/dl-srb2 /opt/srb2-assets

if [ ! -x /opt/ps2dev-x/ps2dev/ee/bin/mips64r5900el-ps2-elf-gcc ]; then
  curl -sSL -o /opt/dl-ps2dev/ps2dev.tar.gz https://github.com/ps2dev/ps2dev/releases/latest/download/ps2dev-ubuntu-latest.tar.gz
  mkdir -p /opt/ps2dev-x && tar xzf /opt/dl-ps2dev/ps2dev.tar.gz -C /opt/ps2dev-x
fi

if [ ! -d /opt/pcsx2/slot0/usr ]; then
  curl -sSL -o /opt/dl-pcsx2/pcsx2.AppImage https://github.com/PCSX2/pcsx2/releases/download/$PCSX2_VER/pcsx2-$PCSX2_VER-linux-appimage-x64-Qt.AppImage
  chmod +x /opt/dl-pcsx2/pcsx2.AppImage
  mkdir -p /opt/pcsx2/slot0 && (cd /opt/pcsx2/slot0 && /opt/dl-pcsx2/pcsx2.AppImage --appimage-extract >/dev/null && cp -a squashfs-root/. . && rm -rf squashfs-root)
fi

if [ ! -f /opt/srb2-assets/srb2.pk3 ]; then
  curl -sSL -o /opt/dl-srb2/srb2full.zip https://github.com/STJr/SRB2/releases/download/SRB2_release_2.2.15/SRB2-v2215-Full.zip
  unzip -q -o /opt/dl-srb2/srb2full.zip '*.pk3' models.dat -d /opt/srb2-assets
fi

# slot0 = standard 32 MB profile: ExtraMemory off, HostFs, SOFTWARE GS renderer, null audio, user BIOS
S0=/opt/pcsx2/slot0/usr/bin
mkdir -p $S0/bios $S0/inis
cp -n "$BIOSSRC"/* $S0/bios/
BIOSFILE=$(cd $S0/bios && ls *.bin | head -1)
cat > $S0/inis/PCSX2.ini <<EOF
[UI]
SettingsVersion = 1
SetupWizardIncomplete = false
HideMouseCursor = true
StartFullscreen = false
InhibitScreensaver = false
ConfirmShutdown = false
EnableDiscordPresence = false
[Folders]
Bios = bios
Snapshots = snaps
Savestates = sstates
MemoryCards = memcards
Logs = logs
Cheats = cheats
Patches = patches
Cache = cache
Textures = textures
InputProfiles = inputprofiles
Videos = videos
[Filenames]
BIOS = $BIOSFILE
[EmuCore]
HostFs = true
EnablePatches = false
EnableCheats = false
EnableFastBoot = true
EnableWideScreenPatches = false
EnableNoInterlacingPatches = false
[EmuCore/CPU]
FPU.DenormalsAreZero = true
[EmuCore/CPU/Recompiler]
EnableEE = true
EnableEECache = false
[EmuCore/Speedhacks]
EECycleRate = 0
EECycleSkip = 0
[EmuCore/GS]
Renderer = 13
VsyncEnable = 0
FrameLimitEnable = false
[SPU2/Output]
OutputModule = nullout
[Logging]
EnableSystemConsole = false
EnableFileLogging = true
EnableEEConsole = true
EnableIOPConsole = true
[MemoryCards]
Slot1_Enable = true
Slot1_Filename = Mcd001.ps2
Slot2_Enable = false
EOF
# slots 1..3: identical copies (tools/ps2/run_pcsx2.py takes the first free one)
for i in 1 2 3; do [ -d /opt/pcsx2/slot$i ] || cp -a /opt/pcsx2/slot0 /opt/pcsx2/slot$i; done
echo "ps2dev: $(/opt/ps2dev-x/ps2dev/ee/bin/mips64r5900el-ps2-elf-gcc --version | head -1)"
echo "pcsx2 slots: $(ls -d /opt/pcsx2/slot*/AppRun | wc -l); bios: $BIOSFILE"
echo "assets: $(ls /opt/srb2-assets)"
