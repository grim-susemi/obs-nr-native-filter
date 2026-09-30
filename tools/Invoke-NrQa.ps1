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
$owned = [IO.Path]::GetFullPath((Join-Path $work 'out/task-2/host'))
$root = [IO.Path]::GetFullPath($HostRoot).TrimEnd('\','/')
$evidenceRoot = [IO.Path]::GetFullPath((Join-Path $work '.omo/evidence/obs-nr-native-filter-20260930/task-2'))
if ([IO.Path]::GetFullPath($Evidence).TrimEnd('\','/') -ne $evidenceRoot) { throw 'NR_QA_EVIDENCE_REFUSED' }
if ($Scenario -notin @('host-foundation','host-isolation-refusal')) { throw 'NR_QA_NOT_IMPLEMENTED' }
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
$sourceAfter = @()
$command = $null
$exit = $null
$stdout = $null
$stderr = $null
$output = ''
$exitSignal = $null
New-Item -ItemType Directory -Force $Evidence | Out-Null
# Teardown registered before the host, links, named events or child exist.
@{state='registered';root=$root;policy='Signal own stop; wait bounded; terminate exact own identity only; remove minimal private host copies; never alter source bytes/ACL/attributes'} |
    ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $Evidence 'cleanup-registration.json')
try {
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
    $process.EnableRaisingEvents = $true
    $exitSignal = [NrProcessExit]::new($process)
    $command = $process.StartInfo.FileName + ' ' + $process.StartInfo.Arguments
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
    $events[1].Set() | Out-Null
    if (!$events[2].WaitOne(15000)) { throw 'NR_QA_ACTION_TIMEOUT' }
    $events[3].Set() | Out-Null
    if (!$events[4].WaitOne(15000)) { throw 'NR_QA_EXIT_TIMEOUT' }
    if (!$process.WaitForExit(15000)) { throw 'NR_QA_PROCESS_EXIT_TIMEOUT' }
    $exit = $process.ExitCode
    if ($exit) { throw ('NR_QA_HOST_FAILED: ' + $exit) }
    $output = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult()
    foreach ($token in @('NR_QA_READY libobs=32.2.2 module=obs-nr-filter encoder=obs_x264','NR_QA_ACTION content_id=2583','NR_QA_EXIT cleanup=complete result=0')) {
        if (!$output.Contains($token)) { throw ('NR_QA_EVENT_LITERAL_MISSING: ' + $token) }
    }
    $result = 'PASS'
} catch { $failure = $_.Exception.Message } finally {
    if ($process -and $identity -and !$process.HasExited) {
        if ((Identity $process | ConvertTo-Json -Compress) -ne ($identity | ConvertTo-Json -Compress)) { throw 'NR_QA_CLEANUP_IDENTITY_REFUSED' }
        if ($events.Count -eq 5) { $events[3].Set() | Out-Null }
        if (!$process.WaitForExit(15000)) { $process.Kill(); if (!$process.WaitForExit(15000)) { throw 'NR_QA_CLEANUP_BLOCKED' } }
    }
    foreach ($event in $events) { $event.Dispose() }
    if ($exitSignal) { $exitSignal.Dispose() }
    foreach ($source in $sources) {
        $after = FileIdentity $source.path
        $sourceAfter += $after
        if ($after.sha256 -ne $source.sha256 -or $after.file_id -ne $source.file_id -or $after.attributes -ne $source.attributes) { $result='FAIL';$failure='NR_QA_SOURCE_CHANGED' }
    }
    if ($created) { Remove-Item -LiteralPath $root -Recurse -Force }
    $cleanup = @{host_absent=!(Test-Path $root);child_exited=(!$process -or $process.HasExited);event_handles_closed=$true;linked_bytes_unchanged=($failure -ne 'NR_QA_SOURCE_CHANGED')}
    if ($process -and $process.HasExited -and $stdout) { $output = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult() }
    if ($process -and $process.HasExited) { $exit = $process.ExitCode }
    @{scenario=$Scenario;result=$result;failure=$failure;command=$command;exit=$exit;identity=$identity;events=@('ready','action','exit');output=$output;inventory=$inventory;sources_before=@($sources.ToArray());sources_after=$sourceAfter;staging='minimal-private-copy';runtime=@{path=$Runtime;sha256=$spec.runtime.sha256;loaded=$false};cleanup=$cleanup} |
        ConvertTo-Json -Depth 12 | Set-Content -Encoding UTF8 (Join-Path $Evidence 'host-foundation.json')
    if ($process) { $process.Dispose() }
}
if ($result -ne 'PASS') { throw $failure }
