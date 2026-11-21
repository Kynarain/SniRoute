#requires -Version 5.1
<#
.SYNOPSIS
    Compile the C++ tray application.

.DESCRIPTION
    ASCII ONLY. Do not add non-ASCII characters to this file.

    Toolchain: a portable llvm-mingw in build\llvm (clang++ with MinGW headers and
    the Win32 import libraries). Nothing is installed system-wide, so no elevation
    is involved - the same rule the app itself follows.

    Output: build\tray\Sniroute.exe
#>
param(
    [string]$Out = ''
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$LlvmRoot = Join-Path $RepoRoot 'build\llvm'
$Src = Join-Path $RepoRoot 'gui\SnirouteTray.cpp'
if (-not $Out) { $Out = Join-Path $RepoRoot 'build\tray' }

function Step($m) { Write-Host "== $m" -ForegroundColor Cyan }
function Fail($m) { Write-Host "!! $m" -ForegroundColor Red; exit 1 }

Step 'locate toolchain'
# Zig ships a complete clang + MinGW headers/libs in one binary, so `zig c++`
# builds the tray exe with no system-wide toolchain and no elevation.
$Zig = Join-Path $RepoRoot 'build\zig\zig.exe'
if (-not (Test-Path $Zig)) {
    Fail "missing $Zig (pip install ziglang, then copy the package into build\zig)"
}
Write-Host "   $Zig"
$env:PATH = (Split-Path -Parent $Zig) + ';' + $env:PATH
Step 'compile'
New-Item -ItemType Directory -Path $Out -Force | Out-Null
$Exe = Join-Path $Out 'Sniroute.exe'
if (Test-Path $Exe) { Remove-Item $Exe -Force }

# -municode  : use wWinMain as entry point
# -mwindows  : GUI subsystem, so no console window flashes when launched
$compileArgs = @(
    'c++', '-target', 'x86_64-windows-gnu',
    '-std=c++17', '-O2', '-municode', '-Wl,--subsystem,windows',
    '-DUNICODE', '-D_UNICODE', '-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN',
    '-o', $Exe, $Src,
    '-static', '-luser32', '-lshell32', '-ladvapi32', '-lgdi32', '-lws2_32'
)
& $Zig @compileArgs
if ($LASTEXITCODE -ne 0) { Fail 'zig c++ failed' }
if (-not (Test-Path $Exe)) { Fail 'no exe produced' }

# zig -O2 output carries no separate debug info, so there is nothing to strip.

Write-Host ''
Write-Host ("Built: {0} ({1:N0} KB)" -f $Exe, ((Get-Item $Exe).Length / 1KB)) -ForegroundColor Green

# 先签 EXE 再打包：签名后的二进制会原样进入 MSI 与 ZIP。
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'sign-and-hash.ps1') -Path $Exe -SkipHash -Quiet 2>&1 | Out-Null
