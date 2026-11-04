# SPDX-License-Identifier: GPL-3.0-only
param([Parameter(Mandatory)][string]$Work)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$locale = Join-Path $Work 'data/locale'
$sets = @{}
foreach ($name in 'en-US', 'ko-KR', 'ru-RU') {
    $entries = @{}
    foreach ($line in [IO.File]::ReadAllLines((Join-Path $locale "$name.ini"), [Text.Encoding]::UTF8)) {
        if ($line -notmatch '^([^=]+)="(.*)"$') { throw "Invalid locale entry: $name" }
        if ($entries.ContainsKey($Matches[1])) { throw "Duplicate locale key: $name $($Matches[1])" }
        $entries[$Matches[1]] = $Matches[2]
    }
    $sets[$name] = $entries
}
foreach ($name in 'ko-KR', 'ru-RU') {
    $diff = @(Compare-Object @($sets['en-US'].Keys | Sort-Object) @($sets[$name].Keys | Sort-Object))
    if ($diff.Count) { throw "Locale keys differ: $name" }
    foreach ($key in $sets['en-US'].Keys) {
        $expected = @([regex]::Matches($sets['en-US'][$key], '\{\d+(?:[^}]*)\}|%[0-9]*[a-zA-Z]') | ForEach-Object Value | Sort-Object)
        $actual = @([regex]::Matches($sets[$name][$key], '\{\d+(?:[^}]*)\}|%[0-9]*[a-zA-Z]') | ForEach-Object Value | Sort-Object)
        if (@(Compare-Object $expected $actual).Count) { throw "Locale placeholders differ: $name $key" }
    }
}
$script = Join-Path $Work 'packaging/windows/install.ps1'
$bytes = [IO.File]::ReadAllBytes($script)
if ($bytes[0] -ne 0xef -or $bytes[1] -ne 0xbb -or $bytes[2] -ne 0xbf) { throw 'Installer requires UTF-8 BOM for Windows PowerShell' }
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($script, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw ($errors | Out-String) }
$assign = $ast.Find({ param($n) $n -is [Management.Automation.Language.AssignmentStatementAst] -and $n.Left.Extent.Text -eq '$Text' }, $true)
$tables = Invoke-Expression $assign.Right.Extent.Text
foreach ($name in 'ko', 'ru') {
    if (@(Compare-Object @($tables.en.Keys | Sort-Object) @($tables[$name].Keys | Sort-Object)).Count) { throw "Installer keys differ: $name" }
    foreach ($key in $tables.en.Keys) {
        $expected = @([regex]::Matches($tables.en[$key], '\{\d+\}') | ForEach-Object Value | Sort-Object)
        $actual = @([regex]::Matches($tables[$name][$key], '\{\d+\}') | ForEach-Object Value | Sort-Object)
        if (@(Compare-Object $expected $actual).Count) { throw "Installer placeholders differ: $name $key" }
    }
}
$release = [IO.File]::ReadAllText((Join-Path $Work 'packaging/windows/make-release.ps1'))
foreach ($name in 'en-US', 'ko-KR', 'ru-RU') {
    if (-not $release.Contains("data\obs-plugins\obs-nr-filter\locale\$name.ini")) { throw "Release locale missing: $name" }
}
Write-Output "PASS locale and installer key/placeholder parity, BOM, PowerShell parse, release registration"
