"""PNG lumps -> "cooked pictures" for the PS2 profile (PS2-20). Library used by cook.py, verify_pack.py and strip_pics_test.py.

The engine on PS2 has no libpng/zlib. The 15 PNG lumps of srb2.pk3 are decoded at cook time by the ORIGINAL engine code
(src/r_picformats.c built with libpng on the host: Picture_PNGConvert, tools/ps2/strip_pics_host.c) and stored as

    8 bytes   0x89 'S' 'R' 'P' 'I' 'C' 0x0D 0x0A         (marker, takes the place of the PNG signature)
    n bytes   Doom patch (softwarepatch_t, little endian) with the pixels, transparency and offsets of the conversion

r_picformats.c (PS2_PROFILE) maps Picture_IsLumpPNG/Picture_PNGConvert/Picture_PNGDimensions to the cooked-picture
functions, so the texture/patch code keeps working on these lumps unchanged.

Also here: an independent pure-Python model of the original decoder (PNG parse + nearest palette colour with the
engine's first-colour-per-RGB565-bucket memo) used by verify_pack.py as a second opinion.
"""
import struct
import subprocess
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MARKER = b'\x89SRPIC\r\n'
PNG_SIG = b'\x89PNG\r\n\x1a\n'
TRANSPARENT = 0x100
VCVARS = Path(r'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat')
DEPS = Path('D:/AI-projects/SRB2B-plus/build/deps/vcpkg_installed/x64-windows')


# ---- Doom patch (tall-patch aware) --------------------------------------------------------------------------------

def _enc_top(t, prev):
    """Byte b that the engine's decoders turn into absolute top t after a post with absolute top prev (None = first), or None."""
    p = 0 if prev is None else prev
    if t == 0 and p == 0:
        return 0
    if 0 < t < 255 and t > p:
        return t
    b = t - p
    if 1 <= b <= p and b < 255:        # decoder: b <= prev -> b + prev
        return b
    return None


def encode_doom_patch(width, height, left, top, pix):
    """pix: row-major list of width*height values, 0..255 = palette index, 0x100 = transparent."""
    cols = []
    for x in range(width):
        out = bytearray()
        prev = None
        y = 0
        while y < height:
            if pix[y * width + x] == TRANSPARENT:
                y += 1
                continue
            end = y
            while end < height and pix[end * width + x] != TRANSPARENT:
                end += 1
            # run [y, end): split into posts of at most 255 pixels whose tops are all encodable
            while y < end:
                b = _enc_top(y, prev)
                while b is None:                       # empty post to move the decoder state closer
                    p = 0 if prev is None else prev
                    d = min(y - 1, p + min(p, 254) if p else 254)
                    assert prev is None or d > prev
                    out += bytes([_enc_top(d, prev), 0, 0, 0])
                    prev = d
                    b = _enc_top(y, prev)
                n = min(255, end - y)
                while y + n < end and _enc_top(y + n, y) is None and n > 1:
                    n -= 1
                out += bytes([b, n, 0]) + bytes(pix[(y + k) * width + x] for k in range(n)) + b'\x00'
                prev = y
                y += n
        out.append(0xFF)
        cols.append(bytes(out))
    offs = []
    pos = 8 + 4 * width
    for c in cols:
        offs.append(pos)
        pos += len(c)
    return struct.pack('<hhhh', width, height, left, top) + struct.pack(f'<{width}I', *offs) + b''.join(cols)


def decode_doom_patch(data):
    """Mirror of the engine's Patch_MakeColumns/Picture_GetPatchPixel. Returns (w, h, left, top, row-major values)."""
    width, height, left, top = struct.unpack_from('<hhhh', data, 0)
    offs = struct.unpack_from(f'<{width}I', data, 8)
    pix = [TRANSPARENT] * (width * height)
    for x in range(width):
        p = offs[x]
        prev = 0
        while data[p] != 0xFF:
            t, n = data[p], data[p + 1]
            if t <= prev:
                t += prev
            prev = t
            for k in range(n):
                if t + k < height:
                    pix[(t + k) * width + x] = data[p + 3 + k]
                else:
                    raise ValueError('post leaves the patch')
            p += n + 4
    return width, height, left, top, pix


def cooked_from_matrix(width, height, left, top, pix):
    return MARKER + encode_doom_patch(width, height, left, top, pix)


def is_cooked(data):
    return len(data) >= 16 and data[:8] == MARKER


def read_matrix(path):
    raw = Path(path).read_bytes()
    w, h, left, top = struct.unpack_from('<4i', raw, 0)
    n = w * h
    pix = list(struct.unpack_from(f'<{n}H', raw, 16))
    return w, h, left, top, pix


# ---- MSVC host tools built from the real engine sources ---------------------------------------------------------------

def _slice(text, start, end):
    if text.count(start) != 1 or text.count(end) != 1:
        raise RuntimeError(f'source anchors changed: {start!r} / {end!r}')
    a = text.index(start)
    return text[a:text.index(end, a)]


def write_stub_sources(work):
    """Verbatim slices of NearestPaletteColor, InitColorLUT/GetColorLUT and Patch_Create* (the stub .inc files)."""
    rd = (ROOT / 'src/r_data.c').read_text(encoding='utf-8', errors='replace').replace('\r\n', '\n')
    vv = (ROOT / 'src/v_video.c').read_text(encoding='utf-8', errors='replace').replace('\r\n', '\n')
    rp = (ROOT / 'src/r_patch.c').read_text(encoding='utf-8', errors='replace').replace('\r\n', '\n')
    (work / 'strip_nearest.inc').write_text(_slice(rd, 'UINT8 NearestPaletteColor(UINT8 r, UINT8 g, UINT8 b, RGBA_t *palette)', '#ifdef EXTRACOLORMAPLUMPS\nconst char *R_NameForColormap'), encoding='utf-8')
    (work / 'strip_clut.inc').write_text(_slice(vv, '// Generates a RGB565 color look-up table\nvoid InitColorLUT', '// V_Init\n// old software stuff'), encoding='utf-8')
    (work / 'strip_patch.inc').write_text(_slice(rp, 'patch_t *Patch_Create(INT16 width, INT16 height)', '//\n// Frees a patch from memory.'), encoding='utf-8')


def _build_tool_posix(work, name, defines, sources, libs):
    """Linux/macOS host tool: gcc + the system libpng/zlib (the same engine code as the MSVC build)."""
    import os
    exe = work / name
    gen = Path(os.environ.get('SRB2_PS2_HOST_GEN', str(ROOT / 'build/host-gen')))
    gen.mkdir(parents=True, exist_ok=True)
    if not (gen / 'config.h').exists():  # generated config.h (cmake/Comptime.cmake equivalent), same text the PS2 build uses
        text = (ROOT / 'src/config.h.in').read_text()
        for k, v in {'${SRB2_COMP_REVISION}': 'host', '${SRB2_COMP_BRANCH}': 'host', '${SRB2_COMP_NOTE}': 'host', '${CMAKE_BUILD_TYPE}': 'Release'}.items():
            text = text.replace(k, v)
        text = text.replace('#cmakedefine SRB2_COMP_UNCOMMITTED', '/* clean */').replace('#cmakedefine01 SRB2_COMP_OPTIMIZED', '#define SRB2_COMP_OPTIMIZED 1')
        (gen / 'config.h').write_text(text)
    inc = [str(ROOT / 'src'), str(gen), str(ROOT / 'tools/ps2'), str(work)]
    compat = work / 'strip_posix_compat.c'  # MSVC CRT functions the engine uses (strupr/strlwr are not in glibc)
    compat.write_text('#include <ctype.h>\nchar *strupr(char *s){char *p=s;for(;*p;p++)*p=toupper((unsigned char)*p);return s;}\n'
                      'char *strlwr(char *s){char *p=s;for(;*p;p++)*p=tolower((unsigned char)*p);return s;}\n')
    sources = list(sources) + [compat]
    cmd = ['gcc', '-O2', '-w', '-std=gnu17', '-fwrapv', '-DNDEBUG', '-DNOHW', '-DNOMD5', '-DCMAKECONFIG', '-DUNIXCOMMON', '-DNOEXECINFO'] \
          + [f'-D{d}' for d in defines] + [f'-I{i}' for i in inc] + [str(x) for x in sources] + ['-o', str(exe), '-lm']
    if any('png' in l for l in libs):
        cmd += ['-lpng', '-lz']
    cmd += ['-lm']
    pr = subprocess.run(cmd, cwd=work, capture_output=True, text=True)
    (work / (name + '.build.log')).write_text(pr.stdout + pr.stderr, encoding='utf-8')
    if pr.returncode or not exe.exists():
        raise RuntimeError(f'host tool build failed ({name}): see {work / (name + ".build.log")}\n{(pr.stdout + pr.stderr)[-2000:]}')
    return exe


def build_tool(work, name, defines, sources, libs=()):
    """cl the host tool; returns the exe path. Raises with the compiler output on failure."""
    work = Path(work).resolve()
    work.mkdir(parents=True, exist_ok=True)
    write_stub_sources(work)
    import os
    if os.name != 'nt':
        return _build_tool_posix(work, name, defines, sources, libs)
    exe = work / (name + '.exe')
    inc = [str(ROOT / 'src'), str(ROOT / 'build/pc-golden/src'), str(ROOT / 'tools/ps2'), str(work), str(DEPS / 'include')]
    cmd = ['cl', '/nologo', '/O2', '/W3', '/wd4244', '/wd4267', '/wd4018', '/wd4146', '/wd4996', '/wd4005', '/wd4101', '/wd4133', '/wd4047',
           '/D_CRT_SECURE_NO_WARNINGS', '/DNDEBUG', '/DNOHW', '/DNOMD5'] + [f'/D{d}' for d in defines] + [f'/I{i}' for i in inc] \
          + [f'/Fe:{exe.name}'] + [str(s) for s in sources] + (['/link', f'/LIBPATH:{DEPS / "lib"}'] + list(libs) if libs else [])
    bat = work / (name + '.bat')
    bat.write_text(f'@echo off\ncall "{VCVARS}" >nul 2>&1\nif errorlevel 1 exit /b 1\n' + subprocess.list2cmdline(cmd) + '\n', encoding='utf-8')
    p = subprocess.run(['cmd', '/c', str(bat)], cwd=work, capture_output=True, text=True, encoding='oem', errors='replace')
    (work / (name + '.build.log')).write_text(p.stdout + p.stderr, encoding='utf-8')
    if p.returncode or not exe.exists():
        raise RuntimeError(f'host tool build failed ({name}): see {work / (name + ".build.log")}\n{p.stdout[-2000:]}')
    return exe


def build_oracle(work):
    """The unmodified engine PNG path: r_picformats.c with HAVE_PNG (not PS2_PROFILE) + libpng/zlib."""
    return build_tool(work, 'strip_png2pic', ['HAVE_PNG', 'HAVE_ZLIB'], [ROOT / 'tools/ps2/strip_pics_host.c', ROOT / 'src/r_picformats.c'],
                      ['libpng16.lib', 'z.lib'])


def build_check(work):
    """The PS2 profile path: r_picformats.c with PS2_PROFILE, no libpng/zlib."""
    return build_tool(work, 'strip_cooked_check', ['PS2_PROFILE'], [ROOT / 'tools/ps2/strip_pics_host.c', ROOT / 'src/r_picformats.c'])


def run_tool(exe, playpal, outdir, inputs):
    exe, playpal, outdir = Path(exe).resolve(), Path(playpal).resolve(), Path(outdir).resolve()
    outdir.mkdir(parents=True, exist_ok=True)
    env = None
    # libpng16.dll / zlib1.dll of the vcpkg tree must be found by the oracle
    import os
    env = dict(os.environ, PATH=str(DEPS / 'bin') + ';' + os.environ.get('PATH', '')) if os.name == 'nt' else None
    p = subprocess.run([str(exe), str(playpal), str(outdir)] + [str(Path(i).resolve()) for i in inputs], capture_output=True, text=True, env=env)
    if p.returncode:
        raise RuntimeError(f'{exe.name} failed ({p.returncode}): {p.stderr}{p.stdout}')
    return p.stdout


# ---- independent Python model of the original decoder -------------------------------------------------------------

def _unfilter(raw, width, height, bpp_bits, channels_bits):
    stride = (width * channels_bits + 7) // 8
    bpp = max(1, channels_bits // 8)
    rows, prev = [], bytearray(stride)
    pos = 0
    for _ in range(height):
        ft = raw[pos]
        line = bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if ft == 1:
                line[i] = (line[i] + a) & 255
            elif ft == 2:
                line[i] = (line[i] + b) & 255
            elif ft == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif ft == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
            elif ft != 0:
                raise ValueError('bad PNG filter')
        rows.append(bytes(line))
        prev = line
    return rows


def decode_png(data):
    """Returns dict(width, height, ctype, depth, plte, trns, grab, rows(list of raw rows))."""
    assert data[:8] == PNG_SIG
    pos, idat, info = 8, b'', {'plte': None, 'trns': None, 'grab': None}
    while pos < len(data):
        n, typ = struct.unpack_from('>I4s', data, pos)
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if typ == b'IHDR':
            info['width'], info['height'], info['depth'], info['ctype'], _c, _f, info['interlace'] = struct.unpack('>IIBBBBB', body)
        elif typ == b'PLTE':
            info['plte'] = body
        elif typ == b'tRNS':
            info['trns'] = body
        elif typ == b'grAb':
            info['grab'] = struct.unpack('>ii', body[:8])
        elif typ == b'IDAT':
            idat += body
    if info['interlace']:
        raise ValueError('interlaced PNG not modelled')
    chans = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[info['ctype']]
    info['rows'] = _unfilter(zlib.decompress(idat), info['width'], info['height'], info['depth'], chans * info['depth'])
    return info


class PyOracle:
    """Model of PNG_Read + Picture_PNGConvert(PICFMT_PATCH): same pixel order, same persistent first-colour-per-bucket memo."""

    def __init__(self, palette):
        self.pal = palette                 # list of 256 (r, g, b)
        self.lut = {}

    def nearest(self, r, g, b):
        best, bd = 0, 256 * 256 * 4
        for i, (pr, pg, pb) in enumerate(self.pal):
            d = (r - pr) ** 2 + (g - pg) ** 2 + (b - pb) ** 2
            if d < bd:
                if not d:
                    return i
                bd, best = d, i
        return best

    def lookup(self, r, g, b):
        k = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        if k not in self.lut:
            self.lut[k] = self.nearest(r, g, b)
        return self.lut[k]

    def convert(self, data):
        """Returns (w, h, left, top, row-major matrix) like strip_pics_host's .matrix."""
        im = decode_png(data)
        w, h, depth, ct = im['width'], im['height'], im['depth'], im['ctype']
        left, top = im['grab'] if im['grab'] else (0, 0)
        use_pal = False
        if ct == 3 and im['plte'] is not None and len(im['plte']) // 3 == 256 and \
                all(tuple(im['plte'][i * 3:i * 3 + 3]) == tuple(self.pal[i]) for i in range(256)):
            use_pal = True
            if im['trns'] and any(a < 255 for a in im['trns']):
                use_pal = False
        pix = []
        for row in im['rows']:
            for x in range(w):
                if ct == 3:
                    if depth == 8:
                        idx = row[x]
                    else:
                        per = 8 // depth
                        idx = (row[x // per] >> (8 - depth * (x % per + 1))) & ((1 << depth) - 1)
                    if use_pal:
                        pix.append(idx)
                        continue
                    pl = im['plte']
                    r, g, b = pl[idx * 3], pl[idx * 3 + 1], pl[idx * 3 + 2]
                    a = im['trns'][idx] if im['trns'] and idx < len(im['trns']) else 255
                elif ct == 2:
                    assert depth == 8
                    r, g, b = row[x * 3], row[x * 3 + 1], row[x * 3 + 2]
                    a = 255
                    if im['trns']:
                        raise ValueError('tRNS on RGB not modelled')
                elif ct == 6:
                    assert depth == 8
                    r, g, b, a = row[x * 4:x * 4 + 4]
                else:
                    raise ValueError('colour type not modelled')
                pix.append(self.lookup(r, g, b) if a else TRANSPARENT)
        return w, h, left, top, pix


def read_palette(playpal_bytes):
    return [tuple(playpal_bytes[i * 3:i * 3 + 3]) for i in range(256)]
