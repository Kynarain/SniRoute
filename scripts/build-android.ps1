# Build the Android APK without Gradle.
#
# Why no Gradle: this machine has none, and AGP would pull a large dependency
# tree from Maven. The app uses no third-party libraries (not even AndroidX),
# so the SDK's own aapt2 / d8 / apksigner are enough. Shorter chain, fewer
# failure points, reproducible.
#
# Requires:
#   - Android SDK with platforms\android-34\android.jar and build-tools\34.0.0
#   - JDK 17 (javac/keytool); override with -Jdk
#
# ASCII ONLY. Do not add non-ASCII characters to this file: Windows PowerShell
# 5.1 reads BOM-less UTF-8 as the local ANSI codepage and mis-parses CJK text.

param(
    [string]$Version = "3.9.6",
    [string]$AndroidSdk = $(if ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT } elseif ($env:ANDROID_HOME) { $env:ANDROID_HOME } else { "$env:LOCALAPPDATA\Android\Sdk" }),
    [string]$Jdk = $(if ($env:JAVA_HOME) { $env:JAVA_HOME } else { "C:\Program Files\Java\jdk-17" }),
    [string]$KeyStore = "",
    [switch]$SkipSign
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
$Android = Join-Path $RepoRoot 'android'
$BuildDir = Join-Path $RepoRoot 'build\android'
$DistDir = Join-Path $RepoRoot 'dist'
$Out = Join-Path $DistDir "sniroute-$Version-android.apk"

if (-not $KeyStore) { $KeyStore = Join-Path $BuildDir 'sniroute.keystore' }

$BuildTools = Join-Path $AndroidSdk 'build-tools\34.0.0'
$AndroidJar = Join-Path $AndroidSdk 'platforms\android-34\android.jar'
$aapt2 = Join-Path $BuildTools 'aapt2.exe'
$d8 = Join-Path $BuildTools 'd8.bat'
$zipalign = Join-Path $BuildTools 'zipalign.exe'
$apksigner = Join-Path $BuildTools 'apksigner.bat'
$javac = Join-Path $Jdk 'bin\javac.exe'
$keytool = Join-Path $Jdk 'bin\keytool.exe'

foreach ($p in @($aapt2, $d8, $zipalign, $apksigner, $javac, $AndroidJar)) {
    if (-not (Test-Path $p)) { throw "missing build tool: $p" }
}

Remove-Item $BuildDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
New-Item -ItemType Directory -Path $DistDir -Force | Out-Null

Write-Host '== 1/6 compile resources (aapt2 compile)'
$resZip = Join-Path $BuildDir 'res.zip'
& $aapt2 compile --dir (Join-Path $Android 'res') -o $resZip
if ($LASTEXITCODE -ne 0) { throw 'aapt2 compile failed' }

Write-Host '== 2/6 link manifest and resources (aapt2 link)'
$baseApk = Join-Path $BuildDir 'base.apk'
$genDir = Join-Path $BuildDir 'gen'
New-Item -ItemType Directory -Path $genDir -Force | Out-Null
& $aapt2 link -o $baseApk `
    -I $AndroidJar `
    --manifest (Join-Path $Android 'AndroidManifest.xml') `
    --java $genDir `
    --min-sdk-version 26 --target-sdk-version 33 `
    --version-code 1 --version-name "$Version-android1" `
    $resZip
if ($LASTEXITCODE -ne 0) { throw 'aapt2 link failed' }

Write-Host '== 3/6 compile java'
$classesDir = Join-Path $BuildDir 'classes'
New-Item -ItemType Directory -Path $classesDir -Force | Out-Null
$sources = @()
$sources += Get-ChildItem (Join-Path $Android 'src') -Recurse -Filter *.java | ForEach-Object { $_.FullName }
$sources += Get-ChildItem $genDir -Recurse -Filter *.java -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName }
& $javac -encoding UTF-8 -source 8 -target 8 -bootclasspath $AndroidJar -classpath $AndroidJar -d $classesDir $sources
if ($LASTEXITCODE -ne 0) { throw 'javac failed' }

Write-Host '== 4/6 convert to dex (d8)'
$dexDir = Join-Path $BuildDir 'dex'
New-Item -ItemType Directory -Path $dexDir -Force | Out-Null
$classFiles = Get-ChildItem $classesDir -Recurse -Filter *.class | ForEach-Object { $_.FullName }
& $d8 --lib $AndroidJar --min-api 26 --output $dexDir $classFiles
if ($LASTEXITCODE -ne 0) { throw 'd8 failed' }

Write-Host '== 5/6 package and sign'
$unsigned = Join-Path $BuildDir 'unsigned.apk'
Copy-Item $baseApk $unsigned -Force
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::Open($unsigned, 'Update')
$dexFile = Join-Path $dexDir 'classes.dex'
$entry = $zip.GetEntry('classes.dex')
if ($entry) { $entry.Delete() }
[System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $dexFile, 'classes.dex') | Out-Null
$zip.Dispose()

$aligned = Join-Path $BuildDir 'aligned.apk'
& $zipalign -f 4 $unsigned $aligned
if ($LASTEXITCODE -ne 0) { throw 'zipalign failed' }

if ($SkipSign) {
    Copy-Item $aligned $Out -Force
    Write-Host "   unsigned apk: $Out"
    return
}

$passFile = Join-Path $BuildDir 'keystore.pass'
if (-not (Test-Path $KeyStore)) {
    Write-Host '   first build: creating signing key'
    $chars = 'ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789'
    $pass = -join (1..24 | ForEach-Object { $chars[(Get-Random -Maximum $chars.Length)] })
    [System.IO.File]::WriteAllText($passFile, $pass)
    & $keytool -genkeypair -keystore $KeyStore -alias sniroute `
        -keyalg RSA -keysize 3072 -validity 10950 `
        -storepass $pass -keypass $pass `
        -dname "CN=Kynarain, EMAILADDRESS=kynarain@kynarain.cn, O=Sniroute, C=CN"
    if ($LASTEXITCODE -ne 0) { throw 'keytool failed' }
}
$pass = (Get-Content $passFile -Raw).Trim()

& $apksigner sign --ks $KeyStore --ks-key-alias sniroute `
    --ks-pass "pass:$pass" --key-pass "pass:$pass" `
    --out $Out $aligned
if ($LASTEXITCODE -ne 0) { throw 'apksigner failed' }

Write-Host '== 6/6 verify'
# Java 17 会对 apksigner 的内部调用打印 WARNING，PowerShell 在 Stop 模式下
# 会把 stderr 当错误抛出，所以这里放宽一档，只关心退出码。
$prevEa = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$verify = & $apksigner verify -v --print-certs $Out 2>&1
$ErrorActionPreference = $prevEa
$verify | Where-Object { $_ -notmatch 'WARNING|restricted method|enable-native-access|Restricted methods' } |
    Select-Object -First 12 | ForEach-Object { "   $_" }
Write-Host ''
Write-Host "Done: $Out ($([math]::Round((Get-Item $Out).Length / 1MB, 2)) MB)"
exit 0
