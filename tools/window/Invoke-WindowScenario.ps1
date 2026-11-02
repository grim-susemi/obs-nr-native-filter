[CmdletBinding()]
param([Parameter(Mandatory)][string]$Scenario,[Parameter(Mandatory)][string]$Evidence,
    [Parameter(Mandatory)][string]$HostRoot,[Parameter(Mandatory)][string]$Runtime,
    [Parameter(Mandatory)][string]$CandidateManifest,[Parameter(Mandatory)][string]$CandidateSha256)
$ErrorActionPreference='Stop';Set-StrictMode -Version Latest
if($MyInvocation.InvocationName -cne '.' -or !$script:WindowSession -or
    $script:WindowSession.parentPid -ne $PID -or !$script:WindowSession.active -or
    $script:WindowSession.routeEntered){throw 'WINDOW_ORIGINAL_ACTIVE_SCENARIO_REQUIRED'}
$w=$script:WindowSession
$i=$w.inputs;$s=$w.active
Assert-WindowNext $i $s $w.state $HostRoot
if($Scenario -cne $s.scenario -or [IO.Path]::GetFullPath($Evidence) -ine [IO.Path]::GetFullPath($s.evidenceRoot) -or
    [IO.Path]::GetFullPath($Runtime) -ine [IO.Path]::GetFullPath($i.params.runtime.path) -or
    [IO.Path]::GetFullPath($CandidateManifest) -ine [IO.Path]::GetFullPath($i.identity.inputs) -or
    $CandidateSha256 -cne $w.inputsPin.sha256){throw 'WINDOW_ROUTE_IDENTITY_BINDING'}
Assert-WindowPin $w.consumption
if($w.consumptionRecord.scenario -cne $s.scenario -or
    $w.consumptionRecord.sessionAdmissionSha256 -cne $w.admissionPin.sha256){throw 'WINDOW_SCENARIO_CONSUMPTION_REQUIRED'}
$w.routeEntered=$true
Assert-WindowPin $s.runner;Assert-WindowPin $s.validator
. $s.runner.path
. $s.validator.path
# The approved runner starts every original through Start-WindowOriginal (publish, recheck, Start).
# It runs only this scenario, respects actionBoundMs and returns { output, failure }.
$raw=& $s.runner.function $w $s
if($w.originals.Count -eq 0){throw 'WINDOW_ORIGINALS_REQUIRED'}
$failure=$raw.failure
if(!$failure) {
    try {$checked=Invoke-WindowValidator $s $raw.output}
    catch {$failure=$_.Exception.Message}
}
Assert-WindowOriginals @($w.originals.ToArray())
foreach($o in $w.originals) {
    if($o.process.ExitCode -ne 0 -and !$failure){$failure='WINDOW_NONZERO_ORIGINAL_EXIT'}
}
$result=if($failure){'FAIL'}else{'PASS'}
[ordered]@{scenario=$s.scenario;machineResult=$result;failure=$failure;custody='TERMINAL';
    checked=if($failure){$null}else{$checked};originals=@($w.originals.ToArray())}
