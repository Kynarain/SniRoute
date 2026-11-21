#requires -Version 5.1
<#
.SYNOPSIS
    Build a per-user MSI installer (no UAC, choose-your-own install directory).

.DESCRIPTION
    ASCII ONLY. Do not add non-ASCII characters to this file.

    Why this is possible without administrator rights:
      * jpackage --win-per-user-install produces an MSI that installs into the
        user's own profile, so Windows never asks for elevation.
      * The CA is installed into the CURRENT USER's trust store by the app on
        first launch (setup-browser-ech.ps1), which also needs no elevation.
      * The DoH endpoint is addressed as https://127.0.0.1:5444/... so the
        protected hosts file is untouched.

    WiX: jpackage needs candle.exe/light.exe from WiX 3.x on PATH. The binaries
    are expected in build\wix (see the note in the repository README) because
    the WiX installer itself wants administrator rights and .NET 3.5.

    Output: dist\Sniroute-<version>.msi
#>
param(
    [string]$Version = '1.0.0',
    [string]$Out
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (-not $Out) { $Out = Join-Path $RepoRoot 'dist' }

$JdkBin = 'C:\Program Files\Java\jdk-27\bin'
$Jpackage = Join-Path $JdkBin 'jpackage.exe'
$JarExe = Join-Path $JdkBin 'jar.exe'
$Javac = Join-Path $JdkBin 'javac.exe'
$Wix = Join-Path $RepoRoot 'build\wix'
$NginxSrc = if ($env:SNIROUTE_NGINX_SRC) { $env:SNIROUTE_NGINX_SRC } else {
    Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages\nginxinc.nginx_Microsoft.Winget.Source_8wekyb3d8bbwe\nginx-1.31.8\nginx.exe'
}
$OsslSrc = 'C:\Program Files\Git\mingw64'
$PySrc = Join-Path $env:LOCALAPPDATA 'Programs\DSH Desktop\resources\office-runtime\primary-runtime\dependencies\python'

$BuildDir = Join-Path $RepoRoot 'build\msi'
$Payload = Join-Path $BuildDir 'payload'
$Classes = Join-Path $RepoRoot 'build\gui-classes'
$JarInput = Join-Path $BuildDir 'jar'
$Icon = Join-Path $RepoRoot 'gui\icon.ico'

function Step($m) { Write-Host "== $m" -ForegroundColor Cyan }
function Fail($m) { Write-Host "!! $m" -ForegroundColor Red; exit 1 }

Step 'preflight'
if (-not (Test-Path $Jpackage)) { Fail "missing $Jpackage" }
foreach ($tool in 'candle.exe', 'light.exe') {
    if (-not (Test-Path (Join-Path $Wix $tool))) {
        Fail "missing $tool in build\wix - jpackage cannot build an MSI without WiX 3.x"
    }
}
if (-not (Test-Path $Icon)) { Fail "missing $Icon (run: python tools\make_icon.py)" }
$env:PATH = $Wix + ';' + $env:PATH

Step 'clean'
if (Test-Path $BuildDir) { Remove-Item $BuildDir -Recurse -Force }
New-Item -ItemType Directory -Path $BuildDir, $Payload, $JarInput -Force | Out-Null

Step 'compile java'
if (Test-Path $Classes) { Remove-Item $Classes -Recurse -Force }
New-Item -ItemType Directory -Path $Classes -Force | Out-Null
& $Javac -encoding UTF-8 -d $Classes (Join-Path $RepoRoot 'gui\SnirouteGui.java')
if ($LASTEXITCODE -ne 0) { Fail 'javac failed' }

Step 'build jar'
$Jar = Join-Path $JarInput 'sniroute.jar'
& $JarExe --create --file $Jar --main-class SnirouteGui -C $Classes .
if ($LASTEXITCODE -ne 0) { Fail 'jar failed' }

Step 'stage payload'
# Everything the app needs at runtime, next to the launcher inside the app dir.
foreach ($item in @('scripts', 'tools', 'conf', 'data', 'sniroute.cmd', 'uninstall.cmd')) {
    Copy-Item (Join-Path $RepoRoot $item) (Join-Path $Payload $item) -Recurse -Force
}
foreach ($junk in @('logs', 'temp', 'chromeprofile', '__pycache__')) {
    Get-ChildItem $Payload -Recurse -Directory -Filter $junk -ErrorAction SilentlyContinue |
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
}
$certs = Join-Path $Payload 'conf\certs'
if (Test-Path $certs) { Remove-Item $certs -Recurse -Force }   # per-machine, generated on first run
$nginxPath = Join-Path $Payload 'nginx.path'
if (Test-Path $nginxPath) { Remove-Item $nginxPath -Force }

# bundled runtimes: nginx, openssl (Git layout), trimmed python
$NginxDst = Join-Path $Payload 'nginx'
New-Item -ItemType Directory -Path $NginxDst -Force | Out-Null
Copy-Item $NginxSrc (Join-Path $NginxDst 'nginx.exe') -Force

$OsslBin = Join-Path $Payload 'openssl\mingw64\bin'
New-Item -ItemType Directory -Path $OsslBin -Force | Out-Null
foreach ($f in 'openssl.exe', 'libcrypto-3-x64.dll', 'libssl-3-x64.dll', 'zlib1.dll') {
    Copy-Item (Join-Path $OsslSrc "bin\$f") $OsslBin -Force
}
$Modules = Join-Path $Payload 'openssl\mingw64\lib\ossl-modules'
New-Item -ItemType Directory -Path $Modules -Force | Out-Null
Copy-Item (Join-Path $OsslSrc 'lib\ossl-modules\*') $Modules -Force
$SslCnf = Join-Path $Payload 'openssl\mingw64\etc\ssl'
New-Item -ItemType Directory -Path $SslCnf -Force | Out-Null
Copy-Item (Join-Path $OsslSrc 'etc\ssl\openssl.cnf') $SslCnf -Force

# reuse the python runtime already trimmed by build-package.ps1 if present
$ReusePy = Join-Path $RepoRoot 'build\package\Sniroute\python'
if (Test-Path (Join-Path $ReusePy 'python.exe')) {
    Copy-Item $ReusePy (Join-Path $Payload 'python') -Recurse -Force
}
else {
    Write-Host '   [!] no trimmed python found; run build-package.ps1 once first'
    Fail 'python runtime missing'
}
$pyExe = Join-Path $Payload 'python\python.exe'
$probe = & $pyExe -c "import ssl,socketserver,http.server;print('ok')" 2>&1
if ($probe -notmatch 'ok') { Fail "staged python is broken: $probe" }

# tray icon: must end up as <app>\gui\icon.png, so the DIRECTORY is created first.
# (Copying a file onto a not-yet-existing path named "gui" silently creates a
#  FILE called "gui" - which then breaks both the directory and the icon lookup.)
$guiDst = Join-Path $Payload 'gui'
New-Item -ItemType Directory -Path $guiDst -Force | Out-Null
Copy-Item (Join-Path $RepoRoot 'gui\icon.png') $guiDst -Force
$zhReadme = Join-Path $RepoRoot 'data\README-zh.txt'
if (Test-Path $zhReadme) {
    [System.IO.File]::WriteAllText((Join-Path $Payload 'README-zh.txt'),
        (Get-Content $zhReadme -Raw -Encoding UTF8), (New-Object System.Text.UTF8Encoding($true)))
}

Step 'jpackage msi'
$msiDir = Join-Path $BuildDir 'out'
New-Item -ItemType Directory -Path $msiDir -Force | Out-Null
# --app-content copies each path it is given. Passing the payload DIRECTORY would
# nest everything under an extra "payload\" folder, and the app locates itself by
# walking up until it finds sniroute.cmd - so the files must sit next to the exe.
$contentItems = @('scripts', 'tools', 'conf', 'data', 'python', 'nginx', 'openssl', 'gui',
    'sniroute.cmd', 'uninstall.cmd', 'README-zh.txt')
$contentArg = ($contentItems |
    ForEach-Object { Join-Path $Payload $_ } |
    Where-Object { Test-Path $_ }) -join ','
Write-Host "   app-content: $((($contentArg -split ',').Count)) 项"

$argList = @(
    '--type', 'msi',
    '--name', 'Sniroute',
    '--app-version', $Version,
    '--input', $JarInput,
    '--main-jar', 'sniroute.jar',
    '--main-class', 'SnirouteGui',
    '--icon', $Icon,
    '--dest', $msiDir,
    '--app-content', $contentArg,
    '--java-options', '-Dfile.encoding=UTF-8',
    '--win-per-user-install',
    '--win-dir-chooser',
    '--win-menu',
    '--win-shortcut',
    '--win-upgrade-uuid', '7f1c2a54-2f2e-4b7a-9d63-5b2a4f0d9c11',
    '--vendor', 'sniroute',
    '--description', 'sniroute'
)
& $Jpackage @argList
if ($LASTEXITCODE -ne 0) { Fail 'jpackage failed' }

$msi = Get-ChildItem $msiDir -Filter *.msi | Select-Object -First 1
if (-not $msi) { Fail 'no msi produced' }

New-Item -ItemType Directory -Path $Out -Force | Out-Null
$target = Join-Path $Out $msi.Name
Copy-Item $msi.FullName $target -Force
Write-Host ''
Write-Host ("Done: {0} ({1:N1} MB)" -f $target, ((Get-Item $target).Length / 1MB)) -ForegroundColor Green
Write-Host 'Install: double-click the msi (per-user, no UAC, you pick the folder).'
Write-Host 'Silent:  msiexec /i "Sniroute-<ver>.msi" /qn INSTALLDIR="D:\Apps\Sniroute"'
