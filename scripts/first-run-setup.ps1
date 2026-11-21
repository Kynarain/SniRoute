#requires -Version 5.1
<#
.SYNOPSIS
    First-launch setup: global DNS mode by default, per-user fallback.

.DESCRIPTION
    ASCII ONLY. Do not add non-ASCII characters to this file.

    Default is the global mode (same idea as SNIBypassGUI): one elevation, then
    every program that resolves through the Windows DNS Client is covered -
    images are redirected to the local proxy, and Chrome/Edge get the local DoH
    policy for the main site (only DoH can carry ECH, so the main site cannot go
    through plain DNS).

    Order of operations:
      1. import the CA into the current user's roots      (no admin)
      2. global mode: NRPT rule + machine CA + machine browser policy
         (one UAC prompt; skipped if already installed)
      3. if elevation was declined or impossible: per-user browser policy only
         (no admin) - images still work through the browser DoH path

    Exit codes:
      0  global mode active
      2  per-user mode active (global needs one elevation)
      3  certificate step failed
      5  elevation was declined and the per-user policy is unavailable too
#>
param(
    [int]$DohPort = 5444,
    [switch]$NoElevate
)

$ErrorActionPreference = 'Continue'


# Shortcut icons: the MSI stores its icon in the Icon table and Windows copies it to
# %APPDATA%\Microsoft\Installer\{GUID}\AppIcon. That cache can go stale across an
# upgrade, which shows up as a black (blank) icon on the desktop shortcut. Point both
# shortcuts at the exe's own icon instead - per-user, no elevation needed.
function Repair-ShortcutIcons {
    try {
        $root = Split-Path -Parent $PSScriptRoot
        $exe = Join-Path $root 'Sniroute.exe'
        if (-not (Test-Path $exe)) { return }
        $shell = New-Object -ComObject WScript.Shell
        $links = @(
            (Join-Path ([Environment]::GetFolderPath('Desktop')) 'Sniroute.lnk'),
            (Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\Sniroute.lnk')
        )
        foreach ($lnk in $links) {
            if (Test-Path $lnk) {
                $sc = $shell.CreateShortcut($lnk)
                if ($sc.IconLocation -ne ($exe + ',0')) {
                    $sc.IconLocation = $exe + ',0'
                    $sc.Save()
                }
            }
        }
    } catch { }
}

Repair-ShortcutIcons

$Root = Split-Path -Parent $PSScriptRoot
$Setup = Join-Path $PSScriptRoot 'setup-browser-ech.ps1'
$Policy = Join-Path $PSScriptRoot 'set-browser-policy.ps1'
$Nrpt = Join-Path $PSScriptRoot 'nrpt.ps1'

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $pr = New-Object Security.Principal.WindowsPrincipal($id)
    return $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

Write-Host ''
Write-Host '=== sniroute: first launch setup ===' -ForegroundColor Cyan

# 1. certificate (per-user, never needs admin)
$caOk = $true
if (Test-Path $Setup) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File $Setup 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { $caOk = $false }
}
if (-not $caOk) {
    Write-Host '  [!] certificate setup failed' -ForegroundColor Yellow
    exit 3
}

# 2. global mode
$globalOk = $false
if (Test-Path $Nrpt) {
    if (-not $NoElevate) {
        Write-Host '  global mode needs one administrator prompt' -ForegroundColor DarkGray
        & powershell -NoProfile -ExecutionPolicy Bypass -File $Nrpt -Install 2>&1 | Out-Null
        if ($LASTEXITCODE -eq 0) { $globalOk = $true }
    }
}

if ($globalOk) {
    Write-Host ''
    Write-Host 'Global mode active: images are redirected for every program,' -ForegroundColor Green
    Write-Host 'Chrome/Edge use the local DoH for the main site.'
    # Start the services here too: NRPT is already pointing at our resolver, so
    # leaving them down means every lookup fails until the user finds the tray
    # menu - which is exactly the "cannot find the IP address" symptom.
    $cli = Join-Path $Root 'sniroute.cmd'
    if (Test-Path $cli) {
        Write-Host 'Starting services...'
        & cmd.exe /c "`"$cli`" up" 2>&1 | Out-Null
    }
    exit 0
}

# 3. fallback: per-user policy (no admin)
if (Test-Path $Policy) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File $Policy -Browsers Chrome -NoElevate -DohPort $DohPort 2>&1 | Out-Null
    switch ($LASTEXITCODE) {
        0 { Write-Host '  per-user browser policy active (no admin needed)'; exit 2 }
        2 { Write-Host '  [!] browser policy also needs one elevation' -ForegroundColor Yellow; exit 5 }
        default { Write-Host '  [!] browser policy failed' -ForegroundColor Yellow; exit 5 }
    }
}

exit 5
