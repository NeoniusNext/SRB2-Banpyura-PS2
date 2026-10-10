#!/bin/bash
# Prepare the Linux cloud container for the PS2 port when github.com release downloads are blocked (OPT12, 2026-10-08).
# NOTE: written down after doing the same steps by hand in the OPT12 session; the script itself was not re-run end to end.
# Differs from setup_linux_env.sh in where things come from:
#   ps2dev  -> Docker Hub image ps2dev/ps2dev (Alpine/musl; the layers are unpacked into /opt/ps2root, /opt/ps2dev-x/ps2dev links into it)
#   PCSX2   -> Flathub net.pcsx2.PCSX2 (flatpak --system install; slot0 = copy of the app with its interpreter/RPATH patched to the KDE runtime)
#   SRB2 2.2.15 data -> archive.org item srb-2-v-2215-full (SRB2-v2215-Full.zip; sha256 of the four pk3 files match docs/GATES/g1/report.json)
# usage: tools/ps2/setup_linux_env_nogithub.sh <dir with the user's BIOS: *.bin [+ .mec .nvm]>
# The BIOS is copied ONLY into /opt/pcsx2/slot*/usr/bin/bios (outside the repository); never commit it.
set -e
BIOSSRC=${1:?usage: setup_linux_env_nogithub.sh <bios dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
DL=/opt/dl; mkdir -p $DL

# 1. ps2dev from the Docker Hub image (no docker daemon needed: the registry API is enough)
if [ ! -x /opt/ps2dev-x/ps2dev/ee/bin/mips64r5900el-ps2-elf-gcc ]; then
  TOK=$(curl -sS "https://auth.docker.io/token?service=registry.docker.io&scope=repository:ps2dev/ps2dev:pull" | python3 -c "import sys,json;print(json.load(sys.stdin)['token'])")
  IDX=$(curl -sS -H "Authorization: Bearer $TOK" -H "Accept: application/vnd.oci.image.index.v1+json" https://registry-1.docker.io/v2/ps2dev/ps2dev/manifests/latest)
  MAN=$(echo "$IDX" | python3 -c "import sys,json;d=json.load(sys.stdin);print([m['digest'] for m in d['manifests'] if m['platform']['architecture']=='amd64' and m['platform']['os']=='linux'][0])")
  curl -sS -H "Authorization: Bearer $TOK" -H "Accept: application/vnd.oci.image.manifest.v1+json" https://registry-1.docker.io/v2/ps2dev/ps2dev/manifests/$MAN > $DL/ps2dev-manifest.json
  mkdir -p /opt/ps2root
  for L in $(python3 -c "import json;print(' '.join(l['digest'] for l in json.load(open('$DL/ps2dev-manifest.json'))['layers']))"); do
    curl -sS -L -H "Authorization: Bearer $TOK" -o $DL/${L#sha256:}.tgz https://registry-1.docker.io/v2/ps2dev/ps2dev/blobs/$L
    tar -xzf $DL/${L#sha256:}.tgz -C /opt/ps2root
  done
  ln -sf /opt/ps2root/lib/ld-musl-x86_64.so.1 /lib/ld-musl-x86_64.so.1
  printf '/opt/ps2root/lib:/opt/ps2root/usr/lib\n' > /etc/ld-musl-x86_64.path
  mkdir -p /opt/ps2dev-x && ln -sfn /opt/ps2root/usr/local/ps2dev /opt/ps2dev-x/ps2dev
fi
/opt/ps2dev-x/ps2dev/ee/bin/mips64r5900el-ps2-elf-gcc --version | head -1

# 2. host packages (Vulkan/GL software drivers for the emulator window, flatpak, patchelf, PC build dependencies)
DEBIAN_FRONTEND=noninteractive apt-get update -qq
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq flatpak ostree bubblewrap patchelf xvfb x11-utils mesa-vulkan-drivers libgl1-mesa-dri \
  libpng-dev zlib1g-dev libsdl2-dev libsdl2-mixer-dev libcurl4-openssl-dev libgme-dev libopenmpt-dev ninja-build imagemagick
pip install lz4 2>/dev/null || pip install --break-system-packages lz4

# 3. PCSX2 from Flathub -> /opt/pcsx2/slot0 (AppImage-like tree, portable mode)
FP=/var/lib/flatpak
if [ ! -d $FP/app/net.pcsx2.PCSX2 ]; then
  flatpak remote-add --if-not-exists --system flathub https://dl.flathub.org/repo/flathub.flatpakrepo
  flatpak install -y --system --noninteractive flathub net.pcsx2.PCSX2
fi
APP=$FP/app/net.pcsx2.PCSX2/x86_64/stable/active/files
RT=$(readlink -f $FP/runtime/org.kde.Platform/x86_64/6.10/active/files)
GL=$(readlink -f $FP/runtime/org.freedesktop.Platform.GL.default/x86_64/25.08-extra/active/files)
S=/opt/pcsx2/slot0
if [ ! -x $S/AppRun ]; then
  mkdir -p $S/usr/bin $S/usr/lib $S/usr/bin/bios $S/usr/bin/inis
  cp -a $APP/bin/. $S/usr/bin/ && cp -a $APP/lib/. $S/usr/lib/ && touch $S/usr/bin/portable.txt
  # run the app against the runtime's own glibc/Qt: new interpreter + DT_RPATH (not RUNPATH, so the libraries it loads inherit it)
  patchelf --set-interpreter $RT/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 $S/usr/bin/pcsx2-qt
  patchelf --force-rpath --set-rpath "$S/usr/lib:$RT/lib/x86_64-linux-gnu:$RT/lib:$GL/lib/x86_64-linux-gnu:$GL/lib" $S/usr/bin/pcsx2-qt
  cat > $S/AppRun <<EOF
#!/bin/bash
HERE="\$(dirname "\$(readlink -f "\$0")")"
export QT_PLUGIN_PATH=$RT/lib/x86_64-linux-gnu/qt6/plugins:\$HERE/usr/bin
export QML2_IMPORT_PATH=$RT/lib/x86_64-linux-gnu/qt6/qml
export XDG_DATA_DIRS=$RT/share:/usr/share
export LIBGL_ALWAYS_SOFTWARE=1
export VK_ICD_FILENAMES=$GL/lib/vulkan/icd.d/lvp_icd.x86_64.json
export LIBGL_DRIVERS_PATH=$GL/lib/dri
export __EGL_VENDOR_LIBRARY_DIRS=$GL/share/glvnd/egl_vendor.d
exec \$HERE/usr/bin/pcsx2-qt "\$@"
EOF
  chmod +x $S/AppRun
fi
cp -n "$BIOSSRC"/*.bin "$BIOSSRC"/*.mec "$BIOSSRC"/*.nvm $S/usr/bin/bios/ 2>/dev/null || true
BIOSFILE=$(cd $S/usr/bin/bios && ls *.bin | head -1)
# PCSX2.ini of the standard 32 MB profile: the [EmuCore] ... sections of setup_linux_env.sh (software GS, HostFs, null audio)
awk '/^cat > \$S0\/inis\/PCSX2.ini/{f=1;next} /^EOF/{f=0} f' "$HERE/setup_linux_env.sh" | sed "s/\$BIOSFILE/$BIOSFILE/" > $S/usr/bin/inis/PCSX2.ini
for i in 1 2 3; do [ -d /opt/pcsx2/slot$i ] || cp -a /opt/pcsx2/slot0 /opt/pcsx2/slot$i; done

# 4. SRB2 2.2.15 data
if [ ! -f /opt/srb2-assets/srb2.pk3 ]; then
  curl -sS -L -o $DL/SRB2-v2215-Full.zip https://archive.org/download/srb-2-v-2215-full/SRB2-v2215-Full.zip
  mkdir -p /opt/srb2-assets && unzip -q -o $DL/SRB2-v2215-Full.zip '*.pk3' models.dat 'models/*' -d /opt/srb2-assets
  cp /opt/srb2-assets/models.dat /opt/srb2-assets/models/models.dat 2>/dev/null || true
fi
echo "ps2dev: $(/opt/ps2dev-x/ps2dev/ee/bin/mips64r5900el-ps2-elf-gcc --version | head -1)"
echo "pcsx2 slots: $(ls -d /opt/pcsx2/slot*/AppRun | wc -l); bios: $BIOSFILE"
echo "assets: $(ls /opt/srb2-assets | tr '\n' ' ')"
echo "next: python3 tools/ps2/cook.py --src /opt/srb2-assets --out build/pak; python3 tools/ps2/gen_fineacon.py build/pak"
echo "      tools/ps2/make_golden_linux.sh (needs build/pc-golden: cmake -S . -B build/pc-golden -G Ninja -DSRB2_CONFIG_PS2REF=ON -DSRB2_CONFIG_STATIC_STDLIB=OFF -DSRB2_CONFIG_USE_GME=OFF -DSRB2_CONFIG_USE_CURL=OFF)"
