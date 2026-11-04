# SPDX-License-Identifier: GPL-2.0-or-later
# Invoked by the existing CPU-only host_runner_model contract.
foreach($case in @('two','create2')) {
    $failed=$case -eq 'create2';$count=if($failed){1}else{2}
    $rows=@(foreach($step in 0..5){
        @{lane=0;step=$step;requested_passes=2;creation_jobs=[Math]::Min($step+1,2);
            creation_pass=$(if($step -lt 2){$step+1}else{0});width=1920;height=1080;feature=18;
            raw_hash='10';analysis_hash='10';producer=1;producer_done=1;physical_done=1;
            capture_pin_open=$true;capture_pin=3;consumer_done=2;ingress_sequence=$step+1;submission_serial=$step+1;
            created=($step -lt 2 -and (!$failed -or $step -eq 0));evaluated=($step -gt 1);composed=($step -gt 1);
            gpu_composed=($step -gt 1);completed_passes=$(if($step -gt 1){$count}else{0});
            submitted=(!$failed -or $step -ne 1);code=$(if($step -gt 1){1}elseif($failed -and $step -eq 1){0xe008}else{1});
            selected=$(if($step -gt 2){3}else{1});selected_sequence=$step;
            invalid_status=0;epoch=$step+1;creation_epoch=2;answer_resource='12';model_hash='20';output_hash='30';input_hash='40';
            reset=($step -eq 2);reference_valid=($step -gt 2);changed_pixels=$(if($step -gt 2){1}else{0});
            chronology=@(1,2,3,4,5,6,7,8)}
    })
    $context=@(@{lane=0;software=$false;private_direct_queue=$true;adapter='RTX 4090';device='1';queue='2';
        renderer_luid_high=0;worker_luid_high=0;renderer_luid_low=1;worker_luid_low=1})
    $cleanup=@(@{lane=0;worker_exit=$true;closed=$true;retained=$false;applied=3;nr_selections=3})
    $summary=@(@{requested=2;effective=$count;failed_pass=$(if($failed){2}else{0});code=$(if($failed){0xe008}else{0});
        creation_jobs=2;reason=$(if($failed){'passFailed'}else{'none'})})
    # Given complete native-form receipts. When consumed by the real validator.
    Assert-ModelPassRows $rows $context $cleanup @(@{}) $summary $case
    # Then both success and permanent second-creation reduction are admitted.
    Write-Output "PASS native multipass receipt fixture $case CPU_only=true"
    $saved=ConvertTo-Json -InputObject $rows -Depth 8
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
