#requires -Version 5.1
<#
.SYNOPSIS
    Regenerate THIRD_PARTY_LICENSES.txt with the full license texts inline.

.DESCRIPTION
    ASCII ONLY. Do not add non-ASCII characters to this file.

    The notice must carry the verbatim license texts, not links to them, so that a
    single file is enough to satisfy the attribution requirements. Keeping those
    texts inline by hand would drift; this script rebuilds the file from
    data\third-party-header.txt plus the files in licenses\.

    Run after changing any license file:  powershell -File scripts\make-third-party-notice.ps1
#>
param()

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Header = Join-Path $RepoRoot 'data\third-party-header.txt'
$LicDir = Join-Path $RepoRoot 'licenses'
$OutFile = Join-Path $RepoRoot 'THIRD_PARTY_LICENSES.txt'

if (-not (Test-Path $Header)) { Write-Host "!! missing $Header" -ForegroundColor Red; exit 1 }
if (-not (Test-Path $LicDir)) { Write-Host "!! missing $LicDir" -ForegroundColor Red; exit 1 }

# Appendix order is deliberate: runtimes first, small bundled libraries after.
$appendices = @(
    @{ Key = 'A'; Title = 'CPython 3.12.14 - PSF License Version 2'; File = 'python-PSF-2.0.txt' },
    @{ Key = 'B'; Title = 'OpenSSL 3.2.1 / 3.5.8 - Apache License 2.0'; File = 'openssl-Apache-2.0.txt' },
    @{ Key = 'C'; Title = 'nginx 1.31.8 - BSD 2-Clause'; File = 'nginx-BSD-2-Clause.txt' },
    @{ Key = 'D'; Title = 'PCRE2 (inside nginx.exe) - BSD 3-Clause'; File = 'pcre2-BSD-3-Clause.txt' },
    @{ Key = 'E'; Title = 'zlib (inside nginx.exe) - zlib license'; File = 'zlib.txt' },
    @{ Key = 'F'; Title = 'libffi - MIT'; File = 'libffi-MIT.txt' },
    @{ Key = 'G'; Title = 'expat - MIT'; File = 'expat-MIT.txt' },
    @{ Key = 'H'; Title = 'Tcl/Tk 8.6 - Tcl/Tk license'; File = 'tcl-tk.txt' }
)

$sb = New-Object System.Text.StringBuilder
$banner = '=' * 80

[void]$sb.Append((Get-Content $Header -Raw -Encoding UTF8))
[void]$sb.Append("`r`n")

foreach ($ap in $appendices) {
    $path = Join-Path $LicDir $ap.File
    if (-not (Test-Path $path)) {
        Write-Host "!! missing license file: $($ap.File)" -ForegroundColor Red
        exit 1
    }
    [void]$sb.Append($banner + "`r`n")
    [void]$sb.Append("Appendix $($ap.Key) - $($ap.Title)`r`n")
    [void]$sb.Append($banner + "`r`n")
    [void]$sb.Append("Source file: licenses\$($ap.File)`r`n`r`n")
    $text = (Get-Content $path -Raw -Encoding UTF8) -replace "`r`n", "`n" -replace "`n", "`r`n"
    [void]$sb.Append($text.TrimEnd() + "`r`n`r`n")
}

# SQLite has no license text: it is public domain, so state that verbatim.
[void]$sb.Append($banner + "`r`n")
[void]$sb.Append("Appendix I - SQLite (inside python\DLLs\sqlite3.dll) - Public domain`r`n")
[void]$sb.Append($banner + "`r`n")
[void]$sb.Append(@'
SQLite is in the public domain. The authors (Hwaci / the SQLite team) state:

  The author disclaims copyright to this source code. In place of a legal
  notice, here is a blessing:

     May you do good and not evil.
     May you find forgiveness for yourself and forgive others.
     May you share freely, never taking more than you give.

  Source: https://sqlite.org/copyright.html

The bundled copy is the sqlite3.dll that ships with the official CPython 3.12
Windows build, unmodified.

'@.Replace("`r`n", "`n").Replace("`n", "`r`n") + "`r`n")

# Our own license is not third-party, so it is referenced rather than copied in.
[void]$sb.Append($banner + "`r`n")
[void]$sb.Append("Appendix J - sniroute itself - GPL-3.0-only`r`n")
[void]$sb.Append($banner + "`r`n")
[void]$sb.Append(@'
The full GPL-3.0 text ships as LICENSE next to this file (233 lines). It is not
repeated here because it is not third-party material.

When redistributing sniroute:
  * ship LICENSE and make the corresponding source available (this repository);
  * state your changes if you modify it;
  * a modified version distributed to others must also be GPL-3.0.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE.
'@.Replace("`r`n", "`n").Replace("`n", "`r`n") + "`r`n")

$out = $sb.ToString()
[System.IO.File]::WriteAllText($OutFile, $out, (New-Object System.Text.UTF8Encoding($false)))

$size = (Get-Item $OutFile).Length
$lines = ($out -split "`r`n").Count
Write-Host ''
Write-Host ("Wrote {0} ({1:N1} KB, {2} lines, {3} appendices)" -f $OutFile, ($size / 1KB), $lines, ($appendices.Count + 2)) -ForegroundColor Green
