<#
    sign-and-hash.ps1 -- 给产物做自签名 + 生成 SHA-256

    为什么需要这个脚本
    ------------------
    Windows 对未签名的安装包会显示"未知发布者"，用户无法判断文件有没有被改过。
    自签名证书可以解决这两个问题（代价是：它不像商业证书那样在所有机器上默认
    受信任，需要对方先导入 ca\codesign.cer）。

    证书策略
    --------
    复用当前用户证书库里的代码签名证书（Subject 含 Kynarain）；没有就新建一张，
    期限 5 年，并把 .cer 导出到 ca\codesign.cer 供分发。
    新建后会把证书导入"受信任根"和"受信任发布者"，否则校验结果是
    UnknownError / 证书链终止于不受信任的根。

    时间戳
    ------
    默认不带时间戳：这台机器的网络到公共 TSA（digicert 等）不通，带时间戳会
    卡住好几分钟。需要时间戳时用 -TimestampServer 指定一个可用的地址。
    代价是：证书过期后，已签名的文件也会一起失去有效性。

    用法
    ----
    # 签名 + 控制台显示哈希（不再写任何哈希文件）
    scripts\sign-and-hash.ps1 -Path dist\Sniroute-3.5.1.msi

    # 只算哈希，不签名（同样不落盘）
    scripts\sign-and-hash.ps1 -Path dist\*.zip -HashOnly

    # 指定时间戳服务器
    scripts\sign-and-hash.ps1 -Path dist\a.msi -TimestampServer http://timestamp.example.com
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string[]]$Path,
    [string]$SumsFile = 'dist\SHA256SUMS.txt',
    # 'auto' = 自动挑一个可达的公共 TSA；空字符串也按 auto 处理。
    [string]$TimestampServer = 'auto',
    [switch]$NoTimestamp,
    [switch]$HashOnly,
    [switch]$SkipHash,
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path (Join-Path $RepoRoot 'scripts'))) { $RepoRoot = (Get-Location).Path }

function Write-Step([string]$m) { if (-not $Quiet) { Write-Host "  [sign] $m" } }

function Get-SigningCertificate {
    $cert = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert -ErrorAction SilentlyContinue |
            Where-Object { $_.Subject -match 'Kynarain' } |
            Sort-Object NotAfter -Descending | Select-Object -First 1
    if ($cert) { return $cert }

    Write-Step '未找到代码签名证书，创建一张（5 年）'
    $cert = New-SelfSignedCertificate -Type CodeSigningCert `
        -Subject 'CN=Kynarain, E=kynarain@kynarain.cn, O=Sniroute, C=CN' `
        -CertStoreLocation Cert:\CurrentUser\My `
        -KeyUsage DigitalSignature -KeyAlgorithm RSA -KeyLength 3072 `
        -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(5)

    $cerPath = Join-Path $RepoRoot 'ca\codesign.cer'
    New-Item -ItemType Directory -Path (Split-Path -Parent $cerPath) -Force | Out-Null
    [System.IO.File]::WriteAllBytes($cerPath, $cert.Export('Cert'))
    Write-Step "证书已导出: $cerPath"
    return $cert
}

function Ensure-Trusted([System.Security.Cryptography.X509Certificates.X509Certificate2]$cert) {
    # 自签证书必须同时出现在"受信任根"与"受信任发布者"，否则签名校验失败。
    foreach ($name in 'Root', 'TrustedPublisher') {
        $store = New-Object System.Security.Cryptography.X509Certificates.X509Store($name, 'CurrentUser')
        $store.Open('ReadWrite')
        try {
            if (-not ($store.Certificates | Where-Object { $_.Thumbprint -eq $cert.Thumbprint })) {
                $store.Add((New-Object System.Security.Cryptography.X509Certificates.X509Certificate2(
                    $cert.Export('Cert'))))
                Write-Step "$name : 已导入"
            }
        } finally { $store.Close() }
    }
}

function Find-WorkingTsa {
    # 这台机器到部分公共 TSA 不通（digicert 的 443 直接超时），所以不能写死一个：
    # 逐个探测 80 端口，用第一个能连上的。连不上就返回空，签名退化为不带时间戳。
    # 顺序按实测：DigiCert 0.7s、Sectigo 3.6s、freetsa 0.5s 都能真正给出时间戳。
    # 注意两个坑：
    #   · globalsign / acs.microsoft 的 80 端口能连上，但 POST 回来不带时间戳 ——
    #     只探测端口是不够的，所以它们不在列表里。
    #   · 国家授时中心（tsa.ntsc.ac.cn）在本网络没有 A 记录，试过的 www.ntsc.ac.cn
    #     几个路径也都拿不到时间戳，所以不可用。
    $candidates = @(
        'http://timestamp.digicert.com',
        'http://timestamp.sectigo.com',
        'http://freetsa.org/tsr'
    )
    foreach ($url in $candidates) {
        $host_ = ([System.Uri]$url).Host
        $client = New-Object System.Net.Sockets.TcpClient
        try {
            $iar = $client.BeginConnect($host_, 80, $null, $null)
            if ($iar.AsyncWaitHandle.WaitOne(3000) -and $client.Connected) {
                Write-Step "时间戳服务器可达: $url"
                return $url
            }
        } catch { }
        finally { $client.Close() }
    }
    Write-Step '没有可达的时间戳服务器，签名将不带时间戳'
    return ''
}

function Get-Sha256([string]$file) {
    return (Get-FileHash -Path $file -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-Sha1([string]$file) {
    return (Get-FileHash -Path $file -Algorithm SHA1).Hash.ToLowerInvariant()
}

# 从 powershell -File 调用时，-Path a,b 会被塞成一个字符串，这里自己拆开。
$Path = $Path | ForEach-Object { $_ -split ',' } | Where-Object { $_.Trim() } | ForEach-Object { $_.Trim() }

$targets = @()
foreach ($p in $Path) {
    $resolved = @(Get-Item -Path $p -ErrorAction SilentlyContinue)
    if (-not $resolved) { throw "找不到文件: $p" }
    foreach ($f in $resolved) { if (-not $f.PSIsContainer) { $targets += $f } }
}
if ($targets.Count -eq 0) { throw '没有可处理的文件' }

if (-not $HashOnly) {
    $cert = Get-SigningCertificate
    Ensure-Trusted $cert
    Write-Step "签名证书: $($cert.Thumbprint)  ($($cert.Subject))"

    foreach ($f in $targets) {
        # MSI 与 EXE 支持 Authenticode；ZIP 不支持，只能算哈希。
        if ($f.Extension -notin '.msi', '.exe', '.dll') {
            Write-Step "跳过签名（该类型不支持 Authenticode）: $($f.Name)"
            continue
        }
        $args = @{ FilePath = $f.FullName; Certificate = $cert; HashAlgorithm = 'SHA256' }
        if (-not $NoTimestamp) {
            $tsa = if ($TimestampServer -and $TimestampServer -ne 'auto') { $TimestampServer } else { Find-WorkingTsa }
            if ($tsa) { $args['TimestampServer'] = $tsa }
        }
        try {
            $sig = Set-AuthenticodeSignature @args
            if ($sig.Status -eq 'Valid') {
                Write-Step "已签名: $($f.Name)"
            } else {
                Write-Step "签名状态异常: $($f.Name) -> $($sig.Status)"
            }
        } catch {
            Write-Step "签名失败: $($f.Name) -> $($_.Exception.Message)"
        }
    }
}

# 不再生成哈希文件（.sha256/.sha1/.md5 与 *SUMS.txt）。
# 需要校验时用：sniroute.cmd verify <文件>，或直接 Get-FileHash。
# 这里只在控制台打印一次，方便复制，不落盘。
Write-Step "哈希（仅显示，不写文件）："
foreach ($f in $targets) {
    $h256 = Get-Sha256 $f.FullName
    $h1 = Get-Sha1 $f.FullName
    $hMd5 = (Get-FileHash -Path $f.FullName -Algorithm MD5).Hash.ToLowerInvariant()
    Write-Step ("  SHA256 {0}  {1}" -f $h256, $f.Name)
    Write-Step ("  SHA1   {0}  {1}" -f $h1, $f.Name)
    Write-Step ("  MD5    {0}  {1}" -f $hMd5, $f.Name)
}
return

