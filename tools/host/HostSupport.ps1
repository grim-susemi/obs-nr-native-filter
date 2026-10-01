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
function Assert-NoUnrelatedProcess {
    $active = @(Get-Process | Where-Object { $_.ProcessName -match '^(obs64|nr-qa-host|CrimsonDesert|GTA5|GTA5_Enhanced|Cyberpunk2077|eldenring|BlackMythWukong|cs2|dota2|FortniteClient.*)$' })
    if ($active.Count) { throw ('NR_QA_UNRELATED_PROCESS: ' + ($active.Id -join ',')) }
}
function Assert-ColorRows($Rows, $Dispatches, $Contexts, $Controls) {
    if ($Rows.Count -ne 15 -or $Dispatches.Count -ne 50 -or $Contexts.Count -ne 1 -or $Controls.Count -ne 8) { throw 'NR_QA_COLOR_ROW_SET' }
    $context=$Contexts[0]
    if ($context.software -isnot [bool] -or $context.private_direct_queue -isnot [bool]) { throw 'NR_QA_COLOR_CONTEXT' }
    if ($context.software -or !$context.private_direct_queue -or $context.vendor_id -ne 4318 -or $context.adapter -ne 'RTX 4090' -or !$context.device -or !$context.queue -or [UInt64]$context.device -eq 0 -or [UInt64]$context.queue -eq 0 -or $context.renderer_luid_high -ne $context.worker_luid_high -or $context.renderer_luid_low -ne $context.worker_luid_low) { throw 'NR_QA_COLOR_CONTEXT' }
    for ($i=0; $i -lt 50; $i++) {
        $d=$Dispatches[$i]
        foreach ($field in @('restore','status_cleared','subscribed_before_trigger','common_handoff','cleanup')) {
            if ($d.$field -isnot [bool]) { throw 'NR_QA_COLOR_FENCE' }
        }
        if ($d.serial -ne ($i+1) -or $d.width -ne 16 -or $d.height -ne 8 -or !$d.status_cleared -or !$d.subscribed_before_trigger -or $d.fence_signaled -ne 1 -or $d.fence_completed -ne 1 -or !$d.common_handoff -or !$d.cleanup) { throw 'NR_QA_COLOR_FENCE' }
        $ids=@($d.source_resource,$d.canonical_resource,$d.output_resource,$d.status_resource)
        if (@($ids | Select-Object -Unique).Count -ne 4 -or @($ids | Where-Object { !$_ -or [UInt64]$_ -eq 0 }).Count -or !$d.heap -or [UInt64]$d.heap -eq 0) { throw 'NR_QA_COLOR_ALIAS' }
        if (!$d.raw_expected_hash -or !$d.raw_readback_hash -or $d.raw_expected_hash -ne $d.raw_readback_hash -or !$d.output_hash -or [UInt64]$d.raw_expected_hash -eq 0 -or [UInt64]$d.output_hash -eq 0) { throw 'NR_QA_COLOR_SOURCE_IDENTITY' }
    }
    for ($i=0; $i -lt 15; $i++) {
        $row=$Rows[$i]
        $color=if($i -lt 6){1}elseif($i -lt 9){2}elseif($i -lt 12){3}else{4}
        $format=if($i -lt 3){2}elseif($i -lt 6){1}elseif($i -lt 12){3}else{4}
        $white=if($color -eq 1){1}else{3.80625}
        $scale=if($color -eq 1){1}elseif($color -eq 2){1.5}elseif($color -eq 3){3.80625}else{0.01875}
        foreach ($field in @('source_load','canonical_load','canonical_store','output_store','basic_double')) {
            if ($row.$field -isnot [bool]) { throw 'NR_QA_COLOR_METADATA_OR_CAPS' }
        }
        if ($row.normalized_codes -is [string] -or [double]::IsNaN($row.normalized_codes) -or [double]::IsInfinity($row.normalized_codes) -or $row.half_ulps -is [string] -or [double]::IsNaN($row.half_ulps) -or [double]::IsInfinity($row.half_ulps)) { throw 'NR_QA_COLOR_ORACLE' }
        if ($row.case -ne $i -or $row.color -ne $color -or $row.format -ne $format -or $row.alpha -ne (($i%3)+1) -or $row.width -ne 16 -or $row.height -ne 8 -or $row.source_white_nits -ne 203 -or $row.pre_exposure -ne 2 -or $row.exposure -ne 3 -or $row.white_point -ne $white -or $row.linear_scale -ne $scale -or !$row.source_load -or !$row.canonical_load -or !$row.canonical_store -or !$row.output_store -or !$row.basic_double) { throw 'NR_QA_COLOR_METADATA_OR_CAPS' }
        if ($row.compared_pixels -ne 256 -or $row.identity_pixels -ne 128 -or $row.alpha_errors -ne 0 -or $row.normalized_codes -lt 0 -or $row.normalized_codes -gt 1 -or $row.half_ulps -lt 0 -or $row.half_ulps -gt 2 -or $row.negative_checks -lt 0 -or ($color -in @(2,3) -and $row.negative_checks -eq 0)) { throw 'NR_QA_COLOR_ORACLE' }
        foreach ($field in @('raw_hash','raw_output_hash','identity_hash','prepared_hash','oracle_prepared_hash','restored_hash','oracle_restored_hash')) {
            if (!$row.$field -or [UInt64]$row.$field -eq 0) { throw 'NR_QA_COLOR_HASH' }
        }
        if ($row.identity_hash -ne $row.raw_output_hash) { throw 'NR_QA_COLOR_IDENTITY' }
        $dxgi=if($format -eq 1){87}elseif($format -eq 2){28}elseif($format -eq 3){10}else{24}
        for ($phase=0; $phase -lt 3; $phase++) {
            $d=$Dispatches[3*$i+$phase]
            $expectedOutput=if($phase -eq 0){10}elseif($format -eq 1){28}else{$dxgi}
            $serial=@($row.prepare_serial,$row.identity_serial,$row.restore_serial)[$phase]
            if ($serial -ne $d.serial -or $d.restore -ne ($phase -ne 0) -or $d.invalid_status -ne 0 -or $d.source_format -ne $dxgi -or $d.output_format -ne $expectedOutput -or $d.raw_readback_hash -ne $row.raw_hash) { throw 'NR_QA_COLOR_PHASE' }
            $hash=@($row.prepared_hash,$row.identity_hash,$row.restored_hash)[$phase]
            if ($d.output_hash -ne $hash) { throw 'NR_QA_COLOR_PHASE' }
        }
    }
    $names=@('nonfinite-input','nonfinite-resolve','canonical-overflow','restore-overflow','malformed-premultiplication')
    $status=@(1,8,4,16,2)
    for ($i=0; $i -lt 5; $i++) {
        $c=$Controls[$i];$d=$Dispatches[45+$i]
        if ($c.rejected -isnot [bool] -or $c.published -isnot [bool]) { throw 'NR_QA_COLOR_INVALID_STATUS' }
        if ($c.name -ne $names[$i] -or $c.stage -ne 'completed_gpu' -or !$c.rejected -or $c.published -or $c.serial -ne $d.serial -or $c.invalid_status -ne $status[$i] -or $d.invalid_status -ne $status[$i]) { throw 'NR_QA_COLOR_INVALID_STATUS' }
    }
    for ($i=5; $i -lt 8; $i++) {
        $c=$Controls[$i];$name=@('unknown-primaries','unavailable-format-capability','unavailable-output-format')[$i-5]
        if ($c.rejected -isnot [bool] -or $c.submitted -isnot [bool]) { throw 'NR_QA_COLOR_ADMISSION_CONTROL' }
        $stage=if($i -eq 7){'record'}else{'plan'}
        if ($c.name -ne $name -or $c.stage -ne $stage -or !$c.rejected -or $c.submitted) { throw 'NR_QA_COLOR_ADMISSION_CONTROL' }
    }
}
function Assert-FlowRows($Rows, $Caps) {
    if ($Rows.Count -ne 96 -or $Caps.Count -ne 2) { throw 'NR_QA_FLOW_ROWS' }
    foreach ($cap in $Caps) {
        $grids = @($cap.supported_grids)
        if (!$grids.Count -or @($grids | Where-Object { $_ -notin @(1,2,4) }).Count -or $cap.grid -ne ($grids | Measure-Object -Minimum).Minimum -or !$cap.signed_nvidia_system32 -or $cap.api_max -lt 80 -or $cap.cost_is_probability -or $cap.cost_dxgi_format -notin @(62,42) -or 87 -notin $cap.input_formats -or 38 -notin $cap.output_formats -or $cap.cost_dxgi_format -notin $cap.cost_formats -or $cap.module_signer -ne 'NVIDIA Corporation') { throw 'NR_QA_FLOW_CAPABILITY' }
    }
    foreach ($width in @(1920,2560)) {
        $height = if ($width -eq 1920) { 1080 } else { 1440 }
        $group = @($Rows | Where-Object { $_.width -eq $width })
        if ($group.Count -ne 48) { throw 'NR_QA_FLOW_EXTENT_SET' }
        for ($i=0; $i -lt $group.Count; $i+=2) {
            $cold=$group[$i]; $flow=$group[$i+1]
            if (!$cold.reset -or $flow.reset -or $cold.sequence -ne ($i+1) -or $flow.sequence -ne ($i+2) -or $cold.history_epoch -ne ($i/2+1) -or $flow.history_epoch -ne $cold.history_epoch) { throw 'NR_QA_FLOW_CHRONOLOGY' }
            $sub = $i -ge 24
            foreach ($row in @($cold,$flow)) {
                $instance=if($width -eq 1920){81}else{82}
                if ($row.instance_id -ne $instance -or $row.content_epoch -ne $row.history_epoch -or $row.resource_generation -ne 1 -or $row.submission_serial -ne $row.sequence) { throw 'NR_QA_FLOW_CHRONOLOGY' }
                $x=if($sub){16}else{0}; $y=if($sub){24}else{0}
                $w=if($sub){$width-32}else{$width}; $h=if($sub){$height-48}else{$height}
                if ($row.height -ne $height -or $row.active_x -ne $x -or $row.active_y -ne $y -or $row.active_width -ne $w -or $row.active_height -ne $h -or $row.errors -ne 0 -or $row.fixed_vector_bound_pixels -ne 1 -or $row.max_error_pixels -lt 0 -or $row.max_error_pixels -gt 1 -or !$row.motion_hash) { throw 'NR_QA_FLOW_PIXELS' }
            }
            $local = [int](($i % 24)/2)
            $amount = @(1,8,32)[[int][Math]::Floor($local/4)]
            $sign = if (($local % 4) -lt 2) { -1 } else { 1 }
            $dx = if (($local % 2) -eq 0) { $amount*$sign } else { 0 }
            $dy = if (($local % 2) -eq 1) { $amount*$sign } else { 0 }
            if ($cold.dx -ne 0 -or $cold.dy -ne 0 -or $cold.scored_pixels -ne 0 -or $flow.dx -ne $dx -or $flow.dy -ne $dy -or $flow.expected_mv_x -ne (-$dx) -or $flow.expected_mv_y -ne (-$dy) -or $flow.scored_pixels -ne (($flow.active_width-128)*($flow.active_height-128))) { throw 'NR_QA_FLOW_DIRECTION_OR_UNITS' }
        }
    }
}
function Assert-FlowPasses($Passes, $Inputs, $Rows, $Caps) {
    $ordinary=@($Rows | Where-Object { !$_.reset })
    if ($Passes.Count -ne 48 -or $Inputs.Count -ne 48 -or $ordinary.Count -ne 48) { throw 'NR_QA_FLOW_PASS_SET' }
    for($i=0;$i -lt 48;$i++) {
        $p=$Passes[$i]; $input=$Inputs[$i]; $row=$ordinary[$i]
        foreach($field in @('instance_id','history_epoch','content_epoch','resource_generation','submission_serial')) {
            if($p.$field -ne $row.$field -or $input.$field -ne $row.$field) { throw 'NR_QA_FLOW_PASS_KEY' }
        }
        if($p.sequence -ne $row.sequence -or $input.current_sequence -ne $row.sequence -or $input.previous_sequence -ne ($row.sequence-1)) { throw 'NR_QA_FLOW_PASS_KEY' }
        $cap=$Caps[$(if($row.width -eq 1920){0}else{1})]
        $hintGrids=@($cap.hint_grids)
        $admitted=@($hintGrids | Where-Object { $_ -ge $cap.grid })
        if(($cap.hint_support -join ',') -ne '1' -or !$admitted.Count -or @($hintGrids | Where-Object { $_ -notin @(1,2,4,8) }).Count -or 38 -notin $cap.hint_formats -or 38 -notin $cap.global_formats -or $p.hint_grid -ne ($admitted | Measure-Object -Minimum).Minimum -or $p.output_grid -ne $cap.grid) { throw 'NR_QA_FLOW_HINT_CAPABILITY' }
        if($p.native_passes -ne 2 -or $p.predictor_source -ne 'native_global' -or $p.hint_published_as_final_estimate -or $p.hint_errors -ne 0 -or $p.global_width -ne 1 -or $p.global_height -ne 1 -or !$p.bootstrap_hash -or !$p.hint_hash -or !$row.hardware_hash -or $row.hardware_hash -eq '0') { throw 'NR_QA_FLOW_ESTIMATE_IDENTITY' }
        $ids=@('current_resource','reference_resource','bootstrap_resource','global_resource','hint_resource','local_resource','bootstrap_current_resource','bootstrap_reference_resource') | ForEach-Object { $p.$_ }
        if(@($ids | Select-Object -Unique).Count -ne 8 -or @($ids | Where-Object { !$_ -or [UInt64]$_ -eq 0 }).Count -or $p.registered_handles.Count -ne 10 -or @($p.registered_handles | Select-Object -Unique).Count -ne 10 -or @($p.registered_handles | Where-Object { !$_ -or [UInt64]$_ -eq 0 }).Count) { throw 'NR_QA_FLOW_SESSION_ALIAS' }
        if($input.width -ne $row.width -or $input.height -ne $row.height -or $input.format -ne 87 -or $input.compared_pixels -ne (4*$row.width*$row.height) -or $input.gpu_analysis_errors -ne 0 -or $input.input_hashes.Count -ne 4 -or $input.input_hashes[0] -ne $input.input_hashes[2] -or $input.input_hashes[1] -ne $input.input_hashes[3]) { throw 'NR_QA_FLOW_INPUT_IDENTITY' }
        if($p.input_admitted -le 0 -or $p.predictor_worker -le $p.input_admitted -or $p.conversion_worker -le $p.predictor_worker -or $p.bootstrap_ofa -le 0 -or $p.refinement_ofa -le $p.bootstrap_ofa -or $p.ofa_completed -ne $p.refinement_ofa -or $p.worker_completed -ne $p.conversion_worker -or $p.consumer_completed -ne $p.consumer -or $p.current_producer -lt $p.previous_producer -or $p.previous_producer -le 0 -or $p.consumer -le $p.current_producer -or $p.ofa_completed -eq [UInt64]::MaxValue -or $p.worker_completed -eq [UInt64]::MaxValue -or $p.consumer_completed -eq [UInt64]::MaxValue) { throw 'NR_QA_FLOW_PASS_FENCE' }
        $fences=@($p.producer_fence,$p.ofa_fence,$p.worker_fence,$p.consumer_fence)
        if(@($fences | Where-Object { !$_ -or [UInt64]$_ -eq 0 }).Count -or $p.producer_fence -ne $p.consumer_fence -or @($fences | Select-Object -Unique).Count -ne 3) { throw 'NR_QA_FLOW_PASS_FENCE' }
    }
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
    if ($Scenario -eq 'flow-translation') {
        Copy-Item (Join-Path (Split-Path $build) 'nr_guides.cso') (Join-Path $root 'bin/64bit/nr_guides.cso')
    }
    if ($Scenario -eq 'color-alpha') {
        $shaderSource=Join-Path (Split-Path $build) 'nr_color.dxil'
        $shaderDest=Join-Path $root 'bin/64bit/nr_color.dxil'
        $before=FileIdentity $shaderSource
        $sources.Add($before)
        Copy-Item $shaderSource $shaderDest
        $staged=FileIdentity $shaderDest
        if ($staged.sha256 -ne $before.sha256 -or $staged.bytes -ne $before.bytes) { throw 'NR_QA_COLOR_SHADER_IDENTITY' }
        $script:binding += $staged
    }
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
