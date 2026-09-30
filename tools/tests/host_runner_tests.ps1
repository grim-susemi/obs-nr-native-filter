param([Parameter(Mandatory)][string]$Work)
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
if (Test-Path $root) { throw 'QA root must be absent before CPU tests' }
Reject 'unknown-qa' $root $runtime 'ParameterArgumentValidationError'
Reject 'sampler' $root $runtime 'NR_QA_EVIDENCE_REFUSED'
Reject 'flow-translation' $root $runtime 'NR_QA_NOT_IMPLEMENTED'
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
    Reject $future $root $runtime 'NR_QA_NOT_IMPLEMENTED'
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
