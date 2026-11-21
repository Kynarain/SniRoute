#requires -Version 5.1
<#
.SYNOPSIS
    用一个带 ECH 开关的独立 Chrome 实例访问目标站点（不改注册表、不需要管理员）。

.DESCRIPTION
    为什么不用注册表策略：本机的 HKCU\Software\Policies 属主是 SYSTEM，当前账号
    只有 ReadKey —— 策略键被加固过，写不进去。改它的 ACL 属于动安全配置，本脚本不做。

    改用启动参数，全部只影响这一次启动：
        --enable-features=EncryptedClientHello     打开 ECH
        --dns-over-https-mode=secure               安全 DNS 走本项目的 DoH
        --dns-over-https-templates=<DoH 地址>
        --host-resolver-rules="MAP <DoH 域名> 127.0.0.1"   让 DoH 域名能解析到本机
        --ignore-certificate-errors-spki-list=<SPKI>       只信任这张 DoH 证书（按公钥固定）

    最后一条是替代"装根证书"的关键：它按公钥指纹放行**特定一张证书**，
    而不是像装根 CA 那样信任一大片。而且不需要管理员。

.PARAMETER DryRun
    只打印将要执行的命令，不启动浏览器。

.PARAMETER UseDefaultProfile
    用你平时的 Chrome 配置（必须先完全退出 Chrome，否则参数会被已有进程吞掉）。
    默认使用仓库内的独立配置目录 chromeprofile\，互不干扰。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\start-chrome-ech.ps1 -DryRun
    powershell -ExecutionPolicy Bypass -File scripts\start-chrome-ech.ps1
#>
param(
    [switch]$DryRun,
    [switch]$UseDefaultProfile,
    [int]$DohPort = 5444,
    [string]$DohHost = 'doh.sniroute.lan',
    [switch]$Pin,
    [string]$ChromeExe = 'C:\Program Files\Google\Chrome\Application\chrome.exe'
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$CertPath = Join-Path $RepoRoot 'conf\certs\doh.crt'
$ProfileDir = Join-Path $RepoRoot 'chromeprofile'
$DohUrl = "https://127.0.0.1:${DohPort}/dns-query"

function Find-OpenSsl {
    $c = Get-Command openssl -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    foreach ($p in @(
            'C:\Program Files\Git\usr\bin\openssl.exe',
            'C:\Program Files\Git\mingw64\bin\openssl.exe',
            'C:\Program Files (x86)\Git\usr\bin\openssl.exe')) {
        if (Test-Path $p) { return $p }
    }
    throw '找不到 openssl（装 Git for Windows，或把它放进 PATH）'
}

function Get-SpkiHash([string]$certPath, [string]$workDir) {
    $openssl = Find-OpenSsl
    if (-not (Test-Path $workDir)) { New-Item -ItemType Directory -Path $workDir -Force | Out-Null }
    $pub = Join-Path $workDir 'pub.pem'
    $der = Join-Path $workDir 'pub.der'
    $sha = Join-Path $workDir 'pub.sha256'
    $b64 = Join-Path $workDir 'pub.b64'
    & $openssl x509 -in $certPath -pubkey -noout -out $pub 2>$null
    & $openssl pkey -pubin -in $pub -outform der -out $der 2>$null
    & $openssl dgst -sha256 -binary -out $sha $der 2>$null
    & $openssl base64 -A -in $sha -out $b64 2>$null
    return (Get-Content $b64 -Raw).Trim()
}

Write-Host ''
Write-Host '=== sniroute: 带 ECH 的 Chrome 启动器 ===' -ForegroundColor Cyan

if (-not (Test-Path $ChromeExe)) { throw "找不到 Chrome：$ChromeExe" }
if (-not (Test-Path $CertPath)) { throw "找不到 DoH 证书：$CertPath（先生成，见 README）" }

if ($Pin) { $spki = Get-SpkiHash -certPath $CertPath -workDir (Join-Path $RepoRoot 'temp') }
Write-Host "  DoH 端点 : $DohUrl"
if ($Pin) { Write-Host "  证书 SPKI: $spki" }

# DoH 服务没在跑的话，浏览器会什么都解析不出来，所以先检查
$listening = Get-NetTCPConnection -State Listen -LocalPort $DohPort -ErrorAction SilentlyContinue
if (-not $listening) {
    Write-Host "  [!] $DohPort 端口没有服务在监听 —— 浏览器会无法解析任何域名。" -ForegroundColor Yellow
    Write-Host "      先另开一个终端跑: scripts\doh-ech.cmd $DohPort"
} else {
    Write-Host "  [OK] DoH 服务已在 $DohPort 监听"
}

$args = @(
    '--enable-features=EncryptedClientHello',
    "--dns-over-https-mode=secure",
    "--dns-over-https-templates=$DohUrl"
)
# The local CA is in the current user's trust store, so pinning is no longer
# required. Keep it available behind -Pin for machines where the store cannot
# be written (locked-down corporate images) — it trusts exactly one cert.
if ($Pin) {
    $args += "--ignore-certificate-errors-spki-list=$spki"
}

if (-not $UseDefaultProfile) {
    if (-not (Test-Path $ProfileDir)) { New-Item -ItemType Directory -Path $ProfileDir -Force | Out-Null }
    $args += "--user-data-dir=$ProfileDir"
    Write-Host "  独立配置目录: $ProfileDir"
} else {
    $running = Get-Process chrome -ErrorAction SilentlyContinue
    if ($running) {
        Write-Host '  [!] Chrome 正在运行，-UseDefaultProfile 的参数会被忽略。' -ForegroundColor Yellow
        Write-Host '      请先完全退出 Chrome（Get-Process chrome | Stop-Process），或去掉 -UseDefaultProfile。'
        exit 1
    }
}

Write-Host ''
Write-Host '  启动命令：'
Write-Host "    `"$ChromeExe`" $($args -join ' ')" -ForegroundColor DarkGray

if ($DryRun) {
    Write-Host ''
    Write-Host '  -DryRun：未启动。' -ForegroundColor Green
    exit 0
}

Write-Host ''
Start-Process -FilePath $ChromeExe -ArgumentList $args
Write-Host '  已启动。验证步骤：' -ForegroundColor Green
Write-Host '    1. 访问 https://crypto.cloudflare.com/cdn-cgi/trace ，看输出里有没有 sni=encrypted'
Write-Host '    2. 再访问 https://www.pixiv.net'
Write-Host ''
Write-Host '  退出这个实例后一切恢复，系统没有任何持久改动。'
