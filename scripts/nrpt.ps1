#requires -Version 5.1
<#
.SYNOPSIS
    Route the system resolver's answers for selected domains to sniroute (NRPT).

.DESCRIPTION
    ASCII ONLY. Do not add non-ASCII characters to this file.

    This is the "global mode": instead of teaching each browser to use DoH, this
    tells the Windows DNS Client service that certain namespaces are answered by
    sniroute's local DNS server. Every program that resolves through the system
    stack then gets the redirect - Edge, Firefox, curl, the updater, everything.
    Chrome is no longer special.

    Mechanism: one NRPT rule in
      HKLM\SYSTEM\CurrentControlSet\Services\Dnscache\Parameters\DnsPolicyConfig\{GUID}
    with Version=2, ConfigOptions=0x8 (GenericDNSServers present),
    GenericDNSServers=127.0.0.1 and a fixed GUID of our own. A single fixed key
    means removal is exact: nothing else in the policy table is ever touched.

    Why not the Group Policy table (HKLM\SOFTWARE\Policies\...\DnsPolicyConfig):
    that one belongs to the administrator. The service's own table is the same
    thing the PowerShell cmdlets write, and it is what this tool owns.

    This needs administrator rights (it writes HKLM and, in global mode, puts the
    CA in the machine store so every account trusts it).

    What global mode does NOT do, and why:
    It cannot hide SNI for the pixiv main site. Plain DNS answers are not
    authenticated, so a browser cannot trust an ECH config obtained over them;
    without ECH the upstream handshake carries SNI=www.pixiv.net in the clear,
    which is the block we are here to avoid. That domain therefore keeps using
    the browser's own DoH (which does carry ECH). Images are different: their
    origin is reached by a local TLS termination, no ECH needed, so they are
    routed here for every program.

    -IncludePixiv exists for experimentation only; it routes the main site here
    too, and that connection is expected to be unreliable for the reason above.

.PARAMETER Revert
    Remove the rule and the machine-store CA.

.PARAMETER Check
    Report the current rule, change nothing.

.PARAMETER IncludePixiv
    Also route the ECH-based domains (not recommended).

.PARAMETER DnsServer
    Address of sniroute's plain DNS listener. Loopback only by design.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\nrpt.ps1 -Install
#>
param(
    [switch]$Install,
    [switch]$Revert,
    [switch]$Check,
    [switch]$IncludePixiv,
    [int]$DnsPort = 53
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$CaCert = Join-Path $RepoRoot 'ca\rootCA.crt'
$Sites = Join-Path $RepoRoot 'data\sites.json'

$PolicyKey = 'SYSTEM\CurrentControlSet\Services\Dnscache\Parameters\DnsPolicyConfig'
# Fixed GUID: one rule, ours, so -Revert deletes exactly what -Install wrote.
$RuleKey = '{6E1F2C74-9A3B-4D2E-8F51-0C7A5B3D9E48}'
$RulePath = "$PolicyKey\$RuleKey"
# NRPT accepts "address:port". Port 53 is often taken by Internet Connection
# Sharing (svchost binds 0.0.0.0:53), so the default here is 5353.
if ($DnsPort -eq 53) { $DnsServer = '127.0.0.1' } else { $DnsServer = "127.0.0.1:$DnsPort" }
$Comment = 'sniroute - local DoH/DNS redirect; safe to delete'

function Write-Ok([string]$m) { Write-Host "  [OK] $m" -ForegroundColor Green }
function Write-Warn2([string]$m) { Write-Host "  [!] $m" -ForegroundColor Yellow }

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $pr = New-Object Security.Principal.WindowsPrincipal($id)
    return $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-Namespaces {
    # Loopback-mode sites are the ones a local TLS termination can serve.
    if (-not (Test-Path $Sites)) { return @('.pximg.net', 'i.pximg.net') }
    $cfg = Get-Content $Sites -Raw -Encoding UTF8 | ConvertFrom-Json
    $names = New-Object System.Collections.Generic.List[string]
    foreach ($site in $cfg.sites) {
        # loopback 与 dns 两种模式都需要 NRPT 接管：前者返回 127.0.0.1 交给本机代理，
        # 后者只借我们的 DoH 拿真实地址（修 DNS 污染）而不做任何代理。
        $loopback = ($site.dns_mode -in @('loopback', 'dns')) -or ($site.dns_override -and -not $site.enabled)
        if ($site.dns_mode -eq 'ech' -and -not $IncludePixiv) { continue }
        if ($site.dns_mode -eq 'ech' -and $IncludePixiv) { $loopback = $true }
        if (-not $loopback -and -not $site.enabled) { continue }
        foreach ($h in $site.hosts) { if ($h) { $names.Add($h) } }
        foreach ($san in $site.cert_san) {
            # Only "*.domain" is a wildcard. A plain "domain" must not be sliced -
            # doing that once produced bogus namespaces like ".img.net".
            if ($san.StartsWith('*.')) {
                $apex = $san.Substring(2)
                $names.Add(".$apex")
            }
        }
    }
    return ($names | Sort-Object -Unique)
}

function Get-RuleState {
    if (-not (Test-Path "HKLM:\$RulePath")) { return $null }
    $props = Get-ItemProperty "HKLM:\$RulePath"
    $names = ($props.Name -join ', ')
    return "names=[$names] server=$($props.GenericDNSServers) version=$($props.Version) options=$($props.ConfigOptions)"
}

function Test-MachineCa {
    if (-not (Test-Path $CaCert)) { return $false }
    $thumb = (New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 $CaCert).Thumbprint
    $out = certutil.exe -store Root 2>&1 | Out-String
    return [bool]($out -match [regex]::Escape($thumb))
}

Write-Host ''
Write-Host '=== sniroute: global DNS mode (NRPT) ===' -ForegroundColor Cyan

if ($Check) {
    $state = Get-RuleState
    Write-Host ("  rule        : {0}" -f $(if ($state) { $state } else { 'not installed' }))
    Write-Host ("  machine CA  : {0}" -f (Test-MachineCa))
    Write-Host ("  dns server  : {0}" -f $DnsServer)
    Write-Host ("  namespaces  : {0}" -f ((Get-Namespaces) -join ', '))
    Write-Host ("  admin       : {0}" -f (Test-Admin))
    exit 0
}

if (-not (Test-Admin)) {
    Write-Warn2 'global mode writes HKLM and needs administrator rights once'
    $self = $MyInvocation.MyCommand.Path
    $ps = (Get-Process -Id $PID).Path
    $argLine = "-NoProfile -ExecutionPolicy Bypass -File `"$self`" " + $(if ($Revert) { '-Revert' } else { '-Install' }) + " -DnsPort $DnsPort"
    if ($IncludePixiv) { $argLine += ' -IncludePixiv' }
    try {
        $p = Start-Process -FilePath $ps -ArgumentList $argLine -Verb RunAs -Wait -PassThru
        exit $p.ExitCode
    } catch {
        Write-Warn2 'elevation was cancelled'
        exit 5
    }
}

if ($Revert) {
    if (Test-Path "HKLM:\$RulePath") {
        Remove-Item "HKLM:\$RulePath" -Recurse -Force
        Write-Ok 'NRPT rule removed'
    } else {
        Write-Host '  no NRPT rule to remove'
    }
    if (Test-MachineCa) {
        $thumb = (New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 $CaCert).Thumbprint
        certutil.exe -delstore Root $thumb | Out-Null
        Write-Ok 'machine-store CA removed'
    }
    # Remove exactly the two policy values this tool wrote; other browser policies
    # belong to the administrator and are left alone.
    foreach ($vendor in 'Google\Chrome', 'Microsoft\Edge') {
        $vpath = "HKLM:\SOFTWARE\Policies\$vendor"
        if (-not (Test-Path $vpath)) { continue }
        foreach ($name in 'DnsOverHttpsMode', 'DnsOverHttpsTemplates') {
            if (Get-ItemProperty -Path $vpath -Name $name -ErrorAction SilentlyContinue) {
                Remove-ItemProperty -Path $vpath -Name $name -Force -ErrorAction SilentlyContinue
                Write-Ok "browser policy value removed: $vendor\$name"
            }
        }
    }
    $modeFile = Join-Path $RepoRoot 'data\mode.txt'
    if (Test-Path $modeFile) { Remove-Item $modeFile -Force -ErrorAction SilentlyContinue }
    ipconfig /flushdns | Out-Null
    exit 0
}

# ---- install -------------------------------------------------------------
$namespaces = Get-Namespaces
if (-not $namespaces -or $namespaces.Count -eq 0) {
    Write-Warn2 'no namespaces to route; is data\sites.json present?'
    exit 1
}

New-Item -Path "HKLM:\$RulePath" -Force | Out-Null
# REG_MULTI_SZ via New-ItemProperty would need the double-null block; Set-ItemProperty
# on a MultiString accepts a plain array and Windows encodes it correctly.
Set-ItemProperty -Path "HKLM:\$RulePath" -Name 'Version' -Value 2 -Type DWord
Set-ItemProperty -Path "HKLM:\$RulePath" -Name 'Name' -Value $namespaces -Type MultiString
Set-ItemProperty -Path "HKLM:\$RulePath" -Name 'GenericDNSServers' -Value $DnsServer -Type String
Set-ItemProperty -Path "HKLM:\$RulePath" -Name 'ConfigOptions' -Value 8 -Type DWord
Set-ItemProperty -Path "HKLM:\$RulePath" -Name 'Comment' -Value $Comment -Type String

$state = Get-RuleState
if (-not $state) {
    Write-Warn2 'rule was not stored'
    exit 1
}
Write-Ok "NRPT rule installed: $state"

if (Test-Path $CaCert) {
    if (Test-MachineCa) {
        Write-Ok 'machine-store CA already trusted'
    } else {
        certutil.exe -addstore -f Root $CaCert | Out-Null
        if ($LASTEXITCODE -eq 0) { Write-Ok 'CA installed into the machine store (all accounts)' }
        else { Write-Warn2 "certutil failed with $LASTEXITCODE" }
    }
}

# The main site still needs the browser's own DoH, because only DoH carries ECH:
# a plain-DNS answer is unauthenticated, so no browser will trust an ECH config
# obtained that way. We are already elevated, so write the machine-wide policy
# too - Chrome and Edge then pick it up with no further prompts.
$dohUrl = 'https://127.0.0.1:5444/dns-query'
foreach ($vendor in 'Google\Chrome', 'Microsoft\Edge') {
    $vpath = "HKLM:\SOFTWARE\Policies\$vendor"
    try {
        New-Item -Path $vpath -Force -ErrorAction Stop | Out-Null
        Set-ItemProperty -Path $vpath -Name 'DnsOverHttpsMode' -Value 'secure' -ErrorAction Stop
        Set-ItemProperty -Path $vpath -Name 'DnsOverHttpsTemplates' -Value $dohUrl -ErrorAction Stop
        Write-Ok "browser DoH policy set for $vendor (machine)"
    } catch {
        Write-Warn2 "could not set browser policy for $vendor"
    }
}

# Remember the chosen mode so the tray and the docs can report it.
$modeFile = Join-Path $RepoRoot 'data\mode.txt'
Set-Content -Path $modeFile -Value 'global' -Encoding ASCII

ipconfig /flushdns | Out-Null
Write-Host ''
Write-Host 'Global DNS mode is on. Every resolver that goes through the Windows' -ForegroundColor Green
Write-Host 'DNS Client now sends these names to sniroute:'
Write-Host ("  " + ($namespaces -join ' '))
Write-Host ''
Write-Host 'Undo with:  scripts\nrpt.ps1 -Revert'
exit 0
