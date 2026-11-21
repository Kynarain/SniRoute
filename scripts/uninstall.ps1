#requires -Version 5.1
<#
.SYNOPSIS
    Uninstall helper: stop services, remove the local CA, then remove the app.

.DESCRIPTION
    ASCII ONLY (see the note in setup-browser-ech.ps1 for why).

    Windows already offers its own uninstall entry (Settings > Apps), which only
    deletes files. This helper exists because deleting files is not enough:
    the root CA would stay in the user's trust store, and the background
    services would keep running from a deleted folder.

    Order matters: services first (their binaries are locked while running),
    then the certificate, then the MSI removal.
#>
param(
    [switch]$KeepCertificate,
    [switch]$Quiet
)

$ErrorActionPreference = 'Continue'
$RepoRoot = Split-Path -Parent $PSScriptRoot
$Python = Join-Path $RepoRoot 'python\python.exe'
$Cli = Join-Path $RepoRoot 'scripts\sniroute.py'

function Say([string]$m) { if (-not $Quiet) { Write-Host "  $m" } }

Say 'stopping services'
if ((Test-Path $Python) -and (Test-Path $Cli)) {
    & $Python $Cli down 2>&1 | Out-Null
}
else {
    # no bundled runtime: fall back to killing what we own by pid file
    $pidFile = Join-Path $RepoRoot 'logs\nginx.pid'
    if (Test-Path $pidFile) {
        $nginxPid = (Get-Content $pidFile -Raw).Trim()
        if ($nginxPid) { taskkill /F /PID $nginxPid 2>&1 | Out-Null }
    }
}

if (-not $KeepCertificate) {
    Say 'removing the local CA from the user trust store'
    $setup = Join-Path $RepoRoot 'scripts\setup-browser-ech.ps1'
    if (Test-Path $setup) {
        & powershell -NoProfile -ExecutionPolicy Bypass -File $setup -Revert 2>&1 | Out-Null
    }
}
else {
    Say 'keeping the local CA (-KeepCertificate)'
}

# Find our own MSI entry the same way Windows does: by display name.
Say 'looking for the installed product'
$keys = @(
    'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall',
    'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall',
    'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall'
)
$product = $null
foreach ($root in $keys) {
    if (-not (Test-Path $root)) { continue }
    Get-ChildItem $root -ErrorAction SilentlyContinue | ForEach-Object {
        if ($product) { return }
        $props = Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue
        if ($props.DisplayName -eq 'Sniroute' -and $_.PSChildName -match '^\{') {
            $product = $_.PSChildName
        }
    }
}

if ($product) {
    Say "removing via Windows Installer ($product)"

    # The MSI only deletes files IT installed. Everything the app generated at
    # runtime (ca\, logs\, conf\certs ...) survives and leaves the folder behind.
    # A detached helper waits for msiexec, then removes just those generated
    # paths, and the folder only if nothing foreign is left inside.
    $cleanupScript = Join-Path $env:TEMP 'sniroute-cleanup.ps1'
    $generated = @('ca', 'logs', 'temp', 'chromeprofile', 'conf\certs', 'data\.first-run-declined')
    $lines = @(
        '# wait for msiexec to finish its own removal, then clean generated data',
        '# Leave the folder first: a process cannot delete its own working directory,',
        '# and this script inherits the install folder as CWD.',
        'Set-Location $env:TEMP',
        ('$root = ''' + $RepoRoot + ''''),
        ('$rels = @(' + (($generated | ForEach-Object { "'" + $_ + "'" }) -join ', ') + ')'),
        '$deadline = (Get-Date).AddSeconds(90)',
        'while ((Get-Date) -lt $deadline) {',
        '    Start-Sleep -Seconds 3',
        '    foreach ($rel in $rels) {',
        '        $p = Join-Path $root $rel',
        '        if (Test-Path $p) { Remove-Item $p -Recurse -Force -ErrorAction SilentlyContinue }',
        '    }',
        '    if (-not (Test-Path $root)) { break }',
        '    $left = @(Get-ChildItem $root -Recurse -Force -ErrorAction SilentlyContinue)',
        '    if ($left.Count -eq 0) {',
        '        Remove-Item $root -Recurse -Force -ErrorAction SilentlyContinue',
        '        if (-not (Test-Path $root)) { break }',
        '    }',
        '}'
    )
    Set-Content -Path $cleanupScript -Value $lines -Encoding UTF8
    Start-Process -FilePath 'powershell.exe' -WindowStyle Hidden -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $cleanupScript
    ) | Out-Null

    # Not -Wait: the installer deletes this directory, so we must exit first.
    Start-Process msiexec.exe -ArgumentList @('/x', $product, '/qb') | Out-Null
    if (-not $Quiet) {
        Write-Host ''
        Write-Host 'The Windows Installer window will finish the removal.' -ForegroundColor Green
        Write-Host 'Certs and logs created at runtime are cleaned up automatically.'
        Write-Host 'Remember to set the browser Secure DNS back to "Off".'
    }
}
else {
    if (-not $Quiet) {
        Write-Host ''
        Write-Host 'No MSI install found (portable copy).' -ForegroundColor Yellow
        Write-Host 'Done here - delete this folder to finish.'
        Write-Host 'Remember to set the browser Secure DNS back to "Off".'
    }
}
