# SPDX-License-Identifier: GPL-3.0-only
# CPU-only tests for the Task27 ghost-baseline wrapper pieces. No native run, no OBS, no GPU.
param(
    [Parameter(Mandatory)][string]$SupportDirectory,   # directory holding GhostDigestSupport.ps1 and Scenario-GhostBaseline.ps1
    [Parameter(Mandatory)][string]$Scratch,            # new empty directory
    [string]$BaseWrapper,                              # Invoke-NrQa.ps1 at 029695b7 (optional hunk apply check)
    [string]$Hunk,
    [string]$ExpectedPatchedSha256
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
. (Join-Path $SupportDirectory 'GhostDigestSupport.ps1')
. (Join-Path $SupportDirectory 'Scenario-GhostBaseline.ps1')
$Scratch=Join-Path $Scratch ([Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $Scratch -Force | Out-Null
$NL=[string][char]10
$script:checks=0
function Expect([bool]$Condition,[string]$What){ $script:checks++; if(!$Condition){throw "TEST_FAIL: $What"} }
function Expect-Throw([string]$Code,[scriptblock]$Body) {
    $script:checks++
    try{ & $Body }catch{ if($_.Exception.Message -ceq $Code){return}; throw "TEST_FAIL: expected $Code got $($_.Exception.Message)" }
    throw "TEST_FAIL: expected $Code but nothing was thrown"
}
function Sha([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
$calSha=('a'*64); $ctlSha=('b'*64)
$names=@('blend_2percent','blur_1pixel','frozen','extra_delay','dropped','stale_guide','reordered_completion','poststamp_only','raw_current')
function New-Baseline([string]$Name,[bool]$NrPass) {
    $art=Join-Path $Scratch $Name
    New-Item -ItemType Directory -Path $art | Out-Null
    $rungs=@()
    foreach($rate in 0,1){foreach($mode in 0..3){foreach($rung in 'raw','binary','unprotected_nr'){
        $rungs+=,[ordered]@{rung=$rung;rate_index=$rate;mode=$mode;pass=($rung -ne 'unprotected_nr' -or $NrPass);epsilon=0.01;
            ticks=256;key_anomalies=0;instance=(100+$rate*4+$mode);first_ingress=1000;last_ingress=1255;
            kinds=[ordered]@{current_source=$(if($rung -eq 'raw'){256}else{0});priming_raw=$(if($rung -eq 'raw'){0}else{1});
                delayed_raw=0;delayed_nr=$(if($rung -eq 'raw'){0}else{255})};
            raw_transport=[ordered]@{checks=$(if($rung -eq 'raw'){256}else{0});failures=0;off_current_samples=$(if($rung -eq 'raw'){256}else{0})};
            codec=[ordered]@{required=$true;frames=32;code_errors=0}}
            $dir=Join-Path $art ('baseline-'+$(if($rung -eq 'raw'){'raw'}elseif($rung -eq 'binary'){'binary'}else{'nr'})+"-rate-$rate-mode-$mode")
            New-Item -ItemType Directory -Path $dir | Out-Null
            foreach($f in 'source.bmp','filter.bmp','program.bmp','program.hevc'){[IO.File]::WriteAllBytes((Join-Path $dir $f),[byte[]](1,2,3,$rate,$mode))}
        }}}
    $defects=@()
    if(!$NrPass){$defects=@([ordered]@{id='D-0001';rung='unprotected_nr';category='temporal_trail';detail='incremental trail above bound';
        rate_index=0;mode=0;tick=12;prior=1;region=0;persistence=3;occurrences=9;measured=0.07;bound=0.01;for_task28=$true})}
    $summary=[ordered]@{schema=1;kind='GHOST_BASELINE';evidence_class='physical';test_not_release=$true;
        identity_digest_algorithm='sha256-chunk-root-v1';host_sha256=('c'*64);host_source_sha256=('d'*64);holdout_seed=2857;
        calibration_seed_measured_here=$false;
        freeze=[ordered]@{policy_hashes=[ordered]@{'calibration.json'=('1'*64);'holdout.json'=('2'*64);'masks.json'=('3'*64);
            'epsilon.json'=('4'*64);'thresholds.json'=('5'*64)};manifest_sha256=('6'*64);read_only=$true;files_written=0};
        accepted_calibration=[ordered]@{calibration_sha256=$calSha;controls_sha256=$ctlSha;host_sha256=('e'*64);host_source_sha256=('f'*64)};
        result=$(if($NrPass){'BASELINE_VALID_NR_PASS'}else{'BASELINE_VALID_NR_DEFECTS'});baseline_valid=$true;raw_pass=$true;binary_pass=$true;
        controls_pass=$true;unprotected_nr_pass=$NrPass;faults=@();rungs=$rungs;
        bad_controls=@($names|ForEach-Object {[ordered]@{name=$_;cases=8;actual_pixels=$true;rejected=$true}});positive_anchors=8;
        inherited_trail_attribution=@(0..3|ForEach-Object {[ordered]@{label='x';mode=$_;ticks=255;episode_ticks=256;first_scored_tick=1;last_scored_tick=255;templates=10;maximum_incremental=0;
            maximum_raw=0.1;maximum_total=0.1;raw_equals_total=$true;attributed_to_plugin=$false}});defects=$defects}
    return @{art=$art;summary=$summary}
}
function Save-Baseline($Fixture) {
    $path=Join-Path $Fixture.art 'ghost-baseline.json'
    ConvertTo-Json -InputObject $Fixture.summary -Depth 12 | Set-Content -LiteralPath $path -Encoding UTF8
    $sha=Sha $path; $s=$Fixture.summary
    $out=@("NR_GHOST_BASELINE_ADMITTED holdout_seed=2857 accepted_calibration_sha256=$calSha accepted_controls_sha256=$ctlSha freeze_read_only=true",
        'NR_GHOST_BASELINE holdout_started=true freeze_written=false rungs=raw,binary,unprotected_nr',
        ('NR_GHOST_BASELINE_RESULT result='+$s.result+' baseline_valid=1 raw_pass=1 binary_pass=1 controls_pass=1 unprotected_nr_pass='+
            $(if($s.unprotected_nr_pass){1}else{0})+' defects='+@($s.defects).Count+' summary_sha256='+$sha+' freeze_files_written=0'))
    return ($out -join $NL)+$NL
}
function Run($Fixture,[string]$Output) { Assert-GhostBaselinePublic $Output $Fixture.art $calSha $ctlSha }
$script:n=0
function Fresh([bool]$NrPass=$false){$script:n++;New-Baseline ("case$script:n") $NrPass}

# GREEN: unprotected NR failing is a measurement with a machine DEFECTS list; NR passing has none.
$f=Fresh $false; $r=Run $f (Save-Baseline $f)
Expect ($r.result -ceq 'BASELINE_VALID_NR_DEFECTS' -and !$r.unprotected_nr_pass -and $r.DEFECTS.Count -eq 1 -and $r.rungs -eq 24 -and $r.snapshots.Count -eq 96) 'defects variant accepted'
Expect ($r.DEFECTS[0].for_task28 -eq $true) 'defect flagged for Task28'
$f=Fresh $true; $r=Run $f (Save-Baseline $f)
Expect ($r.result -ceq 'BASELINE_VALID_NR_PASS' -and $r.unprotected_nr_pass -and $r.DEFECTS.Count -eq 0) 'nr pass variant accepted'

# RED: late-bound hash binding, freeze immutability, rung and control integrity.
$f=Fresh; $o=Save-Baseline $f
Expect-Throw 'NR_QA_GHOST_BASELINE_ACCEPTED_HASH' { Assert-GhostBaselinePublic $o $f.art ('9'*64) $ctlSha }
Expect-Throw 'NR_QA_GHOST_SHA256_ENCODING' { Assert-GhostBaselinePublic $o $f.art '' $ctlSha }
Expect-Throw 'NR_QA_GHOST_SHA256_ENCODING' { Assert-GhostBaselinePublic $o $f.art $calSha 'ABC' }
Expect-Throw 'NR_QA_GHOST_BASELINE_ACTUAL_FAILURE' { Run $f ($o+'NR_GHOST_FAILURE baseline rung=raw'+$NL) }
Expect-Throw 'NR_QA_GHOST_BASELINE_ACTUAL_FAILURE' { Run $f ($o+'NR_GHOST_RETAINED x'+$NL) }
Expect-Throw 'NR_QA_GHOST_BASELINE_MARKERS' { Run $f ($o -replace 'NR_GHOST_BASELINE_ADMITTED','NR_GHOST_BASELINE_ADMITTEE') }
Expect-Throw 'NR_QA_GHOST_BASELINE_ADMISSION' { Run $f ($o -replace 'holdout_seed=2857','holdout_seed=2583') }
Expect-Throw 'NR_QA_GHOST_BASELINE_ADMISSION' { Run $f ($o -replace 'freeze_read_only=true','freeze_read_only=false') }
Expect-Throw 'NR_QA_GHOST_BASELINE_CONTROL_RUNG_FAILED' { Run $f ($o -replace 'binary_pass=1','binary_pass=0') }
Expect-Throw 'NR_QA_GHOST_BASELINE_RESULT_LINE' { Run $f ($o -replace 'freeze_files_written=0','freeze_files_written=1') }
$f=Fresh; $f.summary.freeze.files_written=1; $o=Save-Baseline $f
Expect-Throw 'NR_QA_GHOST_BASELINE_FREEZE_WRITTEN' { Run $f $o }
$f=Fresh; $f.summary.rungs=@($f.summary.rungs|Select-Object -First 23); $o=Save-Baseline $f
Expect-Throw 'NR_QA_GHOST_BASELINE_RUNG_COUNT' { Run $f $o }
$f=Fresh; $f.summary.rungs[0].pass=$false; $o=Save-Baseline $f
Expect-Throw 'NR_QA_GHOST_BASELINE_CONTROL_RUNG_FAILED' { Run $f $o }
$f=Fresh; $f.summary.rungs[1].pass=$false; $o=Save-Baseline $f
Expect-Throw 'NR_QA_GHOST_BASELINE_CONTROL_RUNG_FAILED' { Run $f $o }
$f=Fresh; $f.summary.rungs[2].ticks=255; $o=Save-Baseline $f
Expect-Throw 'NR_QA_GHOST_BASELINE_RUNG_CORPUS' { Run $f $o }
$f=Fresh; $f.summary.rungs[0].kinds=$f.summary.rungs[2].kinds; $o=Save-Baseline $f
Expect-Throw 'NR_QA_GHOST_BASELINE_RAW_CURRENT' { Run $f $o }
$f=Fresh; $f.summary.rungs[0].raw_transport.off_current_samples=1; $o=Save-Baseline $f
Expect-Throw 'NR_QA_GHOST_BASELINE_RAW_CURRENT' { Run $f $o }
$f=Fresh; $f.summary.rungs[0].codec.required=$false; $o=Save-Baseline $f
Expect-Throw 'NR_QA_GHOST_BASELINE_CODEC' { Run $f $o }
$f=Fresh; $o=Save-Baseline $f; Remove-Item -LiteralPath (Join-Path $f.art 'baseline-raw-rate-1-mode-2/program.hevc')
Expect-Throw 'NR_QA_GHOST_BASELINE_PHYSICAL_ARTIFACT' { Run $f $o }
$f=Fresh; $f.summary.bad_controls[3].rejected=$false; $o=Save-Baseline $f
Expect-Throw 'NR_QA_GHOST_BASELINE_PLANTED_CONTROL' { Run $f $o }
foreach($count in 254,256){
    $f=Fresh; $f.summary.inherited_trail_attribution[0].ticks=$count; $o=Save-Baseline $f
    Expect-Throw 'NR_QA_GHOST_BASELINE_INHERITED' { Run $f $o }
}
$f=Fresh; $f.summary.faults=@('nr rate 0 mode 1: lost'); $o=Save-Baseline $f
Expect-Throw 'NR_QA_GHOST_BASELINE_FAULT_LABELS' { Run $f $o }
$f=Fresh; $f.summary.defects=@(); $o=Save-Baseline $f
Expect-Throw 'NR_QA_GHOST_BASELINE_DEFECTS_MISSING' { Run $f $o }
$f=Fresh; $f.summary.accepted_calibration.calibration_sha256=('7'*64); $o=Save-Baseline $f
Expect-Throw 'NR_QA_GHOST_BASELINE_ACCEPTED_HASH' { Run $f $o }
$f=Fresh; $o=Save-Baseline $f; Add-Content -LiteralPath (Join-Path $f.art 'ghost-baseline.json') -Value ' '
Expect-Throw 'NR_QA_GHOST_BASELINE_SUMMARY_HASH' { Run $f $o }
$f=Fresh; $o=Save-Baseline $f; Remove-Item -LiteralPath (Join-Path $f.art 'baseline-nr-rate-1-mode-2/program.hevc')
Expect-Throw 'NR_QA_GHOST_BASELINE_PHYSICAL_ARTIFACT' { Run $f $o }
$f=Fresh; $o=Save-Baseline $f; Remove-Item -LiteralPath (Join-Path $f.art 'ghost-baseline.json')
Expect-Throw 'NR_QA_GHOST_BASELINE_SUMMARY_MISSING' { Run $f $o }

# Accepted-calibration binding is read-only and hash-exact.
$work=Join-Path $Scratch 'work'; $t26=Join-Path $work '.omo/evidence/obs-nr-native-filter-20260930/task-26'
New-Item -ItemType Directory -Path $t26 -Force | Out-Null
$calPath=Join-Path $t26 'calibration.json'; $ctlPath=Join-Path $t26 'bad-controls.json'
Set-Content -LiteralPath $calPath -Value '{"cal":1}' -Encoding UTF8; Set-Content -LiteralPath $ctlPath -Value '{"ctl":1}' -Encoding UTF8
$cs=Sha $calPath; $ks=Sha $ctlPath
$b=Get-GhostBaselineAcceptedBinding $work $cs $ks
$j=$b.json|ConvertFrom-Json
Expect ($j.calibration_sha256 -ceq $cs -and $j.controls_sha256 -ceq $ks -and $j.calibration_path.Contains('/task-26/') -and !$j.calibration_path.Contains('\')) 'binding json'
Expect ($cs -ceq (Sha $calPath) -and $ks -ceq (Sha $ctlPath)) 'accepted documents unchanged by the binding'
Expect-Throw 'NR_QA_GHOST_BASELINE_ACCEPTED_HASH_REQUIRED' { Get-GhostBaselineAcceptedBinding $work '' $ks }
Expect-Throw 'NR_QA_GHOST_BASELINE_ACCEPTED_HASH_REQUIRED' { Get-GhostBaselineAcceptedBinding $work $cs 'XYZ' }
Expect-Throw 'NR_QA_GHOST_BASELINE_ACCEPTED_HASH_MISMATCH' { Get-GhostBaselineAcceptedBinding $work ('0'*64) $ks }
Remove-Item -LiteralPath $ctlPath
Expect-Throw 'NR_QA_GHOST_BASELINE_ACCEPTED_MISSING' { Get-GhostBaselineAcceptedBinding $work $cs $ks }

# Hunk applies to exactly 029695b7 and the result parses.
if($BaseWrapper -and $Hunk) {
    $tree=Join-Path $Scratch 'hunk-tree'
    New-Item -ItemType Directory -Path (Join-Path $tree 'tools') -Force | Out-Null
    Copy-Item -LiteralPath $BaseWrapper -Destination (Join-Path $tree 'tools/Invoke-NrQa.ps1')
    Expect ((Sha (Join-Path $tree 'tools/Invoke-NrQa.ps1')) -ceq '029695b7cb341cf914665f127f524aadb6d21de2f7acf0d6e8bac699bbe74b4b') 'hunk base is 029695b7'
    # Exact-context replay without git or fuzzy application.
    $before=[IO.File]::ReadAllText($BaseWrapper)
    $sourceLines=@($before -split "`n");$result=[Collections.Generic.List[string]]::new();$cursor=0
    $diff=@([IO.File]::ReadAllLines($Hunk))
    for($i=0;$i -lt $diff.Count;$i++){
        if($diff[$i] -cnotmatch '^@@ -([0-9]+)(?:,[0-9]+)? \+[0-9]+(?:,[0-9]+)? @@'){continue}
        $start=[int]$Matches[1]-1
        while($cursor -lt $start){$result.Add($sourceLines[$cursor]);$cursor++}
        $i++
        while($i -lt $diff.Count -and $diff[$i] -cnotmatch '^@@ '){
            $line=$diff[$i]
            if($line.StartsWith(' ') -or $line.StartsWith('-')){
                if($cursor -ge $sourceLines.Count -or $sourceLines[$cursor] -cne $line.Substring(1)){throw 'TEST_FAIL: hunk context mismatch'}
                if($line.StartsWith(' ')){$result.Add($sourceLines[$cursor])}
                $cursor++
            }elseif($line.StartsWith('+')){$result.Add($line.Substring(1))}
            $i++
        }
        $i--
    }
    while($cursor -lt $sourceLines.Count){$result.Add($sourceLines[$cursor]);$cursor++}
    [IO.File]::WriteAllText((Join-Path $tree 'tools/Invoke-NrQa.ps1'),($result -join "`n"),[Text.UTF8Encoding]::new($false))
    $patched=Join-Path $tree 'tools/Invoke-NrQa.ps1'
    $errors=$null;$tokens=$null
    [void][System.Management.Automation.Language.Parser]::ParseFile($patched,[ref]$tokens,[ref]$errors)
    Expect ($errors.Count -eq 0) 'patched wrapper parses'
    if($ExpectedPatchedSha256){Expect ((Sha $patched) -ceq $ExpectedPatchedSha256) 'patched wrapper digest'}
    $text=[IO.File]::ReadAllText($patched)
    Expect ($text.Contains("'ghost-baseline'") -and $text.Contains('Assert-GhostBaselinePublic') -and $text.Contains('NR_GHOST_ACCEPTED_CALIBRATION')) 'dispatch tokens'
}
"GHOST_BASELINE_RUNNER_TESTS_OK checks=$script:checks"
