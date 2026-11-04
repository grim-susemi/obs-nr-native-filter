# SPDX-License-Identifier: GPL-2.0-or-later
# Invoked by the existing CPU-only host_runner_model contract.
foreach($case in @('two','create2','eval2')) {
    $creationFailure=$case -eq 'create2';$evaluationFailure=$case -eq 'eval2'
    $failed=$creationFailure -or $evaluationFailure;$count=if($failed){1}else{2}
    $firstInference=if($evaluationFailure){3}else{2}
    $last=if($evaluationFailure){6}else{5}
    $rows=@(foreach($step in 0..$last){
        @{lane=0;step=$step;requested_passes=2;creation_jobs=[Math]::Min($step+1,2);
            creation_pass=$(if($step -lt 2){$step+1}else{0});width=1920;height=1080;feature=18;
            raw_hash='10';analysis_hash='10';producer=1;producer_done=1;physical_done=1;
            capture_pin_open=$true;capture_pin=3;consumer_done=2;ingress_sequence=$step+1;submission_serial=$step+1;
            created=($step -lt 2 -and (!$creationFailure -or $step -eq 0));evaluated=($step -ge 2);composed=($step -ge $firstInference);
            gpu_composed=($step -ge $firstInference);completed_passes=$(if($evaluationFailure -and $step -eq 2){1}elseif($step -ge $firstInference){$count}else{0});
            job_failed_pass=$(if($evaluationFailure -and $step -eq 2){2}else{0});
            submitted=!(($creationFailure -and $step -eq 1) -or ($evaluationFailure -and $step -eq 2));
            code=$(if($evaluationFailure -and $step -eq 2){0xe018}elseif($step -ge $firstInference){1}elseif($creationFailure -and $step -eq 1){0xe008}else{1});
            selected=$(if($step -gt $firstInference){3}else{1});selected_sequence=$step;
            invalid_status=0;epoch=$step+1;creation_epoch=2;answer_resource='12';model_hash='20';output_hash='30';input_hash='40';
            reset=($step -eq $firstInference);reference_valid=($step -gt $firstInference);changed_pixels=$(if($step -gt $firstInference){1}else{0});
            chronology=@(1,2,3,4,5,6,7,8)}
    })
    $context=@(@{lane=0;software=$false;private_direct_queue=$true;adapter='RTX 4090';device='1';queue='2';
        renderer_luid_high=0;worker_luid_high=0;renderer_luid_low=1;worker_luid_low=1})
    $cleanup=@(@{lane=0;worker_exit=$true;closed=$true;retained=$false;applied=3;nr_selections=3})
    $summary=@(@{requested=2;effective=$count;failed_pass=$(if($failed){2}else{0});code=$(if($evaluationFailure){0xe018}elseif($creationFailure){0xe008}else{0});
        creation_jobs=2;reason=$(if($failed){'passFailed'}else{'none'})})
    # Given complete native-form receipts. When consumed by the real validator.
    Assert-ModelPassRows $rows $context $cleanup @(@{}) $summary $case
    # Then both success and permanent second-creation reduction are admitted.
    Write-Output "PASS native multipass receipt fixture $case CPU_only=true"
    $saved=ConvertTo-Json -InputObject $rows -Depth 8
    if($evaluationFailure) {
        $altered=ConvertFrom-Json $saved;$altered[2].job_failed_pass=1
        $reason=$null;try{Assert-ModelPassRows $altered $context $cleanup @(@{}) $summary $case}catch{$reason=$_.Exception.Message}
        if($reason -ne 'NR_QA_MODEL_PASS_EVALUATION_DISCARD'){throw "Wrong evaluation discard refusal: $reason"}
        continue
    }
    foreach($mutation in @(
        @{row=0;field='completed_passes';value=1;reason='NR_QA_MODEL_PASS_EARLY_PUBLICATION'},
        @{row=2;field='physical_done';value=0;reason='NR_QA_MODEL_PASS_IDENTITY'},
        @{row=4;field='creation_jobs';value=3;reason='NR_QA_MODEL_PASS_IDENTITY'},
        @{row=4;field='completed_passes';value=3;reason='NR_QA_MODEL_PASS_PHYSICAL_RESULT'},
        @{row=3;field='changed_pixels';value=0;reason='NR_QA_MODEL_PASS_PHYSICAL_RESULT'},
        @{row=4;field='selected';value=2;reason='NR_QA_MODEL_PASS_PUBLICATION'}
    )) {
        # Given one changed machine field. When the same validator consumes it.
        $altered=ConvertFrom-Json $saved;$altered[$mutation.row].($mutation.field)=$mutation.value
        $reason=$null
        try{Assert-ModelPassRows $altered $context $cleanup @(@{}) $summary $case}
        catch{$reason=$_.Exception.Message}
        # Then the exact intended refusal is required.
        if($reason -ne $mutation.reason){throw "Wrong multipass refusal $($mutation.field): $reason"}
        Write-Output "PASS native multipass mutation $case $($mutation.field) $reason"
    }
}
foreach($case in @('scale50','odd50','align100','align50')) {
    $w=if($case -eq 'odd50'){1281}else{1920};$h=if($case -eq 'odd50'){721}else{1080}
    $ww=if($case -eq 'odd50'){641}elseif($case -eq 'align100'){1920}else{960}
    $wh=if($case -eq 'odd50'){361}elseif($case -eq 'align100'){1080}else{540}
    $scale=if($case -eq 'align100'){100}else{50}
    $rows=@(foreach($step in 0..5){
        @{lane=0;step=$step;width=$w;height=$h;work_scale=$scale;requested_passes=1;creation_jobs=1;
          physical_done=1;capture_pin_open=$true;producer=1;producer_done=1;raw_hash='1';analysis_hash='1';
          created=($step -eq 0);evaluated=($step -gt 0);composed=($step -gt 0);submitted=$true;
          gpu_composed=($step -gt 0);reset=($step -in @(1,4));reference_valid=($step -in @(2,3,5));
          creation_pass=$(if($step -eq 0){1}else{0});selected=$(if($step -in @(2,3)){3}else{1});selected_sequence=$step;
          completed_passes=$(if($step -gt 0){1}else{0});
          answer_width=$ww;answer_height=$wh;answer_resource='2';model_hash='3';output_hash='4';code=1;invalid_status=0}
    })
    $context=@(@{lane=0;software=$false;private_direct_queue=$true;adapter='RTX 4090';device='1';queue='2';
        renderer_luid_high=0;worker_luid_high=0;renderer_luid_low=1;worker_luid_low=1})
    $cleanup=@(@{worker_exit=$true;closed=$true;retained=$false;applied=3})
    Assert-ModelPassRows $rows $context $cleanup @(@{}) @() $case
    $badWidth=if($ww -eq $w){$ww-1}else{$w}
    foreach($mutation in @(@{field='answer_width';value=$badWidth},@{field='physical_done';value=0})){
        $altered=ConvertFrom-Json (ConvertTo-Json -InputObject $rows -Depth 8)
        $altered[2].($mutation.field)=$mutation.value
        $reason=$null;try{Assert-ModelPassRows $altered $context $cleanup @(@{}) @() $case}catch{$reason=$_.Exception.Message}
        $expected=if($mutation.field -eq 'answer_width'){'NR_QA_MODEL_SCALED_RESULT'}else{'NR_QA_MODEL_SCALED_IDENTITY'}
        if($reason -ne $expected){throw "Wrong scaled refusal $($mutation.field): $reason"}
    }
    Write-Output "PASS native scaled receipt fixture and mutation refusals $case CPU_only=true"
}
foreach($n in 1..3) {
    $rows=@(foreach($step in 0..($n+5)){
        $create=$step -lt $n;$reset=$step -eq $n -or $step -eq $n+3
        @{lane=0;step=$step;width=1920;height=1080;work_scale=100;requested_passes=$n;
          creation_jobs=[Math]::Min($step+1,$n);physical_done=1;capture_pin_open=$true;
          producer=1;producer_done=1;raw_hash='1';analysis_hash='1';created=$create;evaluated=(!$create);
          composed=(!$create);submitted=$true;gpu_composed=(!$create);reset=$reset;reference_valid=(!$create -and !$reset);
          creation_pass=$(if($create){$step+1}else{0});selected=$(if(!$create -and !$reset){3}else{1});
          selected_sequence=$step;completed_passes=$(if($create){0}else{$n});
          answer_width=1920;answer_height=1080;answer_resource='2';model_hash='3';output_hash='4';code=1;invalid_status=0}
    })
    $context=@(@{software=$false;private_direct_queue=$true;adapter='RTX 4090';device='1';queue='2';
        renderer_luid_high=0;worker_luid_high=0;renderer_luid_low=1;worker_luid_low=1})
    $cleanup=@(@{worker_exit=$true;closed=$true;retained=$false;applied=4})
    $status=@(@{requested=$n;effective=$n;creation_jobs=$n})
    Assert-ModelQualityRows $rows $context $cleanup @(@{}) $status "quality$n"
    foreach($mutation in @(@{row=0;field='selected';value=3;reason='NR_QA_MODEL_QUALITY_PRIMING'},
        @{row=$n;field='physical_done';value=0;reason='NR_QA_MODEL_QUALITY_IDENTITY'},
        @{row=$n;field='reset';value=$false;reason='NR_QA_MODEL_QUALITY_RESULT'})){
        $altered=ConvertFrom-Json (ConvertTo-Json -InputObject $rows -Depth 8)
        $altered[$mutation.row].($mutation.field)=$mutation.value
        $reason=$null;try{Assert-ModelQualityRows $altered $context $cleanup @(@{}) $status "quality$n"}catch{$reason=$_.Exception.Message}
        if($reason -ne $mutation.reason){throw "Wrong quality refusal $($mutation.field): $reason"}
    }
    Write-Output "PASS native quality receipt fixture and mutation refusals quality$n CPU_only=true"
}
# Given: actual-form HDR rows for two creation jobs and four physical evaluations.
$hdrRows=@(foreach($step in 0..5){
    @{lane=0;step=$step;width=1920;height=1080;work_scale=50;source_format=10;requested_passes=2;
      creation_jobs=[Math]::Min($step+1,2);physical_done=1;capture_pin_open=$true;producer=1;producer_done=1;
      raw_hash='1';analysis_hash='2';created=($step -lt 2);evaluated=($step -ge 2);composed=($step -ge 2);
      submitted=$true;gpu_composed=($step -ge 2);reset=($step -eq 2);reference_valid=($step -gt 2);
      creation_pass=$(if($step -lt 2){$step+1}else{0});selected=1;completed_passes=$(if($step -ge 2){2}else{0});
      answer_width=960;answer_height=540;answer_resource='2';model_hash='3';output_hash='4';code=1;invalid_status=0}
})
$hdrStats=@(2..5 | ForEach-Object {@{step=$_;encoded_min=0;encoded_max=1;encoded_bad=0;source_max=3.5;output_nonfinite=0}})
$hdrGuides=@(0..5 | ForEach-Object {@{step=$_;format=87;dimension=3;width=1920;height=1080;depth=1;mips=1;samples=1;
    resource_device_match=$true;fence_device_match=$true;fence_required=1;fence_done=1}})
$context=@(@{software=$false;private_direct_queue=$true;adapter='RTX 4090';device='1';queue='2';
    renderer_luid_high=0;worker_luid_high=0;renderer_luid_low=1;worker_luid_low=1})
$cleanup=@(@{worker_exit=$true;closed=$true;retained=$false;applied=3})
$status=@(@{requested=2;effective=2;failed_pass=0;code=0;creation_jobs=2;reason='none'})
Assert-ModelHdrRows $hdrRows $context $cleanup @(@{}) $status $hdrStats $hdrGuides
foreach($mutation in @(@{field='encoded_bad';value=1},@{field='output_nonfinite';value=1},
    @{field='source_max';value=1},@{field='encoded_max';value=1.1})){
    $altered=ConvertFrom-Json (ConvertTo-Json -InputObject $hdrStats -Depth 8)
    $altered[0].($mutation.field)=$mutation.value
    $reason=$null;try{Assert-ModelHdrRows $hdrRows $context $cleanup @(@{}) $status $altered $hdrGuides}catch{$reason=$_.Exception.Message}
    if($reason -ne 'NR_QA_MODEL_HDR_RESULT'){throw "Wrong HDR refusal $($mutation.field): $reason"}
}
$altered=ConvertFrom-Json (ConvertTo-Json -InputObject $hdrGuides -Depth 8);$altered[0].format=90
$reason=$null;try{Assert-ModelHdrRows $hdrRows $context $cleanup @(@{}) $status $hdrStats $altered}catch{$reason=$_.Exception.Message}
if($reason -ne 'NR_QA_MODEL_HDR_GUIDE_INPUT'){throw "Wrong HDR guide format refusal: $reason"}
Write-Output 'PASS native HDR receipt fixture and mutation refusals CPU_only=true'
$pairRows=@(foreach($i in 0..10){
    $lane=if($i -eq 10){1}else{$i%2};$step=if($i -eq 10){5}else{[Math]::Floor($i/2)}
    $wanted=if($lane -eq 0){3}else{1};$created=$step -lt $wanted
    @{lane=$lane;step=$step;instance_id=111+$lane;width=1920;height=1080;work_scale=100;
      requested_passes=$wanted;creation_jobs=[Math]::Min($step+1,$wanted);physical_done=1;capture_pin_open=$true;
      producer=1;producer_done=1;raw_hash='1';analysis_hash='1';created=$created;evaluated=!$created;
      composed=!$created;submitted=$true;gpu_composed=!$created;creation_pass=$(if($created){$step+1}else{0});
      completed_passes=$(if($created){0}else{$wanted});code=1;invalid_status=0;answer_resource='1';
      model_hash='2';output_hash='3'}
})
$pairContext=@(@{processor_a='1';processor_b='2';device_a='3';device_b='3'})
$pairStatus=@(@{stage='live';a_requested=3;a_effective=3;b_requested=1;b_effective=1;a_creations=3;b_creations=1},
    @{stage='after_a';b_requested=1;b_effective=1;b_creations=1;b_completed=5})
$pairClose=@(@{lane='A';session=9;sdk=2;owners_at_detachment=2;feature_closed=$true},
    @{lane='B';session=9;sdk=4;owners_at_detachment=1;feature_closed=$true})
Assert-ModelPairRows $pairRows @(@{}) $pairContext $pairStatus $pairClose
foreach($mutation in @(@{row=0;field='creation_pass';value=2;reason='NR_QA_MODEL_PAIR_CREATION'},
    @{row=10;field='physical_done';value=0;reason='NR_QA_MODEL_PAIR_IDENTITY'},
    @{row=10;field='completed_passes';value=2;reason='NR_QA_MODEL_PAIR_RESULT'})){
    $altered=ConvertFrom-Json (ConvertTo-Json -InputObject $pairRows -Depth 8)
    $altered[$mutation.row].($mutation.field)=$mutation.value
    $reason=$null;try{Assert-ModelPairRows $altered @(@{}) $pairContext $pairStatus $pairClose}catch{$reason=$_.Exception.Message}
    if($reason -ne $mutation.reason){throw "Wrong pair refusal $($mutation.field): $reason"}
}
Write-Output 'PASS native pair receipt fixture and mutation refusals CPU_only=true'
$churnChanges=@(foreach($change in 1..20){
    @{change=$change;revision=$change+1;generation=$change+1;passes=$(if($change%2){3}else{1});
      scale=$(if($change%2){50}else{100});creation_jobs=$change+3;created=$true;physical_complete=$true}
})
$churnRows=@(foreach($i in 0..26){
    $changing=$i -ge 5 -and $i -lt 25
    $created=$i -lt 3 -or $changing
    $c=if($changing){$churnChanges[$i-5]}else{$null}
    $wanted=if($changing){$c.passes}elseif($i -lt 5){3}else{1}
    @{lane=0;step=$i;instance_id=111;width=1280;height=720;physical_done=1;capture_pin_open=$true;
      producer=1;producer_done=1;raw_hash='1';analysis_hash='1';created=$created;evaluated=!$created;
      composed=!$created;submitted=$true;gpu_composed=!$created;creation_pass=$(if($i -lt 3){$i+1}elseif($changing){1}else{0});
      settings_revision=$(if($changing){$c.revision}elseif($i -lt 5){1}else{21});
      resource_generation=$(if($changing){$c.generation}elseif($i -lt 5){1}else{21});
      requested_passes=$wanted;work_scale=$(if($changing){$c.scale}else{100});
      completed_passes=$(if($created){0}else{$wanted});code=1;invalid_status=0;answer_resource='1';model_hash='2';output_hash='3'}
})
$churnContext=@(@{device='1';instance=111;width=1280;height=720;frame_interval_ns=16666666})
$churnSummary=@(@{changes=20;elapsed_ms=1;creations=23;requested=1;effective=1;generation=21})
$churnClose=@(@{closed=$true;worker_exit=$true;retained=$false})
Assert-ModelChurnRows $churnRows @(@{}) $churnContext $churnChanges $churnSummary $churnClose
foreach($mutation in @(@{row=5;field='resource_generation';value=1;reason='NR_QA_MODEL_CHURN_CHANGE'},
    @{row=15;field='physical_done';value=0;reason='NR_QA_MODEL_CHURN_IDENTITY'},
    @{row=26;field='completed_passes';value=2;reason='NR_QA_MODEL_CHURN_RESULT'})){
    $altered=ConvertFrom-Json (ConvertTo-Json -InputObject $churnRows -Depth 8)
    $altered[$mutation.row].($mutation.field)=$mutation.value
    $reason=$null;try{Assert-ModelChurnRows $altered @(@{}) $churnContext $churnChanges $churnSummary $churnClose}catch{$reason=$_.Exception.Message}
    if($reason -ne $mutation.reason){throw "Wrong churn refusal $($mutation.field): $reason"}
}
Write-Output 'PASS native churn receipt fixture and mutation refusals CPU_only=true'
