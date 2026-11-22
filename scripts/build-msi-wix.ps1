#requires -Version 5.1
<#
.SYNOPSIS
    Build the MSI with WiX directly (no Java, no jpackage).

.DESCRIPTION
    ASCII ONLY. Do not add non-ASCII characters to this file.

    Why not jpackage any more: the tray app is C++ now, so there is no Java
    runtime to package. WiX gives the same end result - a per-user MSI that
    installs without UAC and lets the user pick the folder - with a much smaller
    payload and no JVM.

    Per-user install rules that must hold for "no UAC" to be true:
      * <Package InstallScope="perUser">
      * no HKLM writes (autostart lives in HKCU and is written by the app)
      * shortcuts under the current user's Start Menu
    Installing per-user also means Windows puts the uninstall entry in HKCU,
    which is why the uninstall helper searches both hives.

    Output: dist\Sniroute-<version>.msi
#>
param(
    [string]$Version = '2.0.0',
    [string]$Out = ''
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (-not $Out) { $Out = Join-Path $RepoRoot 'dist' }
$Wix = Join-Path $RepoRoot 'build\wix'
$Candle = Join-Path $Wix 'candle.exe'
$Light = Join-Path $Wix 'light.exe'
$Heat = Join-Path $Wix 'heat.exe'
$BuildDir = Join-Path $RepoRoot 'build\wixpkg'
$Payload = Join-Path $BuildDir 'payload'
$Ext = Join-Path $Wix 'WixUIExtension.dll'

function Step($m) { Write-Host "== $m" -ForegroundColor Cyan }
function Fail($m) { Write-Host "!! $m" -ForegroundColor Red; exit 1 }

Step 'preflight'
foreach ($t in @($Candle, $Light, $Heat)) {
    if (-not (Test-Path $t)) { Fail "missing $t" }
}
if (-not (Test-Path $Ext)) { Fail "missing WiX UI extension: $Ext" }

Step 'site list'
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $RepoRoot 'scripts\gen-sites-list.ps1') | Out-Null

Step 'build tray exe'
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $RepoRoot 'scripts\build-tray.ps1')
if ($LASTEXITCODE -ne 0) { Fail 'tray build failed' }
$TrayExe = Join-Path $RepoRoot 'build\tray\Sniroute.exe'
if (-not (Test-Path $TrayExe)) { Fail "missing $TrayExe" }

# 版本文件：安装后 sniroute.cmd update check 靠它判断本地版本。
[System.IO.File]::WriteAllText((Join-Path $RepoRoot 'data\version.txt'), $Version, (New-Object System.Text.UTF8Encoding($false)))
Step 'stage payload'
if (Test-Path $BuildDir) { Remove-Item $BuildDir -Recurse -Force }
New-Item -ItemType Directory -Path $Payload -Force | Out-Null
Copy-Item $TrayExe (Join-Path $Payload 'Sniroute.exe') -Force

foreach ($item in @('scripts', 'tools', 'conf', 'data', 'gui', 'sniroute.cmd', 'uninstall.cmd', 'LICENSE', 'licenses', 'THIRD_PARTY_LICENSES.txt')) {
    Copy-Item (Join-Path $RepoRoot $item) (Join-Path $Payload $item) -Recurse -Force
}
# 自签名证书：对方导入后，签名校验才会显示"已验证"。
$signCer = Join-Path $RepoRoot 'ca\codesign.cer'
if (Test-Path $signCer) { Copy-Item $signCer (Join-Path $Payload 'codesign.cer') -Force }

foreach ($junk in @('logs', 'temp', 'chromeprofile', '__pycache__')) {
    # 敏感运行期文件绝不能进安装包：WARP 账号含私钥，DoH 缓存含浏览记录。
# 这些都是本机生成的，用户自己跑一次 export 就有了。
foreach ($leak in @('data\warp-account.json', 'data\doh-cache.json', 'data\doh-cache.tmp')) {
    $lp = Join-Path $Payload $leak
    if (Test-Path $lp) {
        Remove-Item $lp -Force -ErrorAction SilentlyContinue
        Step ("  removed sensitive: " + $leak)
    }
}

Get-ChildItem $Payload -Recurse -Directory -Filter $junk -ErrorAction SilentlyContinue |
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
}
$certs = Join-Path $Payload 'conf\certs'
if (Test-Path $certs) { Remove-Item $certs -Recurse -Force }
$pidFile = Join-Path $Payload 'logs\nginx.pid'
if (Test-Path $pidFile) { Remove-Item $pidFile -Force }

# runtime payload reused from the previous packaged build when present
$ReusePy = Join-Path $RepoRoot 'build\package\Sniroute\python'
if (Test-Path (Join-Path $ReusePy 'python.exe')) {
    Copy-Item $ReusePy (Join-Path $Payload 'python') -Recurse -Force
} else {
    Fail 'no trimmed python runtime (run build-package.ps1 once first)'
}
$NginxSrc = if ($env:SNIROUTE_NGINX_SRC) { $env:SNIROUTE_NGINX_SRC } else {
    # 让读者按自己机器改一行即可：
    #   $env:SNIROUTE_NGINX_SRC = 'C:\path\to\nginx.exe'
    Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages\nginxinc.nginx_Microsoft.Winget.Source_8wekyb3d8bbwe\nginx-1.31.8\nginx.exe'
}
$NginxDst = Join-Path $Payload 'nginx'
New-Item -ItemType Directory -Path $NginxDst -Force | Out-Null
Copy-Item $NginxSrc (Join-Path $NginxDst 'nginx.exe') -Force

$OsslSrc = 'C:\Program Files\Git\mingw64'
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

$zhReadme = Join-Path $RepoRoot 'data\README-zh.txt'
if (Test-Path $zhReadme) {
    [System.IO.File]::WriteAllText((Join-Path $Payload 'README-zh.txt'),
        (Get-Content $zhReadme -Raw -Encoding UTF8), (New-Object System.Text.UTF8Encoding($true)))
}

"   payload files: $((Get-ChildItem $Payload -Recurse -File).Count)"
"   payload size : $([math]::Round((Get-ChildItem $Payload -Recurse -File | Measure-Object Length -Sum).Sum / 1MB, 1)) MB"

Step 'harvest files'
$FilesWxs = Join-Path $BuildDir 'files.wxs'
& $Heat dir $Payload -cg AppFiles -dr INSTALLFOLDER -gg -g1 -srd -sreg -sfrag -var var.SourceDir -out $FilesWxs
if ($LASTEXITCODE -ne 0) { Fail 'heat failed' }

Step 'author product'
$MainWxs = Join-Path $BuildDir 'sniroute.wxs'
$guid = [guid]::NewGuid().ToString().ToUpper()
$main = @"
<?xml version="1.0" encoding="utf-8"?>
<Wix xmlns="http://schemas.microsoft.com/wix/2006/wi">
  <Product Id="*"
           Name="Sniroute"
           Language="2052"
           Version="$Version"
           Manufacturer="Kynarain"
           UpgradeCode="7F1C2A54-2F2E-4B7A-9D63-5B2A4F0D9C11">
    <Package InstallerVersion="500" Compressed="yes" InstallScope="perUser" Platform="x64" SummaryCodepage="936"
             Description="Sniroute $Version installer (per-user, x64)"
             Comments="Verify integrity with: sniroute.cmd verify Sniroute-$Version.msi (or Get-FileHash -Algorithm SHA256). Hash files are no longer shipped."
             Keywords="Sniroute;installer;sha256;sha1;md5;per-user;x64"/>
    <MajorUpgrade DowngradeErrorMessage="A newer version of Sniroute is already installed."/>
    <MediaTemplate EmbedCab="yes"/>

    <Property Id="ARPPRODUCTICON" Value="AppIcon"/>
    <Icon Id="AppIcon" SourceFile="$($RepoRoot.Replace('\','\\'))\gui\icon.ico"/>

    <!-- Custom copy of WixUI_InstallDir with the license page removed:
         Welcome -> InstallDir -> VerifyReady -> Progress -> Finish.
         A license page would be noise for a WTFPL project. -->
    <UIRef Id="WixUI_Common"/>
    <UI Id="SnirouteUI">
      <TextStyle Id="WixUI_Font_Normal" FaceName="Tahoma" Size="8"/>
      <TextStyle Id="WixUI_Font_Bigger" FaceName="Tahoma" Size="12"/>
      <TextStyle Id="WixUI_Font_Title" FaceName="Tahoma" Size="9" Bold="yes"/>
      <Property Id="DefaultUIFont" Value="WixUI_Font_Normal"/>
      <Property Id="WixUI_Mode" Value="InstallDir"/>

      <DialogRef Id="BrowseDlg"/>
      <DialogRef Id="DiskCostDlg"/>
      <DialogRef Id="ErrorDlg"/>
      <DialogRef Id="FatalError"/>
      <DialogRef Id="FilesInUse"/>
      <DialogRef Id="InvalidDirDlg"/>
      <DialogRef Id="MsiRMFilesInUse"/>
      <DialogRef Id="PrepareDlg"/>
      <DialogRef Id="ProgressDlg"/>
      <DialogRef Id="ResumeDlg"/>
      <DialogRef Id="UserExit"/>

      <Publish Dialog="BrowseDlg" Control="OK" Event="DoAction" Value="WixUIValidatePath" Order="3">1</Publish>
      <Publish Dialog="BrowseDlg" Control="OK" Event="SpawnDialog" Value="InvalidDirDlg" Order="4"><![CDATA[NOT WIXUI_DONTVALIDATEPATH AND WIXUI_INSTALLDIR_VALID<>"1"]]></Publish>

      <Publish Dialog="ExitDialog" Control="Finish" Event="EndDialog" Value="Return" Order="999">1</Publish>

      <!-- no license step: straight from the welcome page into folder choice -->
      <Publish Dialog="WelcomeDlg" Control="Next" Event="NewDialog" Value="InstallDirDlg">NOT Installed</Publish>
      <Publish Dialog="WelcomeDlg" Control="Next" Event="NewDialog" Value="VerifyReadyDlg">Installed AND PATCH</Publish>

      <Publish Dialog="InstallDirDlg" Control="Back" Event="NewDialog" Value="WelcomeDlg">1</Publish>
      <Publish Dialog="InstallDirDlg" Control="Next" Event="SetTargetPath" Value="[WIXUI_INSTALLDIR]" Order="1">1</Publish>
      <Publish Dialog="InstallDirDlg" Control="Next" Event="DoAction" Value="WixUIValidatePath" Order="2">NOT WIXUI_DONTVALIDATEPATH</Publish>
      <Publish Dialog="InstallDirDlg" Control="Next" Event="SpawnDialog" Value="InvalidDirDlg" Order="3"><![CDATA[NOT WIXUI_DONTVALIDATEPATH AND WIXUI_INSTALLDIR_VALID<>"1"]]></Publish>
      <Publish Dialog="InstallDirDlg" Control="Next" Event="NewDialog" Value="VerifyReadyDlg" Order="4">WIXUI_DONTVALIDATEPATH OR WIXUI_INSTALLDIR_VALID="1"</Publish>
      <Publish Dialog="InstallDirDlg" Control="ChangeFolder" Property="_BrowseProperty" Value="[WIXUI_INSTALLDIR]" Order="1">1</Publish>
      <Publish Dialog="InstallDirDlg" Control="ChangeFolder" Event="SpawnDialog" Value="BrowseDlg" Order="2">1</Publish>

      <Publish Dialog="VerifyReadyDlg" Control="Back" Event="NewDialog" Value="InstallDirDlg" Order="1">NOT Installed</Publish>
      <Publish Dialog="VerifyReadyDlg" Control="Back" Event="NewDialog" Value="MaintenanceTypeDlg" Order="2">Installed AND NOT PATCH</Publish>
      <Publish Dialog="VerifyReadyDlg" Control="Back" Event="NewDialog" Value="WelcomeDlg" Order="2">Installed AND PATCH</Publish>

      <Publish Dialog="MaintenanceWelcomeDlg" Control="Next" Event="NewDialog" Value="MaintenanceTypeDlg">1</Publish>
      <Publish Dialog="MaintenanceTypeDlg" Control="RepairButton" Event="NewDialog" Value="VerifyReadyDlg">1</Publish>
      <Publish Dialog="MaintenanceTypeDlg" Control="RemoveButton" Event="NewDialog" Value="VerifyReadyDlg">1</Publish>
      <Publish Dialog="MaintenanceTypeDlg" Control="Back" Event="NewDialog" Value="MaintenanceWelcomeDlg">1</Publish>

      <Property Id="ARPNOMODIFY" Value="1"/>
    </UI>
    <Property Id="WIXUI_INSTALLDIR" Value="INSTALLFOLDER"/>

    <Directory Id="TARGETDIR" Name="SourceDir">
      <Directory Id="LocalAppDataFolder">
        <Directory Id="INSTALLFOLDER" Name="Sniroute"/>
      </Directory>
      <Directory Id="ProgramMenuFolder">
        <Directory Id="AppMenuFolder" Name="Sniroute"/>
      </Directory>
      <Directory Id="DesktopFolder"/>
    </Directory>

    <DirectoryRef Id="AppMenuFolder">
      <Component Id="StartMenuShortcut" Guid="$guid">
        <Shortcut Id="AppShortcut" Name="Sniroute" Target="[INSTALLFOLDER]Sniroute.exe"
                  WorkingDirectory="INSTALLFOLDER" Icon="AppIcon"/>
        <RemoveFolder Id="RemoveAppMenuFolder" On="uninstall"/>
        <RegistryValue Root="HKCU" Key="Software\Sniroute" Name="installed" Type="integer"
                       Value="1" KeyPath="yes"/>
      </Component>
    </DirectoryRef>

    <DirectoryRef Id="DesktopFolder">
      <Component Id="DesktopShortcut" Guid="$([guid]::NewGuid().ToString().ToUpper())">
        <Shortcut Id="DesktopAppShortcut" Name="Sniroute" Target="[INSTALLFOLDER]Sniroute.exe"
                  WorkingDirectory="INSTALLFOLDER" Icon="AppIcon"/>
        <RegistryValue Root="HKCU" Key="Software\Sniroute" Name="desktopShortcut" Type="integer"
                       Value="1" KeyPath="yes"/>
      </Component>
    </DirectoryRef>

    <DirectoryRef Id="INSTALLFOLDER">
    </DirectoryRef>

    <!-- Stop our own processes before files are replaced. Without this an
         upgrade fails with "error writing python312.dll": the DoH server and
         nginx are running from the very folder being overwritten. -->
    <CustomAction Id="StopBeforeUpgrade"
                  Directory="INSTALLFOLDER"
                  ExeCommand="cmd.exe /c &quot;scripts\stop-for-upgrade.cmd&quot;"
                  Execute="immediate"
                  Return="ignore" />
    <InstallExecuteSequence>
      <Custom Action="StopBeforeUpgrade" Before="InstallFiles">WIX_UPGRADE_DETECTED OR Installed</Custom>
    </InstallExecuteSequence>

    <Feature Id="Main" Title="Sniroute" Level="1" ConfigurableDirectory="INSTALLFOLDER">
      <ComponentGroupRef Id="AppFiles"/>
      <ComponentRef Id="StartMenuShortcut"/>
      <ComponentRef Id="DesktopShortcut"/>
    </Feature>
  </Product>
</Wix>
"@
[System.IO.File]::WriteAllText($MainWxs, $main, (New-Object System.Text.UTF8Encoding($false)))

# A license page is required by WixUI_InstallDir; a placeholder with real text
# (not lorem ipsum) so the dialog is not confusing.
# No license page any more: the project is WTFPL, so nothing to accept.

Step 'candle'
Push-Location $BuildDir
$defineArg = "-dSourceDir=$Payload"
& $Candle $defineArg -out (Join-Path $BuildDir 'files.wixobj') $FilesWxs
if ($LASTEXITCODE -ne 0) { Pop-Location; Fail 'candle failed on files.wxs' }
& $Candle -ext $Ext -out (Join-Path $BuildDir 'sniroute.wixobj') $MainWxs
if ($LASTEXITCODE -ne 0) { Pop-Location; Fail 'candle failed on sniroute.wxs' }

Step 'light'
$msiOut = Join-Path $BuildDir "Sniroute-$Version.msi"
& $Light -ext $Ext -cultures:zh-cn -loc (Join-Path $Wix 'WixUI_zh-CN.wxl') -sice:ICE38 -sice:ICE43 -sice:ICE64 -sice:ICE91 -out $msiOut `
    (Join-Path $BuildDir 'sniroute.wixobj') (Join-Path $BuildDir 'files.wixobj')
$code = $LASTEXITCODE
Pop-Location
if ($code -ne 0) { Fail 'light failed' }

New-Item -ItemType Directory -Path $Out -Force | Out-Null
$target = Join-Path $Out "Sniroute-$Version.msi"
Copy-Item $msiOut $target -Force
Write-Host ''
Write-Host ("Done: {0} ({1:N1} MB)" -f $target, ((Get-Item $target).Length / 1MB)) -ForegroundColor Green

# 自签名 + SHA-256：让 Windows 显示发布者，并给用户一个可核对的指纹。
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $RepoRoot 'scripts\sign-and-hash.ps1') -Path $target 2>&1 | Out-Null
