param([Parameter(Mandatory)][string]$Work)
$ErrorActionPreference = 'Stop'
$runner = Join-Path $Work 'tools/Invoke-NrQa.ps1'
$evidence = Join-Path $Work '.omo/evidence/obs-nr-native-filter-20260930/task-2'
$root = Join-Path $Work 'out/task-2/host'
$runtime = 'C:/Program Files/obs-studio/bin/64bit/nvngx_dlssnr.dll'
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
    try {
        if (!$child.Start()) { throw 'host runner child start failed' }
        $stdout = $child.StandardOutput.ReadToEndAsync()
        $stderr = $child.StandardError.ReadToEndAsync()
        if (!$child.WaitForExit(15000)) { $child.Kill(); if (!$child.WaitForExit(15000)) { throw 'host runner child cleanup blocked' }; throw 'host runner child timed out' }
        $text = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult()
        if (!$child.ExitCode -or !$text.Contains($Token)) { throw ('Wrong refusal: ' + $text) }
        Write-Output ('PASS refusal ' + $Token)
    } finally { $child.Dispose() }
}
if (Test-Path $root) { throw 'QA root must be absent before CPU tests' }
Reject 'unknown-qa' $root $runtime 'ParameterArgumentValidationError'
Reject 'sampler' $root $runtime 'NR_QA_NOT_IMPLEMENTED'
Reject 'host-foundation' $Work $runtime 'NR_QA_FOREIGN_ROOT'
Reject 'host-foundation' $root 'C:/not-selected/runtime.dll' 'NR_QA_RUNTIME_REFUSED'
Reject 'host-foundation' $root $runtime 'NR_QA_FOREIGN_PROCESS' ('-ForeignPid ' + $PID)
$savedEvidence = $evidence
$evidence = $Work
Reject 'host-foundation' $root $runtime 'NR_QA_EVIDENCE_REFUSED'
$evidence = $savedEvidence
# Boundary checks run the actual admission function, not a replacement.
# Real loaded-library integration remains the separate host-foundation proof.
. (Join-Path $Work 'tools/host/HostSupport.ps1')
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
