#requires -Version 5.1
<#
.SYNOPSIS
    Generate the "supported sites" submenu data, from data\sites.json.

.DESCRIPTION
    ASCII ONLY. Do not add non-ASCII characters to this file.

    The tray is C++ and has no JSON parser, so it reads one pre-flattened list.
    Generating that list from sites.json (instead of typing it twice) is the only
    way to keep the menu and the actual configuration in agreement.

    Format, one entry per line:      label|url
      label  what the user sees      "pixiv"
      url    what clicking opens     "https://www.pixiv.net/"

    The label is the `service` field, not `name`: the internal split between the
    main site and the image host is an implementation detail, and to a user both
    of them are simply "pixiv". The URL is the one a person would want to open,
    which is the site itself, not the image CDN.

    Output: data\sites-tray.txt
#>
param()

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Sites = Join-Path $RepoRoot 'data\sites.json'
$OutFile = Join-Path $RepoRoot 'data\sites-tray.txt'

if (-not (Test-Path $Sites)) {
    Write-Host "!! missing $Sites" -ForegroundColor Red
    exit 1
}

$cfg = Get-Content $Sites -Raw -Encoding UTF8 | ConvertFrom-Json
$seen = @{}
$lines = New-Object System.Collections.Generic.List[string]

foreach ($site in $cfg.sites) {
    $label = $site.service
    if (-not $label) { $label = $site.name }
    if (-not $label) { continue }
    if ($seen.ContainsKey($label)) { continue }

    $url = $site.url
    if (-not $url) { continue }              # nothing to open: skip silently
    $seen[$label] = $true
    $lines.Add("$label|$url")
}

[System.IO.File]::WriteAllLines($OutFile, $lines, (New-Object System.Text.UTF8Encoding($false)))
Write-Host ("   supported sites: " + ($lines -join ', '))
