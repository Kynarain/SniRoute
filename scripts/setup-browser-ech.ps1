#requires -Version 5.1
<#
.SYNOPSIS
    One-time setup: trust this project's local CA for the current user.

.DESCRIPTION
    ASCII ONLY. Do not add non-ASCII characters to this file.
    Windows PowerShell 5.1 decodes .ps1 files in the ANSI codepage unless the
    file has a UTF-8 BOM; a mangled string can swallow a quote and break parsing
    for the whole script.

    Why no administrator is needed:
      * The CA goes into the CURRENT USER's Trusted Root store
        (certutil -user -addstore Root). Chrome and Edge validate against the
        user store too, so this is enough for the DoH endpoint certificate.
      * The DoH endpoint is addressed by IP (https://127.0.0.1:5444/...), so no
        hosts entry is required and the protected hosts file is never touched.

    The optional -WithHosts switch exists for people who prefer the hostname
    https://doh.sniroute.lan:5444/... ; that path does need administrator rights
    because it writes the system hosts file.

.PARAMETER Revert
    Remove the CA from the user store (and the hosts entry if present).

.PARAMETER Check
    Print current state and exit. Changes nothing.

.PARAMETER WithHosts
    Also add "127.0.0.1 doh.sniroute.lan" to the hosts file. Requires elevation.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\scripts\setup-browser-ech.ps1
    powershell -ExecutionPolicy Bypass -File .\scripts\setup-browser-ech.ps1 -Check
#>
param(
    [switch]$Revert,
    [switch]$Check,
    [switch]$WithHosts,
    [string]$DohHost = 'doh.sniroute.lan'
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$CaCert = Join-Path $RepoRoot 'ca\rootCA.crt'
$HostsPath = Join-Path $env:SystemRoot 'System32\drivers\etc\hosts'
$BlockBegin = '# >>> sniroute doh >>>'
$BlockEnd = '# <<< sniroute doh <<<'
$DohUrl = 'https://127.0.0.1:5444/dns-query'

function Write-Step([string]$m) { Write-Host "  $m" }
function Write-Ok([string]$m) { Write-Host "  [OK] $m" -ForegroundColor Green }
function Write-Warn2([string]$m) { Write-Host "  [!] $m" -ForegroundColor Yellow }

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $pr = New-Object Security.Principal.WindowsPrincipal($id)
    return $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-CaThumbprint {
    if (-not (Test-Path $CaCert)) { return $null }
    return (New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 $CaCert).Thumbprint
}

function Test-CaInUserStore {
    $thumb = Get-CaThumbprint
    if (-not $thumb) { return $false }
    # certutil -user, not Cert:\CurrentUser\Root: the PowerShell provider shows a
    # MERGED view that includes machine-wide roots, so it would claim "trusted"
    # even when only the machine store has the certificate.
    $store = certutil.exe -user -store Root 2>&1 | Out-String
    return ($store -match [regex]::Escape($thumb))
}

function Remove-StaleCa {
    # Every install generates a new CA. A leftover CA from an earlier install
    # makes a name-based "already trusted" check say yes while the served
    # certificate is signed by the new one - the browser then rejects our DoH.
    # Remove same-subject certificates whose thumbprint differs from the current CA.
    $thumb = Get-CaThumbprint
    if (-not $thumb) { return }
    $store = New-Object System.Security.Cryptography.X509Certificates.X509Store('Root', 'CurrentUser')
    $store.Open('ReadWrite')
    $stale = @($store.Certificates | Where-Object { $_.Subject -match 'Sniroute Local' -and $_.Thumbprint -ne $thumb })
    foreach ($c in $stale) {
        try { $store.Remove($c); Write-Ok "removed stale user CA $($c.Thumbprint)" } catch { }
    }
    $store.Close()
}
function Install-CaUserStore {
    if (-not (Test-Path $CaCert)) { Write-Warn2 "CA file missing: $CaCert"; return $false }
    if (Test-CaInUserStore) { Write-Ok 'root CA already trusted (current user)'; return $true }
    # -user: current user's store, no elevation, no UAC
    certutil.exe -user -addstore -f Root $CaCert | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Warn2 "certutil failed with exit code $LASTEXITCODE"; return $false }
    Write-Ok 'root CA installed into Trusted Root (current user, no admin needed)'
    return $true
}

function Remove-CaUserStore {
    $thumb = Get-CaThumbprint
    if (-not $thumb) { return }
    if (Test-CaInUserStore) {
        certutil.exe -user -delstore Root $thumb | Out-Null
        Write-Ok 'root CA removed from the current user store'
    }
}

function Read-HostsText {
    $bytes = [System.IO.File]::ReadAllBytes($HostsPath)
    return (New-Object System.Text.UTF8Encoding($false)).GetString($bytes)
}

function Write-HostsText([string]$text) {
    # never write a BOM: the DNS client misreads the first line if one is present
    [System.IO.File]::WriteAllText($HostsPath, $text, (New-Object System.Text.UTF8Encoding($false)))
}

function Get-HostsBlockPattern {
    return "(?s)\r?\n*" + [regex]::Escape($BlockBegin) + ".*?" + [regex]::Escape($BlockEnd) + "\r?\n?"
}

function Test-HostsEntry {
    if (-not (Test-Path $HostsPath)) { return $false }
    return (Read-HostsText) -match [regex]::Escape($BlockBegin)
}

function Add-HostsEntry {
    if (-not (Test-Admin)) {
        Write-Warn2 'hosts needs administrator rights; skipped (the IP-based DoH does not need it)'
        return $false
    }
    $text = [regex]::Replace((Read-HostsText), (Get-HostsBlockPattern), "`n")
    $block = "$BlockBegin`n# written by sniroute`n127.0.0.1`t$DohHost`n$BlockEnd`n"
    Write-HostsText ($text.TrimEnd() + "`n`n" + $block)
    Write-Ok "hosts updated: 127.0.0.1 $DohHost"
    ipconfig /flushdns | Out-Null
    return $true
}

function Remove-HostsEntry {
    if (-not (Test-Admin)) { Write-Warn2 'hosts needs administrator rights; skipped'; return }
    $text = [regex]::Replace((Read-HostsText), (Get-HostsBlockPattern), "`n")
    Write-HostsText $text
    Write-Ok 'hosts entry removed'
    ipconfig /flushdns | Out-Null
}

# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '=== sniroute: one-time setup ===' -ForegroundColor Cyan
Write-Host "DoH endpoint: $DohUrl"

if ($Check) {
    Write-Host ''
    $userStore = Test-CaInUserStore
    $listening = [bool](Get-NetTCPConnection -State Listen -LocalPort 5444 -ErrorAction SilentlyContinue)
    Write-Host '=== state ===' -ForegroundColor Cyan
    Write-Host "  CA file present        : $(Test-Path $CaCert)"
    Write-Host "  CA trusted (this user) : $userStore"
    Write-Host "  DoH listening :5444    : $listening"
    Write-Host "  hosts entry            : $(Test-HostsEntry)"
    Write-Host "  running as admin       : $(Test-Admin)"
    exit 0
}

if ($Revert) {
    Remove-CaUserStore
    if (Test-HostsEntry) { Remove-HostsEntry }
    Write-Host ''
    Write-Host 'Reverted. In the browser set Secure DNS back to "Off".' -ForegroundColor Green
    exit 0
}

Remove-StaleCa
$ok = Install-CaUserStore
if ($WithHosts) { Add-HostsEntry | Out-Null }

Write-Host ''
if ($ok) {
    Write-Host 'Done. Next step (once), in the browser:' -ForegroundColor Green
    Write-Host '  Settings > Privacy and security > Security > Use secure DNS > Custom'
    Write-Host "  $DohUrl"
}
else {
    Write-Host 'Setup incomplete: the CA could not be installed.' -ForegroundColor Yellow
    Write-Host 'Check that ca\rootCA.crt exists (run: sniroute.cmd gen-ca).'
}
