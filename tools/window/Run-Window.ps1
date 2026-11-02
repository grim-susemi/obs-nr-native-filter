[CmdletBinding()]
param([Parameter(Mandatory)][string]$InstanceRoot,[Parameter(Mandatory)][string]$InputsSha256,
    [Parameter(Mandatory)]$RootAdmission)
$ErrorActionPreference='Stop';Set-StrictMode -Version Latest
if($MyInvocation.InvocationName -cne '.' -or
    !([Environment]::GetCommandLineArgs() -contains '-NoExit')){throw 'WINDOW_ORIGINAL_NOEXIT_PARENT_REQUIRED'}
if(Get-Variable WindowSession -Scope Script -ErrorAction SilentlyContinue){throw 'WINDOW_SESSION_REPLAY'}
$i=Assert-WindowInstance $InstanceRoot $InputsSha256
$inputPin=Get-WindowPin (Join-Path $InstanceRoot 'INPUTS.json')
$refsPin=@($i.generatorSources|Where-Object {(Split-Path -Leaf $_.path) -ceq 'R32-REFERENCES.json'})[0]
$refs=Read-WindowPin $refsPin
. Import-WindowR32 $refs
$approval=Assert-WindowBuild $i
$root=Read-WindowPin $RootAdmission
Assert-WindowRootAdmission $root $i $inputPin
$r32Source=Get-Content -Raw -LiteralPath (Join-Path $refs.root $refs.source.file)|ConvertFrom-Json
$r32Inputs=Read-WindowPin $r32Source.inputs
$prior=@{r4=(Assert-T16PriorR4Closure $r32Inputs)}
Assert-T16PreviousRecordingClosure $r32Inputs
$null=Assert-T23HistoricalDisposition $r32Inputs
$state=@{next=1;stopped=$false;previousClosed=$true;verdicts=(Read-WindowExternalVerdicts $i)}
if(Test-Path -LiteralPath $i.identity.stage){throw 'WINDOW_FRESH_STAGE_REQUIRED'}
foreach($s in $i.scenarios) {
    foreach($path in @($s.evidenceRoot,$s.artifactRoot)) {
        if(Test-Path -LiteralPath $path){throw 'WINDOW_FRESH_SCENARIO_OUTPUT_REQUIRED'}
    }
}
Assert-WindowOrdinaryPath $InstanceRoot;Assert-WindowOrdinaryPath $i.identity.stage
$before=Measure-WindowPreservation $i.params.preservation
$current=Read-WindowCurrent $i $refs @()
Write-WindowNew (Join-Path $InstanceRoot 'PRESERVATION-BEFORE.json') (ConvertTo-WindowText $before)
$admission=[ordered]@{schema='nr-window-session-admission-v1';inputs=$inputPin;rootAdmission=$RootAdmission;
    orderSha256=$i.orderSha256;parentPid=$PID;utc=[DateTime]::UtcNow.ToString('o');build=$approval;
    prior=$prior;current=$current;maximumScenarios=$i.scenarios.Count;
    automaticRetry=$false;automaticKill=$false;pidAdoption=$false;taskAcceptance=$false}
$admissionPath=Join-Path $InstanceRoot 'single-session-admission.json'
Write-WindowNew $admissionPath (ConvertTo-WindowText $admission)
$script:WindowSession=@{parentPid=$PID;inputs=$i;inputsPin=$inputPin;state=$state;
    admissionPin=(Get-WindowPin $admissionPath);active=$null;consumption=$null;consumptionRecord=$null;
    routeEntered=$false;originals=[Collections.Generic.List[object]]::new();ownedGui=@();references=$refs;
    preStartCensuses=[Collections.Generic.List[object]]::new();
    retainedOnFailure=$true}
$w=$script:WindowSession
$promoted=$false;$errorText=$null;$restore=$null;$preservationAfter=$null
try {
    $protocol=$i.params.wrapper
    Assert-WindowPin $protocol.before;Assert-WindowPin $protocol.after
    $live=Get-WindowPin $protocol.livePath
    if($live.sha256 -cne $protocol.before.sha256 -or $live.bytes -ne $protocol.before.bytes){
        throw 'WINDOW_WRAPPER_BEFORE_REQUIRED'
    }
    [IO.File]::WriteAllBytes($protocol.livePath,[IO.File]::ReadAllBytes($protocol.after.path))
    $promoted=$true
    $live=Get-WindowPin $protocol.livePath
    if($live.sha256 -cne $protocol.after.sha256 -or $live.bytes -ne $protocol.after.bytes){throw 'WINDOW_WRAPPER_AFTER_REQUIRED'}
    Assert-WindowPin $i.params.stageAdapter
    . $i.params.stageAdapter.path
    $stage=& $i.params.stageAdapter.function $i
    Assert-WindowStage $stage $i
    Assert-WindowOrdinaryPath $i.identity.stage
    foreach($pair in @(@($i.params.pair.host,'bin/64bit/nr-qa-host.exe'),
        @($i.params.pair.plugin,'obs-plugins/64bit/obs-nr-filter.dll'))) {
        $actual=Get-WindowPin (Join-Path $i.identity.stage $pair[1])
        if($actual.sha256 -cne $pair[0].sha256 -or $actual.bytes -ne $pair[0].bytes){throw 'WINDOW_STAGE_PAIR_CHANGED'}
    }
    Write-WindowNew (Join-Path $InstanceRoot 'STAGE.json') (ConvertTo-WindowText $stage)
    foreach($s in $i.scenarios) {
        if($state.stopped){break}
        if(Skip-WindowBlockedDependent $state $s) {
            Write-WindowNew (Join-Path $InstanceRoot ('SKIPPED-'+$s.ordinal.ToString('000')+'.json')) (
                ConvertTo-WindowText ([ordered]@{scenario=$s.scenario;machineResult='NOT_RUN';consumed=$false;
                    reason='prerequisite-machine-FAIL-or-NOT_RUN';orderSha256=$i.orderSha256}))
            continue
        }
        Assert-WindowNext $i $s $state $i.identity.stage
        # Re-read current prerequisites and all pinned executable inputs before consumption.
        $external=Read-WindowExternalVerdicts $i
        foreach($key in $external.Keys){$state.verdicts[$key]=$external[$key]}
        Assert-WindowNext $i $s $state $i.identity.stage
        $null=Assert-WindowBuild $i
        $fresh=Read-WindowCurrent $i $refs $w.ownedGui
        Write-WindowNew (Join-Path $InstanceRoot ('CURRENT-'+$s.ordinal.ToString('000')+'.json')) (ConvertTo-WindowText $fresh)
        $w.consumption=New-WindowConsumption $InstanceRoot $i $s $w.admissionPin.sha256
        $w.consumptionRecord=Read-WindowPin $w.consumption
        $w.active=$s;$w.routeEntered=$false;$w.originals=[Collections.Generic.List[object]]::new()
        $args=@{Scenario=$s.scenario;Evidence=$s.evidenceRoot;HostRoot=$i.identity.stage;Runtime=$i.params.runtime.path}
        $args[$s.publicCandidatePrefix+'CandidateManifest']=$i.identity.inputs
        $args[$s.publicCandidatePrefix+'CandidateSha256']=$inputPin.sha256
        $outcome=. $protocol.livePath @args
        if(!$w.routeEntered -or $outcome.scenario -cne $s.scenario -or $outcome.custody -cne 'TERMINAL' -or
            $outcome.machineResult -cnotin @('PASS','FAIL') -or
            @($outcome.originals).Count -ne $w.originals.Count){throw 'WINDOW_ROUTE_RESULT_UNBOUND'}
        for($n=0;$n -lt $w.originals.Count;$n++) {
            if(![object]::ReferenceEquals($outcome.originals[$n],$w.originals[$n])){throw 'WINDOW_UNCERTAIN_CUSTODY'}
        }
        # Explicit release of measured terminal originals, never a catch/finally disposal.
        $closed=@(Close-WindowOriginals @($w.originals.ToArray()))
        $machine=[ordered]@{schema='nr-window-machine-v1';scenario=$s.scenario;task=$s.task;
            inputs=$inputPin;consumption=$w.consumption;orderSha256=$i.orderSha256;
            machineResult=$outcome.machineResult;failure=$outcome.failure;accepted=$false;
            originals=$closed;originalsClosed=$true;validator=$s.validator;preStartCensuses=@($w.preStartCensuses.ToArray())}
        Write-WindowNew (Join-Path $InstanceRoot ('MACHINE-'+$s.ordinal.ToString('000')+'.json')) (ConvertTo-WindowText $machine)
        Set-WindowOutcome $state $s $outcome.machineResult $true
        $w.active=$null
    }
}catch {
    $errorText=$_.Exception.Message;$state.stopped=$true
    # Exact originals remain reachable in this -NoExit parent. No disposal, adoption or kill.
}finally {
    try {
        $preservationAfter=Measure-WindowPreservation $i.params.preservation
        Write-WindowNew (Join-Path $InstanceRoot 'PRESERVATION-AFTER.json') (ConvertTo-WindowText $preservationAfter)
    }catch {$errorText=([string]$errorText+'; '+$_.Exception.Message);$state.stopped=$true}
    if($promoted) {
        try {$restore=Restore-WindowWrapper $i.params.wrapper}
        catch {$errorText=([string]$errorText+'; '+$_.Exception.Message);$state.stopped=$true}
    }
    Write-WindowNew (Join-Path $InstanceRoot 'WINDOW-RETURNED.json') (ConvertTo-WindowText ([ordered]@{
        schema='nr-window-returned-v1';inputs=$inputPin;parentPid=$PID;normalParentExitPending=$true;
        stopped=$state.stopped;nextOrdinal=$state.next;error=$errorText;wrapperRestored=$restore;
        remainingScenariosUnconsumed=@($i.scenarios|Where-Object {$_.ordinal -ge $state.next}|ForEach-Object {$_.scenario});
        uncertainOriginalsRetained=($null -ne $w.active);taskAcceptance=$false}))
}
Write-Output 'WINDOW_RETURNED_ORIGINAL_PARENT_RETAINED'
