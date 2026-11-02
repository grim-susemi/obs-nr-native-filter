# SPDX-License-Identifier: GPL-2.0-or-later
param([Parameter(Mandatory)][string]$Work,[string]$CandidateManifest)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$runner=Join-Path $Work 'tools/Invoke-NrQa.ps1'
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($runner,[ref]$tokens,[ref]$errors)
if($errors.Count){throw ($errors|Out-String)}
$support=Join-Path $Work 'tools/host/PropertySupport.ps1'
$null=[Management.Automation.Language.Parser]::ParseFile($support,[ref]$tokens,[ref]$errors)
if($errors.Count){throw ($errors|Out-String)}
. (Join-Path $Work 'tools/host/HostSupport.ps1')
. $support
foreach($name in @('Test-NrQaOriginalOwnerClosed','Start-NrInstanceOriginal')) {
    $definition=$ast.Find({param($n)$n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $name},$true)
    if(!$definition){throw ('MISSING_FUNCTION '+$name)}
    . ([scriptblock]::Create($definition.Extent.Text))
}
$count=0
function Refuse([scriptblock]$Action,[string]$Expected) {
    $caught=$null
    try {& $Action|Out-Null}catch{$caught=$_.FullyQualifiedErrorId+' '+$_.Exception.Message}
    if(!$caught -or !$caught.Contains($Expected)){throw "EXPECTED=$Expected ACTUAL=$caught"}
    $script:count++
}
$base=Join-Path $Work '.omo/evidence/obs-nr-native-filter-20260930/task-15'
$evidence=Join-Path $base 'attempts/native9999'
$root=Join-Path $Work 'out/task-15-native9999/host'
if((Test-Path $root) -or (Test-Path $evidence)){throw 'CPU_FRESH_PATH_REQUIRED'}
$routeArgs=@{Scenario='property-roundtrip';Evidence=$evidence;HostRoot=$root;Runtime='C:/not-selected/runtime.dll';
    Task15CandidateManifest=(Join-Path $base 'not-admitted.json');Task15CandidateSha256=('a'*64);
    Task15CandidateBuild=(Join-Path $Work 'out/task-15-control-event-repair/build02');Task15Attempt='native9999'}
$missing=$routeArgs.Clone();$missing.Remove('Task15CandidateManifest')
Refuse {& $runner @missing} 'NR_PROPERTY_CANDIDATE_REQUIRED'
$wrong=$routeArgs.Clone();$wrong.Evidence=$base
Refuse {& $runner @wrong} 'NR_QA_EVIDENCE_REFUSED'
$wrong=$routeArgs.Clone();$wrong.Evidence=$evidence+'-near'
Refuse {& $runner @wrong} 'NR_QA_EVIDENCE_REFUSED'
$wrong=$routeArgs.Clone();$wrong.HostRoot=Join-Path $Work 'out/task-2/host'
Refuse {& $runner @wrong} 'NR_QA_FOREIGN_ROOT'
$wrong=$routeArgs.Clone();$wrong.HostRoot=$root+'-near'
Refuse {& $runner @wrong} 'NR_QA_FOREIGN_ROOT'
Refuse {& $runner @routeArgs} 'NR_QA_RUNTIME_REFUSED'
$wrong=$routeArgs.Clone();$wrong.Scenario='sampler'
Refuse {& $runner @wrong} 'NR_PROPERTY_CANDIDATE_SCOPE'
$wrong=$routeArgs.Clone();$wrong.Task15Attempt='../foreign'
Refuse {& $runner @wrong} 'ParameterArgumentValidationError'
if((Test-Path $root) -or (Test-Path $evidence)){throw 'REFUSAL_CREATED_NATIVE_PATH'}

# Use the actual current source gate. Mutate parsed manifest fields only; all
# artifact/source hashes and the immutable on-disk manifest still use real I/O.
$candidatePath=if($CandidateManifest){$CandidateManifest}else{Join-Path $base 'property-control-event-repair-20261012/CandidateSourceManifest-event-repair.json'}
$candidateSha=(Get-FileHash -LiteralPath $candidatePath -Algorithm SHA256).Hash.ToLowerInvariant()
$candidateBuild=Join-Path $Work 'out/task-15-control-event-repair/build02'
$f=Assert-NrPropertyCandidate $candidatePath $candidateSha $candidateBuild $Work
if($f.artifacts.Count -ne 9){throw 'CANDIDATE_POSITIVE_FAILED'};$count++
Refuse {Assert-NrPropertyCandidate $candidatePath ('b'*64) $candidateBuild $Work} 'NR_PROPERTY_FREEZE_CHANGED'
Refuse {Assert-NrPropertyCandidate $candidatePath $candidateSha (Join-Path $Work 'out/foreign') $Work} 'NR_PROPERTY_CANDIDATE_PATH'
$candidateJson=Get-Content -LiteralPath $candidatePath -Raw
function Get-Content {
    param($LiteralPath,[switch]$Raw)
    if($LiteralPath -eq $candidatePath){return ($script:candidateFixture|ConvertTo-Json -Depth 32)}
    Microsoft.PowerShell.Management\Get-Content -LiteralPath $LiteralPath -Raw:$Raw
}
foreach($case in @('artifact','source','recipe','hash','plugin')) {
    $script:candidateFixture=$candidateJson|ConvertFrom-Json
    switch($case) {
        'artifact' {$script:candidateFixture.artifacts=@($script:candidateFixture.artifacts|Select-Object -Skip 1);$expected='NR_PROPERTY_ARTIFACT_SET'}
        'source' {$script:candidateFixture.sources=@($script:candidateFixture.sources|Where-Object {$_.path -notlike '*Invoke-NrQa.ps1'});$expected='NR_PROPERTY_SOURCE_SET'}
        'recipe' {$script:candidateFixture.reused_inputs=@($script:candidateFixture.reused_inputs|Select-Object -Skip 1);$expected='NR_PROPERTY_REUSED_INPUT_SET'}
        'hash' {$script:candidateFixture.sources[0].sha256=('b'*64);$expected='NR_PROPERTY_INPUT_CHANGED'}
        'plugin' {$script:candidateFixture.plugin.sha256=('b'*64);$expected='NR_PROPERTY_R15_PAIR'}
    }
    Refuse {Assert-NrPropertyCandidate $candidatePath $candidateSha $candidateBuild $Work} $expected
}
Remove-Item Function:Get-Content

# Exercise the actual constructor and caller disposal, including a graph whose
# construction failed before exit/output subscriptions. No host is started.
$disposalFailures=[Collections.Generic.List[string]]::new()
$task='task-15'
$global:NrQaPropertyOwners=[Collections.Generic.List[object]]::new()
$retainedEventsFunction=${function:New-RetainedHostEvents}
function New-RetainedHostEvents {throw 'PROPERTY_PARTIAL_CONSTRUCTION'}
Refuse {New-NrPropertyOwner restore $root $evidence} 'PROPERTY_PARTIAL_CONSTRUCTION'
Set-Item Function:New-RetainedHostEvents $retainedEventsFunction
$partial=$global:NrQaPropertyOwners[0]
if($partial.started -isnot [bool] -or $partial.started -or $partial.events.Count -ne 5){
    throw 'PARTIAL_ORIGINAL_NOT_PUBLISHED'
};$count++
$partial.started=$null
Refuse {Close-NrPropertyOriginal $partial} 'NR_PROPERTY_DISPOSE_UNRESOLVED'
if($partial.kernelWait.SafeWaitHandle.IsClosed){throw 'UNKNOWN_START_DISPOSED'};$count++
$partial.started=$false
try {
    Close-NrPropertyOriginal $partial
    if(!$partial.kernelWait.SafeWaitHandle.IsClosed -or
        $partial.state -cne 'original-objects-disposed-never-started' -or
        $partial.closed -or $partial.identity -or $partial.receipt){
        throw 'NEVER_STARTED_CLOSURE_FABRICATED_OR_INCOMPLETE'
    };$count++
}catch{$disposalFailures.Add('PARTIAL: '+$_.Exception.Message)}

# Real CPU process integration through the unchanged original start/reap gate:
# subscribe before Start, retain exact Process and SafeHandle, and both stream tasks.
$task='task-15'
$global:NrQaPropertyOwners=[Collections.Generic.List[object]]::new()
$p=[Diagnostics.Process]::new()
$o=@{process=$p;handle=$null;started=$false;closed=$false;identity=$null;events=@();retainedEvents=@();
    exitSignal=$null;nativeOutput=$null;stdout=$null;stderr=$null;outputStarted=$false;errorStarted=$false;
    kernelWait=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset);
    kernelAttached=$false;unknownGate=[Threading.ManualResetEvent]::new($false);
    primaryError=$null;secondaryErrors=@();state='original-owner-published';sdkSettled=$false;validated=$false}
$p.StartInfo.FileName="$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe"
$p.StartInfo.Arguments='-NoProfile -Command "[Console]::Out.WriteLine(''PROPERTY_CPU_OUT'');[Console]::Error.WriteLine(''PROPERTY_CPU_ERR'');exit 0"'
$p.StartInfo.UseShellExecute=$false;$p.StartInfo.CreateNoWindow=$true
$p.StartInfo.RedirectStandardOutput=$true;$p.StartInfo.RedirectStandardError=$true
$p.EnableRaisingEvents=$true;$o.exitSignal=[NrProcessExit]::new($p)
Start-NrInstanceOriginal $o
if(!$global:NrQaPropertyOwners.Contains($o) -or ![object]::ReferenceEquals($o.handle,$p.SafeHandle)){throw 'ORIGINAL_OWNER_LOST'}
if(!$o.exitSignal.Exited.WaitOne(15000) -or !$o.kernelWait.WaitOne(15000) -or !$p.WaitForExit(15000) -or
    !$o.stdout.Wait(15000) -or !$o.stderr.Wait(15000) -or !(Test-NrQaOriginalOwnerClosed $o)){throw 'CPU_ORIGINAL_UNRESOLVED'}
if($p.ExitCode -ne 0 -or !$o.stdout.Result.Contains('PROPERTY_CPU_OUT') -or !$o.stderr.Result.Contains('PROPERTY_CPU_ERR')){throw 'CPU_PROCESS_FAILED'}
$o.closed=$false;$o.outputStarted=$null
if(Test-NrQaOriginalOwnerClosed $o){throw 'UNKNOWN_OUTPUT_ACCEPTED'};$count++
Refuse {Close-NrPropertyOriginal $o} 'NR_PROPERTY_DISPOSE_UNRESOLVED'
$o.outputStarted=$true
$o.errorStarted=$null
if(Test-NrQaOriginalOwnerClosed $o){throw 'UNKNOWN_ERROR_ACCEPTED'};$count++
$o.errorStarted=$true
$o.kernelAttached=$false
if(Test-NrQaOriginalOwnerClosed $o){throw 'UNKNOWN_KERNEL_ACCEPTED'};$count++
$o.kernelAttached=$true
# OS closure is real; SDK settlement is a modeled independent state in this
# CPU-only test. A failed scenario must stay failed even after safe disposal.
try {
    Refuse {Close-NrPropertyOriginal $o} 'NR_PROPERTY_DISPOSE_UNRESOLVED'
    if($o.handle.IsClosed){throw 'UNSETTLED_SDK_OWNER_DISPOSED'};$count++
    $o.sdkSettled=$true
    $o.primaryError=[InvalidOperationException]::new('CPU_MODELED_SCENARIO_FAILURE')
    Close-NrPropertyOriginal $o
    if(!$o.handle.IsClosed -or $o.state -cne 'original-objects-disposed-after-terminal-reap-eof' -or
        $o.validated -or $o.primaryError.Message -cne 'CPU_MODELED_SCENARIO_FAILURE'){throw 'CPU_DISPOSAL_FAILED'};$count++
}catch{$disposalFailures.Add('SDK: '+$_.Exception.Message)}
if($disposalFailures.Count){throw ('PROPERTY_DISPOSAL_REGRESSIONS '+($disposalFailures -join '; '))}

# Exercise the actual two-run coordinator with reviewed fixture rows. This mock
# represents run completion; the real source/restart/parser predicates stay active.
$fixture=Join-Path $base 'property-roundtrip-source-20261011/property_parser_tests.ps1'
$fixtureAst=[Management.Automation.Language.Parser]::ParseFile($fixture,[ref]$tokens,[ref]$errors)
foreach($name in @('FixtureRows','OutputRows')) {
    $definition=$fixtureAst.Find({param($n)$n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $name},$true)
    . ([scriptblock]::Create($definition.Extent.Text))
}
$script:triggered=[Collections.Generic.List[string]]::new()
function Get-FileHash {param($LiteralPath,$Algorithm) @{Hash=('a'*64)}}
function Invoke-NrPropertyOriginal($Owner,$Freeze) {
    [void]$script:triggered.Add($Owner.run)
    if($Owner.failure){throw $Owner.failure}
    $Owner.validated=$true
}
function Test-NrQaOriginalOwnerClosed($Owner) {$Owner.closed}
function FixtureOwner([string]$Run,[int]$HostPid,[string]$Birth,[string]$Reaped) {
    @{run=$Run;validated=$false;closed=$true;failure=$null;
        nativeOutput=@{Text=(OutputRows @(FixtureRows $Run $HostPid))};
        receipt=@{pid=$HostPid;creation=$Birth;image='C:/owned/nr-qa-host.exe';exit=0;
            exited=$true;stdoutEOF=$true;stderrEOF=$true;reapedUtc=$Reaped;root='C:/owned';
            source_file='C:/owned/profile/task15-source.json';source_sha256=('a'*64)}}
}
foreach($boundary in @('start-unknown','output-unknown','kernel-unknown','reap-unknown','EOF-unknown','saved-scenario-failure')) {
    $save=FixtureOwner save 101 '2026-10-11T01:00:00Z' '2026-10-11T01:00:30Z'
    $restore=FixtureOwner restore 102 '2026-10-11T01:01:00Z' '2026-10-11T01:01:30Z'
    $save.failure=$boundary;$script:triggered.Clear()
    Refuse {Invoke-NrPropertyPair $save $restore @{}} $boundary
    if(($script:triggered -join ',') -cne 'save' -or $restore.validated){throw 'FAILED_SAVE_TRIGGERED_RESTORE'}
}
$save=FixtureOwner save 101 '2026-10-11T01:00:00Z' '2026-10-11T01:00:30Z'
$restore=FixtureOwner restore 102 '2026-10-11T01:01:00Z' '2026-10-11T01:01:30Z'
$save.closed=$false;$script:triggered.Clear()
Refuse {Invoke-NrPropertyPair $save $restore @{}} 'NR_PROPERTY_SAVE_UNRESOLVED'
if(($script:triggered -join ',') -cne 'save'){throw 'OPEN_SAVE_TRIGGERED_RESTORE'}
$save.closed=$true;$script:triggered.Clear()
$result=Invoke-NrPropertyPair $save $restore @{}
if(($script:triggered -join ',') -cne 'save,restore' -or $result.save_rows.Count -ne 21 -or
    $result.restore_rows.Count -ne 20){throw 'PAIR_ROUTE_FAILED'};$count++
Write-Output "PROPERTY_RUNNER_CPU_PASS assertions=$count CPU_ONLY=true native=false"
