[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('host-foundation','host-isolation-refusal','sampler','texture-roundtrip','flow-translation','color-alpha','native-model','native-bootstrap','lifecycle','two-instances','property-roundtrip','recording-output','handled-faults','shutdown','qualification','gui-av')][string]$Scenario,
    [Parameter(Mandatory)][string]$Evidence,
    [Parameter(Mandatory)][string]$HostRoot,
    [Parameter(Mandatory)][string]$Runtime,
    [string]$ForeignRoot,
    [int]$ForeignPid = 0
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$work = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$task6Evidence = [IO.Path]::GetFullPath((Join-Path $work '.omo/evidence/obs-nr-native-filter-20260930/task-6'))
$task6VerifierEvidence = [IO.Path]::GetFullPath((Join-Path $task6Evidence 'verifier'))
$task7Evidence = [IO.Path]::GetFullPath((Join-Path $work '.omo/evidence/obs-nr-native-filter-20260930/task-7'))
$task7VerifierEvidence = [IO.Path]::GetFullPath((Join-Path $task7Evidence 'verifier'))
$task8Evidence = [IO.Path]::GetFullPath((Join-Path $work '.omo/evidence/obs-nr-native-filter-20260930/task-8'))
$task8VerifierEvidence = [IO.Path]::GetFullPath((Join-Path $task8Evidence 'verifier'))
$task9Evidence = [IO.Path]::GetFullPath((Join-Path $work '.omo/evidence/obs-nr-native-filter-20260930/task-9'))
$task9VerifierEvidence = [IO.Path]::GetFullPath((Join-Path $task9Evidence 'verifier'))
$task11Evidence = [IO.Path]::GetFullPath((Join-Path $work '.omo/evidence/obs-nr-native-filter-20260930/task-11'))
$task11VerifierEvidence = [IO.Path]::GetFullPath((Join-Path $task11Evidence 'verifier'))
$task12Evidence = [IO.Path]::GetFullPath((Join-Path $work '.omo/evidence/obs-nr-native-filter-20260930/task-12'))
$task12VerifierEvidence = [IO.Path]::GetFullPath((Join-Path $task12Evidence 'verifier'))
$evidencePath = [IO.Path]::GetFullPath($Evidence).TrimEnd('\','/')
$task = if ($Scenario -eq 'sampler' -or ($Scenario -eq 'host-foundation' -and $evidencePath -in @($task6Evidence,$task6VerifierEvidence))) { 'task-6' } elseif ($Scenario -eq 'texture-roundtrip' -or ($Scenario -eq 'host-foundation' -and $evidencePath -in @($task7Evidence,$task7VerifierEvidence))) { 'task-7' } else { 'task-2' }
$owned = [IO.Path]::GetFullPath((Join-Path $work ('out/' + $task + '/host')))
if ($Scenario -eq 'flow-translation' -or ($Scenario -eq 'host-foundation' -and $evidencePath -in @($task8Evidence,$task8VerifierEvidence))) {
    $task = 'task-8'
    $owned = [IO.Path]::GetFullPath((Join-Path $work 'out/task-8/host'))
}
if ($Scenario -eq 'color-alpha' -or ($Scenario -eq 'host-foundation' -and $evidencePath -in @($task9Evidence,$task9VerifierEvidence))) {
    $task = 'task-9'
    $owned = [IO.Path]::GetFullPath((Join-Path $work 'out/task-9/host'))
}
if ($Scenario -eq 'native-model' -or ($Scenario -eq 'host-foundation' -and $evidencePath -in @($task11Evidence,$task11VerifierEvidence))) {
    $task = 'task-11'
    $owned = [IO.Path]::GetFullPath((Join-Path $work 'out/task-11/host'))
}
if ($Scenario -eq 'native-bootstrap' -or ($Scenario -eq 'host-foundation' -and $evidencePath -in @($task12Evidence,$task12VerifierEvidence))) {
    $task = 'task-12'
    $owned = [IO.Path]::GetFullPath((Join-Path $work 'out/task-12/host'))
}
$root = [IO.Path]::GetFullPath($HostRoot).TrimEnd('\','/')
$evidenceRoot = [IO.Path]::GetFullPath((Join-Path $work ('.omo/evidence/obs-nr-native-filter-20260930/' + $task)))
if ($task -eq 'task-6' -and $evidencePath -eq $task6VerifierEvidence) {
    $evidenceRoot = $task6VerifierEvidence
    $owned = [IO.Path]::GetFullPath((Join-Path $work 'out/task-6-verifier/host'))
}
if ($task -eq 'task-7' -and $evidencePath -eq $task7VerifierEvidence) {
    $evidenceRoot = $task7VerifierEvidence
    $owned = [IO.Path]::GetFullPath((Join-Path $work 'out/task-7-verifier/host'))
}
if ($task -eq 'task-8' -and $evidencePath -eq $task8VerifierEvidence) {
    $evidenceRoot = $task8VerifierEvidence
    $owned = [IO.Path]::GetFullPath((Join-Path $work 'out/task-8-verifier/host'))
}
if ($task -eq 'task-9' -and $evidencePath -eq $task9VerifierEvidence) {
    $evidenceRoot = $task9VerifierEvidence
    $owned = [IO.Path]::GetFullPath((Join-Path $work 'out/task-9-verifier/host'))
}
if ($task -eq 'task-11' -and $evidencePath -eq $task11VerifierEvidence) {
    $evidenceRoot = $task11VerifierEvidence
    $owned = [IO.Path]::GetFullPath((Join-Path $work 'out/task-11-verifier/host'))
}
if ($task -eq 'task-12' -and $evidencePath -eq $task12VerifierEvidence) {
    $evidenceRoot = $task12VerifierEvidence
    $owned = [IO.Path]::GetFullPath((Join-Path $work 'out/task-12-verifier/host'))
}
if ($evidencePath -ne $evidenceRoot) { throw 'NR_QA_EVIDENCE_REFUSED' }
if ($Scenario -notin @('host-foundation','host-isolation-refusal','sampler','texture-roundtrip','flow-translation','color-alpha','native-model','native-bootstrap')) { throw 'NR_QA_NOT_IMPLEMENTED' }
. (Join-Path $PSScriptRoot 'host/HostSupport.ps1')
if ($Scenario -eq 'host-isolation-refusal') {
    if (!$ForeignRoot) { $ForeignRoot = $work }
    if (!$ForeignPid) { $ForeignPid = $PID }
    $before = Identity (Get-Process -Id $ForeignPid)
    $entriesBefore = @(Get-ChildItem -LiteralPath $ForeignRoot -Force | ForEach-Object { $_.FullName })
    $refused = $false
    try { Assert-OwnedRoot $ForeignRoot } catch {
        if ($_.Exception.Message -ne 'NR_QA_FOREIGN_ROOT') { throw }
        $refused = $true
    }
    if (!$refused) { throw 'NR_QA_EXPECTED_REFUSAL' }
    $after = Identity (Get-Process -Id $ForeignPid)
    $entriesAfter = @(Get-ChildItem -LiteralPath $ForeignRoot -Force | ForEach-Object { $_.FullName })
    if (($before | ConvertTo-Json -Compress) -ne ($after | ConvertTo-Json -Compress) -or (Compare-Object $entriesBefore $entriesAfter)) { throw 'NR_QA_FOREIGN_CHANGED' }
    New-Item -ItemType Directory -Force $Evidence | Out-Null
    @{scenario=$Scenario;result='PASS';refusal='NR_QA_FOREIGN_ROOT';foreign_before=$before;foreign_after=$after;foreign_entries_unchanged=$true;stop_calls=0;foreign_write_calls=0;cleanup=@{process_created=$false;host_created=$false}} |
        ConvertTo-Json -Depth 8 | Set-Content -Encoding UTF8 (Join-Path $Evidence 'host-isolation-refusal.json')
    return
}
Assert-OwnedRoot $root
if ($ForeignPid -or $ForeignRoot) { throw 'NR_QA_FOREIGN_PROCESS' }
$spec = Get-Content (Join-Path $work 'provenance/inputs.spec.json') -Raw | ConvertFrom-Json
if (!$spec.runtime.owner_selected) { throw 'NR_QA_RUNTIME_OWNER_REFUSED' }
if ([IO.Path]::GetFullPath($Runtime) -ne [IO.Path]::GetFullPath($spec.runtime.path)) { throw 'NR_QA_RUNTIME_REFUSED' }
if ((Get-FileHash -LiteralPath $Runtime -Algorithm SHA256).Hash.ToLowerInvariant() -ne $spec.runtime.sha256) { throw 'NR_QA_RUNTIME_CHANGED' }
Assert-NoUnrelatedProcess
$installed = 'C:/Program Files/obs-studio'
$build = Join-Path $work 'out/build/windows-x64-debug/Debug'
if ($task -eq 'task-8') { $build = Join-Path $work 'out/task-8/native/Debug' }
if ($task -eq 'task-9') {
    $build = Join-Path $work 'out/task-9/native/Debug'
    if ($evidencePath -eq $task9VerifierEvidence) { $build = Join-Path $work 'out/task-9-verifier/native/Debug' }
}
if ($task -eq 'task-11') {
    $build = Join-Path $work 'out/task-11/native/Debug'
    if ($evidencePath -eq $task11VerifierEvidence) { $build = Join-Path $work 'out/task-11-verifier/native/Debug' }
}
if ($task -eq 'task-12') {
    $build = Join-Path $work 'out/task-12/native/Debug'
    if ($evidencePath -eq $task12VerifierEvidence) { $build = Join-Path $work 'out/task-12-verifier/native/Debug' }
}
foreach ($binary in @('obs-nr-filter.dll','nr-qa-host.exe')) {
    if (!(Test-Path (Join-Path $build $binary))) { throw ('NR_QA_BUILD_MISSING: ' + $binary) }
}
if ($Scenario -eq 'color-alpha' -and !(Test-Path (Join-Path (Split-Path $build) 'nr_color.dxil'))) { throw 'NR_QA_BUILD_MISSING: nr_color.dxil' }
if ($Scenario -in @('native-model','native-bootstrap')) {
    $shaders=@('nr_codec.dxil','nr_color.dxil','nr_guides.cso','nr_interop_vertex.cso','nr_interop_pixel.cso')
    if($Scenario -eq 'native-bootstrap'){$shaders+='nr_obs_analysis.cso'}
    foreach ($shader in $shaders) {
        if (!(Test-Path (Join-Path (Split-Path $build) $shader))) { throw ('NR_QA_BUILD_MISSING: ' + $shader) }
    }
}
$sources = [Collections.Generic.List[object]]::new()
$process = $null
$identity = $null
$events = @()
$created = $false
$result = 'FAIL'
$failure = $null
$inventory = @()
$readyInventory = @()
$sourceAfter = @()
$command = $null
$arguments = $null
$exit = $null
$stdout = $null
$stderr = $null
$output = ''
$exitSignal = $null
$nativeOutput = $null
$nativeLog = $null
$views = @()
$patterns = @()
$flowRows = @()
$flowCaps = @()
$flowPasses = @()
$flowInputs = @()
$colorRows = @()
$colorDispatches = @()
$colorContext = @()
$colorControls = @()
$modelRows = @()
$modelContexts = @()
$modelCleanup = @()
$modelModules = @()
$bootstrapFrames=@();$bootstrapLanes=@();$bootstrapDecoded=@();$bootstrapCleanup=@()
$bootstrapSlots=@();$bootstrapSlotCleanup=@();$bootstrapArtifacts=@();$artifactRoot=$null
$physicalUnresolved = $false
$binding = @()
New-Item -ItemType Directory -Force $Evidence | Out-Null
# Teardown registered before the host, links, named events or child exist.
@{state='registered';root=$root;policy='Signal own stop; wait bounded; terminate exact own identity only; remove minimal private host copies; never alter source bytes/ACL/attributes'} |
    ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $Evidence 'cleanup-registration.json')
try {
    foreach ($path in @($spec.protected_paths)) { if (Test-Path -LiteralPath $path) { $sources.Add((FileIdentity $path)) } }
    foreach ($path in @('src/obs_filter.h','src/obs_filter.cpp','src/plugin-main.c','src/nr_contract.h','CMakeLists.txt','tools/Invoke-NrQa.ps1','tools/host/main.cpp','tools/host/HostSupport.ps1','tools/tests/sampler_contract_tests.cpp','tools/tests/host_runner_tests.ps1','provenance/inputs.spec.json','provenance/sdk-lock.json')) {
        if (Test-Path (Join-Path $work $path)) { $binding += FileIdentity (Join-Path $work $path) }
    }
    foreach ($binary in @('obs-nr-filter.dll','nr-qa-host.exe')) { $binding += FileIdentity (Join-Path $build $binary) }
    if ($Scenario -eq 'texture-roundtrip') {
        foreach ($path in @('src/interop.h','src/interop.cpp','src/shaders/nr_interop_blit.hlsl','cmake/nr/Interop.cmake','tools/host/nr_interop_scenario.h','tools/host/nr_interop_scenario.cpp','tools/tests/interop_contract_tests.cpp')) { $binding += FileIdentity (Join-Path $work $path) }
        foreach ($shader in @('nr_interop_vertex.cso','nr_interop_pixel.cso')) { $binding += FileIdentity (Join-Path (Split-Path $build) $shader) }
    }
    if ($Scenario -eq 'color-alpha') {
        foreach ($path in @('src/nr_color.h','src/nr_color.cpp','src/shaders/nr_color.hlsl','cmake/nr/Color.cmake','tools/host/nr_color_scenario.h','tools/host/nr_color_scenario.cpp','tools/tests/color_contract_tests.cpp','tools/tests/color_software_tests.cpp','src/interop.h','src/interop.cpp','cmake/nr/Interop.cmake')) { $binding += FileIdentity (Join-Path $work $path) }
        $binding += FileIdentity (Join-Path (Split-Path $build) 'nr_color.dxil')
    }
    if ($Scenario -in @('native-model','native-bootstrap')) {
        foreach ($path in @('src/nr_model.h','src/nr_model.cpp','src/nr_runtime.h','src/nr_runtime.cpp',
            'src/nr_core.h','src/nr_core.cpp','src/nr_guides.h','src/nr_guides.cpp','src/nr_color.h','src/nr_color.cpp',
            'src/frame_pipeline.h','src/frame_pipeline.cpp','src/interop.h','src/interop.cpp','src/shaders/nr_codec.hlsl',
            'src/shaders/nr_color.hlsl','src/shaders/nr_guides.hlsl','src/shaders/nr_interop_blit.hlsl',
            'cmake/nr/Model.cmake','cmake/nr/Scheduler.cmake','tools/host/nr_model_scenario.h','tools/host/nr_model_scenario.cpp',
            'tools/tests/model_contract_tests.cpp','tools/tests/model_runner_tests.ps1')) { $binding += FileIdentity (Join-Path $work $path) }
        $sources.Add((FileIdentity 'C:/Windows/System32/DriverStore/FileRepository/nv_dispi.inf_amd64_b20cc8aeaed64fc2/_nvngx.dll'))
    }
    if($Scenario -eq 'native-bootstrap') {
        foreach($path in @('src/obs_native_output.h','src/obs_native_output.cpp','src/shaders/nr_obs_analysis.hlsl',
            'tools/host/nr_slot_scenario.h','tools/host/nr_slot_scenario.cpp','tools/host/nr_bootstrap_scenario.h',
            'tools/host/nr_bootstrap_scenario.cpp','tools/host/nr_bootstrap_images.h','tools/host/nr_bootstrap_images.cpp',
            'tools/host/nr_recording_encode.h','tools/host/nr_recording_encode.cpp','tools/host/nr_recording_decode.h',
            'tools/host/nr_recording_decode.cpp','tools/tests/slot_reservation_tests.cpp','tools/tests/bootstrap_image_tests.cpp',
            'tools/tests/bootstrap_runner_tests.ps1')) { $binding+=FileIdentity (Join-Path $work $path) }
    }
    New-Item -ItemType Directory $root | Out-Null
    $created = $true
    if ($Scenario -eq 'flow-translation') {
        foreach ($path in @('src/nr_guides.h','src/nr_guides.cpp','src/shaders/nr_guides.hlsl','cmake/nr/Guides.cmake','tools/host/nr_flow_scenario.h','tools/host/nr_flow_scenario.cpp','tools/tests/flow_contract_tests.cpp','src/interop.h','cmake/nr/Interop.cmake','deps/nvofa/nvOpticalFlowCommon.h','deps/nvofa/nvOpticalFlowD3D12.h')) { $binding += FileIdentity (Join-Path $work $path) }
        $binding += FileIdentity (Join-Path (Split-Path $build) 'nr_guides.cso')
    }
    Stage-Host
    $nonce = [Guid]::NewGuid().ToString('N')
    if($Scenario -eq 'native-bootstrap') {
        $artifactRoot=Join-Path $Evidence ('bootstrap-artifacts/attempt-'+$nonce)
        New-Item -ItemType Directory -Path $artifactRoot | Out-Null
    }
    $names = @('ready','action','acted','stop','exit') | ForEach-Object { 'Local\nr-qa-' + $nonce + '-' + $_ }
    foreach ($name in $names) { $events += [Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,$name) }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo.FileName = Join-Path $root 'bin/64bit/nr-qa-host.exe'
    $process.StartInfo.WorkingDirectory = Join-Path $root 'bin/64bit'
    $process.StartInfo.UseShellExecute = $false
    $process.StartInfo.CreateNoWindow = $true
    $process.StartInfo.RedirectStandardOutput = $true
    $process.StartInfo.RedirectStandardError = $true

    $process.StartInfo.Arguments = '"' + $root + '" ' + (($names | ForEach-Object { '"' + $_ + '"' }) -join ' ')
    if ($Scenario -eq 'sampler') { $process.StartInfo.Arguments += ' sampler' }
    if ($Scenario -eq 'texture-roundtrip') { $process.StartInfo.Arguments += ' texture-roundtrip' }
    if ($Scenario -eq 'flow-translation') { $process.StartInfo.Arguments += ' flow-translation' }
    if ($Scenario -eq 'color-alpha') { $process.StartInfo.Arguments += ' color-alpha' }
    if ($Scenario -eq 'native-model') { $process.StartInfo.Arguments += ' native-model' }
    if ($Scenario -eq 'native-bootstrap') { $process.StartInfo.Arguments += ' native-bootstrap "'+$artifactRoot+'"' }
    $process.EnableRaisingEvents = $true
    $exitSignal = [NrProcessExit]::new($process)
    if ($Scenario -in @('native-model','native-bootstrap')) {
        $nativeLog=Join-Path $Evidence ('attempt-'+$nonce+'-native-host.log')
        $nativeOutput=[NrHostOutput]::new($process,$nativeLog)
    }
    $command = $process.StartInfo.FileName + ' ' + $process.StartInfo.Arguments
    $arguments = $process.StartInfo.Arguments
    Assert-NoUnrelatedProcess
    if ($Scenario -in @('native-model','native-bootstrap')) {
        @{state='registered';registered=[DateTime]::UtcNow.ToString('o');command=$process.StartInfo.FileName;
            args=$arguments;root=$root;raw_log=$nativeLog;binding=$binding;runtime=$spec.runtime;
            subscription_before_start=$true;identity=$null} | ConvertTo-Json -Depth 12 |
            Set-Content -Encoding UTF8 (Join-Path $Evidence ('attempt-'+$nonce+'-native-registered.json'))
    }
    if (!$process.Start()) { throw 'NR_QA_START_FAILED' }
    $identity = Identity $process
    if ($Scenario -in @('native-model','native-bootstrap')) {
        @{state='started';identity=$identity;command=$process.StartInfo.FileName;args=$arguments;
            raw_log=$nativeLog;registered_receipt=('attempt-'+$nonce+'-native-registered.json')} |
            ConvertTo-Json -Depth 8 | Set-Content -Encoding UTF8 (Join-Path $Evidence ('attempt-'+$nonce+'-native-start.json'))
    }
    if ($nativeOutput) {
        $process.BeginOutputReadLine();$process.BeginErrorReadLine()
    } else {
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
    }
    if ($identity.image -ne $process.StartInfo.FileName) { throw 'NR_QA_CHILD_IDENTITY_REFUSED' }
    $ready = [Threading.WaitHandle]::WaitAny([Threading.WaitHandle[]]@($events[0],$exitSignal.Exited),15000)
    if ($ready -eq 1) { throw ('NR_QA_EARLY_EXIT: ' + $process.ExitCode) }
    if ($ready -ne 0) { throw 'NR_QA_READY_TIMEOUT' }
    $inventory = @($process.Modules | ForEach-Object { @{path=$_.FileName;sha256=(Get-FileHash $_.FileName -Algorithm SHA256).Hash} })
    Assert-CleanInventory $inventory
    if($Scenario -eq 'native-bootstrap') {
        foreach($pair in @(@('nr-qa-host.exe','bin/64bit/nr-qa-host.exe'),@('obs-nr-filter.dll','obs-plugins/64bit/obs-nr-filter.dll'))) {
            $actual=@($inventory | Where-Object {$_.path -eq (Join-Path $root $pair[1])})
            if($actual.Count -ne 1 -or $actual[0].sha256 -ne (Get-FileHash (Join-Path $build $pair[0])).Hash) { throw 'NR_QA_BOOTSTRAP_FRESH_BINARY_BINDING' }
        }
        foreach($module in @('obs-nvenc.dll','obs-ffmpeg.dll')) {
            if(!@($inventory | Where-Object {[IO.Path]::GetFileName($_.path) -eq $module}).Count) { throw ('NR_QA_MODULE_MISSING: '+$module) }
        }
    }
    $readyInventory = $inventory
    $events[1].Set() | Out-Null
    $actionBound = if ($Scenario -eq 'native-bootstrap') { 300000 } elseif ($Scenario -in @('flow-translation','color-alpha','native-model')) { 120000 } else { 15000 }
    $acted = [Threading.WaitHandle]::WaitAny([Threading.WaitHandle[]]@($events[2],$exitSignal.Exited),$actionBound)
    if ($acted -eq 1) { throw ('NR_QA_ACTION_FAILED_EXIT: ' + $process.ExitCode) }
    if ($acted -ne 0) { throw 'NR_QA_ACTION_TIMEOUT' }
    # Process.Modules is cached by .NET; the sampler ends D3D11 before its OpenGL refusal lane.
    $process.Refresh()
    $inventory = @($process.Modules | ForEach-Object { @{path=$_.FileName;sha256=(Get-FileHash $_.FileName -Algorithm SHA256).Hash} })
    if ($Scenario -eq 'sampler') { Assert-CleanInventory $inventory 'libobs-opengl.dll' } else { Assert-CleanInventory $inventory }
    $events[3].Set() | Out-Null
    if (!$events[4].WaitOne(15000)) { throw 'NR_QA_EXIT_TIMEOUT' }
    if (!$process.WaitForExit(15000)) { throw 'NR_QA_PROCESS_EXIT_TIMEOUT' }
    $exit = $process.ExitCode
    if ($exit) { throw ('NR_QA_HOST_FAILED: ' + $exit) }
    if ($nativeOutput) {
        if (!$nativeOutput.OutputClosed.WaitOne(15000) -or !$nativeOutput.ErrorClosed.WaitOne(15000)) { throw 'NR_QA_MODEL_OUTPUT_DRAIN' }
        $output=$nativeOutput.Text
    } else { $output = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult() }
    foreach ($token in @('NR_QA_READY libobs=32.2.2 module=obs-nr-filter encoder=obs_x264','NR_QA_ACTION content_id=2583','NR_QA_EXIT cleanup=complete result=0')) {
        if (!$output.Contains($token)) { throw ('NR_QA_EVENT_LITERAL_MISSING: ' + $token) }
    }
    if ($Scenario -eq 'sampler') {
        foreach ($line in ($output -split '\r?\n')) {
            if ($line.StartsWith('NR_SAMPLER_VIEW ')) { $views += $line.Substring(16) | ConvertFrom-Json }
        }
        if ($views.Count -ne 43) { throw ('NR_QA_SAMPLER_VIEW_COUNT: ' + $views.Count) }
        foreach ($row in $views) {
            if ($row.pixel_errors -ne 0 -or !$row.state_restored -or $row.tick -ne ([Math]::Floor($row.clock / $row.interval) + 1)) { throw 'NR_QA_SAMPLER_PIXEL_OR_CLOCK' }
        }
        foreach ($step in 0..3) {
            $group = @($views | Where-Object { $_.step -eq $step })
            if ($group.Count -ne 6 -or @($group.view | Select-Object -Unique).Count -ne 3 -or @($group.tick | Select-Object -Unique).Count -ne 1 -or @($group.ingress | Select-Object -Unique).Count -ne 1) { throw 'NR_QA_SAMPLER_LATCH' }
            foreach ($row in $group) { if ($row.ingress -ne ($step + 1) -or $row.selected -ne $row.ingress -or $row.admission -ne 0 -or $row.source_render_count -ne ($step + 1)) { throw 'NR_QA_SAMPLER_KEY' } }
        }
        foreach ($step in 4..7) {
            $group = @($views | Where-Object { $_.step -eq $step })
            $expected = @(2,4,1,3)[$step - 4]
            $count = @(6,6,1,6)[$step - 4]
            if ($group.Count -ne $count -or @($group.tick | Select-Object -Unique).Count -ne 1 -or @($group.ingress | Select-Object -Unique).Count -ne 1) { throw 'NR_QA_SAMPLER_BYPASS_LATCH' }
            if (@($group | Where-Object { $_.admission -ne $expected -or $_.selected -ne 0 }).Count) { throw 'NR_QA_SAMPLER_BYPASS' }
        }
    }
    if ($Scenario -eq 'texture-roundtrip') {
        $prefix = 'NR_INTEROP_PATTERN '
        foreach ($line in ($output -split [char]10)) {
            if ($line.StartsWith($prefix)) { $patterns += $line.Substring($prefix.Length) | ConvertFrom-Json }
        }
        if ($patterns.Count -ne 4 -or (@($patterns.format | Sort-Object) -join ',') -ne 'BGRA8,RGB10A2,RGBA16F,RGBA8') { throw 'NR_QA_INTEROP_PATTERN_SET' }
        foreach ($row in $patterns) {
            if ($row.raw_errors -ne 0 -or $row.output_errors -ne 0 -or !$row.state_restored -or !$row.cleanup -or $row.progress_clock -le $row.capture_clock -or $row.raw_fnv1a64 -ne $row.expected_fnv1a64 -or $row.output_fnv1a64 -ne $row.expected_fnv1a64 -or $row.producer_fence_completed -ne 4 -or $row.worker_fence_completed -ne 3 -or $row.consumer_fence_completed -ne 6) { throw 'NR_QA_INTEROP_PATTERN_OR_FENCE' }
        }
        if (!$output.Contains('NR_INTEROP_SCOPE transport=physical same_adapter=true model=not_loaded recording=not_tested HDR=transport_only cleanup=complete') -or $output.Contains('NR_INTEROP_RETAINED')) { throw 'NR_QA_INTEROP_CLEANUP_INCOMPLETE' }
    }
    if ($Scenario -eq 'flow-translation') {
        foreach ($line in ($output -split [char]10)) {
            if ($line.StartsWith('NR_FLOW_PATTERN ')) { $flowRows += $line.Substring(16) | ConvertFrom-Json }
            if ($line.StartsWith('NR_FLOW_CAPABILITY ')) { $flowCaps += $line.Substring(19) | ConvertFrom-Json }
            if ($line.StartsWith('NR_FLOW_REESTIMATION ')) { $flowPasses += $line.Substring(21) | ConvertFrom-Json }
            if ($line.StartsWith('NR_FLOW_INPUT_PROOF ')) { $flowInputs += $line.Substring(20) | ConvertFrom-Json }
        }
        Assert-FlowRows $flowRows $flowCaps
        Assert-FlowPasses $flowPasses $flowInputs $flowRows $flowCaps
        foreach ($cap in $flowCaps) {
            $expectedModule=Join-Path $env:SystemRoot 'System32/nvofapi64.dll'
            if ([IO.Path]::GetFullPath($cap.module_path) -ne [IO.Path]::GetFullPath($expectedModule) -or (Get-FileHash -LiteralPath $expectedModule -Algorithm SHA256).Hash.ToLowerInvariant() -ne $cap.module_sha256) { throw 'NR_QA_FLOW_MODULE_IDENTITY' }
        }
        if (!$output.Contains('NR_FLOW_SCOPE physical_ofa=true model=not_loaded Feature18=not_admitted HDR=not_claimed recording=not_tested cleanup=complete') -or $output.Contains('NR_FLOW_RETAINED')) { throw 'NR_QA_FLOW_CLEANUP_INCOMPLETE' }
    }
    if ($Scenario -eq 'color-alpha') {
        foreach ($line in ($output -split [char]10)) {
            if ($line.StartsWith('NR_COLOR_CASE ')) { $colorRows += $line.Substring(14) | ConvertFrom-Json }
            if ($line.StartsWith('NR_COLOR_DISPATCH ')) { $colorDispatches += $line.Substring(18) | ConvertFrom-Json }
            if ($line.StartsWith('NR_COLOR_CONTEXT ')) { $colorContext += $line.Substring(17) | ConvertFrom-Json }
            if ($line.StartsWith('NR_COLOR_CONTROL ')) { $colorControls += $line.Substring(17) | ConvertFrom-Json }
        }
        Assert-ColorRows $colorRows $colorDispatches $colorContext $colorControls
        if (!$output.Contains('NR_COLOR_SCOPE physical_color=true model=not_loaded Feature18=not_admitted HDR=boundary_only recording=not_tested cleanup=complete') -or $output.Contains('NR_COLOR_RETAINED')) { throw 'NR_QA_COLOR_CLEANUP_INCOMPLETE' }
    }
    if ($Scenario -eq 'native-model') {
        foreach ($line in ($output -split [char]10)) {
            if ($line.StartsWith('NR_MODEL_CASE ')) { $modelRows += $line.Substring(14) | ConvertFrom-Json }
            if ($line.StartsWith('NR_MODEL_CONTEXT ')) { $modelContexts += $line.Substring(17) | ConvertFrom-Json }
            if ($line.StartsWith('NR_MODEL_CLEANUP ')) { $modelCleanup += $line.Substring(17) | ConvertFrom-Json }
            if ($line.StartsWith('NR_MODEL_MODULE ')) { $modelModules += $line.Substring(16) | ConvertFrom-Json }
        }
        Assert-ModelRows $modelRows $modelContexts $modelCleanup $modelModules
        foreach ($module in $modelModules) {
            if ([IO.Path]::GetFullPath($module.path) -ne [IO.Path]::GetFullPath($Runtime) -or
                $module.selected_sha256 -ne $spec.runtime.sha256 -or [UInt64]$module.base -eq 0) { throw 'NR_QA_MODEL_MODULE_IDENTITY' }
        }
        if (!$output.Contains('NR_MODEL_SCOPE Feature18=true integrated=true quality=false performance=false recording=false cleanup=complete') -or
            $output.Contains('NR_MODEL_RETAINED')) { throw 'NR_QA_MODEL_CLEANUP_INCOMPLETE' }
    }
    if($Scenario -eq 'native-bootstrap') {
        foreach($line in ($output -split [char]10)) {
            if($line.StartsWith('NR_BOOTSTRAP_FRAME ')){$bootstrapFrames+=$line.Substring(19)|ConvertFrom-Json}
            if($line.StartsWith('NR_BOOTSTRAP_LANE ')){$bootstrapLanes+=$line.Substring(18)|ConvertFrom-Json}
            if($line.StartsWith('NR_BOOTSTRAP_DECODED ')){$bootstrapDecoded+=$line.Substring(21)|ConvertFrom-Json}
            if($line.StartsWith('NR_BOOTSTRAP_CLEANUP ')){$bootstrapCleanup+=$line.Substring(21)|ConvertFrom-Json}
            if($line.StartsWith('NR_BOOTSTRAP_SLOT ')){$bootstrapSlots+=$line.Substring(18)|ConvertFrom-Json}
            if($line.StartsWith('NR_BOOTSTRAP_SLOT_CLEANUP ')){$bootstrapSlotCleanup+=$line.Substring(26)|ConvertFrom-Json}
        }
        Assert-BootstrapRows $bootstrapFrames $bootstrapLanes $bootstrapDecoded $bootstrapCleanup $bootstrapSlots $bootstrapSlotCleanup
        foreach($row in @($bootstrapFrames | Where-Object {$_.genuine_nr})) {
            if([IO.Path]::GetFullPath($row.module_path) -ne [IO.Path]::GetFullPath($Runtime)) { throw 'NR_QA_MODEL_MODULE_IDENTITY' }
        }
        foreach($lane in $bootstrapLanes) {
            $directory=Join-Path $artifactRoot ('lane-'+$lane.lane)
            if([IO.Path]::GetFullPath($lane.artifacts) -ne [IO.Path]::GetFullPath($directory)) { throw 'NR_QA_BOOTSTRAP_ARTIFACT_PATH' }
            foreach($name in @('source.bmp','filter.bmp','program.bmp','decoded.bmp','program.hevc')) {
                $path=Join-Path $directory $name
                if(!(Test-Path -LiteralPath $path) -or (Get-Item -LiteralPath $path).Length -le 0) { throw ('NR_QA_BOOTSTRAP_IMAGE_MISSING: '+$name) }
                $bootstrapArtifacts+=FileIdentity $path
            }
        }
        if(!$output.Contains('NR_BOOTSTRAP_SCOPE real_filter=true Feature18=true SDR=true HDR=true recording=true independently_decoded=true quality=false performance=false cleanup=complete') -or
            $output.Contains('NR_BOOTSTRAP_RETAINED') -or $output.Contains('NR_BOOTSTRAP_FAILURE')) { throw 'NR_QA_BOOTSTRAP_INCOMPLETE' }
    }
    $result = 'PASS'
} catch { $failure = $_.Exception.Message } finally {
    if ($process -and $identity -and !$process.HasExited) {
        if ((Identity $process | ConvertTo-Json -Compress) -ne ($identity | ConvertTo-Json -Compress)) { throw 'NR_QA_CLEANUP_IDENTITY_REFUSED' }
        if ($events.Count -eq 5) { $events[3].Set() | Out-Null }
        if (!$process.WaitForExit(15000)) {
            if ($Scenario -in @('texture-roundtrip','flow-translation','color-alpha','native-model','native-bootstrap')) { $physicalUnresolved=$true; $result='FAIL'; $failure='NR_QA_GPU_OWNERSHIP_RETAINED' }
            else { $process.Kill(); if (!$process.WaitForExit(15000)) { throw 'NR_QA_CLEANUP_BLOCKED' } }
        }
    }
    if (!$physicalUnresolved) {
        foreach ($event in $events) { $event.Dispose() }
        if ($exitSignal) { $exitSignal.Dispose() }
    }
    foreach ($source in $sources) {
        $after = FileIdentity $source.path
        $sourceAfter += $after
        if ($after.sha256 -ne $source.sha256 -or $after.file_id -ne $source.file_id -or $after.attributes -ne $source.attributes) { $result='FAIL';$failure='NR_QA_SOURCE_CHANGED' }
    }
    if ($created -and !$physicalUnresolved) { Remove-Item -LiteralPath $root -Recurse -Force }
    $cleanup = @{host_absent=!(Test-Path $root);child_exited=(!$process -or $process.HasExited);event_handles_closed=(!$physicalUnresolved);physical_ownership_retained=$physicalUnresolved;linked_bytes_unchanged=($failure -ne 'NR_QA_SOURCE_CHANGED')}
    if ($process -and $process.HasExited -and $stdout) { $output = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult() }
    if ($nativeOutput) { $output=$nativeOutput.Text }
    if ($process -and $process.HasExited) { $exit = $process.ExitCode }
    $receipt = if ($Scenario -eq 'sampler') { 'sampler.json' } elseif ($Scenario -eq 'texture-roundtrip') { 'interop.json' } else { 'host-foundation.json' }
    $scope = if ($Scenario -eq 'texture-roundtrip') { @{class='physical-D3D11-D3D12-transport';NR=$false;FG=$false;recording=$false;HDR=$false;HDR_transport=$true;performance=$false;physical_HDMI_PTS=$false} } else { @{class='native-pass-through-sampler';NR=$false;FG=$false;recording=$false;HDR=$false;performance=$false;physical_HDMI_PTS=$false;unsupported_renderer='actual signed libobs OpenGL bypass lane'} }
    $record = @{scenario=$Scenario;result=$result;failure=$failure;command=$command;args=$arguments;public_command=('powershell.exe -NoProfile -ExecutionPolicy Bypass -File "' + $PSCommandPath + '" -Scenario ' + $Scenario + ' -Evidence "' + $Evidence + '" -HostRoot "' + $HostRoot + '" -Runtime "' + $Runtime + '"');public_args=@('-NoProfile','-ExecutionPolicy','Bypass','-File',$PSCommandPath,'-Scenario',$Scenario,'-Evidence',$Evidence,'-HostRoot',$HostRoot,'-Runtime',$Runtime);exit=$exit;identity=$identity;events=@('ready','action','exit');output=$output;views=$views;patterns=$patterns;binding=$binding;inventory_ready=$readyInventory;inventory=$inventory;sources_before=@($sources.ToArray());sources_after=$sourceAfter;staging='minimal-private-copy';runtime=@{path=$Runtime;sha256=$spec.runtime.sha256;loaded=$false};scope=$scope;assertions=@{result_pass=($result -eq 'PASS');exact_pattern_rows=$patterns.Count};cleanup=$cleanup}
    $json = $record | ConvertTo-Json -Depth 12
    if ($Scenario -eq 'flow-translation') {
        $receipt='flow.json'
        $record.flow_rows=$flowRows; $record.capabilities=$flowCaps
        $record.native_passes=$flowPasses; $record.input_proofs=$flowInputs
        $record.scope=@{class='physical-NVOFA-D3D12-guides';NR=$false;Feature18=$false;recording=$false;HDR=$false;quality=$false;performance=$false}
        $record.assertions.exact_flow_rows=$flowRows.Count
        $record.assertions.exact_native_pass_rows=$flowPasses.Count
        $record.assertions.exact_input_proofs=$flowInputs.Count
        $json=$record | ConvertTo-Json -Depth 12
    }
    if ($Scenario -eq 'color-alpha') {
        $receipt='color-alpha.json'
        $record.color_rows=$colorRows; $record.color_dispatches=$colorDispatches
        $record.color_context=$colorContext; $record.color_controls=$colorControls
        $record.scope=@{class='physical-D3D12-color-boundary';NR=$false;Feature18=$false;recording=$false;HDR=$false;HDR_boundary=$true;quality=$false;performance=$false}
        $record.assertions.exact_color_rows=$colorRows.Count
        $record.assertions.exact_dispatch_rows=$colorDispatches.Count
        $json=$record | ConvertTo-Json -Depth 12
    }
    if ($Scenario -eq 'native-model') {
        $receipt='native-model.json'
        $record.model_rows=$modelRows; $record.model_contexts=$modelContexts
        $record.model_cleanup=$modelCleanup; $record.model_modules=$modelModules
        $record.runtime.loaded_during_inference=($modelModules.Count -gt 0)
        $record.runtime.loaded_after_cleanup=$false
        $record.native_raw_log=$nativeLog
        if ($nativeOutput -and !$physicalUnresolved) { $nativeOutput.Dispose() }
        if ($nativeLog -and (Test-Path $nativeLog)) { $record.native_raw_log_identity=FileIdentity $nativeLog }
        $record.scope=@{class='physical-joined-Feature18-processor';NR=$true;Feature18=$true;recording=$false;HDR=$false;quality=$false;performance=$false;fixture_fps=1}
        $record.assertions.exact_model_rows=$modelRows.Count
        $json=$record | ConvertTo-Json -Depth 12
    }
    if($Scenario -eq 'native-bootstrap') {
        $receipt='bootstrap.json'
        $record.bootstrap_frames=$bootstrapFrames;$record.bootstrap_lanes=$bootstrapLanes
        $record.bootstrap_decoded=$bootstrapDecoded;$record.bootstrap_cleanup=$bootstrapCleanup
        $record.bootstrap_slots=$bootstrapSlots;$record.bootstrap_slot_cleanup=$bootstrapSlotCleanup
        $record.bootstrap_artifacts=$bootstrapArtifacts;$record.artifact_root=$artifactRoot
        $record.runtime.loaded_during_inference=@($bootstrapFrames | Where-Object {
            $_ -and $_.PSObject.Properties['genuine_nr'] -and $_.genuine_nr
        }).Count -gt 0
        $record.runtime.loaded_after_cleanup=$(if($physicalUnresolved){$null}else{$false});$record.native_raw_log=$nativeLog
        if($nativeOutput -and !$physicalUnresolved){$nativeOutput.Dispose()}
        if($nativeLog -and !$physicalUnresolved -and (Test-Path $nativeLog)){$record.native_raw_log_identity=FileIdentity $nativeLog}
        $record.scope=@{class='physical-native-filter-program-HEVC-independent-decoder';NR=$true;Feature18=$true;
            recording=$true;SDR=$true;HDR=$true;quality=$false;performance=$false;fixture_fps=1;
            OBS_chrome='Owned headless libobs has no GUI chrome; source-scoped filter and unrelated program source are checked'}
        $json=$record|ConvertTo-Json -Depth 14
    }
    if ($Scenario -in @('texture-roundtrip','flow-translation','color-alpha','native-model','native-bootstrap')) { $json | Set-Content -Encoding UTF8 (Join-Path $Evidence ('attempt-' + [Guid]::NewGuid().ToString('N') + '-' + $receipt)) }
    $json | Set-Content -Encoding UTF8 (Join-Path $Evidence $receipt)
    if ($process -and !$physicalUnresolved) { $process.Dispose() }
}
if ($result -ne 'PASS') { throw $failure }
