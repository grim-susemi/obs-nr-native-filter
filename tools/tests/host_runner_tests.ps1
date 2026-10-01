param([Parameter(Mandatory)][string]$Work, [switch]$Task8Only)
$ErrorActionPreference = 'Stop'
$runner = Join-Path $Work 'tools/Invoke-NrQa.ps1'
$evidence = Join-Path $Work '.omo/evidence/obs-nr-native-filter-20260930/task-2'
$root = Join-Path $Work 'out/task-2/host'
$runtime = 'C:/Program Files/obs-studio/bin/64bit/nvngx_dlssnr.dll'
. (Join-Path $Work 'tools/host/HostSupport.ps1')
function Reject([string]$Scenario, [string]$Root, [string]$Runtime, [string]$Token, [string]$Extra = '') {
    $child = [Diagnostics.Process]::new()
    $child.StartInfo.FileName = "$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe"
    $child.StartInfo.Arguments = '-NoProfile -ExecutionPolicy Bypass -File "' + $runner + '" -Scenario "' + $Scenario + '" -Evidence "' + $evidence + '" -HostRoot "' + $Root + '" -Runtime "' + $Runtime + '"'
    $child.StartInfo.Arguments += ' ' + $Extra
    $child.StartInfo.UseShellExecute = $false
    $child.StartInfo.CreateNoWindow = $true
    $child.StartInfo.RedirectStandardOutput = $true
    $child.StartInfo.RedirectStandardError = $true
    $child.EnableRaisingEvents = $true
    $exitSignal = [NrProcessExit]::new($child)
    try {
        if (!$child.Start()) { throw 'host runner child start failed' }
        $stdout = $child.StandardOutput.ReadToEndAsync()
        $stderr = $child.StandardError.ReadToEndAsync()
        if (!$exitSignal.Exited.WaitOne(15000)) { $child.Kill(); if (!$exitSignal.Exited.WaitOne(15000) -or !$child.WaitForExit(15000)) { throw 'host runner child cleanup blocked' }; throw 'host runner child timed out' }
        if (!$child.WaitForExit(15000)) { throw 'host runner reaping failed' }
        $text = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult()
        if (!$child.ExitCode -or !$text.Contains($Token)) { throw ('Wrong refusal: ' + $text) }
        Write-Output ('PASS refusal ' + $Token)
    } finally { $exitSignal.Dispose(); $child.Dispose() }
}
if (!$Task8Only) {
if (Test-Path $root) { throw 'QA root must be absent before CPU tests' }
Reject 'unknown-qa' $root $runtime 'ParameterArgumentValidationError'
Reject 'sampler' $root $runtime 'NR_QA_EVIDENCE_REFUSED'
Reject 'flow-translation' $root $runtime 'NR_QA_EVIDENCE_REFUSED'
Reject 'host-foundation' $Work $runtime 'NR_QA_FOREIGN_ROOT'
Reject 'host-foundation' $root 'C:/not-selected/runtime.dll' 'NR_QA_RUNTIME_REFUSED'
Reject 'host-foundation' $root $runtime 'NR_QA_FOREIGN_PROCESS' ('-ForeignPid ' + $PID)
$savedEvidence = $evidence
$evidence = $Work
Reject 'host-foundation' $root $runtime 'NR_QA_EVIDENCE_REFUSED'
$evidence = $savedEvidence
$evidence = Join-Path $Work '.omo/evidence/obs-nr-native-filter-20260930/task-6'
$samplerRoot = Join-Path $Work 'out/task-6/host'
if (Test-Path $samplerRoot) { throw 'Sampler root must be absent before CPU tests' }
Reject 'sampler' $Work $runtime 'NR_QA_FOREIGN_ROOT'
Reject 'sampler' $samplerRoot 'C:/not-selected/runtime.dll' 'NR_QA_RUNTIME_REFUSED'
Reject 'sampler' $samplerRoot $runtime 'NR_QA_FOREIGN_PROCESS' ('-ForeignPid ' + $PID)
$producerEvidence = $evidence
$verifierEvidence = Join-Path $producerEvidence 'verifier'
$verifierRoot = Join-Path $Work 'out/task-6-verifier/host'
if (Test-Path $verifierRoot) { throw 'Verifier root must be absent before CPU tests' }
foreach ($scenario in @('sampler','host-foundation')) {
    $evidence = $verifierEvidence
    # Exact pairing must pass path admission and reach the next deterministic guard.
    Reject $scenario $verifierRoot 'C:/not-selected/runtime.dll' 'NR_QA_RUNTIME_REFUSED'
    Reject $scenario $verifierRoot $runtime 'NR_QA_FOREIGN_PROCESS' ('-ForeignPid ' + $PID)
    Reject $scenario $samplerRoot $runtime 'NR_QA_FOREIGN_ROOT'
    $evidence = $producerEvidence
    Reject $scenario $verifierRoot $runtime 'NR_QA_FOREIGN_ROOT'
}
foreach ($badEvidence in @(($verifierEvidence + '-near'), (Join-Path $verifierEvidence 'nested'), (Join-Path $verifierEvidence '../../foreign'))) {
    $evidence = $badEvidence
    Reject 'sampler' $verifierRoot $runtime 'NR_QA_EVIDENCE_REFUSED'
}
$evidence = $verifierEvidence
foreach ($badRoot in @(($verifierRoot + '-near'), (Join-Path $verifierRoot '../../foreign'), $Work)) {
    Reject 'sampler' $badRoot $runtime 'NR_QA_FOREIGN_ROOT'
}
$evidence = $savedEvidence
Reject 'host-foundation' $verifierRoot $runtime 'NR_QA_FOREIGN_ROOT'
$evidence = $savedEvidence
# Boundary checks run the actual admission function, not a replacement.
# Real loaded-library integration remains the separate host-foundation proof.
$task7Evidence = Join-Path $Work '.omo/evidence/obs-nr-native-filter-20260930/task-7'
$task7VerifierEvidence = Join-Path $task7Evidence 'verifier'
$task7Root = Join-Path $Work 'out/task-7/host'
$task7VerifierRoot = Join-Path $Work 'out/task-7-verifier/host'
if ((Test-Path $task7Root) -or (Test-Path $task7VerifierRoot)) { throw 'Task7 roots must be absent before CPU tests' }
foreach ($scenario in @('texture-roundtrip','host-foundation')) {
    foreach ($pair in @(@($task7Evidence,$task7Root,$task7VerifierRoot),@($task7VerifierEvidence,$task7VerifierRoot,$task7Root))) {
        $evidence = $pair[0]
        Reject $scenario $pair[1] 'C:/not-selected/runtime.dll' 'NR_QA_RUNTIME_REFUSED'
        Reject $scenario $pair[1] $runtime 'NR_QA_FOREIGN_PROCESS' ('-ForeignPid ' + $PID)
        Reject $scenario $pair[2] $runtime 'NR_QA_FOREIGN_ROOT'
    }
}
foreach ($badEvidence in @(($task7Evidence + '-near'), (Join-Path $task7Evidence 'nested'),($task7VerifierEvidence + '-near'),(Join-Path $task7VerifierEvidence 'nested'),(Join-Path $task7VerifierEvidence '../../foreign'))) {
    $evidence = $badEvidence
    Reject 'texture-roundtrip' $task7Root $runtime 'NR_QA_EVIDENCE_REFUSED'
}
$evidence = $task7VerifierEvidence
foreach ($badRoot in @(($task7VerifierRoot + '-near'),(Join-Path $task7VerifierRoot 'nested'),(Join-Path $task7VerifierRoot '../../foreign'),$Work)) {
    Reject 'texture-roundtrip' $badRoot $runtime 'NR_QA_FOREIGN_ROOT'
}
# Cleanup registered before this exact owned existing-root fixture; no host/device is started.
$evidence = $task7Evidence
try {
    New-Item -ItemType Directory -Path $task7Root | Out-Null
    Reject 'texture-roundtrip' $task7Root $runtime 'NR_QA_EXISTING_ROOT'
} finally { Remove-Item -LiteralPath $task7Root }
$evidence = $savedEvidence
foreach ($future in @('flow-translation','color-alpha','native-model','native-bootstrap','handled-faults','shutdown')) {
    $token = if ($future -eq 'flow-translation') { 'NR_QA_EVIDENCE_REFUSED' } else { 'NR_QA_NOT_IMPLEMENTED' }
    Reject $future $root $runtime $token
}
if ((Test-Path $task7Root) -or (Test-Path $task7VerifierRoot)) { throw 'Task7 refusal leaked a host' }
function RejectInventory([string]$Path, [string]$Token) {
    $rejected = $false
    try { Assert-CleanInventory @(@{path=$Path}) } catch {
        if ($_.Exception.Message -ne $Token) { throw }
        $rejected = $true
    }
    if (!$rejected) { throw ('Inventory admitted: ' + $Path) }
    Write-Output ('PASS refusal ' + $Token)
}
RejectInventory (Join-Path $root 'bin/64bit/dxgi.dll') 'NR_QA_PROXY_LOADED'
RejectInventory (Join-Path $root 'bin/64bit/nvngx_dlssnr.dll') 'NR_QA_INJECTION_LOADED'
RejectInventory (Join-Path $root 'bin/64bit/nr-qa-host.exe') 'NR_QA_MODULE_MISSING: obs.dll'
if (Test-Path $root) { throw 'Refusal created a host' }
if (Test-Path $samplerRoot) { throw 'Refusal created a sampler host' }
if (Test-Path $verifierRoot) { throw 'Refusal created a verifier host' }
}
$task8Evidence = Join-Path $Work '.omo/evidence/obs-nr-native-filter-20260930/task-8'
$task8VerifierEvidence = Join-Path $task8Evidence 'verifier'
$task8Root = Join-Path $Work 'out/task-8/host'
$task8VerifierRoot = Join-Path $Work 'out/task-8-verifier/host'
if ((Test-Path $task8Root) -or (Test-Path $task8VerifierRoot)) { throw 'Task8 roots must be absent before CPU tests' }
foreach ($scenario in @('flow-translation','host-foundation')) {
    foreach ($pair in @(@($task8Evidence,$task8Root,$task8VerifierRoot),@($task8VerifierEvidence,$task8VerifierRoot,$task8Root))) {
        $evidence=$pair[0]
        Reject $scenario $pair[1] 'C:/not-selected/runtime.dll' 'NR_QA_RUNTIME_REFUSED'
        Reject $scenario $pair[1] $runtime 'NR_QA_FOREIGN_PROCESS' ('-ForeignPid '+$PID)
        Reject $scenario $pair[2] $runtime 'NR_QA_FOREIGN_ROOT'
    }
}
foreach ($badEvidence in @(($task8Evidence+'-near'),(Join-Path $task8Evidence 'nested'),($task8VerifierEvidence+'-near'),(Join-Path $task8VerifierEvidence 'nested'),(Join-Path $task8VerifierEvidence '../../foreign'))) {
    $evidence=$badEvidence
    Reject 'flow-translation' $task8Root $runtime 'NR_QA_EVIDENCE_REFUSED'
}
$evidence=$task8VerifierEvidence
foreach ($badRoot in @(($task8VerifierRoot+'-near'),(Join-Path $task8VerifierRoot 'nested'),(Join-Path $task8VerifierRoot '../../foreign'),$Work)) { Reject 'flow-translation' $badRoot $runtime 'NR_QA_FOREIGN_ROOT' }
$evidence=$task8Evidence
# Own empty existing-root fixture registered before creation, with exact cleanup.
try {
    New-Item -ItemType Directory -Path $task8Root | Out-Null
    Reject 'flow-translation' $task8Root $runtime 'NR_QA_EXISTING_ROOT'
} finally { Remove-Item -LiteralPath $task8Root }
$evidence=Join-Path $Work '.omo/evidence/obs-nr-native-filter-20260930/task-2'
foreach ($future in @('color-alpha','native-model','native-bootstrap','handled-faults','shutdown')) { Reject $future $root $runtime 'NR_QA_NOT_IMPLEMENTED' }
if ((Test-Path $task8Root) -or (Test-Path $task8VerifierRoot)) { throw 'Task8 refusal leaked a host' }
# Machine-field parser controls exercise the SAME public-run validator, not a prose/snapshot test.
$rows=@()
foreach ($width in @(1920,2560)) {
    $height=if($width -eq 1920){1080}else{1440}; $sequence=0; $epoch=0
    foreach($sub in @($false,$true)) { foreach($amount in @(1,8,32)) { foreach($sign in @(-1,1)) { foreach($axis in @(0,1)) {
        $epoch++
        foreach($reset in @($true,$false)) {
            $sequence++; $w=if($sub){$width-32}else{$width}; $h=if($sub){$height-48}else{$height}
            $dx=if(!$reset -and $axis -eq 0){$amount*$sign}else{0}; $dy=if(!$reset -and $axis -eq 1){$amount*$sign}else{0}
            $rows += [pscustomobject]@{width=$width;height=$height;sequence=$sequence;history_epoch=$epoch;content_epoch=$epoch;instance_id=$(if($width -eq 1920){81}else{82});resource_generation=1;submission_serial=$sequence;reset=$reset;active_x=$(if($sub){16}else{0});active_y=$(if($sub){24}else{0});active_width=$w;active_height=$h;errors=0;fixed_vector_bound_pixels=1;max_error_pixels=0;motion_hash='18446744073709551615';dx=$dx;dy=$dy;expected_mv_x=(-$dx);expected_mv_y=(-$dy);scored_pixels=$(if($reset){0}else{($w-128)*($h-128)})}
        }
    } } } }
}
$cap=[pscustomobject]@{supported_grids=@(4,1,2);grid=1;signed_nvidia_system32=$true;api_max=80;cost_is_probability=$false;cost_dxgi_format=62;input_formats=@(87);output_formats=@(38);cost_formats=@(62,42);module_signer='NVIDIA Corporation'}
$caps=@($cap,$cap)
Assert-FlowRows $rows $caps
Write-Output 'PASS flow parser valid machine fields'
function RejectFlow([string]$Token) {
    $rejected=$false
    try { Assert-FlowRows $rows $caps } catch { if($_.Exception.Message -ne $Token){throw}; $rejected=$true }
    if(!$rejected){throw 'flow parser admitted invalid machine fields'}
    Write-Output ('PASS flow parser refusal '+$Token)
}
$cap.supported_grids=@(); RejectFlow 'NR_QA_FLOW_CAPABILITY'; $cap.supported_grids=@(4,1,2)
$cap.grid=4; RejectFlow 'NR_QA_FLOW_CAPABILITY'; $cap.grid=1
$cap.cost_is_probability=$true; RejectFlow 'NR_QA_FLOW_CAPABILITY'; $cap.cost_is_probability=$false
$cap.cost_dxgi_format=41; RejectFlow 'NR_QA_FLOW_CAPABILITY'; $cap.cost_dxgi_format=62
$rows[1].errors=1; RejectFlow 'NR_QA_FLOW_PIXELS'; $rows[1].errors=0
$rows[1].max_error_pixels=1.01; RejectFlow 'NR_QA_FLOW_PIXELS'; $rows[1].max_error_pixels=0
$rows[1].expected_mv_x=-$rows[1].expected_mv_x; RejectFlow 'NR_QA_FLOW_DIRECTION_OR_UNITS'; $rows[1].expected_mv_x=-$rows[1].dx
$rows[0].reset=$false; RejectFlow 'NR_QA_FLOW_CHRONOLOGY'; $rows[0].reset=$true
$rows[1].history_epoch=0; RejectFlow 'NR_QA_FLOW_CHRONOLOGY'; $rows[1].history_epoch=1
$rows[48].instance_id=81; RejectFlow 'NR_QA_FLOW_CHRONOLOGY'; $rows[48].instance_id=82
# New native-pass parser controls are machine consumed, not prompt/prose assertions.
$cap | Add-Member -NotePropertyName hint_support -NotePropertyValue @(1)
$cap | Add-Member -NotePropertyName hint_grids -NotePropertyValue @(1,2,4,8)
$cap | Add-Member -NotePropertyName hint_formats -NotePropertyValue @(38)
$cap | Add-Member -NotePropertyName global_formats -NotePropertyValue @(38)
$passes=@(); $inputs=@()
foreach($row in @($rows | Where-Object { !$_.reset })) {
    $row | Add-Member -NotePropertyName hardware_hash -NotePropertyValue '123456'
    $passes += [pscustomobject]@{sequence=$row.sequence;instance_id=$row.instance_id;history_epoch=$row.history_epoch;content_epoch=$row.content_epoch;resource_generation=$row.resource_generation;submission_serial=$row.submission_serial;native_passes=2;predictor_source='native_global';hint_published_as_final_estimate=$false;hint_errors=0;global_width=1;global_height=1;hint_grid=1;output_grid=1;bootstrap_hash='23456';hint_hash='34567';current_resource='1';reference_resource='2';bootstrap_resource='3';global_resource='4';hint_resource='5';local_resource='6';bootstrap_current_resource='7';bootstrap_reference_resource='8';registered_handles=@('1','2','3','4','5','6','7','8','9','10');input_admitted=3;bootstrap_ofa=6;predictor_worker=4;refinement_ofa=12;conversion_worker=5;consumer=3;ofa_completed=12;worker_completed=5;consumer_completed=3;current_producer=2;previous_producer=1;producer_fence='101';ofa_fence='102';worker_fence='103';consumer_fence='101'}
    $inputs += [pscustomobject]@{current_sequence=$row.sequence;previous_sequence=($row.sequence-1);instance_id=$row.instance_id;history_epoch=$row.history_epoch;content_epoch=$row.content_epoch;resource_generation=$row.resource_generation;submission_serial=$row.submission_serial;width=$row.width;height=$row.height;format=87;compared_pixels=(4*$row.width*$row.height);gpu_analysis_errors=0;input_hashes=@('11','12','11','12')}
}
Assert-FlowPasses $passes $inputs $rows $caps
Write-Output 'PASS native pass parser valid machine fields'
function RejectPass([string]$Token) {
    $rejected=$false
    try { Assert-FlowPasses $passes $inputs $rows $caps } catch { if($_.Exception.Message -ne $Token){throw}; $rejected=$true }
    if(!$rejected){throw 'native pass parser admitted invalid machine fields'}
    Write-Output ('PASS native pass parser refusal '+$Token)
}
$passes[0].resource_generation=2; RejectPass 'NR_QA_FLOW_PASS_KEY'; $passes[0].resource_generation=1
$passes[0].hint_grid=4; RejectPass 'NR_QA_FLOW_HINT_CAPABILITY'; $passes[0].hint_grid=1
$passes[0].hint_published_as_final_estimate=$true; RejectPass 'NR_QA_FLOW_ESTIMATE_IDENTITY'; $passes[0].hint_published_as_final_estimate=$false
$passes[0].hint_errors=1; RejectPass 'NR_QA_FLOW_ESTIMATE_IDENTITY'; $passes[0].hint_errors=0
$passes[0].bootstrap_current_resource='1'; RejectPass 'NR_QA_FLOW_SESSION_ALIAS'; $passes[0].bootstrap_current_resource='7'
$passes[0].registered_handles[0]='2'; RejectPass 'NR_QA_FLOW_SESSION_ALIAS'; $passes[0].registered_handles[0]='1'
$inputs[0].compared_pixels/=2; RejectPass 'NR_QA_FLOW_INPUT_IDENTITY'; $inputs[0].compared_pixels*=2
$inputs[0].input_hashes[2]='13'; RejectPass 'NR_QA_FLOW_INPUT_IDENTITY'; $inputs[0].input_hashes[2]='11'
$passes[0].worker_completed=4; RejectPass 'NR_QA_FLOW_PASS_FENCE'; $passes[0].worker_completed=5
$passes[0].ofa_completed=[UInt64]::MaxValue; RejectPass 'NR_QA_FLOW_PASS_FENCE'; $passes[0].ofa_completed=12
$passes[0].worker_fence='102'; RejectPass 'NR_QA_FLOW_PASS_FENCE'; $passes[0].worker_fence='103'
