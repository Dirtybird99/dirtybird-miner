param(
    [Parameter(Mandatory = $true)]
    [string]$Version,
    [string]$BuildDir = "build",
    [string]$OutputDir = "dist"
)

$ErrorActionPreference = "Stop"

$ScriptRoot = $PSScriptRoot
$RepoRoot = (Resolve-Path (Join-Path $ScriptRoot "..")).Path

$Version = $Version.Trim()
if ($Version.StartsWith("v")) { $Version = $Version.Substring(1) }
if (-not $Version) { throw "Version cannot be empty." }

$AssetVersion = "v$Version"
$BinaryName = "dirtybird-c-miner.exe"
$BuildRoot = Join-Path $RepoRoot $BuildDir
$BinDir = Join-Path $BuildRoot "bin"
$BinaryPath = Join-Path $BinDir $BinaryName
$StageRoot = Join-Path $RepoRoot $OutputDir
$PackageName = "dirtybird-c-miner-win64-$AssetVersion"
$PackageDir = Join-Path $StageRoot $PackageName
$ArchivePath = Join-Path $StageRoot "$PackageName.zip"

Write-Host "================================================"
Write-Host "DIRTYBIRD C Miner Windows Packaging"
Write-Host "Version: $AssetVersion"
Write-Host "================================================"

if (-not (Test-Path $BinaryPath)) { throw "Binary not found: $BinaryPath" }

New-Item -ItemType Directory -Path $StageRoot -Force | Out-Null
Remove-Item $PackageDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $PackageDir -Force | Out-Null

Copy-Item $BinaryPath -Destination $PackageDir -Force
Copy-Item (Join-Path $RepoRoot "README.md") -Destination $PackageDir -Force
Copy-Item (Join-Path $RepoRoot "LICENSE") -Destination $PackageDir -Force
Copy-Item (Join-Path $RepoRoot "config.json") -Destination $PackageDir -Force
Copy-Item (Join-Path $RepoRoot "config.json.example") -Destination $PackageDir -Force

# Bundle the MinGW/OpenSSL runtime DLLs the dynamic build needs (skip any not present).
$RuntimeFiles = @(
    "libgcc_s_seh-1.dll",
    "libwinpthread-1.dll",
    "libstdc++-6.dll",
    "libcrypto-3-x64.dll",
    "libssl-3-x64.dll"
)
foreach ($runtimeFile in $RuntimeFiles) {
    $runtimePath = Join-Path $BinDir $runtimeFile
    if (Test-Path $runtimePath) { Copy-Item $runtimePath -Destination $PackageDir -Force }
}

# Auto-restart launcher. Settings come from config.json; add flags to override (CLI wins).
# Write with explicit CRLF -- Windows cmd.exe mishandles LF-only .bat (esp. :loop/goto).
$startLines = @(
    '@echo off',
    'cd /d "%~dp0"',
    "title DIRTYBIRD C Miner $AssetVersion",
    'REM Edit config.json for daemon-address / wallet / threads / priority.',
    'REM To override per-run, append flags below, e.g.:  .\dirtybird-c-miner.exe -t 20 -p max',
    ':loop',
    '.\dirtybird-c-miner.exe',
    'timeout 3',
    'goto loop'
)
[System.IO.File]::WriteAllText((Join-Path $PackageDir "start.bat"), ($startLines -join "`r`n") + "`r`n", [System.Text.Encoding]::ASCII)

$QuickStart = @"
DIRTYBIRD C Miner $AssetVersion
=============================

Contents:
- dirtybird-c-miner.exe
- *.dll  (runtime; keep them next to the exe)
- config.json   (edit this: daemon-address / wallet / threads / priority)
- config.json.example
- start.bat
- README.md
- LICENSE

Quick start:
1. Edit config.json: "daemon-address" (host:port), "wallet", "threads" (-1 = auto), "priority".
2. Double-click start.bat (auto-restarts the miner). It reads config.json.
   Power users: append flags to start.bat to override, e.g. -t 20 -p max (CLI wins over config.json).
3. -p max for headless/AFK (more hashrate, may stutter the desktop); -p normal (default) is desktop-safe.

Notes:
- 64-bit AVX2 CPUs. Keep the EXE and DLLs in the same folder.
- Startup prints a pow("a") self-test; it must say PASS.
"@
$QuickStart | Out-File -FilePath (Join-Path $PackageDir "QUICKSTART.txt") -Encoding ascii

if (Test-Path $ArchivePath) { Remove-Item $ArchivePath -Force }
Compress-Archive -Path $PackageDir -DestinationPath $ArchivePath -Force

Write-Host "Created package: $ArchivePath"
