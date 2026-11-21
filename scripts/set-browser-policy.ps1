#requires -Version 5.1
<#
.SYNOPSIS
    Point Chrome (and optionally Edge) at sniroute's local DoH endpoint via policy.

.DESCRIPTION
    ASCII ONLY. Do not add non-ASCII characters to this file.

    Why policy instead of the Settings UI: Chrome's "Secure DNS" setting can only
    be changed by a human in chrome://settings or by policy. It cannot be set from
    the command line any more (Chrome 157 ignores --dns-over-https-*), and hand
    edits to the profile file are rejected because those prefs are MAC-protected.

    Two tiers, so that most machines never see a UAC prompt:
      1. HKCU\Software\Policies\... : per-user policy, works without elevation.
         This is attempted first and is enough on a normal Windows install.
      2. HKLM\SOFTWARE\Policies\... : machine policy, needs elevation once. Used
         when the user policy key is not writable - for example on a hardened
         machine where HKCU\Software\Policies is owned by SYSTEM.

    The policy lives in the registry, so it survives browser restarts and updates,
    and it applies to every profile of that browser.

    Reverting deletes the two values it set; it does not touch any other policy.

.PARAMETER Browsers
    Chrome (default), Edge, or Both.

.PARAMETER Revert
    Remove the DoH policy. Falls back to an elevated run when only HKLM holds it.

.PARAMETER Check
    Report what is currently configured, change nothing.

.PARAMETER Elevated
    Internal: relaunch this script elevated and act on HKLM.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\set-browser-policy.ps1 -Check
#>
param(
    [ValidateSet('Chrome', 'Edge', 'Both')]
    [string]$Browsers = 'Chrome',
    [switch]$Revert,
    [switch]$Check,
    [switch]$Elevated,
    [switch]$NoElevate,
    [int]$DohPort = 5444
)

$ErrorActionPreference = 'Stop'
$DohUrl = "https://127.0.0.1:$DohPort/dns-query"
$Mode = 'secure'

function Write-Step([string]$m) { Write-Host "  $m" }
function Write-Ok([string]$m) { Write-Host "  [OK] $m" -ForegroundColor Green }
function Write-Warn2([string]$m) { Write-Host "  [!] $m" -ForegroundColor Yellow }

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $pr = New-Object Security.Principal.WindowsPrincipal($id)
    return $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-VendorKeys([string]$browser) {
    switch ($browser) {
        'Chrome' { return @('Google\Chrome') }
        'Edge' { return @('Microsoft\Edge') }
        'Both' { return @('Google\Chrome', 'Microsoft\Edge') }
    }
}

# ---------------------------------------------------------------- HKCU (no admin)
function Set-UserPolicy([string]$hive, [string]$vendor) {
    $path = "$hive\Software\Policies\$vendor"
    try {
        New-Item -Path $path -Force -ErrorAction Stop | Out-Null
        Set-ItemProperty -Path $path -Name 'DnsOverHttpsMode' -Value $Mode -ErrorAction Stop
        Set-ItemProperty -Path $path -Name 'DnsOverHttpsTemplates' -Value $DohUrl -ErrorAction Stop
        return $true
    } catch {
        return $false
    }
}

function Clear-Policy([string]$hive, [string]$vendor) {
    $path = "$hive\Software\Policies\$vendor"
    if (-not (Test-Path $path)) { return $true }
    try {
        foreach ($name in 'DnsOverHttpsMode', 'DnsOverHttpsTemplates') {
            if (Get-ItemProperty -Path $path -Name $name -ErrorAction SilentlyContinue) {
                Remove-ItemProperty -Path $path -Name $name -Force -ErrorAction Stop
            }
        }
        return $true
    } catch {
        return $false
    }
}

function Get-PolicyState([string]$hive, [string]$vendor) {
    $path = "$hive\Software\Policies\$vendor"
    if (-not (Test-Path $path)) { return $null }
    $mode = (Get-ItemProperty -Path $path -Name 'DnsOverHttpsMode' -ErrorAction SilentlyContinue).DnsOverHttpsMode
    $tpl = (Get-ItemProperty -Path $path -Name 'DnsOverHttpsTemplates' -ErrorAction SilentlyContinue).DnsOverHttpsTemplates
    if (-not $mode -and -not $tpl) { return $null }
    return "mode=$mode templates=$tpl"
}

# ---------------------------------------------------------------- elevated relaunch
function Invoke-Elevated([string]$extraArgs) {
    $self = $MyInvocation.MyCommand.Path
    $powerShell = (Get-Process -Id $PID).Path
    $argLine = "-NoProfile -ExecutionPolicy Bypass -File `"$self`" -Browsers $Browsers -Elevated $extraArgs"
    Write-Step 'requesting administrator rights once'
    try {
        $p = Start-Process -FilePath $powerShell -ArgumentList $argLine -Verb RunAs -Wait -PassThru
        return $p.ExitCode
    } catch {
        Write-Warn2 'elevation was cancelled'
        return 1
    }
}

# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '=== sniroute: browser DoH policy ===' -ForegroundColor Cyan
Write-Host "  target: $DohUrl"

if ($Check) {
    foreach ($vendor in 'Google\Chrome', 'Microsoft\Edge') {
        $u = Get-PolicyState 'HKCU:' $vendor
        $m = Get-PolicyState 'HKLM:' $vendor
        Write-Host ("  {0,-16} user={1}  machine={2}" -f $vendor, $(if ($u) { $u } else { 'none' }), $(if ($m) { $m } else { 'none' }))
    }
    Write-Host ("  running as admin : {0}" -f (Test-Admin))
    exit 0
}

$vendors = Get-VendorKeys $Browsers

if ($Revert) {
    $needElevated = $false
    foreach ($vendor in $vendors) {
        if (-not (Clear-Policy 'HKCU:' $vendor)) { $needElevated = $true }
        if (-not (Clear-Policy 'HKLM:' $vendor)) { $needElevated = $true }
    }
    if ($needElevated -and -not (Test-Admin)) {
        $code = Invoke-Elevated '-Revert'
        exit $code
    }
    Write-Ok 'DoH policy removed'
    Write-Host ''
    Write-Host 'Restart the browser for it to take effect.'
    exit 0
}

# set
$userOk = $true
foreach ($vendor in $vendors) {
    if (Set-UserPolicy 'HKCU:' $vendor) {
        Write-Ok "user policy set for $vendor (no admin needed)"
    } else {
        $userOk = $false
    }
}

if (-not $userOk -and -not $Elevated) {
    Write-Warn2 'the per-user policy key is not writable on this machine'
    if ($NoElevate) {
        # First launch must not surprise the user with a UAC prompt; the tray
        # menu offers the one-time elevation instead.
        Write-Host 'NEED_ELEVATION'
        exit 2
    }
    $code = Invoke-Elevated ''
    exit $code
}

if ($Elevated) {
    # machine-wide policy: covers every user profile of that browser
    if (-not (Test-Admin)) {
        Write-Warn2 'this mode needs administrator rights'
        exit 1
    }
    foreach ($vendor in $vendors) {
        if (Set-UserPolicy 'HKLM:' $vendor) {
            Write-Ok "machine policy set for $vendor"
        } else {
            Write-Warn2 "could not set machine policy for $vendor"
        }
    }
    exit 0
}

Write-Host ''
Write-Host 'Done. Restart the browser, then check chrome://policy' -ForegroundColor Green
Write-Host "  DnsOverHttpsMode      = $Mode"
Write-Host "  DnsOverHttpsTemplates = $DohUrl"
Write-Host ''
Write-Host 'To undo: run this script with -Revert (or the tray menu item).'
