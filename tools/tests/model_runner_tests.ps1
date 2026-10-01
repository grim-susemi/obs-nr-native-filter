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
    foreach($missing in @('obs-nr-filter.dll','nr-qa-host.exe','nr_codec.dxil','nr_color.dxil','nr_guides.cso','nr_interop_vertex.cso','nr_interop_pixel.cso')){
        # Same narrow actual-runner probe as accepted9a: isolate only process
        # presence/file availability, preserving Get-FileHash's PathType checks.
        $probe=@'
function global:Get-Process { @() }
$global:queried=[Collections.Generic.List[string]]::new()
function global:Test-Path {
    [CmdletBinding()]param([Parameter(Position=0)][string]$Path,[string]$LiteralPath,[string]$PathType)
    $candidate=if($LiteralPath){$LiteralPath}else{$Path}
    $name=[IO.Path]::GetFileName($candidate)
    if(!$PathType -and $name -in @('obs-nr-filter.dll','nr-qa-host.exe','nr_codec.dxil','nr_color.dxil','nr_guides.cso','nr_interop_vertex.cso','nr_interop_pixel.cso')){
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
# Machine-proof regression: a higher completed Consumer must not overtake an
# open lower capture pin. Fixtures stop at this exact production-parser boundary.
$pinRows=@()
foreach($lane in 0..4){
    $count=if($lane -eq 0){6}elseif($lane -lt 3){1}else{3}
    foreach($step in 0..($count-1)){
        $pinRows+=@{lane=$lane;step=$step;created=$false;evaluated=$false;composed=$false;
            submitted=$false;gpu_composed=$false;reset=$false;driver_handle=$false;
            capture_pin_open=$true;capture_pin=3;consumer_done=4}
    }
}
$contexts=@(0..4 | ForEach-Object {@{lane=$_;software=$false;private_direct_queue=$true;
    adapter='RTX 4090';device='1';queue='2';renderer_luid_high=0;worker_luid_high=0;renderer_luid_low=1;worker_luid_low=1}})
$cleanup=@(0..4 | ForEach-Object {@{lane=$_;worker_exit=$true;closed=$true;retained=$false;
    applied=$(if($_ -eq 0){3}else{0});nr_selections=3;raw_selections=0}})
foreach($case in @(@(3,4,$true),@(4,4,$true),@(5,4,$false))){
    $pinRows[0].capture_pin=$case[0];$pinRows[0].consumer_done=$case[1];$pinRows[0].capture_pin_open=$case[2]
    $refused=$false
    try{Assert-ModelRows $pinRows $contexts $cleanup @(@{})}
    catch{if($_.Exception.Message -ne 'NR_QA_MODEL_PIN_ORDER'){throw};$refused=$true}
    if(!$refused){throw 'Open capture pin order was accepted'}
}
Write-Output 'PASS native machine capture-pin ordering refusals; parser only, no GPU claim'
