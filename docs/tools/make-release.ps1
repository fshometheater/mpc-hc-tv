# Builds the release packages of one platform from an already built player:
#   MPC-HC-<mpc>-TV-<tv>-<x64|x86>.exe   (installer, Inno Setup 6)
#   MPC-HC-<mpc>-TV-<tv>-<x64|x86>.zip   (portable: the same files as upstream's zip, no crash reporter)
# and prints their SHA-256. Usage (after building the player with MSBuild, see docs/RELEASE.md):
#   powershell -File docs\tools\make-release.ps1 -Platform x64 -OutDir <folder>
# The bin folder must already hold the third-party files from upstream's official zip of the same
# MPC-HC version (LAVFilters, Lang, MediaInfo.dll, mpciconlib.dll, D3DX9_43.dll, MPCVR, Shaders, Toolbars).

param(
    [ValidateSet('x64', 'x86')] [string]$Platform = 'x64',
    [Parameter(Mandatory = $true)] [string]$OutDir
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$root = Split-Path (Split-Path $PSScriptRoot)

function Get-Define([string]$file, [string]$name) {
    $m = Select-String -LiteralPath $file -Pattern "^\s*#define\s+$name\s+(\d+)" | Select-Object -First 1
    if (-not $m) { throw "$name not found in $file" }
    return $m.Matches[0].Groups[1].Value
}
$verH = Join-Path $root 'include\version.h'
$tvH = Join-Path $root 'include\version_tv.h'
$mpcVer = '{0}.{1}.{2}' -f (Get-Define $verH 'MPC_VERSION_MAJOR'), (Get-Define $verH 'MPC_VERSION_MINOR'), (Get-Define $verH 'MPC_VERSION_PATCH')
$tvVer = '{0}.{1}.{2}' -f (Get-Define $tvH 'MPC_TV_VERSION_MAJOR'), (Get-Define $tvH 'MPC_TV_VERSION_MINOR'), (Get-Define $tvH 'MPC_TV_VERSION_PATCH')
$name = "MPC-HC-$mpcVer-TV-$tvVer-$Platform"

$x64 = $Platform -eq 'x64'
$bin = Join-Path $root ($(if ($x64) { 'bin\mpc-hc_x64' } else { 'bin\mpc-hc_x86' }))
$exe = $(if ($x64) { 'mpc-hc64.exe' } else { 'mpc-hc.exe' })
$lav = $(if ($x64) { 'LAVFilters64' } else { 'LAVFilters' })
$mpcvr = $(if ($x64) { 'MpcVideoRenderer64.ax' } else { 'MpcVideoRenderer.ax' })
if (-not (Test-Path -LiteralPath (Join-Path $bin $exe))) { throw "Build the player first: $bin\$exe not found" }
New-Item -ItemType Directory -Force $OutDir | Out-Null

# --- Portable zip (upstream's layout, without CrashReporter: it reports to upstream's crash service)
$stage = Join-Path $OutDir "$name.stage"
if (Test-Path -LiteralPath $stage) { [IO.Directory]::Delete($stage, $true) }
New-Item -ItemType Directory -Force $stage | Out-Null
foreach ($f in @($exe, 'mpciconlib.dll', 'MediaInfo.dll', 'D3DX9_43.dll', 'COPYING.txt')) {
    $src = Join-Path $bin $f
    if (-not (Test-Path -LiteralPath $src)) { throw "missing: $src" }
    Copy-Item -LiteralPath $src -Destination $stage
}
foreach ($d in @('Lang', $lav, 'MPCVR', 'Shaders', 'Shaders11', 'Toolbars')) {
    $src = Join-Path $bin $d
    if (-not (Test-Path -LiteralPath $src)) { throw "missing folder: $src" }
    Copy-Item -LiteralPath $src -Destination $stage -Recurse
}
Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object { $_.Extension -in '.pdb', '.lib', '.exp' } |
    ForEach-Object { [IO.File]::Delete($_.FullName) }
$zip = Join-Path $OutDir "$name.zip"
if (Test-Path -LiteralPath $zip) { [IO.File]::Delete($zip) }
[IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [IO.Compression.CompressionLevel]::Optimal, $false)
[IO.Directory]::Delete($stage, $true)

# --- Installer: the two optional files the setup script looks for (git-ignored), then Inno Setup
$distribArch = Join-Path $root ($(if ($x64) { 'distrib\x64' } else { 'distrib\x86' }))
Copy-Item -LiteralPath (Join-Path $bin 'MediaInfo.dll') -Destination (Join-Path $distribArch 'MediaInfo.dll') -Force
New-Item -ItemType Directory -Force (Join-Path $root 'distrib\MPCVR') | Out-Null
Copy-Item -LiteralPath (Join-Path $bin "MPCVR\$mpcvr") -Destination (Join-Path $root "distrib\MPCVR\$mpcvr") -Force
$iscc = Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'
if (-not (Test-Path -LiteralPath $iscc)) { $iscc = 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe' }
Push-Location (Join-Path $root 'distrib')
try {
    $isccArgs = @('/Q', "/O$OutDir")
    if ($x64) { $isccArgs += '/Dx64Build' }
    & $iscc @isccArgs 'mpc-hc_setup.iss'
    if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed ($LASTEXITCODE)" }
} finally { Pop-Location }

foreach ($f in @("$name.exe", "$name.zip")) {
    $p = Join-Path $OutDir $f
    '{0}  {1:N1} MB  sha256:{2}' -f $f, ((Get-Item -LiteralPath $p).Length / 1MB), (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLower()
}
