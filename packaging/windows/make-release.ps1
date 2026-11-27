# SPDX-License-Identifier: GPL-3.0-only
# Builds the Windows release zip and SHA256SUMS.txt from a CMake build tree.
# The full package bundles a hash-pinned nvngx_dlssnr.dll when one is found
# (or supplied with -BundleNrDll) so install.bat works fully offline.
# Example: .\packaging\windows\make-release.ps1 -BuildDir out\build\windows-x64-release
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [string]$OutDir = '',
    [string]$Version = '0.3.0',
    [string]$Configuration = 'Release',
    [string]$BundleNrDll = '',
    [switch]$NoBundleNrDll
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
$RuntimeRel = 'bin\64bit\nvngx_dlssnr.dll'

function Get-AllowedNrRuntimeShas {
    return @{
        'e67dee209320cdafe0e93e45675d7aa34323a53acc57a72b2e40a181581c989a' = '310.8.0.0'
        'cdfa6c1ec784867182164c7836f324f087b5c9f374d0c9fe46f54999feabf3ed' = '310.9.16.0 (Model B)'
    }
}

function Get-NrRuntimeSha([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Find-VeyraNrRuntime {
    if (-not $env:LOCALAPPDATA) { return '' }
    $versionsDir = Join-Path $env:LOCALAPPDATA 'Programs\Veyra (NVIDIA)\versions'
    if (-not (Test-Path -LiteralPath $versionsDir -PathType Container)) { return '' }
    $allowed = Get-AllowedNrRuntimeShas
    $candidates = Get-ChildItem -LiteralPath $versionsDir -Directory -ErrorAction SilentlyContinue |
        ForEach-Object {
            $parts = [string[]]$_.Name.Split('.')
            if ($parts.Count -ge 1 -and $parts.Count -le 4 -and @($parts | Where-Object { $_ -notmatch '^\d+$' }).Count -eq 0) {
                $padded = [uint64[]]@(0, 0, 0, 0)
                for ($i = 0; $i -lt $parts.Count; $i++) { $padded[$i] = [uint64]$parts[$i] }
                [pscustomobject]@{
                    SortKey = '{0:D20}.{1:D20}.{2:D20}.{3:D20}' -f $padded[0], $padded[1], $padded[2], $padded[3]
                    Path    = Join-Path $_.FullName 'runtime\experimental\nr-ampere\nvngx_dlssnr.dll'
                }
            }
        } |
        Sort-Object SortKey -Descending
    foreach ($cand in $candidates) {
        if ((Test-Path -LiteralPath $cand.Path -PathType Leaf) -and
                $allowed.ContainsKey((Get-NrRuntimeSha $cand.Path))) {
            return $cand.Path
        }
    }
    return ''
}

function Assert-ReleaseArtifacts([string[]]$Entries, [string]$PluginPath, [string]$RuntimePath, [string]$Surface) {
    $pluginCount = 0
    $runtimeCount = 0
    $forbiddenDlls = @('libxess_fg.dll', 'libxell.dll', 'mfgUnlocker.dll')
    foreach ($entry in $Entries) {
        # ZipFile may emit Windows separators; compare the full normalized path.
        $path = $entry.Replace('\', '/')
        if ($path -match '\.(onnx|nrx|7z|nupkg|lib)$') {
            throw "$Surface contains forbidden release artifact: $entry"
        }
        if ($path -match '\.dll$') {
            $leaf = [IO.Path]::GetFileName($path).ToLowerInvariant()
            if ($forbiddenDlls -contains $leaf) {
                throw "$Surface contains forbidden release artifact: $entry"
            }
            if ($path -ceq $PluginPath) { $pluginCount++ }
            elseif ($RuntimePath -and $path -ceq $RuntimePath) { $runtimeCount++ }
            else { throw "$Surface contains forbidden release artifact: $entry" }
        }
    }
    if ($pluginCount -ne 1) { throw "$Surface must contain exactly one $PluginPath (found $pluginCount)." }
    if ($RuntimePath) {
        if ($runtimeCount -ne 1) {
            throw "$Surface must contain exactly one $RuntimePath (found $runtimeCount)."
        }
    } elseif ($runtimeCount -ne 0) {
        throw "$Surface contains an unexpected nvngx_dlssnr.dll."
    }
}

if ($NoBundleNrDll -and $BundleNrDll) { throw 'Use either -BundleNrDll or -NoBundleNrDll.' }
$runtimeSource = ''
$runtimeSha = ''
if (-not $NoBundleNrDll) {
    $candidate = $BundleNrDll
    if (-not $candidate) { $candidate = Find-VeyraNrRuntime }
    if ($candidate) {
        $candidate = (Resolve-Path -LiteralPath $candidate).Path
        if ((Split-Path -Leaf $candidate) -cne 'nvngx_dlssnr.dll') {
            throw "The bundled NR runtime must be named nvngx_dlssnr.dll: $candidate"
        }
        $runtimeSha = Get-NrRuntimeSha $candidate
        $allowed = Get-AllowedNrRuntimeShas
        if (-not $allowed.ContainsKey($runtimeSha)) {
            throw "Refusing to bundle an unapproved nvngx_dlssnr.dll (SHA-256 $runtimeSha): $candidate"
        }
        $runtimeSource = $candidate
        Write-Host "nr     $runtimeSource ($($allowed[$runtimeSha]))"
    } else {
        Write-Host 'nr     not bundled (no verified local Veyra copy; pass -BundleNrDll <path> to bundle one)'
    }
}

if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }

# Release payload, laid out exactly as inside the OBS install folder.
$payload = [ordered]@{
    'obs-plugins\64bit\obs-nr-filter.dll'             = Join-Path $BuildDir "$Configuration\obs-nr-filter.dll"
    'bin\64bit\obs-nr-fg-presenter.exe'                = Join-Path $BuildDir "$Configuration\obs-nr-fg-presenter.exe"
    'data\obs-plugins\obs-nr-filter\locale\en-US.ini' = Join-Path $repo 'data\locale\en-US.ini'
    'data\obs-plugins\obs-nr-filter\locale\ko-KR.ini' = Join-Path $repo 'data\locale\ko-KR.ini'
}
if ($runtimeSource) { $payload[$RuntimeRel] = $runtimeSource }
foreach ($shader in 'nr_codec.dxil', 'nr_color.dxil', 'nr_stabilize.dxil', 'nr_cas.dxil', 'nr_guides.cso', 'nr_interop_vertex.cso', 'nr_interop_pixel.cso', 'nr_obs_analysis.cso') {
    $payload["bin\64bit\$shader"] = Join-Path $BuildDir $shader
}
$lines = @()
foreach ($rel in $payload.Keys) {
    $dst = Join-Path $stage "files\$rel"
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
    Copy-Item -LiteralPath $payload[$rel] -Destination $dst
    $hash = (Get-FileHash -LiteralPath $dst -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($rel -eq $RuntimeRel -and $hash -ne $runtimeSha) {
        throw "Copied NR runtime hash mismatch: $rel"
    }
    $lines += ('{0}  {1}' -f $hash, $rel)
}
[IO.File]::WriteAllLines((Join-Path $stage 'files\manifest.sha256'), [string[]]$lines)

$extra = [ordered]@{
    'install.bat'          = Join-Path $PSScriptRoot 'install.bat'
    'uninstall.bat'        = Join-Path $PSScriptRoot 'uninstall.bat'
    'scripts\install.ps1'  = Join-Path $PSScriptRoot 'install.ps1'
    'INSTALL.md'           = Join-Path $repo 'docs\INSTALL.md'
    'INSTALL-ONECLICK.md'  = Join-Path $repo 'docs\INSTALL-ONECLICK.md'
    'oneclick\installer.png' = Join-Path $repo 'docs\oneclick\installer.png'
    'oneclick\dlss-prompt.png' = Join-Path $repo 'docs\oneclick\dlss-prompt.png'
    'oneclick\obs-filters.png' = Join-Path $repo 'docs\oneclick\obs-filters.png'
    'standalone-upscale.md' = Join-Path $repo 'docs\standalone-upscale.md'
    'pushfps-ab-guide.md'  = Join-Path $repo 'docs\pushfps-ab-guide.md'
    'LICENSE.txt'          = Join-Path $repo 'LICENSE'
    'notices\CREDITS.md'   = Join-Path $repo 'provenance\source-notices\CREDITS.md'
    'notices\README.md'    = Join-Path $repo 'provenance\source-notices\README.md'
    'notices\PushFPS-NOTICE.md' = Join-Path $repo 'provenance\source-notices\PushFPS-NOTICE.md'
    'notices\ONNXRuntime-MIT.txt' = Join-Path $repo 'provenance\source-notices\ONNXRuntime-MIT.txt'
    'notices\RIFE-MIT.txt' = Join-Path $repo 'provenance\source-notices\RIFE-MIT.txt'
    'notices\NR-R3.md'    = Join-Path $repo 'provenance\source-notices\NR-R3.md'
    'notices\RenoDX-MIT.txt' = Join-Path $repo 'provenance\source-notices\RenoDX-MIT.txt'
    'notices\AMD-FidelityFX-CAS-MIT.txt' = Join-Path $repo 'provenance\source-notices\AMD-FidelityFX-CAS-MIT.txt'
    'notices\Intel-XeSS-License.txt' = Join-Path $repo 'provenance\source-notices\Intel-XeSS-License.txt'
    'notices\MFG-Unlocker-MIT.txt' = Join-Path $repo 'provenance\source-notices\MFG-Unlocker-MIT.txt'
}
foreach ($rel in $extra.Keys) {
    $dst = Join-Path $stage $rel
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
    if ($rel -eq 'scripts\install.ps1') {
        # Windows PowerShell 5.1 needs the BOM to decode Korean on non-UTF-8 systems.
        [IO.File]::WriteAllText($dst, [IO.File]::ReadAllText($extra[$rel]), [Text.UTF8Encoding]::new($true))
    } else {
        Copy-Item -LiteralPath $extra[$rel] -Destination $dst
    }
}

$pluginPath = 'files/obs-plugins/64bit/obs-nr-filter.dll'
$runtimePath = if ($runtimeSource) { 'files/bin/64bit/nvngx_dlssnr.dll' } else { '' }
$stageEntries = @(Get-ChildItem -LiteralPath $stage -Recurse -File -Force |
    ForEach-Object { $_.FullName.Substring($stage.Length + 1) })
Assert-ReleaseArtifacts $stageEntries $pluginPath $runtimePath 'Staging directory'

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$candidate = Join-Path $OutDir "$name.$([guid]::NewGuid().ToString('N')).tmp"
try {
    # Write forward-slash entry names: ZipFile.CreateFromDirectory emits backslashes
    # on .NET Framework, which non-Windows unzip tools treat as literal characters.
    $archive = [IO.Compression.ZipFile]::Open($candidate, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($file in Get-ChildItem -LiteralPath $stage -Recurse -File -Force) {
            $entryName = "$name/" + $file.FullName.Substring($stage.Length + 1).Replace('\', '/')
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $archive, $file.FullName, $entryName, [IO.Compression.CompressionLevel]::Optimal)
        }
    } finally {
        $archive.Dispose()
    }
    $archive = [IO.Compression.ZipFile]::OpenRead($candidate)
    try {
        $zipRuntimePath = if ($runtimeSource) { "$name/$runtimePath" } else { '' }
        Assert-ReleaseArtifacts @($archive.Entries.FullName) "$name/$pluginPath" $zipRuntimePath 'ZIP'
    } finally {
        $archive.Dispose()
    }
    Move-Item -LiteralPath $candidate -Destination $zip
} finally {
    if (Test-Path -LiteralPath $candidate) { Remove-Item -LiteralPath $candidate -Force }
}
$hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllLines((Join-Path $OutDir 'SHA256SUMS.txt'), [string[]]@("$hash  $name.zip"))
Write-Host "zip    $zip"
Write-Host "sha256 $hash"
if ($runtimeSource) { Write-Host "nr     bundled $RuntimeRel" } else { Write-Host 'nr     not bundled' }
foreach ($line in $lines) { Write-Host "  $line" }
