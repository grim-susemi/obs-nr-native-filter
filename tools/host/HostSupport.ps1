# SPDX-License-Identifier: GPL-2.0-or-later
Add-Type -TypeDefinition @'
using System;
using System.Diagnostics;
using System.Threading;
public sealed class NrProcessExit : IDisposable {
    public readonly ManualResetEvent Exited = new ManualResetEvent(false);
    public NrProcessExit(Process process) { process.Exited += (sender, args) => Exited.Set(); }
    public void Dispose() { Exited.Dispose(); }
}
'@
function Assert-OwnedRoot([string]$Candidate) {
    if ([IO.Path]::GetFullPath($Candidate).TrimEnd('\','/') -ne $owned) { throw 'NR_QA_FOREIGN_ROOT' }
    if (Test-Path $Candidate) { throw 'NR_QA_EXISTING_ROOT' }
}
function Identity($Process) {
    @{pid=$Process.Id; creation=$Process.StartTime.ToUniversalTime().ToString('o'); image=$Process.MainModule.FileName}
}
function FileIdentity([string]$Path) {
    $fileId = (& "$env:SystemRoot/System32/fsutil.exe" file queryfileid $Path | Out-String).Trim()
    if ($LASTEXITCODE) { throw 'NR_QA_FILEID_FAILED' }
    $item = Get-Item -LiteralPath $Path
    @{path=$Path;sha256=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash;bytes=$item.Length;file_id=$fileId;attributes=$item.Attributes.ToString()}
}
function Stage-Host {
    foreach ($dir in @('bin/64bit','profile','obs-plugins/64bit','data/libobs','data/obs-plugins/obs-nr-filter','data/obs-plugins/obs-x264')) {
        New-Item -ItemType Directory -Force (Join-Path $root $dir) | Out-Null
    }
    $files = @('obs.dll','libobs-d3d11.dll','avcodec-62.dll','avformat-62.dll','avutil-60.dll','swscale-9.dll','swresample-6.dll','zlib.dll','w32-pthreads.dll','libx264-164.dll','librist.dll','srt.dll') | ForEach-Object { 'bin/64bit/' + $_ }
    $files += 'obs-plugins/64bit/obs-x264.dll'
    if ($Scenario -eq 'sampler') { $files += 'bin/64bit/libobs-opengl.dll' }
    $files += @(Get-ChildItem (Join-Path $installed 'data/libobs') -File | ForEach-Object { 'data/libobs/' + $_.Name })
    $files += @(Get-ChildItem (Join-Path $installed 'data/obs-plugins/obs-x264/locale') -File | ForEach-Object { 'data/obs-plugins/obs-x264/locale/' + $_.Name })
    $pins = @{
        'bin/64bit/obs.dll'='9B9A76F64A1D27262209F1CFBEAF6D57A175CC4E823C71882BB7D5B5AB99BDCF'
        'bin/64bit/libobs-d3d11.dll'='A2A065786F146EEE219031BC9A28845CFF78ABF570296E88DD5AD051ABB8188F'
        'obs-plugins/64bit/obs-x264.dll'='0610DAE11937263E4520FC2562845D50EF4B27A7EF3ABDB6997DFDE0CEA80EFC'
    }
    foreach ($file in $files) {
        $source = Join-Path $installed $file
        $dest = Join-Path $root $file
        $before = FileIdentity $source
        if ($pins.ContainsKey($file) -and $before.sha256 -ne $pins[$file]) { throw 'NR_QA_INSTALLED_IDENTITY_CHANGED' }
        if ($file.EndsWith('.dll')) {
            $signature = Get-AuthenticodeSignature -LiteralPath $source
            if ($signature.Status -ne 'Valid' -or !$signature.SignerCertificate.Subject.Contains('OBS Project, LLC')) { throw 'NR_QA_OFFICIAL_SIGNATURE_REFUSED' }
            $before.signature_status = $signature.Status.ToString()
            $before.signer = $signature.SignerCertificate.Subject
        }
        New-Item -ItemType Directory -Force (Split-Path $dest) | Out-Null
        $sources.Add($before)
        # Minimal private runtime copies: linking Program Files is denied by NTFS.
        Copy-Item -LiteralPath $source -Destination $dest
        $copy = FileIdentity $dest
        if ($copy.sha256 -ne $before.sha256 -or $copy.bytes -ne $before.bytes) { throw 'NR_QA_COPY_IDENTITY_CHANGED' }
    }
    Copy-Item (Join-Path $build 'nr-qa-host.exe') (Join-Path $root 'bin/64bit/nr-qa-host.exe')
    Copy-Item (Join-Path $build 'obs-nr-filter.dll') (Join-Path $root 'obs-plugins/64bit/obs-nr-filter.dll')
    if ($Scenario -eq 'texture-roundtrip') {
        foreach ($shader in @('nr_interop_vertex.cso','nr_interop_pixel.cso')) {
            Copy-Item (Join-Path (Split-Path $build) $shader) (Join-Path $root ('bin/64bit/' + $shader))
        }
    }
    Copy-Item (Join-Path $work 'data/locale') (Join-Path $root 'data/obs-plugins/obs-nr-filter/locale') -Recurse
}
function Assert-CleanInventory($Inventory, [string]$Graphics = 'libobs-d3d11.dll') {
    foreach ($module in $Inventory) {
        $name = [IO.Path]::GetFileName($module.path)
        if ($name -match '^(nvngx.*|.*feeder.*|.*dlss5.*|.*reshade.*|.*optiscaler.*)$' -or $name -match '\.(asi|addon64)$') { throw 'NR_QA_INJECTION_LOADED' }
        if ($name -in @('winmm.dll','dxgi.dll') -and !$module.path.StartsWith($env:SystemRoot + '\System32\',[StringComparison]::OrdinalIgnoreCase)) { throw 'NR_QA_PROXY_LOADED' }
        if (!$module.path.StartsWith($root + '\',[StringComparison]::OrdinalIgnoreCase) -and !$module.path.StartsWith($env:SystemRoot + '\',[StringComparison]::OrdinalIgnoreCase)) { throw ('NR_QA_EXTERNAL_MODULE: ' + $module.path) }
    }
    foreach ($required in @('obs.dll',$Graphics,'obs-nr-filter.dll','obs-x264.dll')) {
        if (!($Inventory | Where-Object { [IO.Path]::GetFileName($_.path) -eq $required })) { throw ('NR_QA_MODULE_MISSING: ' + $required) }
    }
}
