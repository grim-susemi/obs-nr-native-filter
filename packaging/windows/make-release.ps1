# SPDX-License-Identifier: GPL-3.0-only
# Builds the Windows release zip and SHA256SUMS.txt from a CMake build tree.
# Example: .\packaging\windows\make-release.ps1 -BuildDir out\build\windows-x64-release
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [string]$OutDir = '',
    [string]$Version = '0.1.0'
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$BuildDir = (Resolve-Path $BuildDir).Path
if (-not $OutDir) { $OutDir = Join-Path $repo 'out\release' }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$name = "obs-nr-native-filter-$Version-windows-x64"
$stage = Join-Path $OutDir $name
$zip = Join-Path $OutDir "$name.zip"
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }

# Release payload, laid out exactly as inside the OBS install folder.
$payload = [ordered]@{
    'obs-plugins\64bit\obs-nr-filter.dll'             = Join-Path $BuildDir 'Release\obs-nr-filter.dll'
    'data\obs-plugins\obs-nr-filter\locale\en-US.ini' = Join-Path $repo 'data\locale\en-US.ini'
    'data\obs-plugins\obs-nr-filter\locale\ko-KR.ini' = Join-Path $repo 'data\locale\ko-KR.ini'
}
foreach ($shader in 'nr_codec.dxil', 'nr_color.dxil', 'nr_guides.cso', 'nr_interop_vertex.cso', 'nr_interop_pixel.cso', 'nr_obs_analysis.cso') {
    $payload["bin\64bit\$shader"] = Join-Path $BuildDir $shader
}
$lines = @()
foreach ($rel in $payload.Keys) {
    $dst = Join-Path $stage "files\$rel"
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
    Copy-Item -LiteralPath $payload[$rel] -Destination $dst
    $lines += ('{0}  {1}' -f (Get-FileHash -LiteralPath $dst -Algorithm SHA256).Hash.ToLowerInvariant(), $rel)
}
[IO.File]::WriteAllLines((Join-Path $stage 'files\manifest.sha256'), [string[]]$lines)

$extra = [ordered]@{
    'install.bat'          = Join-Path $PSScriptRoot 'install.bat'
    'uninstall.bat'        = Join-Path $PSScriptRoot 'uninstall.bat'
    'scripts\install.ps1'  = Join-Path $PSScriptRoot 'install.ps1'
    'INSTALL.md'           = Join-Path $repo 'docs\INSTALL.md'
    'LICENSE.txt'          = Join-Path $repo 'LICENSE'
    'notices\CREDITS.md'   = Join-Path $repo 'provenance\source-notices\CREDITS.md'
    'notices\RenoDX-MIT.txt' = Join-Path $repo 'provenance\source-notices\RenoDX-MIT.txt'
}
foreach ($rel in $extra.Keys) {
    $dst = Join-Path $stage $rel
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
    Copy-Item -LiteralPath $extra[$rel] -Destination $dst
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [IO.Compression.CompressionLevel]::Optimal, $true)
$hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllLines((Join-Path $OutDir 'SHA256SUMS.txt'), [string[]]@("$hash  $name.zip"))
Write-Host "zip    $zip"
Write-Host "sha256 $hash"
foreach ($line in $lines) { Write-Host "  $line" }
