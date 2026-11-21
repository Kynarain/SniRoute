# Build the standalone test client APK (no Gradle).
#
# Why: Android's VpnService only excludes the VPN app itself from its own tunnel.
# A second package (different UID) does route through the tunnel, so this client
# can verify the userspace TCP stack end to end with plain HTTP.
#
# ASCII ONLY (PowerShell 5.1 mis-parses BOM-less UTF-8 CJK).

param(
    [string]$Version = "3.9.6",
    [string]$AndroidSdk = $(if ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT } elseif ($env:ANDROID_HOME) { $env:ANDROID_HOME } else { "$env:LOCALAPPDATA\Android\Sdk" }),
    [string]$Jdk = $(if ($env:JAVA_HOME) { $env:JAVA_HOME } else { "C:\Program Files\Java\jdk-17" }),
    [string]$KeyStore = ""
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
$Src = Join-Path $RepoRoot 'android-test'
$BuildDir = Join-Path $RepoRoot 'build\android-test'
$DistDir = Join-Path $RepoRoot 'dist'
$Out = Join-Path $DistDir "sniroute-$Version-testclient.apk"

if (-not $KeyStore) { $KeyStore = Join-Path (Join-Path $RepoRoot 'build\android') 'sniroute.keystore' }

$BuildTools = Join-Path $AndroidSdk 'build-tools\34.0.0'
$AndroidJar = Join-Path $AndroidSdk 'platforms\android-34\android.jar'
$aapt2 = Join-Path $BuildTools 'aapt2.exe'
$d8 = Join-Path $BuildTools 'd8.bat'
$zipalign = Join-Path $BuildTools 'zipalign.exe'
$apksigner = Join-Path $BuildTools 'apksigner.bat'
$javac = Join-Path $Jdk 'bin\javac.exe'

foreach ($p in @($aapt2, $d8, $zipalign, $apksigner, $javac, $AndroidJar)) {
    if (-not (Test-Path $p)) { throw "missing build tool: $p" }
}

Remove-Item $BuildDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null

Write-Host '== 1/5 link manifest (aapt2 link)'
$baseApk = Join-Path $BuildDir 'base.apk'
$genDir = Join-Path $BuildDir 'gen'
New-Item -ItemType Directory -Path $genDir -Force | Out-Null
& $aapt2 link -o $baseApk `
    -I $AndroidJar `
    --manifest (Join-Path $Src 'AndroidManifest.xml') `
    --java $genDir `
    --min-sdk-version 26 --target-sdk-version 33 `
    --version-code 1 --version-name "$Version-test1"
if ($LASTEXITCODE -ne 0) { throw 'aapt2 link failed' }

Write-Host '== 2/5 compile java'
$classesDir = Join-Path $BuildDir 'classes'
New-Item -ItemType Directory -Path $classesDir -Force | Out-Null
$sources = @()
$sources += Get-ChildItem (Join-Path $Src 'src') -Recurse -Filter *.java | ForEach-Object { $_.FullName }
$sources += Get-ChildItem $genDir -Recurse -Filter *.java -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName }
& $javac -encoding UTF-8 -source 8 -target 8 -bootclasspath $AndroidJar -classpath $AndroidJar -d $classesDir $sources
if ($LASTEXITCODE -ne 0) { throw 'javac failed' }

Write-Host '== 3/5 dex (d8)'
$dexDir = Join-Path $BuildDir 'dex'
New-Item -ItemType Directory -Path $dexDir -Force | Out-Null
$classFiles = Get-ChildItem $classesDir -Recurse -Filter *.class | ForEach-Object { $_.FullName }
& $d8 --lib $AndroidJar --min-api 26 --output $dexDir $classFiles
if ($LASTEXITCODE -ne 0) { throw 'd8 failed' }

Write-Host '== 4/5 package'
$unsigned = Join-Path $BuildDir 'unsigned.apk'
Copy-Item $baseApk $unsigned -Force
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::Open($unsigned, 'Update')
$dexFile = Join-Path $dexDir 'classes.dex'
$entry = $zip.GetEntry('classes.dex')
if ($entry) { $entry.Delete() }
[System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $dexFile, 'classes.dex') | Out-Null
$zip.Dispose()

Write-Host '== 5/5 align + sign'
$aligned = Join-Path $BuildDir 'aligned.apk'
& $zipalign -f 4 $unsigned $aligned
if ($LASTEXITCODE -ne 0) { throw 'zipalign failed' }
$passFile = Join-Path (Join-Path $RepoRoot 'build\android') 'keystore.pass'
$pass = (Get-Content $passFile -Raw).Trim()
& $apksigner sign --ks $KeyStore --ks-key-alias sniroute `
    --ks-pass "pass:$pass" --key-pass "pass:$pass" `
    --out $Out $aligned
if ($LASTEXITCODE -ne 0) { throw 'apksigner failed' }

Write-Host ''
Write-Host ("Done: {0} ({1:N2} MB)" -f $Out, ((Get-Item $Out).Length / 1MB)) -ForegroundColor Green
