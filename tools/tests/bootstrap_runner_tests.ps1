param([Parameter(Mandatory)][string]$Work)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
. (Join-Path $Work 'tools/host/HostSupport.ps1')
$runner=Join-Path $Work 'tools/Invoke-NrQa.ps1'
$runtime='C:/Program Files/obs-studio/bin/64bit/nvngx_dlssnr.dll'
$base=Join-Path $Work '.omo/evidence/obs-nr-native-filter-20260930/task-12'
function Child([string]$Code) {
    $p=[Diagnostics.Process]::new()
    $p.StartInfo.FileName="$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe"
    $p.StartInfo.Arguments='-NoProfile -ExecutionPolicy Bypass -EncodedCommand '+[Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($Code))
    $p.StartInfo.UseShellExecute=$false;$p.StartInfo.CreateNoWindow=$true
    $p.StartInfo.RedirectStandardOutput=$true;$p.StartInfo.RedirectStandardError=$true
    $p.EnableRaisingEvents=$true;$signal=[NrProcessExit]::new($p)
    try {
        if(!$p.Start()){throw 'Bootstrap guard child start'}
        $out=$p.StandardOutput.ReadToEndAsync();$err=$p.StandardError.ReadToEndAsync()
        if(!$signal.Exited.WaitOne(20000)){throw 'Exact CPU guard child timeout; owner retained'}
        if(!$p.WaitForExit(5000)){throw 'CPU guard reaping failed'}
        $stdout=$out.GetAwaiter().GetResult();$stderr=$err.GetAwaiter().GetResult()
        @{exit=$p.ExitCode;stdout=$stdout;stderr=$stderr;text=($stdout+$stderr)}
    }finally{if($p.HasExited){$signal.Dispose();$p.Dispose()}}
}
foreach($pair in @(@($base,(Join-Path $Work 'out/task-12/host'),(Join-Path $Work 'out/task-12-verifier/host'),(Join-Path $Work 'out/task-12/native/Debug')),
    @((Join-Path $base 'verifier'),(Join-Path $Work 'out/task-12-verifier/host'),(Join-Path $Work 'out/task-12/host'),(Join-Path $Work 'out/task-12-verifier/native/Debug')))) {
    if(Test-Path $pair[1]){throw 'CPU bootstrap checks require absent owned host'}
    foreach($case in @(@($pair[0],$pair[2],$runtime,'NR_QA_FOREIGN_ROOT'),
        @($pair[0],$pair[1],'C:/not-selected/model.dll','NR_QA_RUNTIME_REFUSED'),
        @(($pair[0]+'/nested'),$pair[1],$runtime,'NR_QA_EVIDENCE_REFUSED'))) {
        $row=Child "`$ErrorActionPreference='Stop'; & '$runner' -Scenario native-bootstrap -Evidence '$($case[0])' -HostRoot '$($case[1])' -Runtime '$($case[2])'"
        if(!$row.exit -or !$row.text.Contains($case[3])){throw ('Wrong bootstrap refusal '+$row.text)}
    }
    foreach($missing in @('nr-qa-host.exe','nr_obs_analysis.cso')) {
        $probe=@'
$ErrorActionPreference='Stop'
function global:Get-Process { @() }
$global:paths=[Collections.Generic.List[string]]::new()
function global:Test-Path {
    [CmdletBinding()]param([Parameter(Position=0)][string]$Path,[string]$LiteralPath,[string]$PathType)
    $candidate=if($LiteralPath){$LiteralPath}else{$Path}
    $name=[IO.Path]::GetFileName($candidate)
    if(!$PathType -and $name -in @('obs-nr-filter.dll','nr-qa-host.exe','nr_codec.dxil','nr_color.dxil','nr_guides.cso','nr_interop_vertex.cso','nr_interop_pixel.cso','nr_obs_analysis.cso')){
        $global:paths.Add([IO.Path]::GetFullPath($candidate));return $name -ne '@MISSING@'
    }
    Microsoft.PowerShell.Management\Test-Path @PSBoundParameters
}
try {
    & '@RUNNER@' -Scenario native-bootstrap -Evidence '@EVIDENCE@' -HostRoot '@ROOT@' -Runtime '@RUNTIME@'
    throw 'Build guard reached native host'
}catch{
    if($_.Exception.Message -ne 'NR_QA_BUILD_MISSING: @MISSING@'){throw}
    @{paths=@($global:paths.ToArray());host_absent=!(Microsoft.PowerShell.Management\Test-Path '@ROOT@')}|ConvertTo-Json -Compress
}
'@
        foreach($entry in @(@('@RUNNER@',$runner),@('@EVIDENCE@',$pair[0]),@('@ROOT@',$pair[1]),@('@RUNTIME@',$runtime),@('@MISSING@',$missing))){
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
