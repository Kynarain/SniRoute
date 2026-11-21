#requires -Version 5.1
<#
.SYNOPSIS
    Undo everything sniroute configured on this machine.

.DESCRIPTION
    ASCII ONLY. Do not add non-ASCII characters to this file.

    Reverting by deleting files is not enough: the root CA stays in the trust
    store and the browser keeps pointing at a DoH endpoint that no longer exists.
    This wrapper removes both, in the order that keeps the machine usable:

      1. stop the background services (so nginx releases its port and files);
      2. drop the browser DoH policy (elevated only if the policy lives in HKLM);
      3. remove the local root CA from the current user's trust store;
      4. remove the optional hosts entry, if one was ever added.

    It deliberately does not delete the install directory - the uninstaller does
    that, and a user may want to keep logs for diagnosis.
#>
param(
    [switch]$Quiet
)

$ErrorActionPreference = 'Continue'
$Root = Split-Path -Parent $PSScriptRoot
$Python = Join-Path $Root 'python\python.exe'
$Cli = Join-Path $Root 'scripts\sniroute.py'
$Setup = Join-Path $PSScriptRoot 'setup-browser-ech.ps1'
$Policy = Join-Path $PSScriptRoot 'set-browser-policy.ps1'
$Nrpt = Join-Path $PSScriptRoot 'nrpt.ps1'

function Say([string]$m) { if (-not $Quiet) { Write-Host "  $m" } }

Say 'stopping services'
if ((Test-Path $Python) -and (Test-Path $Cli)) {
    & $Python $Cli down 2>&1 | Out-Null
}

Say 'removing global DNS mode (if installed)'
if (Test-Path $Nrpt) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File $Nrpt -Revert 2>&1 | Out-Null
}

Say 'removing the browser DoH policy'
if (Test-Path $Policy) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File $Policy -Browsers Chrome -Revert 2>&1 | Out-Null
}

Say 'removing the local root CA'
if (Test-Path $Setup) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File $Setup -Revert 2>&1 | Out-Null
}

if (-not $Quiet) {
    Write-Host ''
    Write-Host 'Done. Restart the browser; it will resolve names normally again.'
}
exit 0
