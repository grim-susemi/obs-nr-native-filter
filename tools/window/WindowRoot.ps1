# Generic source, loaded once by the original retained window parent.
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
function Assert-WindowCpuAvailable([string]$Work) {
    if(Test-Path -LiteralPath (Join-Path $Work 'out/native-session-lock/ACTIVE')){
        throw 'WINDOW_PAUSED_NATIVE_SESSION'
    }
}
function Get-WindowHash([byte[]]$Bytes) {
    $h=[Security.Cryptography.SHA256]::Create()
    try {([BitConverter]::ToString($h.ComputeHash($Bytes))).Replace('-','').ToLowerInvariant()}
    finally {$h.Dispose()}
}
function ConvertTo-WindowText($Value) {($Value|ConvertTo-Json -Depth 60)+"`n"}
function Get-WindowPin([string]$Path) {
    [pscustomobject][ordered]@{path=[IO.Path]::GetFullPath($Path).Replace('\','/');
        bytes=(Get-Item -LiteralPath $Path).Length;
        sha256=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
}
function Assert-WindowPin($Pin,[switch]$AllowPlaceholder) {
    if($null -eq $Pin){throw 'WINDOW_PIN_REQUIRED'}
    if($Pin.PSObject.Properties['placeholder']) {
        if(!$AllowPlaceholder -or !$Pin.placeholder -or $null -ne $Pin.path -or
            $null -ne $Pin.bytes -or $null -ne $Pin.sha256){throw 'WINDOW_UNRESOLVED_PIN'}
        return
    }
    if(!$Pin.path -or $Pin.sha256 -cnotmatch '^[0-9a-f]{64}$' -or $Pin.bytes -lt 0){throw 'WINDOW_PIN_SCHEMA'}
    $actual=Get-WindowPin $Pin.path
    if($actual.sha256 -cne $Pin.sha256 -or $actual.bytes -ne $Pin.bytes){throw 'WINDOW_STALE_PIN'}
}
function Read-WindowPin($Pin) {
    Assert-WindowPin $Pin
    Get-Content -LiteralPath $Pin.path -Raw -Encoding UTF8|ConvertFrom-Json
}
function Write-WindowNew([string]$Path,[string]$Text) {
    $bytes=[Text.UTF8Encoding]::new($false).GetBytes($Text)
    $f=[IO.File]::Open($Path,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
    try {$f.Write($bytes,0,$bytes.Length);$f.Flush($true)}finally {$f.Dispose()}
}
function Assert-WindowOrdinaryPath([string]$Path) {
    for($p=[IO.Path]::GetFullPath($Path);$p;$p=Split-Path -Parent $p) {
        if((Test-Path -LiteralPath $p) -and ((Get-Item -LiteralPath $p -Force).Attributes -band
            [IO.FileAttributes]::ReparsePoint)){throw 'WINDOW_REPARSE_PATH'}
    }
}
function Get-WindowIdentity([string]$Root,[string]$Work) {
    $r=[IO.Path]::GetFullPath($Root).TrimEnd('\','/').Replace('\','/')
    $w=[IO.Path]::GetFullPath($Work).TrimEnd('\','/').Replace('\','/')
    $leaf=Split-Path -Leaf $r
    if($leaf -cnotmatch '^[a-z0-9-]*window-r([0-9]+)-(n[0-9]+[ab]?)-(u[0-9]+)-(s[0-9]+)-([0-9]{8})$'){
        throw 'WINDOW_ROOT_IDENTITY'
    }
    $round=$Matches[1];$session=$Matches[2].ToUpperInvariant()
    $build=$Matches[3].ToUpperInvariant();$snapshot=$Matches[4].ToUpperInvariant()
    $task=Split-Path -Parent $r
    $tree=$w+'/.omo/evidence/obs-nr-native-filter-20260930'
    if((Split-Path -Leaf $task) -cnotmatch '^task-[0-9]+$' -or
        (Split-Path -Parent $task).Replace('\','/') -ine $tree){throw 'WINDOW_ROOT_TREE'}
    [pscustomobject][ordered]@{root=$r;leaf=$leaf;round=$round;session=$session;build=$build;snapshot=$snapshot;
        stage=$w+'/out/native-window-stage/'+$leaf;route=$r+'/Invoke-Task17Window.ps1';
        genericRoute=$r+'/Invoke-WindowScenario.ps1';inputs=$r+'/INPUTS.json'}
}
function Assert-WindowScenarioOrder($Scenarios) {
    if(@($Scenarios).Count -eq 0){throw 'WINDOW_EMPTY_SCENARIOS'}
    $seen=@{};$faults=$false
    for($n=0;$n -lt @($Scenarios).Count;$n++) {
        $s=$Scenarios[$n]
        if($s.ordinal -ne $n+1 -or $seen.ContainsKey([string]$s.scenario)){throw 'WINDOW_REORDERED_LIST'}
        if($s.scenario -cnotmatch '^[a-z][a-z0-9-]*$' -or $s.task -lt 1 -or
            $s.actionBoundMs -isnot [int] -or $s.actionBoundMs -le 0 -or
            $s.destructive -isnot [bool] -or $s.stopClass -cnotin @('product','harness')){
            throw 'WINDOW_SCENARIO_SCHEMA'
        }
        if($faults -and !$s.destructive){throw 'WINDOW_DESTRUCTIVE_TAIL'}
        if($s.scenario -cin @('handled-faults','shutdown') -and !$s.destructive){throw 'WINDOW_DESTRUCTIVE_TAIL'}
        if($s.scenario -ceq 'shutdown' -and $n -ne @($Scenarios).Count-1){throw 'WINDOW_SHUTDOWN_LAST'}
        if($s.destructive){$faults=$true}
        foreach($p in $s.prerequisites) {
            if($p.requiresAcceptance -isnot [bool] -or !$p.key -or $p.task -lt 1){throw 'WINDOW_PREREQUISITE_SCHEMA'}
            if($p.proof -eq $null -and !$seen.ContainsKey([string]$p.key)){throw 'WINDOW_PREREQUISITE_ORDER'}
            if($s.scenario -ceq 'ghost-baseline' -and $p.task -eq 26 -and !$p.requiresAcceptance){
                throw 'WINDOW_CALIBRATION_ACCEPTANCE_REQUIRED'
            }
        }
        if($s.scenario -ceq 'ghost-baseline' -and
            @($s.prerequisites|Where-Object {$_.task -eq 26 -and $_.requiresAcceptance -and $_.proof}).Count -ne 1){
            throw 'WINDOW_CALIBRATION_ACCEPTANCE_REQUIRED'
        }
        $seen[[string]$s.scenario]=$true
    }
}
function Get-WindowParamsPins($Params) {
    @($Params.pair.host,$Params.pair.plugin,$Params.buildVerdict,$Params.closureReceipt,$Params.closureTool,
        $Params.snapshotManifest,$Params.wrapper.before,$Params.wrapper.after,$Params.runtime,
        $Params.stageAdapter,$Params.preservation)
    foreach($s in $Params.scenarios) {
        $s.runner;$s.validator;$s.validatorReview
        foreach($a in $s.validatorCall.arguments){if($a.source -ceq 'pinSha256'){$a.pin}}
        foreach($p in $s.prerequisites){if($p.proof){$p.proof}}
    }
}
function Import-WindowR32($References) {
    foreach($row in @($References.source,$References.sourceReview,$References.entryReview)+
        @($References.definitions)+@($References.policies)+@($References.derivationOnly)) {
        $pin=[pscustomobject]@{path=(Join-Path $References.root $row.file);bytes=$row.bytes;sha256=$row.sha256}
        Assert-WindowPin $pin
    }
    $source=Get-Content -Raw -LiteralPath (Join-Path $References.root $References.sourceReview.file)|ConvertFrom-Json
    $entry=Get-Content -Raw -LiteralPath (Join-Path $References.root $References.entryReview.file)|ConvertFrom-Json
    if($source.verdict -cne 'APPROVE_SOURCE_ONLY' -or $entry.verdict -cne 'APPROVE_ENTRY' -or
        @($source.blockers).Count -or @($entry.blockers).Count -or
        $source.candidate.freezeSha256 -cne $References.source.sha256 -or
        $entry.precedingReview.sha256 -cne $References.sourceReview.sha256){throw 'WINDOW_R32_REVIEW_CHAIN'}
    foreach($definition in $References.definitions){. (Join-Path $References.root $definition.file)}
}
function Get-WindowRendered($Params,[string]$GeneratorRoot,[switch]$DryRun) {
    if($Params.schema -cne 'nr-window-params-v1'){throw 'WINDOW_PARAMS_SCHEMA'}
    $identity=Get-WindowIdentity $Params.instanceRoot $Params.work
    foreach($name in @('session','build','snapshot')) {
        if($Params.$name -cne $identity.$name){throw 'WINDOW_IDENTITY_PARAMETERS'}
    }
    Assert-WindowScenarioOrder $Params.scenarios
    foreach($pin in Get-WindowParamsPins $Params){Assert-WindowPin $pin -AllowPlaceholder:$DryRun}
    foreach($s in $Params.scenarios){Assert-WindowValidatorCall $s -AllowPlaceholder:$DryRun}
    $sourceNames=@('WindowRoot.ps1','Run-Window.ps1','Invoke-WindowScenario.ps1','New-WindowRoot.ps1',
        'R32-REFERENCES.json')
    $sources=@(foreach($name in $sourceNames){Get-WindowPin (Join-Path $GeneratorRoot $name)})
    $scenarios=@(foreach($s in $Params.scenarios) {
        [ordered]@{ordinal=$s.ordinal;task=$s.task;scenario=$s.scenario;
            evidenceRoot=$identity.root+'/native'+$s.ordinal.ToString('000');
            artifactRoot=$identity.root+'/artifacts/'+$s.scenario;validator=$s.validator;runner=$s.runner;
            validatorReview=$s.validatorReview;validatorCall=$s.validatorCall;
            actionBoundMs=$s.actionBoundMs;prerequisites=@($s.prerequisites);stopClass=$s.stopClass;
            destructive=$s.destructive;publicCandidatePrefix=$s.publicCandidatePrefix}
    })
    $order=Get-WindowHash ([Text.Encoding]::UTF8.GetBytes((ConvertTo-WindowText $scenarios)))
    $inputs=[ordered]@{schema='nr-window-inputs-v1';identity=$identity;params=$Params;
        scenarios=$scenarios;orderSha256=$order;generatorSources=$sources;
        dryRun=[bool]$DryRun;nativeAuthorizable=(!$DryRun);nativeExecuted=$false;gpuExecuted=$false}
    $entry=@'
[CmdletBinding()]
param([Parameter(Mandatory)][string]$InputsSha256,[Parameter(Mandatory)]$RootAdmission)
$ErrorActionPreference='Stop'
$actual=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'INPUTS.json') -Algorithm SHA256).Hash.ToLowerInvariant()
if($actual -cne $InputsSha256){throw 'WINDOW_INPUTS_BINDING'}
$core='@@CORE_PATH@@'
if((Get-FileHash -LiteralPath $core -Algorithm SHA256).Hash.ToLowerInvariant() -cne '@@CORE_SHA@@'){throw 'WINDOW_STALE_PIN'}
. $core
$i=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'INPUTS.json') -Raw -Encoding UTF8|ConvertFrom-Json
foreach($pin in $i.generatorSources){Assert-WindowPin $pin}
$run=@($i.generatorSources|Where-Object {(Split-Path -Leaf $_.path) -ceq 'Run-Window.ps1'})[0]
. $run.path -InstanceRoot $PSScriptRoot -InputsSha256 $InputsSha256 -RootAdmission $RootAdmission
'@
    $entry=$entry.Replace('@@CORE_PATH@@',$sources[0].path.Replace("'","''")).Replace('@@CORE_SHA@@',$sources[0].sha256)
    $route=@'
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Scenario,[Parameter(Mandatory)][string]$Evidence,
    [Parameter(Mandatory)][string]$HostRoot,[Parameter(Mandatory)][string]$Runtime,
    [Parameter(Mandatory)][string]$CandidateManifest,[Parameter(Mandatory)][string]$CandidateSha256)
$actual=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'INPUTS.json') -Algorithm SHA256).Hash.ToLowerInvariant()
if($actual -cne $CandidateSha256){throw 'WINDOW_INPUTS_BINDING'}
if(!(Get-Variable WindowSession -Scope Script -ErrorAction SilentlyContinue)){throw 'WINDOW_ORIGINAL_SESSION_REQUIRED'}
$i=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'INPUTS.json') -Raw -Encoding UTF8|ConvertFrom-Json
if(!$script:WindowSession -or $script:WindowSession.inputsPin.sha256 -cne $CandidateSha256){throw 'WINDOW_ORIGINAL_SESSION_REQUIRED'}
$route=@($i.generatorSources|Where-Object {(Split-Path -Leaf $_.path) -ceq 'Invoke-WindowScenario.ps1'})
if($route.Count -ne 1 -or (Get-FileHash -LiteralPath $route[0].path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $route[0].sha256){throw 'WINDOW_STALE_PIN'}
. $route[0].path -Scenario $Scenario -Evidence $Evidence -HostRoot $HostRoot -Runtime $Runtime `
    -CandidateManifest $CandidateManifest -CandidateSha256 $CandidateSha256
'@
    [ordered]@{'INPUTS.json'=(ConvertTo-WindowText $inputs);'Run-Once-RetainedParent.ps1'=$entry+"`n";
        'Invoke-Task17Window.ps1'=$route+"`n";'Invoke-WindowScenario.ps1'=$route+"`n";
        'Invoke-Task27Window.ps1'=$route+"`n"}
}
function Assert-WindowInstance([string]$Root,[string]$InputsSha256) {
    $inputPin=Get-WindowPin (Join-Path $Root 'INPUTS.json')
    if($inputPin.sha256 -cne $InputsSha256){throw 'WINDOW_INPUTS_BINDING'}
    $i=Read-WindowPin $inputPin
    if($i.dryRun -or !$i.nativeAuthorizable){throw 'WINDOW_DRY_RUN_NOT_ADMISSION'}
    if([IO.Path]::GetFullPath($Root) -ine [IO.Path]::GetFullPath($i.identity.root)){throw 'WINDOW_WRONG_ROOT'}
    foreach($pin in $i.generatorSources){Assert-WindowPin $pin}
    $generatorRoot=Split-Path -Parent $i.generatorSources[0].path
    $expected=Get-WindowRendered $i.params $generatorRoot
    foreach($file in $expected.Keys) {
        $bytes=[IO.File]::ReadAllBytes((Join-Path $Root $file))
        if((Get-WindowHash $bytes) -cne (Get-WindowHash ([Text.Encoding]::UTF8.GetBytes($expected[$file])))){
            throw 'WINDOW_INSTANCE_NOT_GENERATOR_OUTPUT'
        }
    }
    $i
}
function Assert-WindowNext($Inputs,$Scenario,$State,[string]$Stage) {
    Assert-WindowScenarioOrder $Inputs.scenarios
    $order=Get-WindowHash ([Text.Encoding]::UTF8.GetBytes((ConvertTo-WindowText $Inputs.scenarios)))
    if($order -cne $Inputs.orderSha256){throw 'WINDOW_REORDERED_LIST'}
    if([IO.Path]::GetFullPath($Stage) -ine [IO.Path]::GetFullPath($Inputs.identity.stage)){throw 'WINDOW_WRONG_STAGE'}
    if($State.stopped){throw 'WINDOW_SESSION_STOPPED'}
    if($Scenario.ordinal -ne $State.next){throw 'WINDOW_SCENARIO_SEQUENCE'}
    foreach($p in $Scenario.prerequisites) {
        if(!$State.verdicts.ContainsKey([string]$p.key) -or
            $State.verdicts[[string]$p.key].machineResult -cne 'PASS' -or
            ($p.requiresAcceptance -and !$State.verdicts[[string]$p.key].accepted)){
            throw 'WINDOW_PREREQUISITE_NOT_PASS'
        }
    }
    if($State.previousClosed -isnot [bool] -or !$State.previousClosed){throw 'WINDOW_UNCERTAIN_CUSTODY'}
}
function New-WindowConsumption([string]$Root,$Inputs,$Scenario,[string]$AdmissionSha256) {
    $p=Join-Path $Root ('consumed-'+$Scenario.ordinal.ToString('000')+'-'+$Scenario.scenario+'.json')
    if(Test-Path -LiteralPath $p){throw 'WINDOW_SCENARIO_REPLAY'}
    $r=[ordered]@{schema='nr-scenario-consumed-v1';sessionAdmissionSha256=$AdmissionSha256;
        orderSha256=$Inputs.orderSha256;scenario=$Scenario.scenario;ordinal=$Scenario.ordinal;
        actionBoundMs=$Scenario.actionBoundMs;utc=[DateTime]::UtcNow.ToString('o');automaticRetry=$false}
    try {Write-WindowNew $p (ConvertTo-WindowText $r)}
    catch [IO.IOException] {if(Test-Path -LiteralPath $p){throw 'WINDOW_SCENARIO_REPLAY'};throw}
    Get-WindowPin $p
}
function Set-WindowOutcome($State,$Scenario,[string]$Result,[bool]$Closed) {
    if($Result -cnotin @('PASS','FAIL')){throw 'WINDOW_MACHINE_RESULT'}
    $State.previousClosed=$Closed
    $State.verdicts[$Scenario.scenario]=@{machineResult=$Result;accepted=$false}
    $State.next++
    if(!$Closed -or ($Result -ceq 'FAIL' -and $Scenario.stopClass -ceq 'product')){$State.stopped=$true}
}
function Skip-WindowBlockedDependent($State,$Scenario) {
    $blocked=@($Scenario.prerequisites|Where-Object {!$_.proof -and
        $State.verdicts.ContainsKey([string]$_.key) -and
        $State.verdicts[[string]$_.key].machineResult -cne 'PASS'})
    if(!$blocked.Count){return $false}
    if($State.stopped -or $Scenario.ordinal -ne $State.next -or !$State.previousClosed){
        throw 'WINDOW_UNCERTAIN_CUSTODY'
    }
    $State.verdicts[$Scenario.scenario]=@{machineResult='NOT_RUN';accepted=$false}
    $State.next++
    return $true
}
function Test-WindowOwnedObs($Row,[string]$Stage,$Registrations) {
    if(!$Row.ExecutablePath -or !$Row.CreationDate){return $false}
    $image=[IO.Path]::GetFullPath($Row.ExecutablePath)
    $owned=[IO.Path]::GetFullPath((Join-Path $Stage 'bin/64bit/obs64.exe'))
    if($image -ine $owned){return $false}
    $birth=$Row.CreationDate.ToUniversalTime().ToString('o')
    $matches=@($Registrations|Where-Object {$_.pid -eq $Row.ProcessId -and
        $_.creation -ceq $birth -and [IO.Path]::GetFullPath($_.image) -ieq $owned})
    $matches.Count -eq 1
}
function Assert-WindowStage($StageReceipt,$Inputs) {
    if($StageReceipt.stage -ine $Inputs.identity.stage -or $StageReceipt.snapshot -cne $Inputs.params.snapshot -or
        $StageReceipt.hostSha256 -cne $Inputs.params.pair.host.sha256 -or
        $StageReceipt.pluginSha256 -cne $Inputs.params.pair.plugin.sha256 -or
        $StageReceipt.physicalCopies -ne 0 -or !$StageReceipt.profileEmpty -or !$StageReceipt.ordinaryPaths -or
        !$StageReceipt.hardlinkIdentitiesBound){throw 'WINDOW_WRONG_STAGE'}
}
function Assert-WindowValidatorCall($Scenario,[switch]$AllowPlaceholder) {
    if(!$Scenario.PSObject.Properties['validatorCall']){throw 'WINDOW_VALIDATOR_CALL_REQUIRED'}
    $call=$Scenario.validatorCall
    foreach($name in @('schema','validatorSha256','function','arguments')) {
        if(!$call -or !$call.PSObject.Properties[$name]){throw 'WINDOW_VALIDATOR_CALL_SCHEMA'}
    }
    if($call.schema -cne 'nr-window-validator-call-v1' -or
        $call.validatorSha256 -cne $Scenario.validator.sha256 -or
        $call.function -cne $Scenario.validator.function){throw 'WINDOW_VALIDATOR_CALL_BINDING'}
    Assert-WindowPin $Scenario.validator
    $tokens=$null;$errors=$null
    $ast=[Management.Automation.Language.Parser]::ParseFile($Scenario.validator.path,[ref]$tokens,[ref]$errors)
    if(@($errors).Count){throw 'WINDOW_VALIDATOR_CALL_SIGNATURE'}
    $functions=@($ast.FindAll({param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -ceq $Scenario.validator.function},$true))
    if($functions.Count -ne 1){throw 'WINDOW_VALIDATOR_CALL_SIGNATURE'}
    $definition=$functions[0]
    $parameters=@()
    if($definition.Parameters){$parameters=@($definition.Parameters)}
    elseif($definition.Body.ParamBlock){$parameters=@($definition.Body.ParamBlock.Parameters)}
    if(@($call.arguments).Count -ne $parameters.Count){throw 'WINDOW_VALIDATOR_CALL_SIGNATURE'}
    for($n=0;$n -lt $parameters.Count;$n++) {
        $a=$call.arguments[$n]
        if(!$a -or !$a.PSObject.Properties['name'] -or !$a.PSObject.Properties['source'] -or
            $a.name -cne $parameters[$n].Name.VariablePath.UserPath){throw 'WINDOW_VALIDATOR_CALL_SIGNATURE'}
        switch -CaseSensitive ($a.source) {
            'output' {if($a.name -cne 'Output'){throw 'WINDOW_VALIDATOR_CALL_SOURCE'}}
            'artifactRoot' {if($a.name -cne 'ArtifactRoot'){throw 'WINDOW_VALIDATOR_CALL_SOURCE'}}
            'pinSha256' {
                if(!$a.PSObject.Properties['pin']){throw 'WINDOW_VALIDATOR_CALL_SOURCE'}
                Assert-WindowPin $a.pin -AllowPlaceholder:$AllowPlaceholder
            }
            default {throw 'WINDOW_VALIDATOR_CALL_SOURCE'}
        }
    }
}
function Invoke-WindowValidator($Scenario,[string]$Output) {
    Assert-WindowValidatorCall $Scenario
    $arguments=@{}
    foreach($a in $Scenario.validatorCall.arguments) {
        switch -CaseSensitive ($a.source) {
            'output' {$arguments[$a.name]=$Output}
            'artifactRoot' {$arguments[$a.name]=$Scenario.artifactRoot}
            'pinSha256' {$arguments[$a.name]=$a.pin.sha256}
        }
    }
    . $Scenario.validator.path
    & $Scenario.validator.function @arguments
}
function Assert-WindowValidatorApproval($Scenario,[string]$Actor='') {
    if(!$Scenario.PSObject.Properties['validatorReview']){throw 'WINDOW_VALIDATOR_REVIEW_REQUIRED'}
    $review=Read-WindowPin $Scenario.validatorReview
    foreach($name in @('schema','verdict','complete','blockers','reviewer','producer','task','validators',
        'nativeAdmissionGranted','nativeExecuted','gpuExecuted')) {
        if(!$review.PSObject.Properties[$name]){throw 'WINDOW_VALIDATOR_NOT_APPROVED'}
    }
    if($review.schema -cne 'nr-window-validator-review-v1' -or $review.verdict -cne 'APPROVE_SOURCE_ONLY' -or
        $review.complete -isnot [bool] -or !$review.complete -or $null -eq $review.blockers -or
        @($review.blockers).Count -or [string]::IsNullOrWhiteSpace($review.reviewer) -or
        [string]::IsNullOrWhiteSpace($review.producer) -or $review.reviewer -ceq $review.producer -or
        $review.reviewer -ceq 'st_01a13dbc' -or ($Actor -and $review.reviewer -ceq $Actor) -or
        $review.nativeAdmissionGranted -isnot [bool] -or $review.nativeAdmissionGranted -or
        $review.nativeExecuted -isnot [bool] -or $review.nativeExecuted -or
        $review.gpuExecuted -isnot [bool] -or $review.gpuExecuted){throw 'WINDOW_VALIDATOR_NOT_APPROVED'}
    if($review.task -ne $Scenario.task){throw 'WINDOW_VALIDATOR_REVIEW_COVERAGE'}
    $matches=@(foreach($row in $review.validators) {
        foreach($name in @('path','bytes','sha256','function','logicalPath')) {
            if(!$row.PSObject.Properties[$name]){throw 'WINDOW_VALIDATOR_REVIEW_COVERAGE'}
        }
        if([IO.Path]::GetFullPath($row.path) -ieq [IO.Path]::GetFullPath($Scenario.validator.path) -and
            $row.bytes -eq $Scenario.validator.bytes -and $row.sha256 -ceq $Scenario.validator.sha256 -and
            $row.function -ceq $Scenario.validator.function -and $row.logicalPath -ceq $Scenario.validator.logicalPath){$row}
    })
    if($matches.Count -ne 1){throw 'WINDOW_VALIDATOR_REVIEW_COVERAGE'}
}
function Assert-WindowBuild($Inputs) {
    $p=$Inputs.params
    foreach($pin in Get-WindowParamsPins $p){Assert-WindowPin $pin}
    $approval=[pscustomobject]@{schema=1;kind='HOST_BUILD_APPROVAL_PIN';verdict=$p.buildVerdict;
        pair=$p.pair;requiredFields=[pscustomobject]@{verdict='APPROVE_BUILD';recommendation='APPROVE_BUILD';
            gateRecommendation='APPROVE';complete=$true;nativeAdmissionGranted=$false;
            nativeExecuted=$false;gpuExecuted=$false;Task16Accepted=$false};
        requiredPair=[pscustomobject]@{hostSha256Path='pair.host.sha256';pluginSha256Path='pair.plugin.sha256';
            verdictPath='pair.verdict';verdict='APPROVE_BUILD'};
        requiredClosure=[pscustomobject]@{receipt=$p.closureReceipt;verdict='SINGLE_CLOSURE_CONFIRMED';exitCode=0;
            tool=$p.closureTool;citedAtPath='closureReceipt.sha256'}}
    $binding=Assert-T29HostBuildApproval ([pscustomobject]@{host=$p.pair.host;plugin=$p.pair.plugin}) $approval
    $receipt=Read-WindowPin $p.closureReceipt
    $snapshot=Read-WindowPin $p.snapshotManifest
    if($receipt.checks.G0.manifest.sha256 -cne $p.snapshotManifest.sha256 -or
        $receipt.checks.G0.manifest.snapshot -cne $p.snapshot -or $snapshot.snapshot -cne $p.snapshot){
        throw 'WINDOW_CLOSURE_SNAPSHOT_BINDING'
    }
    $rows=@($snapshot.rows|Where-Object {$_.logicalPath -ceq $p.wrapper.snapshotLogicalPath})
    if($rows.Count -ne 1 -or $rows[0].sha256 -cne $p.wrapper.after.sha256 -or
        $rows[0].bytes -ne $p.wrapper.after.bytes){throw 'WINDOW_WRAPPER_NOT_SNAPSHOT_ROW'}
    foreach($s in $Inputs.scenarios) {
        Assert-WindowValidatorCall $s
        Assert-WindowValidatorApproval $s
        $rows=@($snapshot.rows|Where-Object {$_.logicalPath -ceq $s.validator.logicalPath})
        if($rows.Count -ne 1 -or $rows[0].sha256 -cne $s.validator.sha256 -or
            $rows[0].bytes -ne $s.validator.bytes){throw 'WINDOW_VALIDATOR_NOT_SNAPSHOT_ROW'}
    }
    $binding
}
function Assert-WindowReview($Review,[string]$Verdict,[string]$Actor,[string]$Producer) {
    if($Review.verdict -cne $Verdict -or !$Review.complete -or @($Review.blockers).Count -or
        $Review.nativeAdmissionGranted -isnot [bool] -or $Review.nativeAdmissionGranted -or
        $Review.nativeExecuted -isnot [bool] -or $Review.nativeExecuted -or
        $Review.gpuExecuted -isnot [bool] -or $Review.gpuExecuted -or
        !$Review.reviewer -or $Review.reviewer -ceq $Producer -or $Review.reviewer -ceq $Actor){
        throw 'WINDOW_INDEPENDENT_REVIEW_REQUIRED'
    }
}
function Assert-WindowRootAdmission($Admission,$Inputs,$InputsPin) {
    $source=Read-WindowPin $Admission.sourceReview
    $entry=Read-WindowPin $Admission.entryReview
    Assert-WindowReview $source 'APPROVE_SOURCE_ONLY' $Admission.actor 'st_01a13dbc'
    Assert-WindowReview $entry 'APPROVE_ENTRY' $Admission.actor 'st_01a13dbc'
    $sourcePinsSha=Get-WindowHash ([Text.Encoding]::UTF8.GetBytes((ConvertTo-WindowText $Inputs.generatorSources)))
    $age=([DateTime]::UtcNow-[DateTime]::Parse($Admission.utc).ToUniversalTime()).TotalSeconds
    if($Admission.schema -cne 'nr-window-root-admission-v1' -or $Admission.authority -cne 'ROOT' -or
        !$Admission.actor -or $Admission.actor -ceq 'st_01a13dbc' -or
        $Admission.nativeExecutionAuthorized -isnot [bool] -or !$Admission.nativeExecutionAuthorized -or
        $Admission.exclusiveGpuLane -isnot [bool] -or !$Admission.exclusiveGpuLane -or
        $Admission.inputsSha256 -cne $InputsPin.sha256 -or $Admission.orderSha256 -cne $Inputs.orderSha256 -or
        $Admission.root -ine $Inputs.identity.root -or $age -lt 0 -or $age -gt 120 -or
        $source.generatorSourcePinsSha256 -cne $sourcePinsSha -or
        $entry.inputsSha256 -cne $InputsPin.sha256 -or $entry.sourceReviewSha256 -cne $Admission.sourceReview.sha256 -or
        $entry.orderSha256 -cne $Inputs.orderSha256){throw 'WINDOW_ROOT_ADMISSION_REQUIRED'}
    foreach($s in $Inputs.scenarios){Assert-WindowValidatorApproval $s $Admission.actor}
    $lease=Read-WindowPin $Admission.originalParentLease
    Assert-T23OriginalParentLease $lease
    if($lease.ownerSessionId -cne $Admission.ownerSessionId){throw 'WINDOW_PARENT_SESSION_BINDING'}
}
function Read-WindowExternalVerdicts($Inputs) {
    $verdicts=@{}
    foreach($s in $Inputs.scenarios) {
        foreach($p in $s.prerequisites) {
            if(!$p.proof){continue}
            $v=Read-WindowPin $p.proof
            if($v.schema -cne 'nr-window-prerequisite-v1' -or $v.task -ne $p.task -or
                $v.machineResult -cne 'PASS' -or $v.accepted -isnot [bool] -or
                ($p.requiresAcceptance -and !$v.accepted)){throw 'WINDOW_PREREQUISITE_NOT_PASS'}
            if($verdicts.ContainsKey($p.key) -and
                $verdicts[$p.key].proofSha256 -cne $p.proof.sha256){throw 'WINDOW_PREREQUISITE_CONFLICT'}
            # The normalized receipt must cite a pinned independent task verdict.
            $authority=Read-WindowPin $v.authority
            $accepted=Get-T29Field $authority ('Task'+$p.task+'Accepted')
            if(!$authority.complete -or @($authority.blockers).Count -or
                $authority.verdict -cnotmatch '^APPROVE' -or !$authority.reviewer -or
                ($p.requiresAcceptance -and ($accepted -isnot [bool] -or !$accepted))){
                throw 'WINDOW_PREREQUISITE_AUTHORITY'
            }
            $machine=Read-WindowPin $v.machine
            if($machine.result -cne 'PASS'){throw 'WINDOW_PREREQUISITE_NOT_PASS'}
            $verdicts[$p.key]=@{machineResult='PASS';accepted=$v.accepted;proofSha256=$p.proof.sha256}
        }
    }
    $verdicts
}
function Measure-WindowPreservation($InventoryPin) {
    $inventory=Read-WindowPin $InventoryPin
    if($inventory.schema -cne 'nr-window-preservation-v1' -or @($inventory.roots).Count -eq 0){
        throw 'WINDOW_PRESERVATION_INVENTORY'
    }
    $measured=@()
    foreach($root in $inventory.roots) {
        Assert-WindowOrdinaryPath $root.path
        $base=[IO.Path]::GetFullPath($root.path).TrimEnd('\','/')
        $directories=@(Get-ChildItem -LiteralPath $base -Recurse -Directory -Force|Sort-Object FullName)
        if($directories.Count -ne @($root.directories).Count){throw 'WINDOW_PRESERVATION_DIRECTORY_SET'}
        foreach($directory in $directories) {
            Assert-WindowOrdinaryPath $directory.FullName
            $relative=$directory.FullName.Substring($base.Length+1).Replace('\','/')
            if(@($root.directories|Where-Object {$_ -ceq $relative}).Count -ne 1){
                throw 'WINDOW_PRESERVATION_DIRECTORY_SET'
            }
        }
        $files=@(Get-ChildItem -LiteralPath $base -Recurse -File -Force|Sort-Object FullName)
        if($files.Count -ne @($root.files).Count){throw 'WINDOW_PRESERVATION_FILE_SET'}
        foreach($file in $files) {
            Assert-WindowOrdinaryPath $file.FullName
            $relative=$file.FullName.Substring($base.Length+1).Replace('\','/')
            $expected=@($root.files|Where-Object {$_.relative -ceq $relative})
            if($expected.Count -ne 1){throw 'WINDOW_PRESERVATION_FILE_SET'}
            $pin=Get-WindowPin $file.FullName
            if($pin.bytes -ne $expected[0].bytes -or $pin.sha256 -cne $expected[0].sha256){
                throw 'WINDOW_PRESERVATION_CHANGED'
            }
            $measured+=$pin
        }
    }
    [ordered]@{schema='nr-window-preservation-observation-v1';inventory=$InventoryPin;files=$measured}
}
function Read-WindowCurrent($Inputs,$References,$OwnedGui) {
    $rows=@(Get-CimInstance Win32_Process -OperationTimeoutSec 8|
        Select-Object ProcessId,ParentProcessId,Name,ExecutablePath,CommandLine,CreationDate)
    $smi=(Get-Command nvidia-smi.exe -CommandType Application -ErrorAction Stop).Source
    $raw=@(& $smi --query-compute-apps=pid,process_name --format=csv,noheader,nounits 2>&1)
    if($LASTEXITCODE -ne 0){throw 'WINDOW_GPU_METADATA_QUERY'}
    $gpu=@()
    foreach($line in $raw) {
        if([string]::IsNullOrWhiteSpace([string]$line)){continue}
        if([string]$line -notmatch '^\s*([0-9]+),\s*(.+?)\s*$'){throw 'WINDOW_GPU_METADATA_SCHEMA'}
        $gpu+=@{pid=$Matches[1];process_name=$Matches[2].Trim()}
    }
    $ownedRows=@($rows|Where-Object {Test-WindowOwnedObs $_ $Inputs.identity.stage $OwnedGui})
    $foreignRows=@($rows|Where-Object {$row=$_;!@($ownedRows|Where-Object {$_.ProcessId -eq $row.ProcessId}).Count})
    $foreignGpu=@(foreach($g in $gpu) {
        $matched=@($rows|Where-Object {[string]$_.ProcessId -ceq [string]$g.pid -and $_.ExecutablePath -and
            [IO.Path]::GetFullPath($_.ExecutablePath) -ieq [IO.Path]::GetFullPath($g.process_name)})
        if($matched.Count -ne 1 -or !$matched[0].CreationDate){throw 'WINDOW_GPU_IDENTITY_UNBOUND'}
        if(!(Test-WindowOwnedObs $matched[0] $Inputs.identity.stage $OwnedGui)){$g}
    })
    Assert-CarrierLaneRows $foreignRows $foreignGpu
    $policy=Get-Content -Raw -LiteralPath (Join-Path $References.root 'CURRENT-AVAILABILITY-POLICY.json')|ConvertFrom-Json
    foreach($prior in $policy.priorNamedOriginals) {
        foreach($row in @($rows|Where-Object {$_.ProcessId -eq $prior.identity.pid})) {
            if(!$row.ExecutablePath -or !$row.CreationDate){throw 'WINDOW_PRIOR_IDENTITY_UNBOUND'}
            # Any current prior PID at its original path is conservative refusal, not proof of disposal.
            if([IO.Path]::GetFullPath($row.ExecutablePath) -ieq [IO.Path]::GetFullPath($prior.identity.image)){
                throw 'WINDOW_PRIOR_NAMED_CONTEXT_PRESENT'
            }
        }
    }
    $orca=Get-Content -Raw -LiteralPath (Join-Path $References.root 'ORCA-IMAGE-EVIDENCE.json')|ConvertFrom-Json
    $orcaRows=@($rows|Where-Object {$_.ExecutablePath -and
        [IO.Path]::GetFullPath($_.ExecutablePath) -ieq [IO.Path]::GetFullPath($orca.path)})
    if(!$orcaRows.Count){throw 'WINDOW_ORCA_IDENTITY_REQUIRED'}
    $before=Get-WindowPin $orca.path
    $file=Get-Item -LiteralPath $orca.path
    $sig=Get-AuthenticodeSignature -LiteralPath $orca.path
    $after=Get-WindowPin $orca.path
    if($before.sha256 -cne $orca.sha256 -or $after.sha256 -cne $before.sha256 -or
        $before.bytes -ne $orca.bytes -or $file.VersionInfo.ProductName -cne $orca.productName -or
        $file.VersionInfo.InternalName -ine $orca.internalName -or $sig.Status.ToString() -cne $orca.signatureStatus -or
        !$sig.SignerCertificate -or $sig.SignerCertificate.Thumbprint -cne $orca.signerThumbprint){
        throw 'WINDOW_ORCA_IMAGE_CHANGED'
    }
    foreach($row in $orcaRows) {
        if(!$row.CreationDate -or !$row.CommandLine){throw 'WINDOW_ORCA_IDENTITY_UNBOUND'}
    }
    [ordered]@{schema='nr-window-current-v1';utc=[DateTime]::UtcNow.ToString('o');readOnly=$true;
        identitiesBound=$true;osRows=$rows;gpuRows=$gpu;ownedObsRows=$ownedRows;orcaImage=$after;
        orcaRows=$orcaRows;historicalDisposalProved=$false;processControl=$false}
}
function Register-WindowOriginal($Owner) {
    if(!$script:WindowSession -or $Owner.process -isnot [Diagnostics.Process] -or
        $Owner.started -isnot [bool] -or $Owner.started -or !$Owner.exitSignal -or !$Owner.exitSignal.Exited){
        throw 'WINDOW_PUBLISH_BEFORE_START'
    }
    $associated=$false
    try {$null=$Owner.process.Id;$associated=$true}catch [InvalidOperationException] {}
    if($associated){throw 'WINDOW_PID_ADOPTION_REFUSED'}
    foreach($old in $script:WindowSession.originals) {
        if([object]::ReferenceEquals($old,$Owner)){throw 'WINDOW_ORIGINAL_ALREADY_REGISTERED'}
    }
    $script:WindowSession.originals.Add($Owner)
}
function Start-WindowOriginal($Owner) {
    Register-WindowOriginal $Owner
    $w=$script:WindowSession
    Assert-WindowPin $w.inputsPin
    $null=Assert-WindowBuild $w.inputs
    $fresh=Read-WindowCurrent $w.inputs $w.references $w.ownedGui
    $w.preStartCensuses.Add($fresh)
    $task='window'
    Start-NrInstanceOriginal $Owner
}
function Assert-WindowOriginals($Originals) {
    if(@($Originals).Count -eq 0){throw 'WINDOW_ORIGINALS_REQUIRED'}
    foreach($o in $Originals) {
        if($o.started -isnot [bool] -or !$o.started -or $o.sdkSettled -isnot [bool] -or !$o.sdkSettled -or
            $o.process -isnot [Diagnostics.Process] -or !$o.identity -or
            ![object]::ReferenceEquals($o.handle,$o.process.SafeHandle) -or
            $o.kernelWait.SafeWaitHandle.DangerousGetHandle() -ne $o.handle.DangerousGetHandle() -or
            !(Test-NrQaOriginalOwnerClosed $o) -or !$o.exitSignal.Exited.WaitOne(0) -or
            ($o.nativeOutput -and $o.nativeOutput.SinkFailed)){throw 'WINDOW_UNCERTAIN_CUSTODY'}
    }
}
function Close-WindowOriginals($Originals) {
    Assert-WindowOriginals $Originals
    $rows=@()
    foreach($o in $Originals) {
        $row=[ordered]@{identity=$o.identity;exit=$o.process.ExitCode;originalReferenceEqual=$true;
            originalKernelSignaled=$true;originalReaped=$true;bothEOF=$true;sdkSettled=$true}
        Close-NrRecordingOriginal $o
        if(!$o.handle.IsClosed -or $o.state -cne 'original-objects-disposed-after-terminal-reap-eof'){
            throw 'WINDOW_UNCERTAIN_CUSTODY'
        }
        $row.handleClosed=$true;$row.resourcesDisposed=$true;$rows+=$row
    }
    $rows
}
function Restore-WindowWrapper($Protocol) {
    Assert-WindowPin $Protocol.before;Assert-WindowPin $Protocol.after
    $live=Get-WindowPin $Protocol.livePath
    if($live.sha256 -cne $Protocol.after.sha256 -or $live.bytes -ne $Protocol.after.bytes){
        throw 'WINDOW_WRAPPER_UNEXPECTED_DRIFT'
    }
    [IO.File]::WriteAllBytes($Protocol.livePath,[IO.File]::ReadAllBytes($Protocol.before.path))
    $restored=Get-WindowPin $Protocol.livePath
    if($restored.sha256 -cne $Protocol.before.sha256 -or $restored.bytes -ne $Protocol.before.bytes){
        throw 'WINDOW_WRAPPER_RESTORE_FAILED'
    }
    $restored
}
