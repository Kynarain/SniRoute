#requires -Version 5.1
<#
.SYNOPSIS
    sniroute GUI: one window for the whole tool.

.DESCRIPTION
    ASCII ONLY. Do not add non-ASCII characters to this file.
    Windows PowerShell 5.1 decodes .ps1 files in the ANSI codepage unless the
    file has a UTF-8 BOM; a mangled string can swallow a quote and break the
    whole script. All user-visible text therefore lives in
    data\gui-strings.json and is read with -Encoding UTF8.

    This is a thin shell. Every action calls scripts\sniroute.cmd, which is the
    same code path the command line uses, so the two can never disagree.

.PARAMETER SmokeTest
    Build the window, then close it without entering the message loop.
#>
param([switch]$SmokeTest)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Cli = Join-Path $RepoRoot 'sniroute.cmd'
$StringsPath = Join-Path $RepoRoot 'data\gui-strings.json'
$HostsPath = Join-Path $env:SystemRoot 'System32\drivers\etc\hosts'

$T = Get-Content $StringsPath -Raw -Encoding UTF8 | ConvertFrom-Json

$DohPort = 5444
$lampGreen = [System.Drawing.Color]::FromArgb(0, 120, 40)
$lampRed = [System.Drawing.Color]::FromArgb(180, 40, 30)

# ---------------------------------------------------------------------------
# Form
# ---------------------------------------------------------------------------
$form = New-Object System.Windows.Forms.Form
$form.Text = $T.title
$form.Size = New-Object System.Drawing.Size(900, 660)
$form.MinimumSize = New-Object System.Drawing.Size(780, 560)
$form.StartPosition = 'CenterScreen'
$form.Font = New-Object System.Drawing.Font('Segoe UI', 9)

$y = 8

# --- status lamps ---------------------------------------------------------
$grpStatus = New-Object System.Windows.Forms.GroupBox
$grpStatus.Text = $T.group_status
$grpStatus.Location = New-Object System.Drawing.Point(8, $y)
$grpStatus.Size = New-Object System.Drawing.Size(866, 56)
$grpStatus.Anchor = 'Top,Left,Right'
$form.Controls.Add($grpStatus)

$lamps = @{}
$lampNames = @($T.lamp_doh, $T.lamp_nginx, $T.lamp_hosts, $T.lamp_ca)
for ($i = 0; $i -lt $lampNames.Count; $i++) {
    $lbl = New-Object System.Windows.Forms.Label
    $lbl.Text = "$($lampNames[$i]): ?"
    $lbl.Location = New-Object System.Drawing.Point((12 + $i * 212), 22)
    $lbl.Size = New-Object System.Drawing.Size(205, 20)
    $grpStatus.Controls.Add($lbl)
    $lamps[$lampNames[$i]] = $lbl
}
$y += 64

# --- actions --------------------------------------------------------------
$grpActions = New-Object System.Windows.Forms.GroupBox
$grpActions.Text = $T.group_actions
$grpActions.Location = New-Object System.Drawing.Point(8, $y)
$grpActions.Size = New-Object System.Drawing.Size(866, 62)
$grpActions.Anchor = 'Top,Left,Right'
$form.Controls.Add($grpActions)

$log = New-Object System.Windows.Forms.TextBox
$log.Multiline = $true
$log.ScrollBars = 'Vertical'
$log.ReadOnly = $true
$log.Font = New-Object System.Drawing.Font('Consolas', 9)

function Write-Log([string]$text) {
    if ([string]::IsNullOrWhiteSpace($text)) { return }
    $log.AppendText($text.TrimEnd() + "`r`n")
    $script:form.Refresh()
}

function Invoke-Cli([string[]]$cliArgs) {
    Write-Log("=== sniroute $($cliArgs -join ' ') ===")
    $form.Cursor = [System.Windows.Forms.Cursors]::WaitCursor
    try {
        $out = & $Cli @cliArgs 2>&1 | Out-String
        Write-Log($out)
    }
    catch {
        Write-Log("[ERROR] $($_.Exception.Message)")
    }
    finally {
        $form.Cursor = [System.Windows.Forms.Cursors]::Default
    }
    Update-Status
}

function Invoke-ElevatedScript([string]$name, [string[]]$extra) {
    $script = Join-Path $RepoRoot "scripts\$name"
    if (-not (Test-Path $script)) { Write-Log("[ERROR] missing $script"); return }
    $args = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$script`"") + $extra
    Write-Log("=== elevated: $name $($extra -join ' ') ===")
    try {
        Start-Process -FilePath 'powershell.exe' -ArgumentList $args -Verb RunAs -Wait
        Write-Log("done")
    }
    catch {
        Write-Log("[ERROR] elevation refused or failed: $($_.Exception.Message)")
    }
    Update-Status
}

$buttons = @(
    @{ Text = $T.btn_up; Action = { Invoke-Cli @('up') } },
    @{ Text = $T.btn_down; Action = { Invoke-Cli @('down') } },
    @{ Text = $T.btn_refresh; Action = { Update-Status; Write-Log('refreshed') } },
    @{ Text = $T.btn_install; Action = {
            if ([System.Windows.Forms.MessageBox]::Show($T.confirm_install, $T.title_confirm, 'OKCancel') -eq 'OK') {
                Invoke-ElevatedScript 'setup-browser-ech.ps1' @('-Elevated')
            }
        } },
    @{ Text = $T.btn_browser; Action = { Invoke-ElevatedScript 'start-chrome-ech.ps1' @() } },
    @{ Text = $T.btn_revert; Action = {
            if ([System.Windows.Forms.MessageBox]::Show($T.confirm_revert, $T.title_confirm, 'OKCancel') -eq 'OK') {
                Invoke-Cli @('down')
                Invoke-ElevatedScript 'setup-browser-ech.ps1' @('-Revert', '-Elevated')
            }
        } }
)

for ($i = 0; $i -lt $buttons.Count; $i++) {
    $btn = New-Object System.Windows.Forms.Button
    $btn.Text = $buttons[$i].Text
    $btn.Size = New-Object System.Drawing.Size(138, 30)
    $btn.Location = New-Object System.Drawing.Point((12 + $i * 142), 22)
    $btn.Add_Click($buttons[$i].Action)
    $grpActions.Controls.Add($btn)
}
$y += 70

# --- sites ----------------------------------------------------------------
$grpSites = New-Object System.Windows.Forms.GroupBox
$grpSites.Text = $T.group_sites
$grpSites.Location = New-Object System.Drawing.Point(8, $y)
$grpSites.Size = New-Object System.Drawing.Size(866, 150)
$grpSites.Anchor = 'Top,Left,Right'
$form.Controls.Add($grpSites)

$grid = New-Object System.Windows.Forms.DataGridView
$grid.Location = New-Object System.Drawing.Point(10, 20)
$grid.Size = New-Object System.Drawing.Size(846, 118)
$grid.Anchor = 'Top,Left,Right'
$grid.ReadOnly = $true
$grid.AllowUserToAddRows = $false
$grid.RowHeadersVisible = $false
$grid.SelectionMode = 'FullRowSelect'
$grid.AutoSizeColumnsMode = 'Fill'
foreach ($c in @(
        @{ Name = 'state'; Title = $T.col_state; Weight = 12 },
        @{ Name = 'mode'; Title = $T.col_mode; Weight = 14 },
        @{ Name = 'domains'; Title = $T.col_domains; Weight = 58 },
        @{ Name = 'upstreams'; Title = $T.col_upstreams; Weight = 16 })) {
    [void]$grid.Columns.Add($c.Name, $c.Title)
}
$grpSites.Controls.Add($grid)
$y += 158

# --- log ------------------------------------------------------------------
$grpLog = New-Object System.Windows.Forms.GroupBox
$grpLog.Text = $T.group_log
$grpLog.Location = New-Object System.Drawing.Point(8, $y)
$grpLog.Size = New-Object System.Drawing.Size(866, 300)
$grpLog.Anchor = 'Top,Bottom,Left,Right'
$form.Controls.Add($grpLog)

$log.Location = New-Object System.Drawing.Point(10, 20)
$log.Size = New-Object System.Drawing.Size(846, 268)
$log.Anchor = 'Top,Bottom,Left,Right'
$grpLog.Controls.Add($log)

$hint = New-Object System.Windows.Forms.Label
$hint.Text = $T.hint
$hint.Location = New-Object System.Drawing.Point(12, 8)
$hint.AutoSize = $true
$form.Controls.Add($hint)

# ---------------------------------------------------------------------------
# State refresh
# ---------------------------------------------------------------------------
function Test-Doh {
    $c = Get-NetTCPConnection -State Listen -LocalPort $DohPort -ErrorAction SilentlyContinue
    return [bool]$c
}

function Test-Nginx {
    $pidFile = Join-Path $RepoRoot 'logs\nginx.pid'
    if (-not (Test-Path $pidFile)) { return $false }
    $nginxPid = (Get-Content $pidFile -Raw -ErrorAction SilentlyContinue)
    if (-not $nginxPid) { return $false }
    $proc = Get-Process -Id ([int]$nginxPid.Trim()) -ErrorAction SilentlyContinue
    return [bool]$proc
}

function Test-HostsEntry {
    if (-not (Test-Path $HostsPath)) { return $false }
    $text = (New-Object System.Text.UTF8Encoding($false)).GetString(
        [System.IO.File]::ReadAllBytes($HostsPath))
    return $text.Contains('# >>> sniroute doh >>>')
}

function Test-Ca {
    if (-not (Test-Path (Join-Path $RepoRoot 'ca\rootCA.crt'))) { return $false }
    $ca = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 (Join-Path $RepoRoot 'ca\rootCA.crt')
    $found = Get-ChildItem Cert:\LocalMachine\Root -ErrorAction SilentlyContinue |
        Where-Object { $_.Thumbprint -eq $ca.Thumbprint }
    return [bool]$found
}

function Set-Lamp([string]$name, [bool]$ok, [string]$extra) {
    $lamp = $lamps[$name]
    if (-not $lamp) { return }
    $word = if ($ok) { $T.on } else { $T.off }
    $lamp.Text = "$($name): $word$extra"
    $lamp.ForeColor = if ($ok) { $lampGreen } else { $lampRed }
}

function Update-Status {
    Set-Lamp $T.lamp_doh (Test-Doh) ''
    Set-Lamp $T.lamp_nginx (Test-Nginx) ''
    Set-Lamp $T.lamp_hosts (Test-HostsEntry) ''
    Set-Lamp $T.lamp_ca (Test-Ca) ''

    $grid.Rows.Clear()
    $sitesFile = Join-Path $RepoRoot 'data\sites.json'
    if (Test-Path $sitesFile) {
        $cfg = Get-Content $sitesFile -Raw -Encoding UTF8 | ConvertFrom-Json
        foreach ($site in $cfg.sites) {
            $enabled = $true
            if ($null -ne $site.enabled) { $enabled = [bool]$site.enabled }
            $state = if ($enabled) { 'ACTIVE' } elseif ($site.dns_override) { 'DNS-ONLY' } else { 'OFF' }
            $mode = if ($site.dns_mode) { $site.dns_mode } else { 'ech' }
            $count = 0
            $upstream = Join-Path $RepoRoot $site.upstream_file
            if (Test-Path $upstream) {
                $count = (Select-String -Path $upstream -Pattern '^\s*server ' -AllMatches).Count
            }
            [void]$grid.Rows.Add($state, $mode, ($site.hosts -join ', '), "$count")
        }
    }
}

# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------
Update-Status
Write-Log("sniroute GUI ready. CLI: $Cli")

$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 3000
$timer.Add_Tick({ Update-Status })
$timer.Start()

if ($SmokeTest) {
    Write-Host 'SmokeTest: form built, controls =' $form.Controls.Count
    $form.Dispose()
    exit 0
}

[void]$form.ShowDialog()
