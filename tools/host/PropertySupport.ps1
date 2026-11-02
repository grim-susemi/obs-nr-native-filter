# SPDX-License-Identifier: GPL-2.0-or-later
# Reuse the reviewed predicates byte-for-byte; no second interpretation.
$propertyTask=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../.omo/evidence/obs-nr-native-filter-20260930/task-15'))
$propertyParser=Join-Path $propertyTask 'property-roundtrip-source-20261011/PropertySupport.ps1'
if((Get-FileHash -LiteralPath $propertyParser -Algorithm SHA256).Hash.ToLowerInvariant() -cne
    '75d42bc3ff5d2ba5d96419ab7f3f32ec2820f5d89eac60b133131eae0493f86d'){throw 'NR_PROPERTY_PARSER_CHANGED'}
. $propertyParser

function Assert-NrPropertyCandidate([string]$Manifest,[string]$Sha256,[string]$Build,[string]$Work) {
    $taskRoot=Join-Path $Work '.omo/evidence/obs-nr-native-filter-20260930/task-15'
    $prefix=[IO.Path]::GetFullPath($taskRoot).TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar
    if(!$Manifest -or $Sha256 -cnotmatch '^[0-9a-f]{64}$' -or
        ![IO.Path]::GetFullPath($Manifest).StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFullPath($Build) -ne [IO.Path]::GetFullPath((Join-Path $Work 'out/task-15-control-event-repair/build02'))){
        throw 'NR_PROPERTY_CANDIDATE_PATH'
    }
    if((Get-FileHash -LiteralPath $Manifest -Algorithm SHA256).Hash.ToLowerInvariant() -cne $Sha256){throw 'NR_PROPERTY_FREEZE_CHANGED'}
    $freeze=Get-Content -LiteralPath $Manifest -Raw|ConvertFrom-Json
    if($freeze.schema -ne 1 -or $freeze.build_origin -cne 'Task15-R15' -or
        [IO.Path]::GetFullPath($freeze.build) -ne [IO.Path]::GetFullPath($Build)){throw 'NR_PROPERTY_BUILD_ORIGIN'}
    $hostBinary=Join-Path $Build 'nr-qa-host.exe'
    $plugin=Join-Path $Build 'obs-nr-filter.dll'
    $native=Join-Path $Work 'out/task-13-diagnostic-r4/native'
    $required=@($hostBinary,$plugin,(Join-Path $native 'Debug/nr-driver-inspect.exe'))
    $required+=@('nr_codec.dxil','nr_color.dxil','nr_guides.cso','nr_interop_vertex.cso','nr_interop_pixel.cso','nr_obs_analysis.cso')|
        ForEach-Object {Join-Path $native $_}
    if(@($freeze.artifacts).Count -ne $required.Count){throw 'NR_PROPERTY_ARTIFACT_SET'}
    foreach($path in $required) {
        $pins=@($freeze.artifacts|Where-Object {[IO.Path]::GetFullPath($_.path) -eq [IO.Path]::GetFullPath($path)})
        if($pins.Count -ne 1){throw 'NR_PROPERTY_ARTIFACT_SET'}
    }
    if([IO.Path]::GetFullPath($freeze.plugin.path) -ne [IO.Path]::GetFullPath($plugin) -or
        $freeze.plugin.sha256 -cne '2076e4594ad5df603ce9e235e7f6f083b41ad8130b11e3c480043f2b749ba078' -or
        @($freeze.artifacts|Where-Object {$_.sha256 -ceq '014e572568c988547a3e17b7b66dc67cdb08d04f5ad4f96cb2c7548f102183d3' -and
            [IO.Path]::GetFullPath($_.path) -eq [IO.Path]::GetFullPath($hostBinary)}).Count -ne 1){throw 'NR_PROPERTY_R15_PAIR'}
    $overlay=Join-Path $Work '.omo/evidence/obs-nr-native-filter-20260930/task-13/shared-sdk-lifetime-repair-20261005/overlay'
    $inputs=@(Get-ChildItem -LiteralPath (Join-Path $Work 'src'),(Join-Path $Work 'cmake/nr'),(Join-Path $Work 'tools/host'),(Join-Path $Work 'deps'),$overlay -File -Recurse|
        Where-Object {$_.Extension -in @('.h','.cpp','.c','.hlsl','.cmake','.ps1','.lib')}|ForEach-Object {$_.FullName})
    $inputs+=@('CMakeLists.txt','tools/driver-inspect.cpp','tools/Invoke-NrQa.ps1','tools/tests/bootstrap_runner_tests.ps1',
        'tools/tests/property_runner_tests.ps1','provenance/inputs.spec.json','provenance/sdk-lock.json')|ForEach-Object {Join-Path $Work $_}
    $inputs+=@('main.cpp','property_scenario.inc','obs_filter.cpp','Build-Private-r2.ps1','BUILD-RECIPE-r2.json','BUILD-RESULT-r2.json')|
        ForEach-Object {Join-Path $taskRoot ('property-control-event-repair-20261012/'+$_)}
    $inputs+=Join-Path $taskRoot 'property-roundtrip-source-20261011/PropertySupport.ps1'
    foreach($path in $inputs) {
        if(@($freeze.sources|Where-Object {[IO.Path]::GetFullPath($_.path) -eq [IO.Path]::GetFullPath($path)}).Count -ne 1){
            throw ('NR_PROPERTY_SOURCE_SET: '+$path)
        }
    }
    $recipe=Get-Content -LiteralPath (Join-Path $taskRoot 'property-control-event-repair-20261012/BUILD-RECIPE-r2.json') -Raw|ConvertFrom-Json
    foreach($recipeInput in $recipe.inputs) {
        if(@($freeze.reused_inputs|Where-Object {[IO.Path]::GetFullPath($_.path) -eq [IO.Path]::GetFullPath($recipeInput.path) -and
            $_.sha256 -ceq $recipeInput.sha256 -and $_.bytes -eq $recipeInput.bytes}).Count -ne 1){throw 'NR_PROPERTY_REUSED_INPUT_SET'}
    }
    foreach($pin in @($freeze.sources)+@($freeze.artifacts)+@($freeze.reused_inputs)+@($freeze.plugin)) {
        if((Get-Item -LiteralPath $pin.path).Length -ne $pin.bytes -or
            (Get-FileHash -LiteralPath $pin.path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $pin.sha256){
            throw ('NR_PROPERTY_INPUT_CHANGED: '+$pin.path)
        }
    }
    $main=Join-Path $taskRoot 'property-control-event-repair-20261012/main.cpp'
    $scenario=Join-Path $taskRoot 'property-control-event-repair-20261012/property_scenario.inc'
    $filter=Join-Path $taskRoot 'property-control-event-repair-20261012/obs_filter.cpp'
    if((Get-FileHash -LiteralPath $main).Hash.ToLowerInvariant() -cne '392dad1d1925876f15de55b259807c508ac6ffe506029abf5352e7870478efd4' -or
        (Get-FileHash -LiteralPath $scenario).Hash.ToLowerInvariant() -cne '77f5c099037c48118869fe3ca66d15a9670cd178330d4f384a85dbfd1a189d32' -or
        (Get-FileHash -LiteralPath $filter).Hash.ToLowerInvariant() -cne '2c10921a5c27d47663ed486a7f918c67cf419e830332e265af41edd246c1c5a2'){throw 'NR_PROPERTY_OVERLAY_CHANGED'}
    $freeze
}

function New-NrPropertyOwner([string]$Run,[string]$Root,[string]$Evidence) {
    $o=@{run=$Run;process=$null;handle=$null;started=$false;closed=$false;identity=$null;events=@();retainedEvents=@();
        exitSignal=$null;nativeOutput=$null;stdout=$null;stderr=$null;outputStarted=$false;errorStarted=$false;
        kernelWait=$null;kernelAttached=$false;unknownGate=$null;sdkSettled=$false;
        primaryError=$null;secondaryErrors=@();state='original-owner-published';receipt=$null;validated=$false}
    # Append before any further construction can throw, never overwrite a prior graph.
    [void]$global:NrQaPropertyOwners.Add($o)
    $o.process=[Diagnostics.Process]::new();$p=$o.process
    $o.kernelWait=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset)
    $o.unknownGate=[Threading.ManualResetEvent]::new($false)
    $nonce=[Guid]::NewGuid().ToString('N')
    $o.names=@('ready','action','acted','stop','exit')|ForEach-Object {'Local\nr-qa-property-'+$nonce+'-'+$_}
    foreach($name in $o.names){$o.events+=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,$name)}
    $o.retainedEvents=@(New-RetainedHostEvents 'lifecycle' $false $o.names[3])
    $p.StartInfo.FileName=Join-Path $Root 'bin/64bit/nr-qa-host.exe'
    $p.StartInfo.WorkingDirectory=Join-Path $Root 'bin/64bit'
    $p.StartInfo.UseShellExecute=$false;$p.StartInfo.CreateNoWindow=$true
    $p.StartInfo.RedirectStandardOutput=$true;$p.StartInfo.RedirectStandardError=$true
    $p.StartInfo.Arguments='"'+$Root+'" '+(($o.names|ForEach-Object {'"'+$_+'"'}) -join ' ')+' property-roundtrip-'+$Run
    $p.EnableRaisingEvents=$true
    $o.exitSignal=[NrProcessExit]::new($p)
    $o.nativeOutput=[NrHostOutput]::new($p,(Join-Path $Evidence ($Run+'-host.log')))
    $o
}

function Invoke-NrPropertyOriginal($Owner,$Freeze) {
    Assert-NoUnrelatedProcess
    $null=Assert-NrPropertyCandidate $Task15CandidateManifest $Task15CandidateSha256 $build $work
    Start-NrInstanceOriginal $Owner
    $p=$Owner.process;$events=$Owner.events
    if($Owner.identity.image -ne $p.StartInfo.FileName){throw 'NR_QA_CHILD_IDENTITY_REFUSED'}
    if([Threading.WaitHandle]::WaitAny([Threading.WaitHandle[]]@($events[0],$Owner.exitSignal.Exited),15000) -ne 0){throw 'NR_PROPERTY_READY_UNRESOLVED'}
    $inventory=@($p.Modules|ForEach-Object {@{path=$_.FileName;sha256=(Get-FileHash $_.FileName).Hash}})
    Assert-CleanInventory $inventory
    foreach($pair in @(@((Join-Path $build 'nr-qa-host.exe'),'bin/64bit/nr-qa-host.exe'),@($Freeze.plugin.path,'obs-plugins/64bit/obs-nr-filter.dll'))) {
        $loaded=@($inventory|Where-Object {$_.path -eq (Join-Path $root $pair[1])})
        if($loaded.Count -ne 1 -or $loaded[0].sha256 -ne (Get-FileHash $pair[0]).Hash){throw 'NR_PROPERTY_READY_PAIR'}
    }
    $Owner.inventory_ready=$inventory
    $null=Assert-NrPropertyCandidate $Task15CandidateManifest $Task15CandidateSha256 $build $work
    [void]$events[1].Set()
    if([Threading.WaitHandle]::WaitAny([Threading.WaitHandle[]]@($events[2],$Owner.exitSignal.Exited),300000) -ne 0){throw 'NR_PROPERTY_ACTION_UNRESOLVED'}
    $p.Refresh()
    Assert-CleanInventory @($p.Modules|ForEach-Object {@{path=$_.FileName;sha256=(Get-FileHash $_.FileName).Hash}})
    [void]$events[3].Set()
    if(!$events[4].WaitOne(15000) -or !$Owner.kernelWait.WaitOne(15000) -or !$Owner.exitSignal.Exited.WaitOne(15000) -or
        !$p.WaitForExit(15000) -or !$Owner.nativeOutput.OutputClosed.WaitOne(15000) -or
        !$Owner.nativeOutput.ErrorClosed.WaitOne(15000) -or !(Test-NrQaOriginalOwnerClosed $Owner)){throw 'NR_PROPERTY_CLOSE_UNRESOLVED'}
    $reaped=[DateTime]::UtcNow.ToString('o')
    if($p.ExitCode -ne 0){throw ('NR_PROPERTY_HOST_FAILED: '+$p.ExitCode)}
    if($Owner.nativeOutput.SinkFailed){throw 'NR_PROPERTY_OUTPUT_SINK_FAILURE'}
    $source=Join-Path $root 'profile/task15-source.json'
    $Owner.receipt=[ordered]@{pid=$Owner.identity.pid;creation=$Owner.identity.creation;image=$Owner.identity.image;
        exit=$p.ExitCode;exited=$true;stdoutEOF=$true;stderrEOF=$true;reapedUtc=$reaped;root=$root;
        source_file=$source;source_sha256=(Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()}
    Assert-LifecycleClosure @(Read-LifecycleClosure $Owner.nativeOutput.Text)
    $Owner.sdkSettled=$true
    if(@(($Owner.nativeOutput.Text -split [char]10)|Where-Object {$_.TrimEnd() -ceq 'NR_QA_EXIT cleanup=complete result=0'}).Count -ne 1){throw 'NR_PROPERTY_HOST_EXIT'}
    $loaded=Assert-PropertyRun @(Read-PropertyRows $Owner.nativeOutput.Text) $Owner.run $Owner.receipt
    if([IO.Path]::GetFullPath($loaded.settings.nr_runtime_path) -ne [IO.Path]::GetFullPath($Runtime)){throw 'NR_PROPERTY_RUNTIME_PIN'}
    $null=Assert-NrPropertyCandidate $Task15CandidateManifest $Task15CandidateSha256 $build $work
    $Owner.validated=$true
}

function Invoke-NrPropertyPair($Save,$Restore,$Freeze) {
    $null=Invoke-NrPropertyOriginal $Save $Freeze
    if(!$Save.validated -or !(Test-NrQaOriginalOwnerClosed $Save)){throw 'NR_PROPERTY_SAVE_UNRESOLVED'}
    Assert-PropertyProcess $Save.receipt
    if((Get-FileHash -LiteralPath $Save.receipt.source_file -Algorithm SHA256).Hash.ToLowerInvariant() -cne
        $Save.receipt.source_sha256){throw 'NR_PROPERTY_PROFILE_PIN'}
    $null=Invoke-NrPropertyOriginal $Restore $Freeze
    Assert-PropertyRoundtrip $Save.nativeOutput.Text $Restore.nativeOutput.Text $Save.receipt $Restore.receipt
}

function Close-NrPropertyOriginal($Owner) {
    # Explicit caller operation; not an automatic failure/finally path.
    $neverStarted=$Owner.started -is [bool] -and !$Owner.started
    if(!$neverStarted -and ($Owner.started -isnot [bool] -or !$Owner.started -or
        ($Owner.ContainsKey('sdkSettled') -and ($Owner.sdkSettled -isnot [bool] -or !$Owner.sdkSettled)) -or
        !(Test-NrQaOriginalOwnerClosed $Owner) -or
        ![object]::ReferenceEquals($Owner.handle,$Owner.process.SafeHandle) -or
        !$Owner.exitSignal.Exited.WaitOne(0))){throw 'NR_PROPERTY_DISPOSE_UNRESOLVED'}
    if($Owner.nativeOutput){$Owner.nativeOutput.Dispose()}
    if($Owner.exitSignal){$Owner.exitSignal.Dispose()}
    foreach($event in @($Owner.kernelWait)+$Owner.events+$Owner.retainedEvents+@($Owner.unknownGate)){
        if($null -ne $event){$event.Dispose()}
    }
    if($Owner.process){$Owner.process.Dispose()}
    $Owner.state=if($neverStarted){'original-objects-disposed-never-started'}else{'original-objects-disposed-after-terminal-reap-eof'}
}

function Invoke-NrPropertyTrial($Freeze) {
    if(!(Get-Variable NrQaPropertyOwners -Scope Global -ErrorAction SilentlyContinue)){
        $global:NrQaPropertyOwners=[Collections.Generic.List[object]]::new()
    }
    $record=@{schema=1;scenario='property-roundtrip';result='FAIL';native_acceptance=$false;root=$root;
        manifest=$Task15CandidateManifest;manifest_sha256=$Task15CandidateSha256;build=$build;
        original_objects_retained=$true;failure=$null;properties=$null}
    try {
        # No Force and no reuse of a consumed evidence/root pair.
        New-Item -ItemType Directory $Evidence|Out-Null
        $save=New-NrPropertyOwner save $root $Evidence
        $restore=New-NrPropertyOwner restore $root $Evidence
        # Both graphs are now published, including exit and EOF subscriptions.
        $sources=[Collections.Generic.List[object]]::new()
        $driver=Resolve-NrInstanceDriver (Join-Path $work 'out/task-13-diagnostic-r4/native/Debug/nr-driver-inspect.exe')
        $record.driver_binding=$driver
        New-Item -ItemType Directory $root|Out-Null
        $staging=Stage-NrInstanceHost $Freeze
        $record.staging=$staging
        $record.properties=Invoke-NrPropertyPair $save $restore $Freeze
        $record.result='PASS'
    } catch {
        $record.failure=$_.Exception.Message
        foreach($owner in $global:NrQaPropertyOwners) {
            if(!$owner.closed -or ($owner.ContainsKey('sdkSettled') -and !$owner.sdkSettled)){
                $owner.state='original-owner-retained';$owner.primaryError=$_
            }
        }
    }
    # Preserve every actual owner on every failed/unknown boundary. Caller releases
    # terminal originals explicitly through Close-NrPropertyOriginal.
    $record.owners=@($global:NrQaPropertyOwners|ForEach-Object {@{state=$_.state;identity=$_.identity;started=$_.started;closed=$_.closed}})
    if(Test-Path -LiteralPath $Evidence) {
        $record|ConvertTo-Json -Depth 40|Set-Content -LiteralPath (Join-Path $Evidence 'properties.json') -Encoding UTF8
    }
    $record
}
