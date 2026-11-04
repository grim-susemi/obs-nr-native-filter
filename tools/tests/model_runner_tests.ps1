param([Parameter(Mandatory)][string]$Work)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
. (Join-Path $Work 'tools/host/HostSupport.ps1')
$runner=Join-Path $Work 'tools/Invoke-NrQa.ps1'
$runtime='C:/Program Files/obs-studio/bin/64bit/nvngx_dlssnr.dll'
$evidence=Join-Path $Work '.omo/evidence/obs-nr-native-filter-20260930/task-11'
$producer=Join-Path $Work 'out/task-11/host'
$verifier=Join-Path $Work 'out/task-11-verifier/host'
function Child([string]$Command) {
    $p=[Diagnostics.Process]::new()
    $p.StartInfo.FileName="$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe"
    $p.StartInfo.Arguments='-NoProfile -ExecutionPolicy Bypass -Command "'+$Command.Replace('"','\"')+'"'
    $p.StartInfo.UseShellExecute=$false;$p.StartInfo.CreateNoWindow=$true
    $p.StartInfo.RedirectStandardOutput=$true;$p.StartInfo.RedirectStandardError=$true
    $p.EnableRaisingEvents=$true;$exit=[NrProcessExit]::new($p)
    try {
        if(!$p.Start()){throw 'model guard child start'}
        $out=$p.StandardOutput.ReadToEndAsync();$err=$p.StandardError.ReadToEndAsync()
        if(!$exit.Exited.WaitOne(15000)){throw 'model CPU guard timeout; exact child retained'}
        if(!$p.WaitForExit(5000)){throw 'model CPU guard reaping'}
        @{exit=$p.ExitCode;stdout=$out.GetAwaiter().GetResult();stderr=$err.GetAwaiter().GetResult()}
    }finally{if($p.HasExited){$exit.Dispose();$p.Dispose()}}
}
function Reject([string]$Evidence,[string]$Root,[string]$Runtime,[string]$Token,[string]$Extra='') {
    $command="& '$runner' -Scenario native-model -Evidence '$Evidence' -HostRoot '$Root' -Runtime '$Runtime' $Extra"
    $row=Child $command
    if(!$row.exit -or !($row.stdout+$row.stderr).Contains($Token)){throw ('Wrong native guard '+$row.stdout+$row.stderr)}
    Write-Output ('PASS native guard '+$Token)
}
if((Test-Path $producer) -or (Test-Path $verifier)){throw 'Native model CPU guards require absent owned host roots'}
Reject $evidence $producer $runtime 'NR_QA_OUTPUT_DIGEST_SCOPE' '-CoreInitControl -OutputDigest Z:/nope/x.json'
Reject $evidence $producer 'C:/not-selected/runtime.dll' 'NR_QA_RUNTIME_REFUSED' '-OutputDigest Z:/nope/x.json'
foreach($pair in @(@($evidence,$producer,$verifier),@((Join-Path $evidence 'verifier'),$verifier,$producer))){
    Reject $pair[0] $pair[1] 'C:/not-selected/runtime.dll' 'NR_QA_RUNTIME_REFUSED'
    Reject $pair[0] $pair[1] $runtime 'NR_QA_FOREIGN_PROCESS' ('-ForeignPid '+$PID)
    Reject $pair[0] $pair[2] $runtime 'NR_QA_FOREIGN_ROOT'
}
foreach($bad in @(($evidence+'-near'),(Join-Path $evidence 'nested'),(Join-Path $evidence 'verifier/nested'))){
    Reject $bad $producer $runtime 'NR_QA_EVIDENCE_REFUSED'
}
foreach($pair in @(@($evidence,$producer,(Join-Path $Work 'out/task-11/native/Debug')),
    @((Join-Path $evidence 'verifier'),$verifier,(Join-Path $Work 'out/task-11-verifier/native/Debug')))){
    foreach($missing in @('obs-nr-filter.dll','nr-qa-host.exe','nr-driver-inspect.exe','nr_codec.dxil','nr_color.dxil','nr_guides.cso','nr_interop_vertex.cso','nr_interop_pixel.cso')){
        # Same narrow actual-runner probe as accepted9a: isolate only process
        # presence/file availability, preserving Get-FileHash's PathType checks.
        $probe=@'
function global:Get-Process { @() }
$global:queried=[Collections.Generic.List[string]]::new()
function global:Test-Path {
    [CmdletBinding()]param([Parameter(Position=0)][string]$Path,[string]$LiteralPath,[string]$PathType)
    $candidate=if($LiteralPath){$LiteralPath}else{$Path}
    $name=[IO.Path]::GetFileName($candidate)
    if(!$PathType -and $name -in @('obs-nr-filter.dll','nr-qa-host.exe','nr-driver-inspect.exe','nr_codec.dxil','nr_color.dxil','nr_guides.cso','nr_interop_vertex.cso','nr_interop_pixel.cso')){
        $global:queried.Add([IO.Path]::GetFullPath($candidate));return $name -ne '@MISSING@'
    }
    Microsoft.PowerShell.Management\Test-Path @PSBoundParameters
}
try{
    & '@RUNNER@' -Scenario native-model -Evidence '@EVIDENCE@' -HostRoot '@ROOT@' -Runtime '@RUNTIME@'
    throw 'native build probe reached host'
}catch{
    if($_.Exception.Message -ne 'NR_QA_BUILD_MISSING: @MISSING@'){throw}
    @{queried=@($global:queried.ToArray());missing='@MISSING@';host_absent=!(Microsoft.PowerShell.Management\Test-Path '@ROOT@')} | ConvertTo-Json -Compress
}
'@
        foreach($entry in @(@('@RUNNER@',$runner),@('@EVIDENCE@',$pair[0]),@('@ROOT@',$pair[1]),@('@RUNTIME@',$runtime),@('@MISSING@',$missing))){
            $probe=$probe.Replace($entry[0],$entry[1].Replace("'","''"))
        }
        $result=Child $probe
        if($result.exit){throw ('Native build probe failed '+$result.stdout+$result.stderr)}
        $row=$result.stdout | ConvertFrom-Json
        if(!$row.host_absent -or !$row.queried.Count){throw 'Native build probe staged a host'}
        foreach($path in $row.queried){
            $name=[IO.Path]::GetFileName($path)
            $expected=Join-Path $(if($name.EndsWith('.dll') -or $name.EndsWith('.exe')){$pair[2]}else{Split-Path $pair[2]}) $name
            if($path -ne [IO.Path]::GetFullPath($expected)){throw 'Native fresh build fallback or wrong selected path'}
        }
        Write-Output ('PASS native selected '+$row.missing+' '+$row.queried[-1])
    }
}
Write-Output 'PASS native public pair and fresh build refusal probes; CPU only, no model/GPU claim'
# Given: complete receipts for all five lanes, including cold and valid history.
$pinRows=@()
foreach($lane in 0..4){
    $count=if($lane -eq 0){6}elseif($lane -lt 3){1}else{3}
    foreach($step in 0..($count-1)){
        $eligible=$lane -eq 0 -and $step -gt 0
        $reference=$eligible -and $step -in @(2,3,5)
        $selected=$lane -eq 0 -and $step -in @(2,3,5)
        $pinRows+=@{lane=$lane;step=$step;created=($step -eq 0);evaluated=$eligible;composed=$eligible;
            submitted=$eligible;gpu_composed=$eligible;reset=($eligible -and !$reference);
            reference_valid=$reference;driver_handle=$false;capture_pin_open=$true;capture_pin=5;consumer_done=4;
            instance_id=(111+$lane);ingress_sequence=($step+1);submission_serial=($step+1);
            settings_revision=1;resource_generation=1;clock=0;interval=1;tick=1;deadline=2;slot=0;
            feature=18;width=1920;height=1080;raw_hash='10';analysis_hash='10';raw_resource='1';analysis_resource='2';
            source_format=87;producer=1;producer_done=1;consumer=4;code=$(if($eligible){1}else{0});
            invalid_status=0;changed_pixels=$(if($reference){1}else{0});physical_done=1;epoch=2;creation_epoch=1;
            module_base='3';input_hash='11';model_hash='12';motion_hash='13';depth_hash='14';output_hash='15';
            color_resources=@('1','2','3','4','5');descriptor_heaps=@('6','7','8');status_resource='9';
            output_resource='10';motion_resource='11';depth_resource='12';mv_scale_x=1;mv_scale_y=1;
            pre_exposure=1;exposure=1;chronology=@(0,1,2,3,4,5,6,7);
            selected=$(if($selected){3}elseif($lane -ge 3 -and $step -eq 2){2}else{1});
            selected_sequence=$step;target_hash='10';target_changed_pixels=$(if($lane -eq 0 -and $step -eq 3){1}else{0})}
    }
}
$contexts=@(0..4 | ForEach-Object {@{lane=$_;software=$false;private_direct_queue=$true;
    adapter='RTX 4090';device='1';queue='2';renderer_luid_high=0;worker_luid_high=0;renderer_luid_low=1;worker_luid_low=1}})
$cleanup=@(0..4 | ForEach-Object {@{lane=$_;worker_exit=$true;closed=$true;retained=$false;
    applied=$(if($_ -eq 0){3}else{0});nr_selections=3;raw_selections=0}})
# When: the production receipt validator consumes the full valid fixture.
Assert-ModelRows $pinRows $contexts $cleanup @(@{})
# Then: both strict pixel branches and selected n-1 rows are accepted.
Write-Output 'PASS complete native receipt fixture accepted'
$validJson=ConvertTo-Json -InputObject $pinRows -Depth 8
foreach($case in @(
    @{row=1;field='changed_pixels';value=1;error='NR_QA_MODEL_REFERENCE_PIXELS'},
    @{row=2;field='changed_pixels';value=0;error='NR_QA_MODEL_REFERENCE_PIXELS'},
    @{row=2;field='target_changed_pixels';value=1;error='NR_QA_MODEL_SELECTED_REFERENCE_PIXELS'},
    @{row=3;field='target_changed_pixels';value=0;error='NR_QA_MODEL_SELECTED_REFERENCE_PIXELS'},
    @{row=1;field='reference_valid';value=$null;error='NR_QA_MODEL_BOOLEAN'},
    @{row=1;field='reference_valid';value='false';error='NR_QA_MODEL_BOOLEAN'},
    @{row=1;field='reset';value=$false;error='NR_QA_MODEL_RESET'},
    @{row=2;field='reset';value=$true;error='NR_QA_MODEL_RESET'}
)){
    # Given: a fresh accepted fixture with exactly one machine field changed.
    $rows=ConvertFrom-Json $validJson
    if($null -eq $case.value){$rows[$case.row].PSObject.Properties.Remove($case.field)}
    else{$rows[$case.row].($case.field)=$case.value}
    $refused=$false
    # When: the actual production validator consumes the mutation.
    try{Assert-ModelRows $rows $contexts $cleanup @(@{})}
    # Then: only the intended exact rejection counts as success.
    catch{if($_.Exception.Message -ne $case.error){throw};$refused=$true}
    if(!$refused){throw ('Native receipt mutation accepted: '+$case.field)}
    Write-Output ('PASS native receipt refusal '+$case.error+' row='+$case.row+' field='+$case.field)
}
foreach($case in @(@(3,4,$true),@(4,4,$true),@(5,4,$false))){
    $pinRows[0].capture_pin=$case[0];$pinRows[0].consumer_done=$case[1];$pinRows[0].capture_pin_open=$case[2]
    $refused=$false
    try{Assert-ModelRows $pinRows $contexts $cleanup @(@{})}
    catch{if($_.Exception.Message -ne 'NR_QA_MODEL_PIN_ORDER'){throw};$refused=$true}
    if(!$refused){throw 'Open capture pin order was accepted'}
}
Write-Output 'PASS native machine capture-pin ordering refusals; parser only, no GPU claim'
# Given: a cost receipt with a reset frame and five subsequent completed frames.
$costCreate=@(@{width=1280;height=720;result=1})
$costFrames=@(1..6 | ForEach-Object {@{frame=$_;reset=($_ -eq 1);result=1;begin=1000;end=1100;frequency=1000000;gpu_ms=0.1}})
# When: the cost-specific validator consumes it. Then: the independent size is accepted.
Assert-CostRows $costCreate $costFrames '1280x720'
Write-Output 'PASS cost frame fixture'
foreach($mutation in @(
    @{field='reset';value=$false},
    @{field='gpu_ms';value=0.2},
    @{field='frequency';value=0},
    @{field='result';value=0}
)){
    $rows=ConvertFrom-Json (ConvertTo-Json -InputObject $costFrames)
    $rows[0].($mutation.field)=$mutation.value
    $rejected=$false
    try{Assert-CostRows $costCreate $rows '1280x720'}catch{$rejected=$true}
    if(!$rejected){throw ('Cost mutation accepted: '+$mutation.field)}
}
Write-Output 'PASS cost frame mutation refusals'
Assert-CostRows @(@{width=64;height=64;result=1}) @() '64x64' @(@{frame=1;result=3134193675})
$rejected=$false
try{Assert-CostRows $costCreate @() '1280x720' @(@{frame=1;result=3134193675})}catch{$rejected=$true}
if(!$rejected){throw 'Large cost evaluation failure was accepted'}
Write-Output 'PASS small failure outcome and large failure refusal'
Reject $evidence $producer $runtime 'NR_QA_INVALID_SIZE' '-Size 0x0'
Reject $evidence $producer $runtime 'NR_QA_PASS_CASE_SCOPE' '-PassCase two -OutputDigest Z:/nope/x.json'
. (Join-Path $Work 'tools/tests/model_pass_rows_tests.ps1')
