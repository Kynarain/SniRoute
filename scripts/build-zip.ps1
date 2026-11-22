#requires -Version 5.1
<#
.SYNOPSIS
    Build the portable ZIP from the C++ build (no JVM).

.DESCRIPTION
    ASCII ONLY. Do not add non-ASCII characters to this file.

    Same payload the MSI installs, just zipped: unzip anywhere, run Sniroute.exe.
    The Java app-image packaging (build-package.ps1) is now legacy and only kept
    to regenerate the trimmed python runtime it harvests.

    Output: dist\sniroute-portable-win64.zip
#>
param(
    [string]$Out = ''
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (-not $Out) { $Out = Join-Path $RepoRoot 'dist' }
$Stage = Join-Path $RepoRoot 'build\zip\Sniroute'

function Step($m) { Write-Host "== $m" -ForegroundColor Cyan }
function Fail($m) { Write-Host "!! $m" -ForegroundColor Red; exit 1 }

Step 'site list'
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $RepoRoot 'scripts\gen-sites-list.ps1') | Out-Null

Step 'build tray exe'
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $RepoRoot 'scripts\build-tray.ps1')
if ($LASTEXITCODE -ne 0) { Fail 'tray build failed' }

Step 'stage'
if (Test-Path $Stage) { Remove-Item $Stage -Recurse -Force }
New-Item -ItemType Directory -Path $Stage -Force | Out-Null
Copy-Item (Join-Path $RepoRoot 'build\tray\Sniroute.exe') (Join-Path $Stage 'Sniroute.exe') -Force

New-Item -ItemType Directory -Path (Join-Path $Stage 'scripts') -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $Stage 'tools') -Force | Out-Null
foreach ($s in 'sniroute.py', 'setup-browser-ech.ps1', 'start-chrome-ech.ps1', 'start-chrome-ech.cmd',
               'uninstall.ps1', 'doh-ech.cmd', 'stop-for-upgrade.cmd', 'first-run-setup.ps1', 'revert-all.ps1', 'set-browser-policy.ps1', 'nrpt.ps1') {
    Copy-Item (Join-Path $RepoRoot "scripts\$s") (Join-Path $Stage 'scripts') -Force
}
foreach ($s in 'doh_ech.py', 'doh_selftest.py') {
    Copy-Item (Join-Path $RepoRoot "tools\$s") (Join-Path $Stage 'tools') -Force
}
foreach ($item in @('conf', 'data', 'gui', 'sniroute.cmd', 'uninstall.cmd', 'LICENSE', 'licenses', 'THIRD_PARTY_LICENSES.txt')) {
    Copy-Item (Join-Path $RepoRoot $item) (Join-Path $Stage $item) -Recurse -Force
}
foreach ($junk in @('logs', 'temp', 'chromeprofile', '__pycache__')) {
    Get-ChildItem $Stage -Recurse -Directory -Filter $junk -ErrorAction SilentlyContinue |
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
}
$certs = Join-Path $Stage 'conf\certs'
if (Test-Path $certs) { Remove-Item $certs -Recurse -Force }

$ReusePy = Join-Path $RepoRoot 'build\package\Sniroute\python'
if (Test-Path (Join-Path $ReusePy 'python.exe')) {
    # 敏感运行期文件绝不能进发布包：WARP 账号含私钥，DoH 缓存含浏览记录。
foreach ($leak in @('data\warp-account.json', 'data\doh-cache.json', 'data\doh-cache.tmp')) {
    $lp = Join-Path $Stage $leak
    if (Test-Path $lp) {
        Remove-Item $lp -Force -ErrorAction SilentlyContinue
        Write-Host ("  removed sensitive: " + $leak)
    }
}

Copy-Item $ReusePy (Join-Path $Stage 'python') -Recurse -Force
} else {
    Fail 'no trimmed python runtime (run build-package.ps1 once to create it)'
}
$NginxSrc = if ($env:SNIROUTE_NGINX_SRC) { $env:SNIROUTE_NGINX_SRC } else {
    Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages\nginxinc.nginx_Microsoft.Winget.Source_8wekyb3d8bbwe\nginx-1.31.8\nginx.exe'
}
New-Item -ItemType Directory -Path (Join-Path $Stage 'nginx') -Force | Out-Null
Copy-Item $NginxSrc (Join-Path $Stage 'nginx\nginx.exe') -Force

$OsslSrc = 'C:\Program Files\Git\mingw64'
$OsslBin = Join-Path $Stage 'openssl\mingw64\bin'
New-Item -ItemType Directory -Path $OsslBin -Force | Out-Null
foreach ($f in 'openssl.exe', 'libcrypto-3-x64.dll', 'libssl-3-x64.dll', 'zlib1.dll') {
    Copy-Item (Join-Path $OsslSrc "bin\$f") $OsslBin -Force
}
New-Item -ItemType Directory -Path (Join-Path $Stage 'openssl\mingw64\lib\ossl-modules') -Force | Out-Null
Copy-Item (Join-Path $OsslSrc 'lib\ossl-modules\*') (Join-Path $Stage 'openssl\mingw64\lib\ossl-modules') -Force
New-Item -ItemType Directory -Path (Join-Path $Stage 'openssl\mingw64\etc\ssl') -Force | Out-Null
Copy-Item (Join-Path $OsslSrc 'etc\ssl\openssl.cnf') (Join-Path $Stage 'openssl\mingw64\etc\ssl') -Force

$zhReadme = Join-Path $RepoRoot 'data\README-zh.txt'
if (Test-Path $zhReadme) {
    [System.IO.File]::WriteAllText((Join-Path $Stage 'README-zh.txt'),
        (Get-Content $zhReadme -Raw -Encoding UTF8), (New-Object System.Text.UTF8Encoding($true)))
}

"   staged: $([math]::Round((Get-ChildItem $Stage -Recurse -File | Measure-Object Length -Sum).Sum / 1MB, 1)) MB"
"   files : $((Get-ChildItem $Stage -Recurse -File).Count)"

Step 'zip'
New-Item -ItemType Directory -Path $Out -Force | Out-Null
$zip = Join-Path $Out 'sniroute-portable-win64.zip'
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path $Stage -DestinationPath $zip -CompressionLevel Optimal
Write-Host ''
Write-Host ("Done: {0} ({1:N1} MB)" -f $zip, ((Get-Item $zip).Length / 1MB)) -ForegroundColor Green

# ZIP 不支持 Authenticode，只生成 SHA-256。
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'sign-and-hash.ps1') -Path $zip -HashOnly 2>&1 | Out-Null
