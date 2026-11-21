#requires -Version 5.1
<#
.SYNOPSIS
    Build a self-contained ZIP: Sniroute.exe + bundled Java runtime + bundled
    Python runtime + nginx.exe + this project's scripts and config.

.DESCRIPTION
    ASCII ONLY (see the note in setup-browser-ech.ps1 for why).

    Output layout inside the zip:

        Sniroute\
            Sniroute.exe            <- double-click launcher (jpackage app-image)
            runtime\                <- bundled Java runtime
            app\sniroute.jar        <- the GUI classes
            python\                 <- trimmed CPython 3.12 (PSF licence)
            nginx\nginx.exe         <- web server used for the image site
            scripts\ tools\ conf\ data\
            sniroute.cmd            <- CLI entry, still usable inside the package
            README.txt

    Nothing is installed system-wide; delete the folder to uninstall. The root
    CA and hosts entry are still user actions (they need administrator rights).

.PARAMETER Out
    Where to write the zip. Defaults to <repo>\dist.

.PARAMETER SkipZip
    Stop after assembling the folder (useful while iterating).
#>
param(
    [string]$Out,
    [switch]$SkipZip
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (-not $Out) { $Out = Join-Path $RepoRoot 'dist' }

$JdkBin = 'C:\Program Files\Java\jdk-27\bin'
$Jpackage = Join-Path $JdkBin 'jpackage.exe'
$JarExe = Join-Path $JdkBin 'jar.exe'
$Javac = Join-Path $JdkBin 'javac.exe'
$NginxSrc = if ($env:SNIROUTE_NGINX_SRC) { $env:SNIROUTE_NGINX_SRC } else {
    Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages\nginxinc.nginx_Microsoft.Winget.Source_8wekyb3d8bbwe\nginx-1.31.8\nginx.exe'
}
$PySrc = Join-Path $env:LOCALAPPDATA 'Programs\DSH Desktop\resources\office-runtime\primary-runtime\dependencies\python'

$BuildDir = Join-Path $RepoRoot 'build\package'
$StageDir = Join-Path $BuildDir 'Sniroute'
$Classes = Join-Path $RepoRoot 'build\gui-classes'
$JarPath = Join-Path $RepoRoot 'build\sniroute.jar'

function Step($msg) { Write-Host "== $msg" -ForegroundColor Cyan }
function Fail($msg) { Write-Host "!! $msg" -ForegroundColor Red; exit 1 }

# --- preflight -------------------------------------------------------------
Step 'preflight'
foreach ($tool in @($Jpackage, $JarExe, $Javac)) {
    if (-not (Test-Path $tool)) { Fail "missing $tool (install JDK 11+ and check its bin folder)" }
}
if (-not (Test-Path $NginxSrc)) { Fail "missing nginx.exe at $NginxSrc" }
if (-not (Test-Path $PySrc)) { Fail "missing Python runtime at $PySrc" }

# --- clean ----------------------------------------------------------------
Step 'clean'
if (Test-Path $BuildDir) { Remove-Item $BuildDir -Recurse -Force }
New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null

# --- java build -----------------------------------------------------------
Step 'compile java'
if (Test-Path $Classes) { Remove-Item $Classes -Recurse -Force }
New-Item -ItemType Directory -Path $Classes -Force | Out-Null
# Note: no -J-Duser.language here. PowerShell mangles that argument (javac then
# reports "invalid flag: .language=en"); cmd.exe passes it fine, which is why
# build-gui.cmd keeps it. English diagnostics only matter for interactive use.
& $Javac -encoding UTF-8 -d $Classes (Join-Path $RepoRoot 'gui\SnirouteGui.java')
if ($LASTEXITCODE -ne 0) { Fail 'javac failed' }

Step 'build jar'
& $JarExe --create --file $JarPath --main-class SnirouteGui -C $Classes .
if ($LASTEXITCODE -ne 0) { Fail 'jar failed' }

# --- jpackage: exe + bundled java runtime ---------------------------------
Step 'jpackage app-image'
# --input must contain ONLY the jar: pointing it at build\ would drag
# gui-classes\ and the previous package\ into app\ as well.
$JarInput = Join-Path $RepoRoot 'build\jarpkg'
if (Test-Path $JarInput) { Remove-Item $JarInput -Recurse -Force }
New-Item -ItemType Directory -Path $JarInput -Force | Out-Null
Copy-Item $JarPath $JarInput

& $Jpackage --type app-image --name Sniroute --input $JarInput `
    --main-jar 'sniroute.jar' --main-class SnirouteGui `
    --dest $BuildDir --app-version 1.0.0 `
    --java-options '-Xmx256m'
if ($LASTEXITCODE -ne 0) { Fail 'jpackage failed' }
if (-not (Test-Path (Join-Path $StageDir 'Sniroute.exe'))) { Fail 'Sniroute.exe not produced' }

# --- trim the Python runtime ---------------------------------------------
Step 'trim python runtime'
$PyDst = Join-Path $StageDir 'python'
New-Item -ItemType Directory -Path $PyDst -Force | Out-Null

# interpreter + native libs
Get-ChildItem $PySrc -File | Where-Object {
    $_.Name -match '^(python|pythonw)\.exe$' -or
    $_.Name -match '^python3.*\.dll$' -or
    $_.Name -match '^vcruntime.*\.dll$' -or
    $_.Name -match '^LICENSE\.txt$'
} | Copy-Item -Destination $PyDst

# extension modules live here
if (Test-Path (Join-Path $PySrc 'DLLs')) {
    Copy-Item (Join-Path $PySrc 'DLLs') (Join-Path $PyDst 'DLLs') -Recurse
}

# standard library, minus the parts nothing here uses
$skip = @('site-packages', 'test', 'tests', 'idlelib', 'tkinter', 'turtledemo',
    'ensurepip', 'lib2to3', 'venv', 'distutils', 'concurrent\__pycache__')
$LibSrc = Join-Path $PySrc 'Lib'
$LibDst = Join-Path $PyDst 'Lib'
New-Item -ItemType Directory -Path $LibDst -Force | Out-Null
Get-ChildItem $LibSrc -Force | Where-Object { $skip -notcontains $_.Name } | ForEach-Object {
    if ($_.PSIsContainer) {
        Copy-Item $_.FullName (Join-Path $LibDst $_.Name) -Recurse -Force
    } else {
        Copy-Item $_.FullName $LibDst -Force
    }
}

$pyExe = Join-Path $PyDst 'python.exe'
if (-not (Test-Path $pyExe)) { Fail 'trimmed python.exe missing' }
Step 'verify trimmed python can do what the tool needs'
$probe = & $pyExe -c "import ssl,socket,socketserver,http.server,concurrent.futures,ipaddress,urllib.request,email.parser,json,base64,tempfile,argparse,threading;print('ok')" 2>&1
if ($probe -notmatch 'ok') { Fail "trimmed python broken: $probe" }
Write-Host "   python ok ($([math]::Round((Get-ChildItem $PyDst -Recurse -File | Measure-Object Length -Sum).Sum / 1MB, 1)) MB)"

# --- nginx ----------------------------------------------------------------
Step 'copy nginx'
$NginxDst = Join-Path $StageDir 'nginx'
New-Item -ItemType Directory -Path $NginxDst -Force | Out-Null
Copy-Item $NginxSrc (Join-Path $NginxDst 'nginx.exe') -Force

# --- openssl (needed to sign the local CA and leaf certs) -----------------
Step 'copy openssl'
# Not a bare openssl.exe: OpenSSL 3 derives its prefix from the executable path
# and then looks for etc\ssl\openssl.cnf and lib\ossl-modules. Copying Git's
# mingw64 layout files reproduces that, and only these six files are needed.
$OsslSrc = 'C:\Program Files\Git\mingw64'
$OsslBin = Join-Path $StageDir 'openssl\mingw64\bin'
New-Item -ItemType Directory -Path $OsslBin -Force | Out-Null
foreach ($f in 'openssl.exe', 'libcrypto-3-x64.dll', 'libssl-3-x64.dll', 'zlib1.dll') {
    $srcFile = Join-Path (Join-Path $OsslSrc 'bin') $f
    if (-not (Test-Path $srcFile)) { Fail "missing openssl component $srcFile" }
    Copy-Item $srcFile $OsslBin -Force
}
$Modules = Join-Path $StageDir 'openssl\mingw64\lib\ossl-modules'
New-Item -ItemType Directory -Path $Modules -Force | Out-Null
Copy-Item (Join-Path $OsslSrc 'lib\ossl-modules\*') $Modules -Force
$SslCnf = Join-Path $StageDir 'openssl\mingw64\etc\ssl'
New-Item -ItemType Directory -Path $SslCnf -Force | Out-Null
Copy-Item (Join-Path $OsslSrc 'etc\ssl\openssl.cnf') $SslCnf -Force

Step 'verify bundled openssl can sign'
# openssl writes progress dots to stderr; invoking it directly makes PowerShell
# 5.1 raise NativeCommandError under $ErrorActionPreference='Stop'. Start-Process
# with explicit redirection keeps the build quiet and the exit code meaningful.
$testKey = Join-Path $BuildDir 'smoke-key.pem'
$testCrt = Join-Path $BuildDir 'smoke-cert.pem'
$osslOut = Join-Path $BuildDir 'openssl.log'
$osslArgs = @('req', '-x509', '-new', '-nodes', '-newkey', 'rsa:2048', '-sha256',
    '-days', '1', '-keyout', $testKey, '-out', $testCrt,
    '-subj', '/CN=smoke', '-addext', 'basicConstraints=critical,CA:TRUE')
$proc = Start-Process -FilePath (Join-Path $OsslBin 'openssl.exe') -ArgumentList $osslArgs `
    -NoNewWindow -Wait -PassThru `
    -RedirectStandardOutput $osslOut -RedirectStandardError "$osslOut.err"
if (-not (Test-Path $testCrt)) {
    Get-Content "$osslOut.err" -ErrorAction SilentlyContinue | Select-Object -First 5 | ForEach-Object { Write-Host "   $_" }
    Fail 'bundled openssl cannot sign certificates'
}
Remove-Item $testKey, $testCrt, $osslOut, "$osslOut.err" -Force -ErrorAction SilentlyContinue
Write-Host '   openssl ok'

# --- project payload ------------------------------------------------------
Step 'copy project files'
foreach ($item in @('scripts', 'tools', 'conf', 'data', 'sniroute.cmd', 'uninstall.cmd')) {
    $src = Join-Path $RepoRoot $item
    if (-not (Test-Path $src)) { Fail "missing $item" }
    Copy-Item $src (Join-Path $StageDir $item) -Recurse -Force
}
# runtime state must not ship
foreach ($junk in @('logs', 'temp', 'chromeprofile', '__pycache__', 'build')) {
    Get-ChildItem $StageDir -Recurse -Directory -Filter $junk -ErrorAction SilentlyContinue |
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
}
# generated certs are per-machine: ship nothing, tell the user to make their own
$certs = Join-Path $StageDir 'conf\certs'
if (Test-Path $certs) { Remove-Item $certs -Recurse -Force }
# an absolute nginx.path from the dev machine would override the bundled nginx
$pathFile = Join-Path $StageDir 'nginx.path'
if (Test-Path $pathFile) { Remove-Item $pathFile -Force }

$readme = @"
sniroute - portable package
===========================

What is in here
  Sniroute.exe      double-click to open the window
  python\           bundled Python 3.12 runtime (PSF licence)
  nginx\nginx.exe   web server used for the image site
  scripts\ tools\ conf\ data\   the tool itself
  sniroute.cmd      command line, same features as the window

First run (once)
  1. Open the window and click the CA/hosts button, or run in an elevated
     PowerShell:
         powershell -ExecutionPolicy Bypass -File scripts\setup-browser-ech.ps1 -Elevated
  2. In the browser, set Secure DNS to:
         https://doh.sniroute.lan:5444/dns-query
  3. Click "start all".

Note: the root certificate and the hosts entry are the only two things that need
administrator rights. Nothing else touches the system, and deleting this folder
is the uninstall (after using "revert" in the window).

Licences: CPython (PSF-2.0), nginx (BSD-2-Clause), this project's own code as
described in the repository.
"@
Set-Content -Path (Join-Path $StageDir 'README.txt') -Value $readme -Encoding UTF8

# Chinese guide lives in data\README-zh.txt so this script stays pure ASCII.
# It is shipped under an ASCII filename for the same reason.
$zhReadme = Join-Path $RepoRoot 'data\README-zh.txt'
if (Test-Path $zhReadme) {
    $zhText = Get-Content $zhReadme -Raw -Encoding UTF8
    # UTF-8 with BOM: Notepad on older Windows versions misreads plain UTF-8
    [System.IO.File]::WriteAllText((Join-Path $StageDir 'README-zh.txt'), $zhText,
        (New-Object System.Text.UTF8Encoding($true)))
}
else {
    Write-Host '   [!] data\README-zh.txt missing; package will only have the English readme'
}

# --- zip ------------------------------------------------------------------
$totalMb = [math]::Round((Get-ChildItem $StageDir -Recurse -File | Measure-Object Length -Sum).Sum / 1MB, 1)
Write-Host "   staged: $totalMb MB"

if ($SkipZip) {
    Write-Host "Skipping zip (-SkipZip). Folder: $StageDir" -ForegroundColor Green
    exit 0
}

Step 'zip'
New-Item -ItemType Directory -Path $Out -Force | Out-Null
$zip = Join-Path $Out 'sniroute-portable-win64.zip'
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path $StageDir -DestinationPath $zip -CompressionLevel Optimal
$zipMb = [math]::Round((Get-Item $zip).Length / 1MB, 1)
Write-Host "Done: $zip ($zipMb MB)" -ForegroundColor Green
