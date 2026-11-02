param([Parameter(Mandatory)][string]$Work,[switch]$Task13Only,[switch]$ClosureOnly,[switch]$SinkOnly,[switch]$DiagnosticR4Only,[switch]$CoreParserOnly,[switch]$CoreControlOnly,[switch]$CoreEventsOnly,[switch]$Task14Only,[switch]$Task14BaselineOnly,[switch]$Task14CapacityOnly,[string]$Task14TestRoot,[switch]$Task14FaultControl)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
. (Join-Path $Work 'tools/host/HostSupport.ps1')
if($Task14CapacityOnly) {
    if(!$Task14TestRoot -or (Test-Path -LiteralPath $Task14TestRoot)){throw 'Fresh Task14 capacity CPU output root required'}
    [void][IO.Directory]::CreateDirectory($Task14TestRoot)
    $runner=Join-Path $Work 'tools/Invoke-NrQa.ps1'
    $tokens=$null;$errors=$null
    $ast=[Management.Automation.Language.Parser]::ParseFile($runner,[ref]$tokens,[ref]$errors)
    if($errors.Count){throw ($errors|Out-String)}
    $definition=$ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Assert-NrInstanceCapacityReceipts'},$true)
    if(!$definition){throw 'Actual capacity validator missing'}
    . ([scriptblock]::Create($definition.Extent.Text))
    function CapacityFixture {
        $sets=@{VIEW=@();JOB=@();RELEASE=@();RECOVERY=@();CLEANUP=@();SCOPE=@()}
        foreach($lane in 0..1) {
            $instance=101+$lane;$ingress=0;$count=0
            foreach($phase in 0..15) {
                $before=$ingress
                $admitted=if($lane){$phase -lt 14}else{$phase -notin @(3,4,10,11,14,15)}
                if($phase -lt 14 -and ($lane -or $phase -notin @(10,11))){++$ingress}
                $generation=if($lane -or $phase -lt 7){1}elseif($phase -lt 9){2}elseif($phase -lt 12){3}else{4}
                $key=$null
                if($admitted) {
                    ++$count
                    $key=@{instance=$instance;content_epoch=1;ingress=$ingress;history_epoch=$generation;resource_generation=$generation;submission_serial=$count}
                    $sets.JOB+=@{cohort='native-transport-capacity';lane=$lane;phase=$phase;recovery_wait=$false;instance=$instance;tick=100+$phase;
                        key=$key;returned=$true;physical_complete=$true;transport_copy=$true;feature18_evaluated=$false}
                }
                $slots=@()
                foreach($slot in 0..2){$slots+=@{slot=$slot;key=$null;consumer_open=$false;producer=1;producer_done=1;worker=1;worker_done=1;consumer=1;consumer_done=1;reusable=$true}}
                $slotFull=!$lane -and $phase -in @(3,4)
                $budget=!$lane -and $phase -in @(10,11)
                if($slotFull){foreach($slot in $slots){$slot.reusable=$false;$slot.consumer_open=$true}}
                foreach($view in 0..2) {
                    $sets.VIEW+=@{cohort='native-transport-capacity';phase=$phase;case='cpu-receipt-control';lane=$lane;view=$view;recovery_wait=$false;
                        instance=$instance;tick=100+$phase;ingress_before=$before;ingress=$ingress;current_admitted=$admitted;current_job=$key;
                        selected=$ingress;kind=$(if($slotFull -or $budget){0}else{1});reason=$(if($slotFull){13}elseif($budget){14}else{0});
                        public_state=$(if($slotFull -or $budget){'bypassed'}else{'priming'});
                        public_reason=$(if($slotFull){'slotsFull'}elseif($budget){'retirementBudget'}else{'none'});
                        placeholder=$budget;content_id=1+8*$lane;current_content_id=1+8*$lane;output_hash="$lane-$phase";raw_hash="$lane-$phase";
                        source_renders_before=$before;source_renders=$ingress;content_epoch=1;history_epoch=$generation;resource_generation=$generation;
                        retired_generations=$(if($budget){2}else{0});held_consumers=$(if($slotFull){3}else{0});admission_reason=$(if($slotFull){13}else{0});
                        selected_job=$null;counters=@{captures=$ingress;dispatches=$count;selections=$phase+1;collected_slots=0;collected_generations=0};slots=$slots}
                }
            }
            $sets.CLEANUP+=@{lane=$lane;instance=$instance;closed=$true;worker_exit=$true;retained=$false;copy_jobs=$count;capture_ingress=$ingress;held_consumers=0;recovery_wait_frames=0}
        }
        foreach($index in 0..4) {
            $purpose=if($index -lt 3){'slots'}else{'retirement'}
            $sets.RELEASE+=@{lane=0;purpose=$purpose;key=@{instance=101};slot=$index%3;required_consumer=10+$index;actual_consumer=10+$index;
                event_observed=$true;consumer_open_before=$true;consumer_open_after=$false}
        }
        foreach($purpose in @('slots','retirement')) {
            $sets.RECOVERY+=@{purpose=$purpose;instance=101;same_instance=$true;resource_generation=$(if($purpose -ceq 'slots'){1}else{4});
                retired_generations=0;current_job=@{instance=101};selected_job=@{instance=101};subscription_before_release=$true;event_observed=$true}
        }
        $sets.SCOPE+=@{fixed_view_rows=96;recovery_wait_frames=0;copy_jobs_a=10;copy_jobs_b=14;native_transport=$true;Feature18_capacity=$false;source_gate_only=$false}
        $sets
    }
    function CapacityStream($Rows) {
        $lines=@()
        foreach($name in @('VIEW','JOB','RELEASE','RECOVERY','CLEANUP','SCOPE')) {
            foreach($row in $Rows[$name]){$lines+='NR_INSTANCE_CAPACITY_'+$name+' '+($row|ConvertTo-Json -Depth 12 -Compress)}
        }
        $lines -join "`n"
    }
    $positive=CapacityFixture
    $null=Assert-NrInstanceCapacityReceipts (CapacityStream $positive)
    $mutations=@(
        @{id='missing-view';edit={param($rows) $rows.VIEW=$rows.VIEW[1..($rows.VIEW.Count-1)]}},
        @{id='fake-Feature18';edit={param($rows) $rows.JOB[0].feature18_evaluated=$true}},
        @{id='foreign-output';edit={param($rows) $rows.VIEW[0].instance=102}},
        @{id='false-slot-bypass';edit={param($rows) $rows.VIEW[9].reason=0}},
        @{id='reusable-full-slot';edit={param($rows) $rows.VIEW[9].slots[0].reusable=$true}},
        @{id='wrong-retirement-generation';edit={param($rows) $rows.VIEW[30].resource_generation=4}},
        @{id='unknown-consumer';edit={param($rows) $rows.RELEASE[0].actual_consumer=[UInt64]::MaxValue}},
        @{id='string-release-boolean';edit={param($rows) $rows.RELEASE[0].event_observed='true'}},
        @{id='string-recovery-boolean';edit={param($rows) $rows.RECOVERY[0].event_observed='true'}},
        @{id='early-close';edit={param($rows) $rows.CLEANUP[0].closed=$false}},
        @{id='foreign-recovery';edit={param($rows) $rows.RECOVERY[0].instance=102}},
        @{id='mutated-B';edit={param($rows) $rows.VIEW[51].history_epoch=2}}
    )
    $refused=@()
    foreach($mutation in $mutations) {
        $rows=CapacityFixture
        & $mutation.edit $rows
        $failure=$null
        try{$null=Assert-NrInstanceCapacityReceipts (CapacityStream $rows)}catch{$failure=$_.Exception.Message}
        if(!$failure -or !$failure.StartsWith('NR_QA_CAPACITY_')){throw ('Mutation not refused: '+$mutation.id)}
        $refused+=@{id=$mutation.id;failure=$failure}
    }
    @{status='PASS';positive=1;refused=$refused;source=(Get-FileHash -LiteralPath $runner).Hash.ToLowerInvariant();
        nativeExecuted=$false;gpuExecuted=$false;physicalEvidence=$false}|
        ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $Task14TestRoot 'RESULT.json') -Encoding UTF8
    Write-Output ('TASK14_CAPACITY_PARSER_CPU_PASS positive=1 mutations='+$refused.Count+' native=false')
    return
}
if($Task14Only -or $Task14BaselineOnly) {
    if(!$Task14TestRoot){throw 'Task14 CPU tests require an explicit private output root'}
    $runner=Join-Path $Work 'tools/Invoke-NrQa.ps1'
    $tokens=$null;$errors=$null
    $ast=[Management.Automation.Language.Parser]::ParseFile($runner,[ref]$tokens,[ref]$errors)
    if($errors.Count){throw ($errors|Out-String)}
    foreach($name in @('Assert-NrInstanceReceipts','Assert-NrInstanceCandidate','Start-NrInstanceOriginal','Test-NrQaOriginalOwnerClosed')) {
        $definition=@($ast.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name},$true))
        if($definition.Count -ne 1){throw "Real runner function missing: $name"}
        . ([scriptblock]::Create($definition[0].Extent.Text))
    }
    $views=@();$dispatch=@();$cleanup=@()
    foreach($step in 0..5) {
        foreach($lane in @(0,1)) {
            $dispatch+=@{lane=$lane;step=$step;instance=$(if($lane -eq 0){101}else{202});tick=11+$step;
                content_epoch=1;history_epoch=1;resource_generation=1;submission_serial=5+$step;ingress=5+$step;
                evaluated=$true;composed=$true;submitted=$true;returned=$true;physical_complete=$true}
        }
        foreach($view in 0..2) {
            foreach($lane in @(0,1)) {
                $id=$step+$(if($lane -eq 0){1}else{9})
                $kind=if($lane -eq 0 -and $step -eq 3){2}else{3}
                $raw=if($step){"$lane-current-$($step-1)"}else{"$lane-current-$step"}
                $views+=@{lane=$lane;step=$step;view=$view;instance=$(if($lane -eq 0){101}else{202});tick=11+$step;
                    ingress=5+$step;selected=4+$step;kind=$kind;content_epoch=1;history_epoch=1;resource_generation=1;
                    content_id=($id-$(if($step){1}else{0}));current_content_id=$id;raw_hash=$raw;current_hash="$lane-current-$step";
                    output_hash=$(if($kind -eq 3){"$lane-nr-$step"}else{$raw});settings_strength=$(if($lane -eq 0){0.25}else{0.75});
                    source_renders=5+$step;retired_generations=0;selected_job=$(if($kind -eq 3){
                        @{instance=$(if($lane -eq 0){101}else{202});content_epoch=1;ingress=4+$step;history_epoch=1;resource_generation=1;submission_serial=4+$step}
                    }else{$null})}
            }
        }
    }
    foreach($lane in @(0,1)) {
        $cleanup+=@{lane=$lane;instance=$(if($lane -eq 0){101}else{202});closed=$true;worker_exit=$true;retained=$false;actual_evaluations=6}
    }
    $baseline=@(foreach($lane in @(0,1)) {
        @{lane=$lane;instance=$(if($lane -eq 0){101}else{202});tick=10;content_epoch=1;history_epoch=1;
            resource_generation=1;submission_serial=4;ingress=4;source_renders=4;current_hash="$lane-current-0";
            content_id=$(if($lane -eq 0){1}else{9});settings_strength=$(if($lane -eq 0){0.25}else{0.75});
            evaluated=$true;composed=$true;submitted=$true;returned=$true;physical_complete=$true;ordinary=$true}
    })
    function InstanceText {
        $lines=@($baseline|ForEach-Object {'NR_INSTANCE_BASELINE '+($_|ConvertTo-Json -Compress -Depth 8)})
        $lines+=@($views|ForEach-Object {'NR_INSTANCE_VIEW '+($_|ConvertTo-Json -Compress -Depth 8)})
        $lines+=@($dispatch|ForEach-Object {'NR_INSTANCE_DISPATCH '+($_|ConvertTo-Json -Compress)})
        $lines+=@($cleanup|ForEach-Object {'NR_INSTANCE_CLEANUP '+($_|ConvertTo-Json -Compress)})
        $lines+='NR_INSTANCE_SCOPE real_filters=2 repeated_OBS_views=true native=true cleanup=complete'
        $lines -join [Environment]::NewLine
    }
    function InstanceRefused([string]$Expected) {
        $refused=$false
        try{$null=Assert-NrInstanceReceipts (InstanceText)}catch{
            if($_.Exception.Message -ne $Expected){throw};$refused=$true
        }
        if(!$refused){throw "Real Task14 validator admitted $Expected"}
    }
    $accepted=Assert-NrInstanceReceipts (InstanceText)
    if($accepted.views.Count -ne 36 -or $accepted.dispatch.Count -ne 12){throw 'Task14 positive receipt counts'}
    $saved=(@{baseline=$baseline;views=$views;dispatch=$dispatch}|ConvertTo-Json -Depth 10)
    foreach($counts in @(@(3,7),@(8,2))) {
        foreach($lane in @(0,1)) {
            $b=$baseline[$lane];$b.ingress=$counts[$lane];$b.source_renders=$counts[$lane]+2
            $b.submission_serial=20+$counts[$lane];$b.tick=100+$counts[$lane]
            foreach($step in 0..5) {
                $d=@($dispatch|Where-Object {$_.lane -eq $lane -and $_.step -eq $step})[0]
                $d.ingress=$b.ingress+1+$step;$d.submission_serial=$b.submission_serial+1+$step;$d.tick=$b.tick+1+$step
                foreach($v in @($views|Where-Object {$_.lane -eq $lane -and $_.step -eq $step})) {
                    $v.ingress=$d.ingress;$v.selected=$d.ingress-1;$v.tick=$d.tick;$v.source_renders=$b.source_renders+1+$step
                    if($v.selected_job){$v.selected_job.ingress=$b.ingress+$step;$v.selected_job.submission_serial=$b.submission_serial+$step}
                }
            }
        }
        $null=Assert-NrInstanceReceipts (InstanceText)
    }
    $restore=$saved|ConvertFrom-Json;$baseline=@($restore.baseline);$views=@($restore.views);$dispatch=@($restore.dispatch)
    $baseline[0].physical_complete=$false;InstanceRefused 'NR_QA_INSTANCE_BASELINE';$baseline[0].physical_complete=$true
    $baseline[0].submission_serial=3;InstanceRefused 'NR_QA_INSTANCE_KEY';$baseline[0].submission_serial=4
    $baseline[0].current_hash='stale-baseline';InstanceRefused 'NR_QA_INSTANCE_HASH';$baseline[0].current_hash='0-current-0'
    $views[0].selected_job.ingress=5;InstanceRefused 'NR_QA_INSTANCE_SELECTED_KEY';$views[0].selected_job.ingress=4
    # A can advance history after its missed cutoff without changing B or
    # either instance-local content epoch.
    $dispatch[8].history_epoch=$dispatch[10].history_epoch=2
    foreach($index in @(24,26,28,30,32,34)){$views[$index].history_epoch=2}
    foreach($index in @(30,32,34)){$views[$index].selected_job.history_epoch=2}
    $null=Assert-NrInstanceReceipts (InstanceText)
    # Equal instance-local epoch 1 is intentional, not a uniqueness failure.
    $dispatch[2].tick=$dispatch[0].tick;$views[6].tick=$views[8].tick=$views[10].tick=$dispatch[0].tick
    InstanceRefused 'NR_QA_INSTANCE_DUPLICATE_EVALUATION'
    $dispatch[2].tick=12;$views[6].tick=$views[8].tick=$views[10].tick=12
    $views[8].content_id=9;InstanceRefused 'NR_QA_INSTANCE_LINEAGE';$views[8].content_id=1
    $views[7].selected_job.instance=101;InstanceRefused 'NR_QA_INSTANCE_SELECTED_KEY';$views[7].selected_job.instance=202
    $views[8].output_hash='foreign';InstanceRefused 'NR_QA_INSTANCE_HASH';$views[8].output_hash='0-nr-1'
    # A's held step2 dispatch cannot resurrect a job at the step3 raw latch.
    $views[18].selected_job=@{instance=101;content_epoch=1;ingress=6;history_epoch=1;resource_generation=1;submission_serial=6}
    InstanceRefused 'NR_QA_INSTANCE_RAW_KEY';$views[18].selected_job=$null
    $views[19].selected_job.instance=101;InstanceRefused 'NR_QA_INSTANCE_SELECTED_KEY';$views[19].selected_job.instance=202
    $dispatch[7].history_epoch=2
    InstanceRefused 'NR_QA_INSTANCE_B_CHANGED';$dispatch[7].history_epoch=1
    $cleanup[0].retained=$true;InstanceRefused 'NR_QA_INSTANCE_CLOSURE';$cleanup[0].retained=$false
    $dispatch+=@{};InstanceRefused 'NR_QA_INSTANCE_SET';$dispatch=$dispatch[0..11]
    if($Task14BaselineOnly) {
        Write-Output 'TASK14_BASELINE_CPU_PASS variable_positive=2 baseline_negative=3 existing_mutations=true native=false'
        return
    }
    # Execute the real mapping statements, stopping before HostSupport or any I/O.
    $mapping=@()
    foreach($statement in $ast.EndBlock.Statements) {
        if($statement.Extent.Text -eq ". (Join-Path `$PSScriptRoot 'host/HostSupport.ps1')"){break}
        $mapping+=$statement.Extent.Text
    }
    foreach($verifier in @($false,$true)) {
        foreach($route in @('two-instances','host-foundation')) {
            $mapped=& {
                $PSScriptRoot=Join-Path $Work 'tools'
                $Scenario=$route;$Evidence=Join-Path $Work '.omo/evidence/obs-nr-native-filter-20260930/task-14'
                if($verifier){$Evidence=Join-Path $Evidence 'verifier'}
                $HostRoot=Join-Path $Work $(if($verifier){'out/task-14-verifier/host'}else{'out/task-14/host'})
                $LifecycleNative=$false;$LifecycleDiagnosticR4=$false;$CoreInitControl=$false
                $Task14CandidateManifest='';$Task14CandidateSha256='';$Task14CandidateBuild=''
                $Task15CandidateManifest='';$Task15CandidateSha256='';$Task15CandidateBuild='';$Task15Attempt=''
                $Task16CandidateManifest='';$Task16CandidateSha256='';$Task17CandidateManifest='';$Task17CandidateSha256=''
                $Task27CandidateManifest='';$Task27CandidateSha256=''
                $Task14Attempt=''
                $mappingTokens=$null;$mappingErrors=$null
                $mappingAst=[Management.Automation.Language.Parser]::ParseInput(
                    ($mapping -join [Environment]::NewLine),$runner,[ref]$mappingTokens,[ref]$mappingErrors)
                if($mappingErrors.Count){throw ($mappingErrors|Out-String)}
                . ($mappingAst.GetScriptBlock())
                @{task=$task;owned=$owned;evidence=$evidenceRoot;root=$root}
            }
            if($mapped.task -ne 'task-14' -or $mapped.owned -ne $mapped.root -or
                $mapped.evidence -ne (Join-Path $Work ('.omo/evidence/obs-nr-native-filter-20260930/task-14'+$(if($verifier){'/verifier'}else{''})))){throw 'Task14 real producer/verifier mapping'}
        }
    }
    # Only hashing/input enumeration are controlled; the real preflight executes.
    & {
        $candidate=Join-Path $Work '.omo/evidence/obs-nr-native-filter-20260930/task-14/instance-integration-20261011/b'
        $build=$candidate;$manifest=Join-Path $candidate 'candidate-freeze.json';$sha='a'*64
        $reused=Join-Path $Work 'out/task-13-diagnostic-r4/native'
        $artifacts=@('obs-nr-filter.dll','nr-qa-host.exe')|ForEach-Object {@{path=(Join-Path $build $_);sha256=$sha}}
        $artifacts+=@{path=(Join-Path $reused 'Debug/nr-driver-inspect.exe');sha256=$sha}
        $artifacts+=@('nr_codec.dxil','nr_color.dxil','nr_guides.cso','nr_interop_vertex.cso','nr_interop_pixel.cso','nr_obs_analysis.cso')|ForEach-Object {@{path=(Join-Path $reused $_);sha256=$sha}}
        $sourcePaths=@('CMakeLists.txt','tools/driver-inspect.cpp','tools/Invoke-NrQa.ps1','tools/tests/bootstrap_runner_tests.ps1','provenance/inputs.spec.json','provenance/sdk-lock.json')|ForEach-Object {Join-Path $Work $_}
        $freeze=@{build_origin='StageB';build=$build;artifacts=$artifacts;sources=@($sourcePaths|ForEach-Object {@{path=$_;sha256=$sha}});
            reused_inputs=@(@{path=(Join-Path $reused 'core.obj');sha256=$sha},@{path=(Join-Path $reused 'core.lib');sha256=$sha});
            main_sdk_overlay=@{path=(Join-Path (Split-Path $candidate) 'main-sdk-overlay.cpp');sha256=$sha}}
        function Get-FileHash {param($LiteralPath,$Algorithm) @{Hash=$sha}}
        function Get-Content {param($LiteralPath,[switch]$Raw) $freeze|ConvertTo-Json -Depth 8}
        function Get-ChildItem {param($LiteralPath,[switch]$File,[switch]$Recurse) @()}
        $null=Assert-NrInstanceCandidate $manifest $sha $candidate $build $Work
        foreach($case in @('stock','manifest','origin','shader','source','sdk','main')) {
            $actualBuild=$build;$actualSha=$sha
            switch($case) {
                'stock'{$actualBuild=Join-Path $Work 'out/task-13/native/Debug'}
                'manifest'{$actualSha='b'*64}
                'origin'{$freeze.build_origin='historical'}
                'shader'{$artifacts[3].sha256='b'*64}
                'source'{$freeze.sources[0].sha256='b'*64}
                'sdk'{$freeze.reused_inputs[0].sha256='b'*64}
                'main'{$freeze.main_sdk_overlay.sha256='b'*64}
            }
            $refused=$false;try{$null=Assert-NrInstanceCandidate $manifest $actualSha $candidate $actualBuild $Work}catch{$refused=$true}
            if(!$refused){throw "Task14 real candidate preflight admitted $case"}
            $freeze.build_origin='StageB';$artifacts[3].sha256=$sha;$freeze.sources[0].sha256=$sha;$freeze.reused_inputs[0].sha256=$sha;$freeze.main_sdk_overlay.sha256=$sha
        }
    }
    # A real CPU child blocks on a named event, never a sleep or timing window.
    New-Item -ItemType Directory -Force $Task14TestRoot|Out-Null
    # Capture the actual event delegate before disposal, as a queued Process callback can.
    # Both a live notification and a callback already captured at disposal must be safe.
    Add-Type -TypeDefinition @'
using System;
using System.Diagnostics;
using System.Reflection;
public static class NrExitCallbackProbe {
    public static EventHandler Capture(Process process) {
        var field=typeof(Process).GetField("onExited",BindingFlags.Instance|BindingFlags.NonPublic);
        if(field==null)throw new InvalidOperationException("Process Exited delegate field missing");
        return (EventHandler)field.GetValue(process);
    }
    public static void Invoke(EventHandler callback) {callback(null,EventArgs.Empty);}
}
'@
    $probeProcess=[Diagnostics.Process]::new()
    $probeSignal=[NrProcessExit]::new($probeProcess)
    try {
        $queuedCallback=[NrExitCallbackProbe]::Capture($probeProcess)
        [NrExitCallbackProbe]::Invoke($queuedCallback)
        if(!$probeSignal.Exited.WaitOne(0)){throw 'Live process exit notification lost'}
        $probeSignal.Dispose()
        [NrExitCallbackProbe]::Invoke($queuedCallback)
        $probeSignal.Dispose()
    } finally {$probeSignal.Dispose();$probeProcess.Dispose()}
    # The extracted Start-NrInstanceOriginal reads the wrapper's script-scope $task; this branch runs Task14.
    $task='task-14'
    foreach($inject in @($false,$true)) {
        $nonce=[Guid]::NewGuid().ToString('N')
        $readyName='Local\nr-instance-cpu-'+$nonce+'-ready';$stopName='Local\nr-instance-cpu-'+$nonce+'-stop'
        $ready=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,$readyName)
        $stop=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,$stopName)
        $diagnosis=@(New-RetainedHostEvents 'lifecycle' $false $stopName)
        $p=[Diagnostics.Process]::new()
        $code="`$ready=[Threading.EventWaitHandle]::OpenExisting('$readyName');`$stop=[Threading.EventWaitHandle]::OpenExisting('$stopName');Write-Output 'INSTANCE_CPU_READY';`$ready.Set()|Out-Null;if(!`$stop.WaitOne(15000)){exit 3};[Console]::Error.WriteLine('INSTANCE_CPU_CLOSED');exit 0"
        if($Task14FaultControl){$code=$code.Replace(';exit 0',';exit 9')}
        $p.StartInfo.FileName="$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe"
        $p.StartInfo.Arguments='-NoProfile -EncodedCommand '+[Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($code))
        $p.StartInfo.UseShellExecute=$false;$p.StartInfo.CreateNoWindow=$true
        $p.StartInfo.RedirectStandardOutput=$true;$p.StartInfo.RedirectStandardError=$true;$p.EnableRaisingEvents=$true
        $owner=@{process=$p;handle=$null;started=$false;closed=$false;identity=$null;events=@($ready,$stop);retainedEvents=$diagnosis;
            kernelWait=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset);kernelAttached=$false;
            exitSignal=[NrProcessExit]::new($p);nativeOutput=$null;outputStarted=$false;errorStarted=$false;stdout=$null;stderr=$null;state='original-owner-published'}
        $owner.nativeOutput=[NrHostOutput]::new($p,(Join-Path $Task14TestRoot ($nonce+'.log')),[IO.StringWriter]::new())
        $script:NrQaOriginalOwner=$owner
        $realIdentity=${function:Identity}
        try {
            if($inject){function Identity {param($Process) throw 'CPU_POST_START_IDENTITY_FAILURE'}}
            $failed=$false
            try{Start-NrInstanceOriginal $owner}catch{if(!$inject -or $_.Exception.Message -ne 'CPU_POST_START_IDENTITY_FAILURE'){throw};$failed=$true}
            if($failed -ne $inject -or ![object]::ReferenceEquals($script:NrQaOriginalOwner,$owner) -or
                ![object]::ReferenceEquals($owner.process,$p) -or ![object]::ReferenceEquals($owner.handle,$p.SafeHandle) -or
                !$owner.kernelAttached -or $owner.handle.IsClosed -or $owner.kernelWait.WaitOne(0)){throw 'Actual original owner lost at post-start uncertainty'}
            if(!$ready.WaitOne(15000)){throw 'Task14 CPU child ready event timeout; original retained'}
            if($inject) {
                if($owner.outputStarted -ne $false -or $owner.errorStarted -ne $false){throw 'Streams preceded original identity capture'}
                $p.BeginOutputReadLine();$owner.outputStarted=$true;$p.BeginErrorReadLine();$owner.errorStarted=$true
            }
            if(Test-NrQaOriginalOwnerClosed $owner){throw 'Live original falsely closed'}
            foreach($suffix in @('.host-diagnose','.host-diagnosed')) {
                $opened=[Threading.EventWaitHandle]::OpenExisting($stopName+$suffix);$opened.Dispose()
            }
            $stop.Set()|Out-Null
            if(!$owner.kernelWait.WaitOne(15000) -or !$p.WaitForExit(5000) -or
                !$owner.nativeOutput.OutputClosed.WaitOne(5000) -or !$owner.nativeOutput.ErrorClosed.WaitOne(5000) -or
                !(Test-NrQaOriginalOwnerClosed $owner)){throw 'Task14 CPU original terminal/reap/EOF unresolved'}
            if($p.ExitCode -or !$owner.nativeOutput.Text.Contains('INSTANCE_CPU_READY') -or !$owner.nativeOutput.Text.Contains('INSTANCE_CPU_CLOSED')){throw 'Task14 CPU original readers lost'}
        }finally {
            ${function:Identity}=$realIdentity
            # Explicit parent-style close only after actual terminal + reap + both EOFs.
            if($owner.closed){$owner.nativeOutput.Dispose();$owner.exitSignal.Dispose();$owner.kernelWait.Dispose();foreach($event in $owner.events+$owner.retainedEvents){$event.Dispose()};$p.Dispose()}
        }
    }
    Write-Output 'PASS Task14 real AST validator, routing, candidate preflight and original-owner CPU event tests; native trial not performed'
    return
}
if($CoreEventsOnly) {
    $stop='Local\nr-r6b-cpu-events-'+[Guid]::NewGuid().ToString('N')
    $events=@(New-RetainedHostEvents 'native-model' $true $stop)
    try {
        if($events.Count -ne 2){throw 'CORE_EVENT_BEHAVIOR_RED actual Core route requires two host diagnosis events'}
        foreach($suffix in @('.host-diagnose','.host-diagnosed')) {
            $opened=[Threading.EventWaitHandle]::OpenExisting($stop+$suffix);$opened.Dispose()
        }
        if(@(New-RetainedHostEvents 'native-model' $false ($stop+'-ordinary')).Count){throw 'Ordinary model event behavior widened'}
    }finally{foreach($event in $events){$event.Dispose()}}
    foreach($suffix in @('.host-diagnose','.host-diagnosed')) {
        $absent=$false
        try{$opened=[Threading.EventWaitHandle]::OpenExisting($stop+$suffix);$opened.Dispose()}catch [Threading.WaitHandleCannotBeOpenedException]{$absent=$true}
        if(!$absent){throw 'CPU diagnosis event owner leak'}
    }
    $candidate=Join-Path $Work 'out/task-13-core-control-r6b/verified-host';$build=Join-Path $Work 'out/task-13-core-control-r6b/native/Debug'
    function Get-FileHash {param([string]$Path) @{Hash='controlled-exact-build-hash'}}
    $inventory=@(@{path=(Join-Path $candidate 'bin/64bit/nr-qa-host.exe');sha256='controlled-exact-build-hash'},@{path=(Join-Path $candidate 'obs-plugins/64bit/obs-nr-filter.dll');sha256='controlled-exact-build-hash'})
    Assert-ReadyHostPair $inventory $candidate $build 'native-model' $true
    $inventory[1].sha256='stale-plugin';$refused=$false
    try{Assert-ReadyHostPair $inventory $candidate $build 'native-model' $true}catch{$refused=$_.Exception.Message -eq 'NR_QA_BOOTSTRAP_FRESH_BINARY_BINDING'}
    if(!$refused){throw 'CORE ready pair accepted stale plugin'}
    Write-Output 'PASS actual runner shared event-creation CORE two/ordinary zero/exact names/disposal absence and ready host/plugin pair stale refusal CPU_only=true nativeIPC0; hashes controlled dependencies'
    return
}
if($CoreParserOnly) {
    $calls=@(0..6|ForEach-Object {@{stage=$_;count=0;first=@{enter=0;returned=0;result_known=$false;remaining_known=$false};latest=@{enter=0;returned=0;result_known=$false;remaining_known=$false}}})
    $calls[0].count=1;$calls[0].first=@{enter=1;returned=2;result_known=$true;remaining_known=$false;result=1;module=3;procedure=4;thread=5};$calls[0].latest=$calls[0].first
    $calls[5].count=1;$calls[5].first=@{enter=3;returned=4;result_known=$true;remaining_known=$true;result=1;remaining=0;module=3;procedure=6;thread=5};$calls[5].latest=$calls[5].first
    $v=@{owner=7;instance=8;generation=9;device=10;coherent=$true;nested_core_observed=$false;calls=$calls}
    $row=@{stage='normal-close';request=0;pending=$false;normal_closed=$true;callback_attached=$false;exception=$false;model_coherent=$true;completed_steps=6;module_error=0;vendor_trace=$v}
    $capture=@{captures=1;fault_code=57369;dispatch_returned=$true;observed=$true;raw_hash='abc';analysis_hash='abc';created=$false;evaluated=$false;composed=$false;submitted=$false;eligible=$false;resources=$false;runtime_module=$false;applied=$false}
    $context=@{instance=8;device=10;queue=11;vendor_id=4318;device_id=12;software=$false;adapter_description_utf16=@(78,86);renderer_luid_high=1;renderer_luid_low=2;context_luid_high=1;context_luid_low=2;worker_luid_high=1;worker_luid_low=2}
    function RawContext {'NR_CORE_CONTROL_CONTEXT '+($context|ConvertTo-Json -Depth 5 -Compress)}
    function RawCore($stage,$sequence){$row.stage=$stage;$row.request=$sequence;'NR_CORE_CONTROL '+($row|ConvertTo-Json -Depth 20 -Compress)}
    $first=RawCore 'after-first-capture' 0;$normal=RawCore 'normal-close' 0
    Assert-CoreControl @(Read-CoreControlRows ($first+[Environment]::NewLine+$normal)) @($capture) @(Read-CoreControlContext (RawContext))
    $current=(RawContext)+[Environment]::NewLine+(RawCore 'retained-request' 1);$envelope=Get-CoreControlEnvelope $current 1
    if($envelope.failure -or $envelope.current.Count -ne 1){throw 'Core current envelope lost'}
    foreach($raw in @($current+[Environment]::NewLine+$current,$current,'NR_CORE_CONTROL {bad}')) {
        $sequence=if($raw -eq $current){2}else{1}
        if(!(Get-CoreControlEnvelope $raw $sequence).failure){throw 'Core bad reply admitted'}
    }
    $calls[5].first.result=3134193671
    $refused=$false;try{Assert-CoreControl @(Read-CoreControlRows ($first+[Environment]::NewLine+(RawCore 'normal-close' 0))) @($capture) @(Read-CoreControlContext (RawContext))}catch{$refused=$_.Exception.Message -eq 'NR_QA_CORE_CLOSE_REFUSED'}
    if(!$refused){throw 'NotInitialized remaining0 admitted'}
    $calls[5].first.result=1;$calls[1].count=1
    if(!(Get-CoreControlEnvelope (RawCore 'retained-request' 1) 1).failure){throw 'NR entered admitted'}
    $calls[1].count=0;$v.owner=0
    if(!(Get-CoreControlEnvelope (RawCore 'retained-request' 1) 1).failure){throw 'Unknown owner admitted'}
    $v.owner=7;$context.worker_luid_low=3
    if(!(Get-CoreControlEnvelope ((RawContext)+[Environment]::NewLine+(RawCore 'retained-request' 1)) 1).failure){throw 'Mismatched LUID admitted'}
    $context.worker_luid_low=2;$context.software=$true
    if(!(Get-CoreControlEnvelope ((RawContext)+[Environment]::NewLine+(RawCore 'retained-request' 1)) 1).failure){throw 'Software adapter admitted'}
    Write-Output $current
    Write-Output 'PASS shared CORE parser/current-supervisor envelope positive and duplicate/stale/malformed/NotInitialized/NR/unknown-owner refusals CPU_only=true'
    return
}
if($SinkOnly) {
    $root=Join-Path $Work ('.omo/evidence/obs-nr-native-filter-20260930/task-13/codex-continuation-20261004/physical-readiness-r3/sink-'+[Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory $root|Out-Null
    $p=[Diagnostics.Process]::new()
    $p.StartInfo.FileName="$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe"
    $p.StartInfo.Arguments='-NoProfile -Command "Write-Output B2_STDOUT; [Console]::Error.WriteLine(''B2_STDERR''); exit 0"'
    $p.StartInfo.UseShellExecute=$false;$p.StartInfo.CreateNoWindow=$true
    $p.StartInfo.RedirectStandardOutput=$true;$p.StartInfo.RedirectStandardError=$true
    $p.EnableRaisingEvents=$true;$exited=[NrProcessExit]::new($p)
    $output=[NrHostOutput]::new($p,(Join-Path $root 'raw.log'))
    $field=[NrHostOutput].GetField('log',[Reflection.BindingFlags]'Instance,NonPublic')
    $field.GetValue($output).Dispose()
    if(!$p.Start()){throw 'Sink CPU child start'}
    $p.BeginOutputReadLine();$p.BeginErrorReadLine()
    if(!$exited.Exited.WaitOne(15000)){throw 'Sink CPU child unresolved'}
    $p.WaitForExit()
    if(!$output.Text.Contains('B2_STDOUT') -or !$output.Text.Contains('B2_STDERR')){throw 'Raw sink diagnostics lost'}
    if(!$output.SinkFailed){throw 'Sink failure not observable FAIL'}
    Write-Output ('OBSERVED_FAIL sink_failure='+$output.SinkFailure)
    $output.Dispose();$exited.Dispose();$p.Dispose()
    Write-Output 'PASS sink containment regression; underlying sink outcome remains FAIL; CPU only'
    return
}
if($ClosureOnly) {
    $closed=@{stage='post-source-destruction';instance=1;sequence=3;block=0;normal_closed=$true;
        stop=$true;closed=$true;worker_exit=$true;pipeline=2;retiring_count=0;generations=@(@{id=0},@{id=0},@{id=0})}
    $raw='NR_LIFECYCLE_CLOSURE '+($closed|ConvertTo-Json -Compress)
    $parsed=@(Read-LifecycleClosure $raw)
    Assert-LifecycleClosure $parsed
    foreach($field in @('block','pipeline','retiring_count','worker_exit')) {
        $old=$closed[$field];$closed[$field]=if($field -eq 'worker_exit'){$false}else{1}
        $refused=$false
        try{Assert-LifecycleClosure @(Read-LifecycleClosure ('NR_LIFECYCLE_CLOSURE '+($closed|ConvertTo-Json -Compress)))}catch{$refused=$true}
        if(!$refused){throw "Closure admitted false field $field"}
        $closed[$field]=$old
    }
    $closed.normal_closed=$false;$closed.block=10
    $refusal=@(Read-LifecycleClosure ('NR_LIFECYCLE_CLOSURE '+($closed|ConvertTo-Json -Compress)))
    if($refusal[0].block -ne 10 -or $refusal[0].normal_closed){throw 'Refusal diagnostics changed'}
    $closed.normal_closed=$true;$closed.block=0
    $g=@{id=2;closed=$true;bridge=2;generation=2;cutoff_leases=0;graphics=5;graphics_done=5;has_model=$true;
        model_settlement=2;feature_release=2;ofa_release=2;module_release=2;core_close=2;coherent=$true;
        exception=0;module_error=0;core_remaining=0;core_called=$true;core_result=1;module_required=$true;module_present=$false;
        slots=@(@{occupied=$true;ready=$true;returned=$true;pin=$false;producer=2;producer_done=2;worker=3;worker_done=3;consumer=4;consumer_done=4})}
    $closed.generations[0]=$g
    Assert-LifecycleClosure @(Read-LifecycleClosure ('NR_LIFECYCLE_CLOSURE '+($closed|ConvertTo-Json -Depth 8 -Compress)))
    foreach($field in @('bridge','generation','cutoff_leases','graphics_done','model_settlement','feature_release','ofa_release',
        'module_release','core_close','coherent','exception','module_error','core_remaining','core_result','module_present')) {
        $old=$g[$field]
        $g[$field]=if($field -eq 'coherent'){$false}elseif($field -eq 'module_present'){$true}elseif($field -eq 'core_result'){0}else{1}
        $refused=$false
        try{Assert-LifecycleClosure @(Read-LifecycleClosure ('NR_LIFECYCLE_CLOSURE '+($closed|ConvertTo-Json -Depth 8 -Compress)))}catch{$refused=$true}
        if(!$refused){throw "Generation branch admitted $field"}
        $g[$field]=$old
    }
    $slot=$g.slots[0]
    foreach($field in @('ready','returned','pin','producer_done','worker_done','consumer_done')) {
        $old=$slot[$field];$slot[$field]=if($field -eq 'pin'){$true}elseif($field -in @('ready','returned')){$false}else{[UInt64]::MaxValue.ToString()}
        $refused=$false
        try{Assert-LifecycleClosure @(Read-LifecycleClosure ('NR_LIFECYCLE_CLOSURE '+($closed|ConvertTo-Json -Depth 8 -Compress)))}catch{$refused=$true}
        if(!$refused){throw "Slot branch admitted $field"}
        $slot[$field]=$old
    }
    $nonce=[Guid]::NewGuid().ToString('N')
    $eventNames=@('ready','request','reply','stop','exit')|ForEach-Object {'Local\nr-r2-cpu-'+$nonce+'-'+$_}
    $events=@($eventNames|ForEach-Object {[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,$_ )})
    $diagnoseName=$eventNames[3]+'.diagnose'
    $diagnose=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::AutoReset,$diagnoseName)
    $logRoot=Join-Path $Work ('.omo/evidence/obs-nr-native-filter-20260930/task-13/codex-continuation-20261004/physical-readiness-r2/cpu-supervisor-'+$nonce)
    New-Item -ItemType Directory -Path $logRoot|Out-Null
    $code=@'
$ErrorActionPreference='Stop'
$request=[Threading.EventWaitHandle]::OpenExisting('REQUEST_EVENT')
$diagnose=[Threading.EventWaitHandle]::OpenExisting('DIAGNOSE_EVENT')
Write-Output 'NR_QA_RETAINED_REPLY {"request":99,"normal_closed":false}'
$ready=[Threading.EventWaitHandle]::OpenExisting('READY_EVENT')
if(!$ready.WaitOne(15000)){exit 4}
. 'HOST_SUPPORT'
$bad=Get-Content -Raw 'SUPERVISOR_RECEIPT'|ConvertFrom-Json
$bad.host.image='wrong-owned-image'
$bad|ConvertTo-Json -Depth 8|Set-Content -Encoding UTF8 'BAD_RECEIPT'
$refused=$false
try{Request-RetainedHost 'BAD_RECEIPT'}catch{$refused=$_.Exception.Message -eq 'NR_QA_RETAINED_IDENTITY_REFUSED'}
if(!$refused){exit 5}
Write-Output 'CPU_EXTERNAL_WRONG_IDENTITY_REFUSED'
foreach($sequence in 1..2) {
    $response=[Threading.EventWaitHandle]::OpenExisting('DIAGNOSE_EVENT.reply')
    $response.Reset()|Out-Null
    $diagnose.Set()|Out-Null
    if(!$request.WaitOne(15000)){exit 3}
    $request.Reset()|Out-Null
    if($sequence -eq 1) {
        if(!$response.WaitOne(30000)){exit 6}
        $receipt=Get-Content -Raw 'SUPERVISOR_RECEIPT'|ConvertFrom-Json
        if($receipt.request -ne 1 -or $receipt.fresh_reply -or !$receipt.handles_owned -or $receipt.host_exited){exit 7}
        Write-Output 'CPU_NO_RESPONSE_BOUNDED_SUPERVISOR_STILL_OWNS_HANDLES'
        $response.Dispose()
        continue
    }
    Write-Output ('NR_QA_RETAINED_REPLY {"request":'+$sequence+',"normal_closed":false}')
    if(!$response.WaitOne(15000)){exit 8}
    $receipt=Get-Content -Raw 'SUPERVISOR_RECEIPT'|ConvertFrom-Json
    if($receipt.request -ne 2 -or !$receipt.fresh_reply -or !$receipt.handles_owned -or $receipt.host_exited){exit 9}
    Write-Output 'CPU_CURRENT_REPLY_CONFIRMED_WHILE_HOST_AND_SUPERVISOR_LIVE'
    $response.Dispose()
}
$request.Dispose();$diagnose.Dispose()
exit 0
'@
    $code=$code.Replace('REQUEST_EVENT',$eventNames[1]).Replace('DIAGNOSE_EVENT',$diagnoseName).
        Replace('READY_EVENT',$eventNames[0]).Replace('HOST_SUPPORT',(Join-Path $Work 'tools/host/HostSupport.ps1')).
        Replace('SUPERVISOR_RECEIPT',(Join-Path $logRoot 'supervisor.json')).Replace('BAD_RECEIPT',(Join-Path $logRoot 'wrong-identity.json'))
    $p=[Diagnostics.Process]::new()
    $p.StartInfo.FileName="$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe"
    $p.StartInfo.Arguments='-NoProfile -EncodedCommand '+[Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($code))
    $p.StartInfo.UseShellExecute=$false;$p.StartInfo.CreateNoWindow=$true
    $p.StartInfo.RedirectStandardOutput=$true;$p.StartInfo.RedirectStandardError=$true
    $p.EnableRaisingEvents=$true;$exitSignal=[NrProcessExit]::new($p)
    $output=[NrHostOutput]::new($p,(Join-Path $logRoot 'raw.log'))
    try {
        if(!$p.Start()){throw 'CPU supervisor child start'}
        $p.BeginOutputReadLine();$p.BeginErrorReadLine()
        $identity=Identity $p
        if(!$output.Reply.WaitOne(15000) -or $output.ReplySequence -ne 99){throw 'CPU stale reply setup'}
        Wait-RetainedHost $p $identity $events $exitSignal $output $diagnoseName (Join-Path $logRoot 'supervisor.json')
        $final=Get-Content -Raw (Join-Path $logRoot 'supervisor.json')|ConvertFrom-Json
        if($final.state -ne 'supervised-exited' -or $final.request -ne 2 -or $final.exit -ne 0 -or $final.result -ne 'FAIL'){
            throw 'CPU supervisor ownership/sequence/exit classification'
        }
        if(!$output.OutputClosed.WaitOne(15000) -or !$output.ErrorClosed.WaitOne(15000)){throw 'CPU output drain'}
        if(!$output.Text.Contains('CPU_EXTERNAL_WRONG_IDENTITY_REFUSED')){throw 'External wrong identity not refused'}
        if(!$output.Text.Contains('CPU_NO_RESPONSE_BOUNDED_SUPERVISOR_STILL_OWNS_HANDLES')){throw 'Bounded unresolved supervision missing'}
    } finally {
        if($p.HasExited){$output.Dispose();$exitSignal.Dispose();$p.Dispose();foreach($event in $events){$event.Dispose()};$diagnose.Dispose()}
    }
    $p=[Diagnostics.Process]::new()
    $p.StartInfo.FileName="$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe"
    $p.StartInfo.Arguments='-NoProfile -Command exit 7'
    $p.StartInfo.UseShellExecute=$false;$p.StartInfo.CreateNoWindow=$true
    $p.StartInfo.RedirectStandardOutput=$true;$p.StartInfo.RedirectStandardError=$true
    $p.EnableRaisingEvents=$true;$exitSignal=[NrProcessExit]::new($p)
    $output=[NrHostOutput]::new($p,(Join-Path $logRoot 'unexpected-exit.log'))
    $ready=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset)
    try {
        if(!$p.Start()){throw 'Unexpected-exit CPU child start'}
        $p.BeginOutputReadLine();$p.BeginErrorReadLine()
        $identity=Identity $p
        Wait-RetainedHost $p $identity @($ready) $exitSignal $output ($diagnoseName+'-unexpected') (Join-Path $logRoot 'unexpected-exit.json')
        $final=Get-Content -Raw (Join-Path $logRoot 'unexpected-exit.json')|ConvertFrom-Json
        if($final.exit -ne 7 -or $final.result -ne 'FAIL' -or $final.request -ne 0){throw 'Unexpected child exit falsely accepted'}
        if(!$output.OutputClosed.WaitOne(15000) -or !$output.ErrorClosed.WaitOne(15000)){throw 'Unexpected-exit output drain'}
    } finally {if($p.HasExited){$output.Dispose();$exitSignal.Dispose();$ready.Dispose();$p.Dispose()}}
    Write-Output 'PASS actual structured lifecycle closure parser/validator and refusal retention; CPU only'
    Write-Output 'PASS actual retained supervisor process/identity/subscribed exit/IPC/stale sequence/two requests/output ownership; CPU dependency, no OBS/GPU'
    return
}
$runner=Join-Path $Work 'tools/Invoke-NrQa.ps1'
$runtime='C:/Program Files/obs-studio/bin/64bit/nvngx_dlssnr.dll'
$task=if($Task13Only){'task-13'}else{'task-12'}
$scenario=if($Task13Only){'lifecycle'}else{'native-bootstrap'}
$base=Join-Path $Work ('.omo/evidence/obs-nr-native-filter-20260930/'+$task)
function Child([string]$Code) {
    if($CoreControlOnly){$Code=$Code.Replace('NativeDiagnosticR4','NativeCoreInitControl').Replace('Local\nr-task13-r4-','Local\nr-task13-core-r6b-').Replace('-Scenario lifecycle -LifecycleNative -LifecycleDiagnosticR4','-Scenario native-model -CoreInitControl')}
    if($DiagnosticR4Only -or $CoreControlOnly) {
        $Code=@'
$ErrorActionPreference='Stop'
Import-Module Microsoft.PowerShell.Utility
function global:New-Item {throw 'CPU_SIDE_EFFECT_BARRIER:New-Item'}
function global:Copy-Item {throw 'CPU_SIDE_EFFECT_BARRIER:Copy-Item'}
function global:Set-Content {throw 'CPU_SIDE_EFFECT_BARRIER:Set-Content'}
function global:Add-Content {throw 'CPU_SIDE_EFFECT_BARRIER:Add-Content'}
function global:Start-Process {throw 'CPU_SIDE_EFFECT_BARRIER:Start-Process'}
'@ + [Environment]::NewLine + $Code
    }
    $p=[Diagnostics.Process]::new()
    $p.StartInfo.FileName="$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe"
    $p.StartInfo.Arguments='-NoProfile -ExecutionPolicy Bypass -EncodedCommand '+[Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($Code))
    $p.StartInfo.UseShellExecute=$false;$p.StartInfo.CreateNoWindow=$true
    $p.StartInfo.RedirectStandardOutput=$true;$p.StartInfo.RedirectStandardError=$true
    $p.EnableRaisingEvents=$true;$signal=[NrProcessExit]::new($p)
    try {
        if(!$p.Start()){throw 'Bootstrap guard child start'}
        if($DiagnosticR4Only -or $CoreControlOnly){$cpuIdentity=Identity $p}
        $out=$p.StandardOutput.ReadToEndAsync();$err=$p.StandardError.ReadToEndAsync()
        if(!$signal.Exited.WaitOne(20000)){throw 'Exact CPU guard child timeout; owner retained'}
        if(!$p.WaitForExit(5000)){throw 'CPU guard reaping failed'}
        $stdout=$out.GetAwaiter().GetResult();$stderr=$err.GetAwaiter().GetResult()
        if($DiagnosticR4Only -or $CoreControlOnly){$script:diagnosticChildren+=@{identity=$cpuIdentity;exit=$p.ExitCode;terminal=$p.HasExited;reaped=$true}}
        @{exit=$p.ExitCode;stdout=$stdout;stderr=$stderr;text=($stdout+$stderr)}
    }finally{if($p.HasExited){$signal.Dispose();$p.Dispose()}}
}
if($DiagnosticR4Only -or $CoreControlOnly) {
    $runner=Join-Path $Work 'tools/Invoke-NrQa.ps1'
    $runtime=Join-Path $env:ProgramFiles 'obs-studio/bin/64bit/nvngx_dlssnr.dll'
    $freshEvidence=Join-Path $Work '.omo/evidence/obs-nr-native-filter-20260930/task-13/codex-continuation-20261004/diagnostic-r4-verified'
    $freshRoot=Join-Path $Work 'out/task-13-diagnostic-r4/verified-host'
    $build=Join-Path $Work 'out/task-13-diagnostic-r4/native/Debug'
    if($CoreControlOnly) {
        $freshEvidence=Join-Path $Work '.omo/evidence/obs-nr-native-filter-20260930/task-13/codex-continuation-20261004/core-control-r6b-physical'
        $freshRoot=Join-Path $Work 'out/task-13-core-control-r6b/verified-host'
        $build=Join-Path $Work 'out/task-13-core-control-r6b/native/Debug'
    }
    $calls=0
    $script:diagnosticChildren=@()
    function Guard([string]$Code,[string]$Expected) {
        if($CoreControlOnly){$Expected=$Expected.Replace('NR_QA_DIAGNOSTIC_ARTIFACT_CHANGED','NR_QA_CORE_ARTIFACT_CHANGED').Replace('NR_QA_DIAGNOSTIC_CPU_MODE_REFUSED','NR_QA_CORE_CPU_MODE_REFUSED')}
        $result=Child $Code
        if(!$result.exit -or !$result.text.Contains($Expected)){throw ("Expected "+$Expected+"; actual "+$result.text)}
        $script:calls++
        Write-Output ("PASS actual guard "+$Expected)
    }
    $missingFiles=if($CoreControlOnly){@('nr-qa-host.exe','nr-driver-inspect.exe')}else{@('nr-qa-host.exe','nr_obs_analysis.cso','nr-driver-inspect.exe')}
    foreach($missing in $missingFiles) {
        $probe=@'
function global:Get-Process {@()}
$global:seen=[Collections.Generic.List[string]]::new()
function global:Test-Path {
    [CmdletBinding()]param([Parameter(Position=0)][string]$Path,[string]$LiteralPath,[string]$PathType)
    $candidate=if($LiteralPath){$LiteralPath}else{$Path}
    if([IO.Path]::GetFileName($candidate) -eq '@MISSING@'){$global:seen.Add($candidate);return $false}
    Microsoft.PowerShell.Management\Test-Path @PSBoundParameters
}
try {
    & '@RUNNER@' -Scenario lifecycle -LifecycleNative -LifecycleDiagnosticR4 -Evidence '@EVIDENCE@' -HostRoot '@ROOT@' -Runtime '@RUNTIME@'
    throw 'Unexpected staging'
}catch{
    if($_.Exception.Message -ne 'NR_QA_BUILD_MISSING: @MISSING@'){throw}
    if($global:seen.Count -ne 1 -or [IO.Path]::GetFullPath($global:seen[0]) -ne [IO.Path]::GetFullPath('@EXPECTED@')){throw 'R4 guard wrong artifact origin'}
    if(Microsoft.PowerShell.Management\Test-Path '@ROOT@'){throw 'R4 guard staged host'}
    Write-Output 'R4_ARTIFACT_ORIGIN_BOUND GPU0 STAGING0'
}
'@
        $expected=Join-Path $(if($missing.EndsWith('.exe')){$build}else{Split-Path $build}) $missing
        foreach($pair in @(@('@RUNNER@',$runner),@('@EVIDENCE@',$freshEvidence),@('@ROOT@',$freshRoot),@('@RUNTIME@',$runtime),@('@MISSING@',$missing),@('@EXPECTED@',$expected))) {
            $probe=$probe.Replace($pair[0],$pair[1].Replace("'","''"))
        }
        $result=Child $probe
        if($result.exit -or !$result.stdout.Contains('R4_ARTIFACT_ORIGIN_BOUND')){throw $result.text}
        $calls++;Write-Output ("PASS public named R4 missing artifact origin "+$missing)
    }
    $invoke="& '$runner' -Scenario lifecycle -LifecycleNative -LifecycleDiagnosticR4 -Evidence '$freshEvidence' -HostRoot '$freshRoot' -Runtime '$runtime'"
    if($CoreControlOnly) {
        $invoke=$invoke.Replace('-Scenario lifecycle -LifecycleNative -LifecycleDiagnosticR4','-Scenario native-model -CoreInitControl')
        Guard ($invoke.Replace('-Scenario native-model','-Scenario sampler')) 'NR_QA_CORE_CONTROL_SCOPE'
        Guard ($invoke+' -LifecycleNative') 'NR_QA_CORE_CONTROL_SCOPE'
    } else {
    Guard ($invoke.Replace("-Scenario lifecycle","-Scenario sampler")) 'NR_QA_DIAGNOSTIC_SCOPE'
    Guard ($invoke.Replace("-LifecycleNative -LifecycleDiagnosticR4","-LifecycleDiagnosticR4")) 'NR_QA_DIAGNOSTIC_SCOPE'
    }
    Guard ($invoke.Replace($freshEvidence,$freshEvidence+'/nested')) 'NR_QA_EVIDENCE_REFUSED'
    Guard ($invoke.Replace($freshRoot,(Join-Path $Work 'out/task-13-verifier/host'))) 'NR_QA_FOREIGN_ROOT'
    Guard ($invoke.Replace($freshRoot,(Join-Path $Work 'out/task-13/host'))) 'NR_QA_FOREIGN_ROOT'
    Guard ($invoke.Replace($(if($CoreControlOnly){' -CoreInitControl'}else{' -LifecycleDiagnosticR4'}),'')) 'NR_QA_EVIDENCE_REFUSED'
    Guard ($invoke.Replace($runtime,'C:/not-selected/model.dll')) 'NR_QA_RUNTIME_REFUSED'
    $hashProbe=@'
function global:Get-Process {@()}
function global:Get-FileHash {
    [CmdletBinding()]param([string]$LiteralPath,[string]$Path,[string]$Algorithm)
    [Console]::Out.WriteLine('CPU_HASH_PATH '+$LiteralPath+' TARGET @ARTIFACT@')
    if([IO.Path]::GetFullPath(($LiteralPath -replace '[\\/]+','/')) -eq [IO.Path]::GetFullPath(('@ARTIFACT@' -replace '[\\/]+','/'))){return @{Hash=('0'*64)}}
    Microsoft.PowerShell.Utility\Get-FileHash @PSBoundParameters
}
@INVOKE@
'@
    $hashProbe=$hashProbe.Replace('@ARTIFACT@',(Join-Path $build 'nr-qa-host.exe')).Replace('@INVOKE@',$invoke)
    Guard $hashProbe 'NR_QA_DIAGNOSTIC_ARTIFACT_CHANGED'
    $missed="function global:Get-Process {@()}"+[Environment]::NewLine+$invoke
    Guard $missed 'CPU_SIDE_EFFECT_BARRIER:New-Item'
    Guard (". '"+(Join-Path $Work 'tools/host/HostSupport.ps1')+"'; "+'$root='+ "'$freshRoot'; Stage-Host") 'CPU_SIDE_EFFECT_BARRIER:New-Item'
    Guard ($hashProbe.Replace((Join-Path $build 'nr-qa-host.exe'),(Join-Path $build 'nr-qa-host.exe').Replace('\','/'))) 'NR_QA_DIAGNOSTIC_ARTIFACT_CHANGED'
    $helper=Join-Path $Work 'tools/host/Start-NrLifecycleSupervisor.ps1'
    $helperHash=(Get-FileHash -LiteralPath $helper -Algorithm SHA256).Hash.ToLowerInvariant()
    $nonce='f5f5f5f5f5f54f5fa5f5f5f5f5f5f5f5'
    $attempt=Join-Path $Work ('.omo/evidence/obs-nr-native-filter-20260930/task-13/codex-continuation-20261004/readiness-r5/diagnostic-supervisors/attempt-'+$nonce)
    if($CoreControlOnly){$attempt=Join-Path $Work ('.omo/evidence/obs-nr-native-filter-20260930/task-13/codex-continuation-20261004/core-control-r6b/supervisors/attempt-'+$nonce)}
    $own="& '$helper' -Mode Own -Lane NativeDiagnosticR4 -Attempt '$attempt' -Nonce $nonce -HelperHash $helperHash"
    Guard ("& '$helper' -Mode CpuChild -Lane NativeDiagnosticR4") 'NR_QA_DIAGNOSTIC_CPU_MODE_REFUSED'
    Guard ("& '$helper' -Mode Close -Lane NativeDiagnosticR4") 'NR_QA_NATIVE_CLOSE_REFUSED'
    Guard ("& '$helper' -Mode Launch -Lane NativeDiagnosticR4") 'NR_QA_NATIVE_NOT_ADMITTED'
    Guard ("& '$helper' -Mode Launch -Lane NativeDiagnosticR4 -NativeApproved -SinkFault") 'NR_QA_NATIVE_NOT_ADMITTED'
    Guard $own 'NR_QA_NATIVE_MODE_REFUSED'
    Guard ($own.Replace($attempt,$attempt+'/nested')) 'NR_QA_ATTEMPT_REFUSED'
    Guard ($own.Replace($helperHash,('0'*64))) 'NR_QA_HELPER_CHANGED'
    Guard ($own+' -NativeApproved') 'CPU_SIDE_EFFECT_BARRIER:Set-Content'
    $ownedRoute=@'
$registered=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,'Local\nr-task13-r4-@NONCE@-registered')
function global:Set-Content {
    param([Parameter(ValueFromPipeline)]$Value,$Encoding,$LiteralPath,[Parameter(Position=0)]$Path)
    process {
        if([IO.Path]::GetFullPath($Path) -ne [IO.Path]::GetFullPath('@ATTEMPT@/ownership.json')){throw 'CPU_OWN_METADATA_PATH_INVALID'}
        $global:ownershipValue=$Value
    }
}
try {
    try { @OWN@ -NativeApproved; throw 'CPU_OWN_UNEXPECTED_RETURN' }
    catch {
        if($_.Exception.Message -ne 'CPU_SIDE_EFFECT_BARRIER:New-Item'){throw}
        $r=$global:ownershipValue|ConvertFrom-Json
        if($r.lane -ne 'NativeDiagnosticR4' -or $r.nonce -ne '@NONCE@' -or $r.helper_sha256 -ne '@HASH@' -or !$registered.WaitOne(0)){throw 'CPU_OWN_BINDING_INVALID'}
        throw 'CPU_OWN_PUBLIC_ROUTE_BOUND_NO_NATIVE'
    }
}finally{$registered.Dispose()}
'@
    foreach($pair in @(@('@NONCE@',$nonce),@('@HASH@',$helperHash),@('@ATTEMPT@',$attempt),@('@OWN@',$own))){$ownedRoute=$ownedRoute.Replace($pair[0],$pair[1])}
    Guard $ownedRoute 'CPU_OWN_PUBLIC_ROUTE_BOUND_NO_NATIVE'
    $launch=@'
function global:New-Item {}
function global:Start-Process {
    param($FilePath,$ArgumentList,$WindowStyle,[switch]$PassThru,$WorkingDirectory,$RedirectStandardOutput,$RedirectStandardError)
    if($WindowStyle -ne 'Hidden' -or $ArgumentList[8] -ne 'NativeDiagnosticR4' -or $ArgumentList[12] -notmatch '^[0-9a-f]{32}$'){throw 'CPU_HANDOFF_INVALID'}
    if($ArgumentList[10].Trim('"') -ne (Join-Path '@BASE@' ('attempt-'+$ArgumentList[12]))){throw 'CPU_HANDOFF_ATTEMPT_INVALID'}
    if($ArgumentList[14] -ne '@HASH@' -or $ArgumentList[-1] -ne '-NativeApproved'){throw 'CPU_HANDOFF_HASH_INVALID'}
    throw 'CPU_HANDOFF_BOUND_NO_PROCESS'
}
& '@HELPER@' -Mode Launch -Lane NativeDiagnosticR4 -NativeApproved
'@
    $launch=$launch.Replace('@BASE@',(Split-Path $attempt)).Replace('@HASH@',$helperHash).Replace('@HELPER@',$helper)
    Guard $launch 'CPU_HANDOFF_BOUND_NO_PROCESS'
    $query=@'
function global:Get-Content {
    param([switch]$Raw,$LiteralPath)
    if($LiteralPath -ne '@ATTEMPT@/ownership.json' -and [IO.Path]::GetFullPath($LiteralPath) -ne [IO.Path]::GetFullPath('@ATTEMPT@/ownership.json')){throw 'CPU_QUERY_OWNERSHIP_INVALID'}
    '{"helper_sha256":"@HASH@","nonce":"@NONCE@","lane":"NativeDiagnosticR4","supervisor":{"pid":1}}'
}
function global:Get-ChildItem {
    param($LiteralPath,$Filter)
    if([IO.Path]::GetFullPath($LiteralPath) -ne [IO.Path]::GetFullPath('@EVIDENCE@') -or $Filter -ne 'attempt-*-supervisor.json'){throw 'CPU_QUERY_EVIDENCE_INVALID'}
    throw 'CPU_QUERY_FIXED_RECEIPT_BOUND_NO_IPC'
}
& '@HELPER@' -Mode Query -Lane NativeDiagnosticR4 -Attempt '@ATTEMPT@' -Nonce @NONCE@ -HelperHash @HASH@
'@
    foreach($pair in @(@('@ATTEMPT@',$attempt),@('@HASH@',$helperHash),@('@NONCE@',$nonce),@('@EVIDENCE@',$freshEvidence),@('@HELPER@',$helper))){$query=$query.Replace($pair[0],$pair[1])}
    Guard $query 'CPU_QUERY_FIXED_RECEIPT_BOUND_NO_IPC'
    Guard ($query.Replace('"lane":"NativeDiagnosticR4"','"lane":"CPU"')) 'NR_QA_OWNERSHIP_REFUSED'
    Guard ($own.Replace('-Mode Own','-Mode Query').Replace('-Lane NativeDiagnosticR4','-Lane Native')) 'NR_QA_ATTEMPT_REFUSED'
    Guard ($invoke.Replace($freshEvidence,$(if($CoreControlOnly){$freshEvidence+'/nested'}else{$freshEvidence.Replace('diagnostic-r4-verified','diagnostic-r4-physical')})).Replace($freshRoot,$freshRoot.Replace('verified-host','host'))) 'NR_QA_EVIDENCE_REFUSED'
    if(Test-Path $freshRoot){throw 'CPU guard staged verified host'}
    if(Test-Path $freshEvidence){throw 'CPU guard created verified evidence'}
    Write-Output ('CPU_CHILDREN_TERMINAL '+($script:diagnosticChildren|ConvertTo-Json -Depth 6 -Compress))
    Write-Output ("PASS named public route CoreControl="+$CoreControlOnly+" guards="+$calls+" GPU0 STAGING0; handoff/query NoProcess/NoIPC are controlled seams")
    return
}
foreach($pair in @(@($base,(Join-Path $Work "out/$task/host"),(Join-Path $Work "out/$task-verifier/host"),(Join-Path $Work "out/$task/native/Debug")),
    @((Join-Path $base 'verifier'),(Join-Path $Work "out/$task-verifier/host"),(Join-Path $Work "out/$task/host"),(Join-Path $Work "out/$task-verifier/native/Debug")))) {
    if(Test-Path $pair[1]) {
        $row=Child "& '$runner' -Scenario $scenario -Evidence '$($pair[0])' -HostRoot '$($pair[1])' -Runtime '$runtime'"
        if(!$row.exit -or !$row.text.Contains('NR_QA_EXISTING_ROOT')){throw 'Historical owned root was not refused'}
        Write-Output 'PASS existing historical host refused and preserved; fresh staging guards use verifier root'
        continue
    }
    foreach($case in @(@($pair[0],$pair[2],$runtime,'NR_QA_FOREIGN_ROOT'),
        @($pair[0],$pair[1],'C:/not-selected/model.dll','NR_QA_RUNTIME_REFUSED'),
        @(($pair[0]+'/nested'),$pair[1],$runtime,'NR_QA_EVIDENCE_REFUSED'))) {
        $row=Child "`$ErrorActionPreference='Stop'; & '$runner' -Scenario $scenario -Evidence '$($case[0])' -HostRoot '$($case[1])' -Runtime '$($case[2])'"
        if(!$row.exit -or !$row.text.Contains($case[3])){throw ('Wrong bootstrap refusal '+$row.text)}
    }
    $missingBuild=@('nr-qa-host.exe','nr_obs_analysis.cso')
    if($Task13Only){$missingBuild+='nr-driver-inspect.exe'}
    foreach($missing in $missingBuild) {
        $probe=@'
$ErrorActionPreference='Stop'
function global:Get-Process { @() }
$global:paths=[Collections.Generic.List[string]]::new()
$global:preflightInspector=@INSPECTOR@
function global:Test-Path {
    [CmdletBinding()]param([Parameter(Position=0)][string]$Path,[string]$LiteralPath,[string]$PathType)
    $candidate=if($LiteralPath){$LiteralPath}else{$Path}
    $name=[IO.Path]::GetFileName($candidate)
    if(!$PathType -and ($name -in @('obs-nr-filter.dll','nr-qa-host.exe','nr_codec.dxil','nr_color.dxil','nr_guides.cso','nr_interop_vertex.cso','nr_interop_pixel.cso','nr_obs_analysis.cso') -or
        ($global:preflightInspector -and $name -eq 'nr-driver-inspect.exe'))){
        $global:paths.Add([IO.Path]::GetFullPath($candidate));return $name -ne '@MISSING@'
    }
    Microsoft.PowerShell.Management\Test-Path @PSBoundParameters
}
try {
    & '@RUNNER@' -Scenario @SCENARIO@ -Evidence '@EVIDENCE@' -HostRoot '@ROOT@' -Runtime '@RUNTIME@'
    throw 'Build guard reached native host'
}catch{
    if($_.Exception.Message -ne 'NR_QA_BUILD_MISSING: @MISSING@'){throw}
    @{paths=@($global:paths.ToArray());host_absent=!(Microsoft.PowerShell.Management\Test-Path '@ROOT@')}|ConvertTo-Json -Compress
}
'@
        foreach($entry in @(@('@RUNNER@',$runner),@('@EVIDENCE@',$pair[0]),@('@ROOT@',$pair[1]),@('@RUNTIME@',$runtime),@('@MISSING@',$missing),@('@SCENARIO@',$scenario),
            @('@INSPECTOR@','$true'))){
            $probe=$probe.Replace($entry[0],$entry[1].Replace("'","''"))
        }
        $result=Child $probe;if($result.exit){throw $result.text};$row=$result.stdout|ConvertFrom-Json
        if(!$row.host_absent -or !$row.paths.Count){throw 'Build guard staged a host'}
        foreach($path in $row.paths) {
            $name=[IO.Path]::GetFileName($path)
            $expected=Join-Path $(if($name.EndsWith('.dll') -or $name.EndsWith('.exe')){$pair[3]}else{Split-Path $pair[3]}) $name
            if($path -ne [IO.Path]::GetFullPath($expected)){throw 'Fresh bootstrap build fell back or aliased producer'}
        }
    }
}
if($Task13Only) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class NrSuspendedIdentity {
    [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)]
    public struct Startup {
        public uint size;
        public string reserved,desktop,title;
        public uint x,y,width,height,charsX,charsY,fill,flags;
        public ushort show,reservedSize;
        public IntPtr reservedBytes,input,output,error;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct Created { public IntPtr process,thread;public uint pid,tid; }
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)]
    public static extern bool CreateProcess(string app,System.Text.StringBuilder command,IntPtr process,IntPtr thread,
        bool inherit,uint flags,IntPtr environment,string directory,ref Startup startup,out Created created);
    [DllImport("kernel32.dll",SetLastError=true)]
    public static extern uint ResumeThread(IntPtr thread);
    [DllImport("kernel32.dll")]
    public static extern bool CloseHandle(IntPtr handle);
}
'@
    $startup=[NrSuspendedIdentity+Startup]::new()
    $startup.size=[Runtime.InteropServices.Marshal]::SizeOf($startup)
    $created=[NrSuspendedIdentity+Created]::new()
    $systemDirectory=if([Environment]::Is64BitProcess){'System32'}else{'SysWOW64'}
    $image=[IO.Path]::GetFullPath("$env:SystemRoot/$systemDirectory/cmd.exe")
    $command=[Text.StringBuilder]::new("`"$image`" /d /c exit 0")
    if(![NrSuspendedIdentity]::CreateProcess($image,$command,[IntPtr]::Zero,[IntPtr]::Zero,
        $false,0x08000004,[IntPtr]::Zero,$env:SystemRoot,[ref]$startup,[ref]$created)){throw 'Suspended identity child creation'}
    $child=[Diagnostics.Process]::GetProcessById($created.pid)
    $child.EnableRaisingEvents=$true;$exitSignal=[NrProcessExit]::new($child)
    try {
        $first=Identity $child;$second=Identity $child
        if([IO.Path]::GetFullPath($first.image) -ne [IO.Path]::GetFullPath($image) -or
            ($first|ConvertTo-Json -Compress) -cne ($second|ConvertTo-Json -Compress)){
            throw ('Kernel image identity must be stable before the loader runs: '+(@{
                expected=$image;first=$first;second=$second}|ConvertTo-Json -Depth 4 -Compress))
        }
    }finally {
        if([NrSuspendedIdentity]::ResumeThread($created.thread) -eq [UInt32]::MaxValue){throw 'Suspended identity child resume'}
        if(!$exitSignal.Exited.WaitOne(15000) -or !$child.WaitForExit(5000)){throw 'Exact identity child exit unresolved'}
        if($child.ExitCode -ne 0){throw ('Exact identity child failed: '+$child.ExitCode)}
        [NrSuspendedIdentity]::CloseHandle($created.thread)|Out-Null
        [NrSuspendedIdentity]::CloseHandle($created.process)|Out-Null
        $exitSignal.Dispose();$child.Dispose()
    }
    $rows=@()
    $cases=@('initial','identical-valid','explicit-cut','invalid','reconnect','unknown','known-reconnect',
        'HDR','SDR','resize','settings','disable','enable','remove-readd','new-instance')
    $generation=1
    foreach($i in 0..14) {
        if($i -in @(7,8,9,10)){$generation++}
        $placeholder=$i -in @(3,5,11)
        $rows+=@{step=$i;case=$cases[$i];instance=$(if($i -eq 14){52}else{51});tick=(100+$i);
            selected=$(if($placeholder){0}else{1+$i});kind=$(if($placeholder){0}else{1});ingress=(1+$i);
            content_epoch=$(if($i -lt 2){1}else{2+$i});history_epoch=(1+$i);resource_generation=$generation;
            source_token=301;device_token=901;placeholder=$placeholder;physical_signal_known=($i -ne 5);
            validity_metadata_present=$true;source_validity=$(if($i -eq 5){0}elseif($i -eq 3){2}else{1});
            same_latch_views=$true;image_hash='17';raw_hash='19';current_raw_hash='19';output_hash='19';
            current_content_id=(1+$i);raw_content_id=(1+$i);view_content_id=(1+$i);
            deterministic=$true;genuine_nr=$false;ordinary_ofa=$false}
    }
    $cleanup=@(@{closed=$true;worker_exit=$true;retained=$false})
    Assert-LifecycleRows $rows $cleanup
    function Lifecycle-Refused([string]$Token) {
        $refused=$false
        try{Assert-LifecycleRows $rows $cleanup}catch{
            if($_.Exception.Message -ne $Token){throw};$refused=$true
        }
        if(!$refused){throw 'Malformed lifecycle production receipt admitted'}
    }
    $rows[3].selected=1;Lifecycle-Refused 'NR_QA_LIFECYCLE_STALE';$rows[3].selected=0
    $rows[1].content_epoch=2;Lifecycle-Refused 'NR_QA_LIFECYCLE_FALSE_LOSS';$rows[1].content_epoch=1
    $rows[14].instance=51;Lifecycle-Refused 'NR_QA_LIFECYCLE_INSTANCE';$rows[14].instance=52
    $rows[4].source_token=0;Lifecycle-Refused 'NR_QA_LIFECYCLE_IDENTITY';$rows[4].source_token=301
    $rows[5].physical_signal_known=$true;Lifecycle-Refused 'NR_QA_LIFECYCLE_IDENTITY';$rows[5].physical_signal_known=$false
    $old=$rows[7].resource_generation;$rows[7].resource_generation=$rows[6].resource_generation
    Lifecycle-Refused 'NR_QA_LIFECYCLE_EPOCH_TRANSITION';$rows[7].resource_generation=$old
    $old=$rows[2].history_epoch;$rows[2].history_epoch=$rows[1].history_epoch
    Lifecycle-Refused 'NR_QA_LIFECYCLE_EPOCH_TRANSITION';$rows[2].history_epoch=$old
    $rows[8].output_hash='20';Lifecycle-Refused 'NR_QA_LIFECYCLE_IMAGE_PHASE';$rows[8].output_hash='19'
    $rows[8].view_content_id=1;Lifecycle-Refused 'NR_QA_LIFECYCLE_IMAGE_PHASE';$rows[8].view_content_id=9
    $rows[3].view_content_id=1;Lifecycle-Refused 'NR_QA_LIFECYCLE_IMAGE_PHASE';$rows[3].view_content_id=4
    $nativeRefused=$false
    try{Assert-LifecycleRows $rows $cleanup -Native}catch{
        if($_.Exception.Message -ne 'NR_QA_LIFECYCLE_PROCESSOR'){throw};$nativeRefused=$true
    }
    if(!$nativeRefused){throw 'Deterministic lifecycle admitted as actual NR/OFA'}
    $nativeRows=@()
    $witness=@()
    foreach($i in 0..14) {
        $n=@{};foreach($key in $rows[$i].Keys){$n[$key]=$rows[$i][$key]}
        $n.deterministic=$false
        if(!$n.placeholder) {
            $n.ingress+=2
            $n.kind=3;$n.selected=$n.ingress-1;$n.output_hash='23';$n.genuine_nr=$true;$n.ordinary_ofa=$true
            $n.submission_serial=1+$i;$n.selected_history_epoch=$n.history_epoch
            $witness+=@{step=$i;frame=2;instance=$n.instance;ingress=$n.selected;content_epoch=$n.content_epoch;
                history_epoch=$n.history_epoch;resource_generation=$n.resource_generation;submission_serial=(1+$i);
                creation_epoch=1;submission_epoch=2;evaluated=$true;composed=$true;submitted=$true;eligible=$true;
                reference_valid=$true;flow_reset=$false;ordinary_ofa=$true;code=1;invalid=0;
                module_base=101;module_path=$runtime;completion_value=1;completion_done=1;
                producer=1;producer_done=1;worker=1;worker_done=1;consumer=2;consumer_done=2;
                retired_generations=0;renderer_luid_high=0;renderer_luid_low=17;worker_luid_high=0;worker_luid_low=17;
                raw_hash='19';output_hash='23'}
        }
        $nativeRows+=$n
    }
    $cold=@{};foreach($key in $witness[0].Keys){$cold[$key]=$witness[0][$key]}
    $cold.frame=1;$cold.flow_reset=$true;$cold.reference_valid=$false;$cold.ordinary_ofa=$false;$cold.output_hash='19'
    $witness=@($cold)+$witness
    $nativeClose=@(@{closed=$true;worker_exit=$true;retained=$false;genuine_nr_count=12;ordinary_ofa_count=12;
        actual_evaluations=13;actual_reset_samples=1})
    Assert-LifecycleRows $nativeRows $nativeClose -Native -Witness $witness
    function Native-Refused([string]$Token) {
        $refused=$false
        try{Assert-LifecycleRows $nativeRows $nativeClose -Native -Witness $witness}catch{
            if($_.Exception.Message -ne $Token){throw};$refused=$true
        }
        if(!$refused){throw 'Malformed actual lifecycle witness admitted'}
    }
    $cold.output_hash='23';Native-Refused 'NR_QA_LIFECYCLE_RESET_IMAGE';$cold.output_hash='19'
    $witness[1].output_hash='19';Native-Refused 'NR_QA_LIFECYCLE_EDITED_TRANSITION';$witness[1].output_hash='23'
    $witness[1].completion_done=0;Native-Refused 'NR_QA_LIFECYCLE_WITNESS';$witness[1].completion_done=1
    $witness[1].worker_luid_low=18;Native-Refused 'NR_QA_LIFECYCLE_WITNESS';$witness[1].worker_luid_low=17
    $nativeClose[0].actual_reset_samples=2;Native-Refused 'NR_QA_LIFECYCLE_WITNESS_SET';$nativeClose[0].actual_reset_samples=1
    # Cold-only input cannot satisfy any edited transition or the unchanged
    # twelve-apply requirement, even with successful SDK/fence observations.
    $nativeClose[0].genuine_nr_count=0
    Native-Refused 'NR_QA_LIFECYCLE_NR_NOOP'
    $nativeClose[0].genuine_nr_count=12
    $savedWitness=$witness
    $witness=@($cold)
    Native-Refused 'NR_QA_LIFECYCLE_EDITED_TRANSITION'
    $witness=$savedWitness
    $cleanup[0].retained=$true;Lifecycle-Refused 'NR_QA_LIFECYCLE_CLEANUP'
    Write-Output 'PASS lifecycle public producer/verifier route, wrong roots/runtime/nested evidence, fresh missing host/shader, exact transition fields and invalid/repeated-valid/address-reuse/retained-close controls; CPU only'
    return
}
$frames=@();$lanes=@();$decoded=@();$cleanup=@()
foreach($lane in 0..3) {
    $hdr=$lane -ge 2;$det=($lane%2)-eq 0
    foreach($ingress in 1..11) {
        $selected=if($ingress -eq 1){1}else{$ingress-1};$kind=if($ingress -eq 1){1}else{3}
        $frames+=@{lane=$lane;hdr=$hdr;deterministic=$det;genuine_nr=(!$det -and $kind -eq 3);same_latch_views=$true;
            width=1920;height=1080;instance=($lane+1);content_epoch=1;interval=1000000000;clock=((10+$ingress)*1000000000L);
            tick=(11+$ingress);ingress=$ingress;selected=$selected;source_id=$selected;filter_id=$selected;program_id=$selected;kind=$kind;
            alpha_errors=0;raw_hash='11';filter_hash=$(if($det){'11'}else{'22'});program_hash='33';unrelated_hash='44';
            same_key_nr_rms=$(if($det){0.0}else{0.1});submission_serial=$ingress;history_epoch=1;resource_generation=1;module_base=1}
    }
    $lanes+=@{lane=$lane;hdr=$hdr;deterministic=$det;packets=8;decoded_frames=8}
    $cleanup+=@{lane=$lane;closed=$true;worker_exit=$true;retained=$false}
    foreach($id in 3..10) {
        $decoded+=@{lane=$lane;source_id=$id;width=1920;height=1080;bit_depth=$(if($hdr){10}else{8});transfer=$(if($hdr){2}else{0});
            full_range=$false;primaries_matrix_preserved=$true;codes_compared=3110400;code_errors=0;decoded_peak_linear_nits=1000;
            same_id_raw_decoded_nr_rms_codes=$(if($det){0.0}else{3.0})}
    }
}
$slots=@(@{targeted_slot=2;lower_reusable_slot=0;old_api_slot=0;refusal_ownership_unchanged=$true;serial_not_consumed=$true;producer_cursor=6})
$slotCleanup=@(@{closed=$true;worker_exit=$true;retained=$false})
Assert-BootstrapRows $frames $lanes $decoded $cleanup $slots $slotCleanup
function Refused([string]$Token) {
    $refused=$false
    try{Assert-BootstrapRows $frames $lanes $decoded $cleanup $slots $slotCleanup}catch{
        if($_.Exception.Message -ne $Token){throw};$refused=$true
    }
    if(!$refused){throw 'Malformed native output proof admitted'}
}
$frames[12].filter_id=2;Refused 'NR_QA_BOOTSTRAP_LINEAGE';$frames[12].filter_id=$frames[12].source_id
$frames[12].same_key_nr_rms=0;Refused 'NR_QA_BOOTSTRAP_NR_NOOP';$frames[12].same_key_nr_rms=0.1
$decoded[16].bit_depth=8;Refused 'NR_QA_BOOTSTRAP_CODEC_COLOR';$decoded[16].bit_depth=10
$decoded[0].code_errors=1;Refused 'NR_QA_BOOTSTRAP_CODEC_COLOR';$decoded[0].code_errors=0
$decoded[1].source_id=3;Refused 'NR_QA_BOOTSTRAP_DECODED_LINEAGE';$decoded[1].source_id=4
foreach($row in @($decoded|Where-Object {$_.lane -eq 1})){$row.same_id_raw_decoded_nr_rms_codes=0}
Refused 'NR_QA_BOOTSTRAP_DECODED_NR_NOOP'
Write-Output 'PASS bootstrap public exact producer/verifier routing, fresh shader refusal, positive parser and lineage/no-op/HDR/codec/freeze controls; CPU only'
