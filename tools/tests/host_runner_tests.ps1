param([Parameter(Mandatory)][string]$Work, [switch]$Task8Only, [switch]$Task9Only, [switch]$Task14AttemptOnly)
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
if($Task14AttemptOnly) {
    $baseEvidence=Join-Path $Work '.omo/evidence/obs-nr-native-filter-20260930/task-14'
    $evidence=Join-Path $baseEvidence 'attempts/native03'
    $root=Join-Path $Work 'out/task-14-native03/host'
    if(Test-Path -LiteralPath $root){throw 'Fresh Task14 attempt CPU root required'}
    Reject 'two-instances' $root 'C:/not-selected/runtime.dll' 'NR_QA_RUNTIME_REFUSED' '-Task14Attempt native03'
    Reject 'two-instances' (Join-Path $Work 'out/task-14/host') $runtime 'NR_QA_FOREIGN_ROOT' '-Task14Attempt native03'
    Reject 'two-instances' ($root+'-near') $runtime 'NR_QA_FOREIGN_ROOT' '-Task14Attempt native03'
    Reject 'two-instances' $root $runtime 'ParameterArgumentValidationError' '-Task14Attempt ../foreign'
    Reject 'host-foundation' $root $runtime 'NR_QA_INSTANCE_ATTEMPT_SCOPE' '-Task14Attempt native03'
    foreach($badEvidence in @($baseEvidence,(Join-Path $baseEvidence 'verifier'),($evidence+'-near'),(Join-Path $evidence 'nested'))) {
        $evidence=$badEvidence
        Reject 'two-instances' $root $runtime 'NR_QA_EVIDENCE_REFUSED' '-Task14Attempt native03'
    }
    $evidence=Join-Path $baseEvidence 'attempts/native03'
    Reject 'two-instances' $root $runtime 'NR_QA_EVIDENCE_REFUSED'
    if((Test-Path -LiteralPath $root) -or (Test-Path -LiteralPath $evidence)){throw 'Task14 attempt refusal created a native path'}
    Write-Output 'TASK14_ATTEMPT_CPU_PASS cases=10 native=false'
    return
}
if (!$Task8Only -and !$Task9Only) {
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
    $token = if ($future -in @('flow-translation','color-alpha','native-model','native-bootstrap')) { 'NR_QA_EVIDENCE_REFUSED' } else { 'NR_FAULT_PARENT_ADMISSION_REQUIRED' }
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
if (!$Task9Only) {
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
foreach ($future in @('color-alpha','native-model','native-bootstrap','handled-faults','shutdown')) {
    $token=if($future -in @('color-alpha','native-model','native-bootstrap')){'NR_QA_EVIDENCE_REFUSED'}else{'NR_FAULT_PARENT_ADMISSION_REQUIRED'}
    Reject $future $root $runtime $token
}
if ((Test-Path $task8Root) -or (Test-Path $task8VerifierRoot)) { throw 'Task8 refusal leaked a host' }
# Machine-field parser controls exercise the SAME public-run validator, not a prose/snapshot test.
$rows=@()
foreach ($spec in @(@(1920,81),@(2560,82),@(2560,83))) {
    $width=$spec[0]; $instance=$spec[1]
    $height=if($width -eq 1920){1080}else{1440}; $sequence=0; $epoch=0
    foreach($sub in @($false,$true)) { foreach($amount in @(1,8,32)) { foreach($sign in @(-1,1)) { foreach($axis in @(0,1)) {
        $epoch++
        foreach($reset in @($true,$false)) {
            $sequence++; $w=if($sub){$width-32}else{$width}; $h=if($sub){$height-48}else{$height}
            $dx=if(!$reset -and $axis -eq 0){$amount*$sign}else{0}; $dy=if(!$reset -and $axis -eq 1){$amount*$sign}else{0}
            $rows += [pscustomobject]@{width=$width;height=$height;sequence=$sequence;history_epoch=$epoch;content_epoch=$epoch;instance_id=$instance;resource_generation=1;submission_serial=$sequence;reset=$reset;active_x=$(if($sub){16}else{0});active_y=$(if($sub){24}else{0});active_width=$w;active_height=$h;errors=0;fixed_vector_bound_pixels=1;max_error_pixels=0;motion_hash='18446744073709551615';dx=$dx;dy=$dy;expected_mv_x=(-$dx);expected_mv_y=(-$dy);scored_pixels=$(if($reset){0}else{($w-128)*($h-128)})}
        }
    } } } }
}
$cap=[pscustomobject]@{supported_grids=@(4,1,2);grid=1;frame_interval_ns=0;signed_nvidia_system32=$true;api_max=80;cost_is_probability=$false;cost_dxgi_format=62;input_formats=@(87);output_formats=@(38);cost_formats=@(62,42);module_signer='NVIDIA Corporation'}
$fast=[pscustomobject]@{supported_grids=@(4,1,2);grid=4;submitted_perf_level=20;frame_interval_ns=16666667;signed_nvidia_system32=$true;api_max=80;cost_is_probability=$false;cost_dxgi_format=62;input_formats=@(87);output_formats=@(38);cost_formats=@(62,42);module_signer='NVIDIA Corporation'}; $caps=@($cap,$cap,$fast)
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
$rows[96].instance_id=82; RejectFlow 'NR_QA_FLOW_CHRONOLOGY'; $rows[96].instance_id=83
$fast.grid=1; RejectFlow 'NR_QA_FLOW_CAPABILITY'; $fast.grid=4
$fast.submitted_perf_level=10; RejectFlow 'NR_QA_FLOW_CAPABILITY'; $fast.submitted_perf_level=20
$fast.frame_interval_ns=0; RejectFlow 'NR_QA_FLOW_CAPABILITY'; $fast.frame_interval_ns=16666667
# New native-pass parser controls are machine consumed, not prompt/prose assertions.
$cap | Add-Member -NotePropertyName hint_support -NotePropertyValue @(1)
$cap | Add-Member -NotePropertyName hint_grids -NotePropertyValue @(1,2,4,8)
$cap | Add-Member -NotePropertyName hint_formats -NotePropertyValue @(38)
$cap | Add-Member -NotePropertyName global_formats -NotePropertyValue @(38)
foreach($m in @('hint_support','hint_grids','hint_formats','global_formats')){ $fast | Add-Member -NotePropertyName $m -NotePropertyValue $cap.$m }
$passes=@(); $inputs=@()
foreach($row in @($rows | Where-Object { !$_.reset })) {
    $g=if($row.instance_id -eq 83){4}else{1}
    $row | Add-Member -NotePropertyName hardware_hash -NotePropertyValue '123456'
    $passes += [pscustomobject]@{sequence=$row.sequence;instance_id=$row.instance_id;history_epoch=$row.history_epoch;content_epoch=$row.content_epoch;resource_generation=$row.resource_generation;submission_serial=$row.submission_serial;native_passes=2;predictor_source='native_global';hint_published_as_final_estimate=$false;hint_errors=0;global_width=1;global_height=1;hint_grid=$g;output_grid=$g;bootstrap_hash='23456';hint_hash='34567';current_resource='1';reference_resource='2';bootstrap_resource='3';global_resource='4';hint_resource='5';local_resource='6';bootstrap_current_resource='7';bootstrap_reference_resource='8';registered_handles=@('1','2','3','4','5','6','7','8','9','10');input_admitted=3;bootstrap_ofa=6;predictor_worker=4;refinement_ofa=12;conversion_worker=5;consumer=3;ofa_completed=12;worker_completed=5;consumer_completed=3;current_producer=2;previous_producer=1;producer_fence='101';ofa_fence='102';worker_fence='103';consumer_fence='101'}
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
}
if (!$Task8Only) {
    $task9Evidence=Join-Path $Work '.omo/evidence/obs-nr-native-filter-20260930/task-9'
    $task9VerifierEvidence=Join-Path $task9Evidence 'verifier'
    $task9Root=Join-Path $Work 'out/task-9/host'
    $task9VerifierRoot=Join-Path $Work 'out/task-9-verifier/host'
    if ((Test-Path $task9Root) -or (Test-Path $task9VerifierRoot)) { throw 'Task9 roots must be absent before CPU tests' }
    function ProbeBuildSelection([string]$Evidence, [string]$Root, [string]$Build, [string]$Missing) {
        # Only unrelated-process presence and artifact availability are isolated.
        # Actual runner routing, guards and missing-artifact refusal execute;
        # no binary is renamed/copied and no host/GPU can start.
        $probe = @'
$ErrorActionPreference='Stop'
$global:selectedBuildPaths=[Collections.Generic.List[string]]::new()
function global:Get-Process { @() }
function global:Test-Path {
    param([string]$Path,[string]$LiteralPath,[Microsoft.PowerShell.Commands.TestPathType]$PathType)
    $candidate=if($LiteralPath){$LiteralPath}else{$Path}
    $name=[IO.Path]::GetFileName($candidate)
    if($name -in @('obs-nr-filter.dll','nr-qa-host.exe','nr_color.dxil')) {
        $global:selectedBuildPaths.Add([IO.Path]::GetFullPath($candidate))
        return $name -ne '@MISSING@'
    }
    Microsoft.PowerShell.Management\Test-Path @PSBoundParameters
}
try {
    & '@RUNNER@' -Scenario color-alpha -Evidence '@EVIDENCE@' -HostRoot '@ROOT@' -Runtime '@RUNTIME@'
    throw 'build probe unexpectedly reached host execution'
} catch {
    if($_.Exception.Message -ne 'NR_QA_BUILD_MISSING: @MISSING@'){throw}
    @{selected=@($global:selectedBuildPaths.ToArray());missing='@MISSING@';refused=$true;host_absent=!(Microsoft.PowerShell.Management\Test-Path '@ROOT@')} | ConvertTo-Json -Compress
}
'@
        foreach ($entry in @(@('@RUNNER@',$runner),@('@EVIDENCE@',$Evidence),@('@ROOT@',$Root),@('@RUNTIME@',$runtime),@('@MISSING@',$Missing))) {
            $probe=$probe.Replace($entry[0],$entry[1].Replace("'","''"))
        }
        $child=[Diagnostics.Process]::new()
        $child.StartInfo.FileName="$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe"
        $child.StartInfo.Arguments='-NoProfile -ExecutionPolicy Bypass -Command "'+$probe.Replace('"','\"')+'"'
        $child.StartInfo.UseShellExecute=$false
        $child.StartInfo.CreateNoWindow=$true
        $child.StartInfo.RedirectStandardOutput=$true
        $child.StartInfo.RedirectStandardError=$true
        $child.EnableRaisingEvents=$true
        $exitSignal=[NrProcessExit]::new($child)
        try {
            if(!$child.Start()){throw 'build probe child start failed'}
            $stdout=$child.StandardOutput.ReadToEndAsync();$stderr=$child.StandardError.ReadToEndAsync()
            if(!$exitSignal.Exited.WaitOne(15000)){
                $child.Kill()
                if(!$exitSignal.Exited.WaitOne(15000) -or !$child.WaitForExit(15000)){throw 'build probe cleanup blocked'}
                throw 'build probe timed out'
            }
            if(!$child.WaitForExit(15000)){throw 'build probe reaping failed'}
            $text=$stdout.GetAwaiter().GetResult();$errors=$stderr.GetAwaiter().GetResult()
            if($child.ExitCode){throw ('build probe failed: '+$text+$errors)}
            $row=$text | ConvertFrom-Json
            $expected=Join-Path $(if($Missing -eq 'nr_color.dxil'){Split-Path $Build}else{$Build}) $Missing
            if(!$row.refused -or !$row.host_absent -or $row.missing -ne $Missing -or !$row.selected.Count -or $row.selected[-1] -ne [IO.Path]::GetFullPath($expected)){throw 'public selected wrong build path'}
            foreach($selected in $row.selected) {
                $name=[IO.Path]::GetFileName($selected)
                $expected=Join-Path $(if($name -eq 'nr_color.dxil'){Split-Path $Build}else{$Build}) $name
                if($selected -ne [IO.Path]::GetFullPath($expected)){throw 'public artifact fallback selected'}
            }
            Write-Output ('PASS public build selection '+($row | ConvertTo-Json -Compress))
        } finally {$exitSignal.Dispose();$child.Dispose()}
    }
    foreach ($pair in @(@($task9Evidence,$task9Root,(Join-Path $Work 'out/task-9/native/Debug')),@($task9VerifierEvidence,$task9VerifierRoot,(Join-Path $Work 'out/task-9-verifier/native/Debug')))) {
        foreach ($missing in @('obs-nr-filter.dll','nr-qa-host.exe','nr_color.dxil')) { ProbeBuildSelection $pair[0] $pair[1] $pair[2] $missing }
    }
    foreach ($scenario in @('color-alpha','host-foundation')) {
        foreach ($pair in @(@($task9Evidence,$task9Root,$task9VerifierRoot),@($task9VerifierEvidence,$task9VerifierRoot,$task9Root))) {
            $evidence=$pair[0]
            Reject $scenario $pair[1] 'C:/not-selected/runtime.dll' 'NR_QA_RUNTIME_REFUSED'
            Reject $scenario $pair[1] $runtime 'NR_QA_FOREIGN_PROCESS' ('-ForeignPid '+$PID)
            Reject $scenario $pair[2] $runtime 'NR_QA_FOREIGN_ROOT'
        }
    }
    foreach ($badEvidence in @(($task9Evidence+'-near'),(Join-Path $task9Evidence 'nested'),($task9VerifierEvidence+'-near'),(Join-Path $task9VerifierEvidence 'nested'),(Join-Path $task9VerifierEvidence '../../foreign'))) {
        $evidence=$badEvidence;Reject 'color-alpha' $task9Root $runtime 'NR_QA_EVIDENCE_REFUSED'
    }
    $evidence=$task9VerifierEvidence
    foreach ($badRoot in @(($task9VerifierRoot+'-near'),(Join-Path $task9VerifierRoot 'nested'),(Join-Path $Work 'out/task-9/verifier/host'),(Join-Path $task9VerifierRoot '../../foreign'),$Work)) {
        Reject 'color-alpha' $badRoot $runtime 'NR_QA_FOREIGN_ROOT'
    }
    $evidence=$task9Evidence
    try {
        New-Item -ItemType Directory -Path $task9Root | Out-Null
        Reject 'color-alpha' $task9Root $runtime 'NR_QA_EXISTING_ROOT'
    } finally {Remove-Item -LiteralPath $task9Root}
    $evidence=Join-Path $Work '.omo/evidence/obs-nr-native-filter-20260930/task-2'
    Reject 'color-alpha' $root $runtime 'NR_QA_EVIDENCE_REFUSED'
    if ((Test-Path $task9Root) -or (Test-Path $task9VerifierRoot)) {throw 'Task9 refusal leaked a host'}
    $colorRows=@();$colorDispatches=@()
    $colorContext=@([pscustomobject]@{software=$false;private_direct_queue=$true;vendor_id=4318;adapter='RTX 4090';device='101';queue='102';renderer_luid_high=1;renderer_luid_low=2;worker_luid_high=1;worker_luid_low=2})
    for($i=0;$i -lt 15;$i++) {
        $color=if($i -lt 6){1}elseif($i -lt 9){2}elseif($i -lt 12){3}else{4}
        $format=if($i -lt 3){2}elseif($i -lt 6){1}elseif($i -lt 12){3}else{4}
        $dxgi=if($format -eq 1){87}elseif($format -eq 2){28}elseif($format -eq 3){10}else{24}
        $white=if($color -eq 1){1}else{3.80625}
        $scale=if($color -eq 1){1}elseif($color -eq 2){1.5}elseif($color -eq 3){3.80625}else{0.01875}
        $colorRows += [pscustomobject]@{case=$i;color=$color;format=$format;alpha=(($i%3)+1);width=16;height=8;source_white_nits=203;pre_exposure=2;exposure=3;white_point=$white;linear_scale=$scale;source_load=$true;canonical_load=$true;canonical_store=$true;output_store=$true;basic_double=$true;compared_pixels=256;identity_pixels=128;alpha_errors=0;normalized_codes=0;half_ulps=0;negative_checks=1;raw_hash='11';raw_output_hash='11';identity_hash='11';prepared_hash='22';oracle_prepared_hash='22';restored_hash='33';oracle_restored_hash='33';prepare_serial=(3*$i+1);identity_serial=(3*$i+2);restore_serial=(3*$i+3)}
        for($phase=0;$phase -lt 3;$phase++) {
            $colorDispatches += [pscustomobject]@{serial=(3*$i+$phase+1);width=16;height=8;restore=($phase -ne 0);source_format=$dxgi;output_format=$(if($phase -eq 0){10}elseif($format -eq 1){28}else{$dxgi});status_cleared=$true;subscribed_before_trigger=$true;fence_signaled=1;fence_completed=1;common_handoff=$true;cleanup=$true;source_resource='1';canonical_resource='2';output_resource='3';status_resource='4';heap='5';raw_expected_hash='11';raw_readback_hash='11';output_hash=@('22','11','33')[$phase];invalid_status=0}
        }
    }
    $colorControls=@();$controlNames=@('nonfinite-input','nonfinite-resolve','canonical-overflow','restore-overflow','malformed-premultiplication')
    $controlStatus=@(1,8,4,16,2)
    for($i=0;$i -lt 5;$i++) {
        $d=$colorDispatches[0] | ConvertTo-Json | ConvertFrom-Json
        $d.serial=46+$i;$d.invalid_status=$controlStatus[$i];$colorDispatches+=$d
        $colorControls += [pscustomobject]@{name=$controlNames[$i];stage='completed_gpu';serial=(46+$i);invalid_status=$controlStatus[$i];rejected=$true;published=$false}
    }
    foreach($name in @('unknown-primaries','unavailable-format-capability','unavailable-output-format')) {
        $colorControls += [pscustomobject]@{name=$name;stage=$(if($name -eq 'unavailable-output-format'){'record'}else{'plan'});rejected=$true;submitted=$false}
    }
    Assert-ColorRows $colorRows $colorDispatches $colorContext $colorControls
    Write-Output 'PASS color public parser valid machine fields'
    function RejectColor([string]$Token) {
        $refused=$false
        try {Assert-ColorRows $colorRows $colorDispatches $colorContext $colorControls} catch {if($_.Exception.Message -ne $Token){throw};$refused=$true}
        if(!$refused){throw 'color parser admitted invalid machine fields'}
        Write-Output ('PASS color parser refusal '+$Token)
    }
    $colorContext[0].software=$true;RejectColor 'NR_QA_COLOR_CONTEXT';$colorContext[0].software=$false
    $colorContext[0].worker_luid_low=3;RejectColor 'NR_QA_COLOR_CONTEXT';$colorContext[0].worker_luid_low=2
    $colorDispatches[0].fence_completed=0;RejectColor 'NR_QA_COLOR_FENCE';$colorDispatches[0].fence_completed=1
    $colorDispatches[0].status_cleared='false';RejectColor 'NR_QA_COLOR_FENCE';$colorDispatches[0].status_cleared=$true
    $colorDispatches[0].canonical_resource='1';RejectColor 'NR_QA_COLOR_ALIAS';$colorDispatches[0].canonical_resource='2'
    $colorDispatches[0].raw_readback_hash='12';RejectColor 'NR_QA_COLOR_SOURCE_IDENTITY';$colorDispatches[0].raw_readback_hash='11'
    $colorRows[0].source_load='false';RejectColor 'NR_QA_COLOR_METADATA_OR_CAPS';$colorRows[0].source_load=$true
    $colorRows[0].source_white_nits=80;RejectColor 'NR_QA_COLOR_METADATA_OR_CAPS';$colorRows[0].source_white_nits=203
    $colorRows[0].normalized_codes=2;RejectColor 'NR_QA_COLOR_ORACLE';$colorRows[0].normalized_codes=0
    $colorRows[0].normalized_codes=[double]::NaN;RejectColor 'NR_QA_COLOR_ORACLE';$colorRows[0].normalized_codes=0
    $colorRows[0].half_ulps=3;RejectColor 'NR_QA_COLOR_ORACLE';$colorRows[0].half_ulps=0
    $colorRows[0].alpha_errors=1;RejectColor 'NR_QA_COLOR_ORACLE';$colorRows[0].alpha_errors=0
    $colorRows[0].identity_hash='12';RejectColor 'NR_QA_COLOR_IDENTITY';$colorRows[0].identity_hash='11'
    $colorRows[0].prepare_serial=2;RejectColor 'NR_QA_COLOR_PHASE';$colorRows[0].prepare_serial=1
    $colorControls[0].invalid_status=0;RejectColor 'NR_QA_COLOR_INVALID_STATUS';$colorControls[0].invalid_status=1
    $colorControls[7].submitted=$true;RejectColor 'NR_QA_COLOR_ADMISSION_CONTROL';$colorControls[7].submitted=$false
    $savedColorRows=$colorRows;$colorRows=@($colorRows[0..13])
    RejectColor 'NR_QA_COLOR_ROW_SET';$colorRows=$savedColorRows
}
