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
$evidencePath = [IO.Path]::GetFullPath($Evidence).TrimEnd('\','/')
$task = if ($Scenario -eq 'sampler' -or ($Scenario -eq 'host-foundation' -and $evidencePath -in @($task6Evidence,$task6VerifierEvidence))) { 'task-6' } elseif ($Scenario -eq 'texture-roundtrip' -or ($Scenario -eq 'host-foundation' -and $evidencePath -in @($task7Evidence,$task7VerifierEvidence))) { 'task-7' } else { 'task-2' }
$owned = [IO.Path]::GetFullPath((Join-Path $work ('out/' + $task + '/host')))
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
if ($evidencePath -ne $evidenceRoot) { throw 'NR_QA_EVIDENCE_REFUSED' }
if ($Scenario -notin @('host-foundation','host-isolation-refusal','sampler','texture-roundtrip')) { throw 'NR_QA_NOT_IMPLEMENTED' }
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
$active = @(Get-Process | Where-Object { $_.ProcessName -match '^(obs64|nr-qa-host|CrimsonDesert|GTA5|GTA5_Enhanced|Cyberpunk2077|eldenring|BlackMythWukong|cs2|dota2|FortniteClient.*)$' })
if ($active.Count) { throw ('NR_QA_UNRELATED_PROCESS: ' + ($active.Id -join ',')) }
$installed = 'C:/Program Files/obs-studio'
$build = Join-Path $work 'out/build/windows-x64-debug/Debug'
foreach ($binary in @('obs-nr-filter.dll','nr-qa-host.exe')) {
    if (!(Test-Path (Join-Path $build $binary))) { throw ('NR_QA_BUILD_MISSING: ' + $binary) }
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
$views = @()
$patterns = @()
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
    New-Item -ItemType Directory $root | Out-Null
    $created = $true
    Stage-Host
    $nonce = [Guid]::NewGuid().ToString('N')
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
    $process.EnableRaisingEvents = $true
    $exitSignal = [NrProcessExit]::new($process)
    $command = $process.StartInfo.FileName + ' ' + $process.StartInfo.Arguments
    $arguments = $process.StartInfo.Arguments
    if (!$process.Start()) { throw 'NR_QA_START_FAILED' }
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $identity = Identity $process
    if ($identity.image -ne $process.StartInfo.FileName) { throw 'NR_QA_CHILD_IDENTITY_REFUSED' }
    $ready = [Threading.WaitHandle]::WaitAny([Threading.WaitHandle[]]@($events[0],$exitSignal.Exited),15000)
    if ($ready -eq 1) { throw ('NR_QA_EARLY_EXIT: ' + $process.ExitCode) }
    if ($ready -ne 0) { throw 'NR_QA_READY_TIMEOUT' }
    $inventory = @($process.Modules | ForEach-Object { @{path=$_.FileName;sha256=(Get-FileHash $_.FileName -Algorithm SHA256).Hash} })
    Assert-CleanInventory $inventory
    $readyInventory = $inventory
    $events[1].Set() | Out-Null
    $acted = [Threading.WaitHandle]::WaitAny([Threading.WaitHandle[]]@($events[2],$exitSignal.Exited),15000)
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
    $output = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult()
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
    $result = 'PASS'
} catch { $failure = $_.Exception.Message } finally {
    if ($process -and $identity -and !$process.HasExited) {
        if ((Identity $process | ConvertTo-Json -Compress) -ne ($identity | ConvertTo-Json -Compress)) { throw 'NR_QA_CLEANUP_IDENTITY_REFUSED' }
        if ($events.Count -eq 5) { $events[3].Set() | Out-Null }
        if (!$process.WaitForExit(15000)) {
            if ($Scenario -eq 'texture-roundtrip') { $physicalUnresolved=$true; $result='FAIL'; $failure='NR_QA_GPU_OWNERSHIP_RETAINED' }
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
    if ($process -and $process.HasExited) { $exit = $process.ExitCode }
    $receipt = if ($Scenario -eq 'sampler') { 'sampler.json' } elseif ($Scenario -eq 'texture-roundtrip') { 'interop.json' } else { 'host-foundation.json' }
    $scope = if ($Scenario -eq 'texture-roundtrip') { @{class='physical-D3D11-D3D12-transport';NR=$false;FG=$false;recording=$false;HDR=$false;HDR_transport=$true;performance=$false;physical_HDMI_PTS=$false} } else { @{class='native-pass-through-sampler';NR=$false;FG=$false;recording=$false;HDR=$false;performance=$false;physical_HDMI_PTS=$false;unsupported_renderer='actual signed libobs OpenGL bypass lane'} }
    $record = @{scenario=$Scenario;result=$result;failure=$failure;command=$command;args=$arguments;public_command=('powershell.exe -NoProfile -ExecutionPolicy Bypass -File "' + $PSCommandPath + '" -Scenario ' + $Scenario + ' -Evidence "' + $Evidence + '" -HostRoot "' + $HostRoot + '" -Runtime "' + $Runtime + '"');public_args=@('-NoProfile','-ExecutionPolicy','Bypass','-File',$PSCommandPath,'-Scenario',$Scenario,'-Evidence',$Evidence,'-HostRoot',$HostRoot,'-Runtime',$Runtime);exit=$exit;identity=$identity;events=@('ready','action','exit');output=$output;views=$views;patterns=$patterns;binding=$binding;inventory_ready=$readyInventory;inventory=$inventory;sources_before=@($sources.ToArray());sources_after=$sourceAfter;staging='minimal-private-copy';runtime=@{path=$Runtime;sha256=$spec.runtime.sha256;loaded=$false};scope=$scope;assertions=@{result_pass=($result -eq 'PASS');exact_pattern_rows=$patterns.Count};cleanup=$cleanup}
    $json = $record | ConvertTo-Json -Depth 12
    if ($Scenario -eq 'texture-roundtrip') { $json | Set-Content -Encoding UTF8 (Join-Path $Evidence ('attempt-' + [Guid]::NewGuid().ToString('N') + '-interop.json')) }
    $json | Set-Content -Encoding UTF8 (Join-Path $Evidence $receipt)
    if ($process -and !$physicalUnresolved) { $process.Dispose() }
}
if ($result -ne 'PASS') { throw $failure }
