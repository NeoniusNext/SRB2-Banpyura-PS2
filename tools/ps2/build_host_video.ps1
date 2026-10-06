param(
    [string]$Dependencies = 'D:/AI-projects/SRB2B-plus/build/deps/vcpkg_installed',
    [string]$Lz4Source = '',
    [string]$Lz4Include = '',
    [int]$Jobs = 6,
    [string]$Out = 'build/agent-vid-host-asan',
    [string]$ExeName = 'srb2-host-video-asan',
    [switch]$NoAsan
)
# Native SDL host build of the PS2 profile for the video mode tests (docs/VIDEO_MODES.md): the same configuration as
# tools/ps2/build_host_profile.ps1 plus PS2_VIDLIMIT (MAXVIDWIDTH/HEIGHT = 640x512 like the PS2 build, src/screen.h) and,
# unless -NoAsan, the MSVC AddressSanitizer: a write past any screen-size dependent array is reported at the first byte.
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$out = if ([System.IO.Path]::IsPathRooted($Out)) { [System.IO.Path]::GetFullPath($Out) } else { Join-Path $repo $Out }
if (-not (Test-Path -LiteralPath $Dependencies)) { throw "Missing dependency directory: $Dependencies" }
[System.IO.Directory]::CreateDirectory($out) | Out-Null
if (-not $Lz4Source -and -not (Test-Path -LiteralPath (Join-Path $Dependencies 'x64-windows/lib/lz4.lib'))) {
    $externalLz4 = Join-Path $repo 'build/scratch/lz4src/lz4-4.4.5/lz4libs/lz4.c'
    if ((Test-Path -LiteralPath $externalLz4) -and (Test-Path -LiteralPath 'D:/ps2dev/ps2sdk/ports/include/lz4.h')) {
        $Lz4Source = $externalLz4
        if (-not $Lz4Include) { $Lz4Include = 'D:/ps2dev/ps2sdk/ports/include' }
    } else {
        throw 'No existing host LZ4 dependency; supply -Lz4Source and -Lz4Include'
    }
}
$cflags = '/DPS2_VIDLIMIT'
if (-not $NoAsan) { $cflags += ' /fsanitize=address /Zi' }
$configure = @('-S', $repo, '-B', $out, '-G', 'Visual Studio 18 2026', '-A', 'x64',
    '-DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake',
    '-DVCPKG_MANIFEST_MODE=OFF', "-DVCPKG_INSTALLED_DIR=$Dependencies",
    '-DSRB2_CONFIG_HWRENDER=OFF', '-DSRB2_CONFIG_USE_GME=OFF',
    '-DSRB2_CONFIG_STATIC_STDLIB=OFF', '-DSRB2_CONFIG_PS2REF=ON', '-DSRB2_CONFIG_PS2PROFILE=ON',
    "-DCMAKE_C_FLAGS=$cflags", "-DCMAKE_CXX_FLAGS=$cflags",
    "-DSRB2_SDL2_EXE_NAME=$ExeName",
    "-DSRB2_HOST_PROFILE_LZ4_SOURCE=$Lz4Source", "-DSRB2_HOST_PROFILE_LZ4_INCLUDE_DIR=$Lz4Include")
$build = @('--build', $out, '--config', 'Release', '--parallel', "$Jobs")
$ErrorActionPreference = 'Continue' # PowerShell 5.1 wraps native stderr warnings as errors.
& cmake @configure 2>&1 | ForEach-Object { "$_" } | Out-File -Encoding UTF8 -LiteralPath (Join-Path $out 'configure.log')
if ($LASTEXITCODE -ne 0) { throw "Host video configure failed; see $out/configure.log" }
& cmake @build 2>&1 | ForEach-Object { "$_" } | Out-File -Encoding UTF8 -LiteralPath (Join-Path $out 'build.log')
if ($LASTEXITCODE -ne 0) { throw "Host video build failed; see $out/build.log" }
$ErrorActionPreference = 'Stop'
"Built $out/bin/Release/$ExeName.exe"
