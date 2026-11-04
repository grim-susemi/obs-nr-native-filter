# SPDX-License-Identifier: GPL-2.0-or-later
function Assert-ModelPassRows($Rows,$Contexts,$Cleanup,$Modules,$Status,[string]$PassCase) {
    if($Rows.Count -ne 6 -or $Contexts.Count -ne 1 -or $Cleanup.Count -ne 1 -or !$Modules.Count -or
        $Status.Count -ne 1 -or $PassCase -notin @('two','create2')){throw 'NR_QA_MODEL_PASS_SET'}
    $failed=$PassCase -eq 'create2'
    $effective=if($failed){1}else{2}
    $code=if($failed){0xe008}else{0}
    $context=$Contexts[0];$close=$Cleanup[0];$summary=$Status[0]
    if($context.lane -ne 0 -or $context.software -isnot [bool] -or $context.software -or
        $context.private_direct_queue -isnot [bool] -or !$context.private_direct_queue -or
        $context.adapter -ne 'RTX 4090' -or [UInt64]$context.device -eq 0 -or [UInt64]$context.queue -eq 0 -or
        $context.renderer_luid_high -ne $context.worker_luid_high -or
        $context.renderer_luid_low -ne $context.worker_luid_low){throw 'NR_QA_MODEL_PASS_CONTEXT'}
    if($summary.requested -ne 2 -or $summary.effective -ne $effective -or
        $summary.failed_pass -ne $(if($failed){2}else{0}) -or $summary.code -ne $code -or
        $summary.creation_jobs -ne 2 -or $summary.reason -ne $(if($failed){'passFailed'}else{'none'})){
        throw 'NR_QA_MODEL_PASS_STATUS'
    }
    if($close.lane -ne 0 -or $close.worker_exit -isnot [bool] -or !$close.worker_exit -or
        $close.closed -isnot [bool] -or !$close.closed -or $close.retained -isnot [bool] -or $close.retained -or
        $close.applied -ne 3 -or $close.nr_selections -ne 3){throw 'NR_QA_MODEL_PASS_CLEANUP'}
    for($i=0;$i -lt 6;++$i) {
        $row=$Rows[$i]
        foreach($field in @('created','evaluated','composed','submitted','gpu_composed','reset','reference_valid','capture_pin_open')){
            if($row.$field -isnot [bool]){throw 'NR_QA_MODEL_PASS_BOOLEAN'}
        }
        if($row.lane -ne 0 -or $row.step -ne $i -or $row.requested_passes -ne 2 -or
            $row.creation_jobs -ne [Math]::Min($i+1,2) -or $row.creation_pass -ne $(if($i -lt 2){$i+1}else{0}) -or
            $row.width -ne 1920 -or $row.height -ne 1080 -or $row.feature -ne 18 -or
            $row.raw_hash -ne $row.analysis_hash -or !$row.raw_hash -or
            $row.producer -le 0 -or $row.producer_done -lt $row.producer -or
            $row.physical_done -ne 1 -or !$row.capture_pin_open -or
            $row.capture_pin -le $row.consumer_done -or $row.ingress_sequence -ne ($i+1) -or
            $row.submission_serial -ne ($i+1)){throw 'NR_QA_MODEL_PASS_IDENTITY'}
        if($i -lt 2) {
            if($row.evaluated -or $row.composed -or $row.gpu_composed -or $row.completed_passes -ne 0 -or
                $row.created -ne (!$failed -or $i -eq 0) -or $row.selected -eq 3){
                throw 'NR_QA_MODEL_PASS_EARLY_PUBLICATION'
            }
            if($failed -and $i -eq 1 -and ($row.code -ne 0xe008 -or $row.submitted)){
                throw 'NR_QA_MODEL_PASS_CREATE_INJECTION'
            }
            continue
        }
        if(!$row.evaluated -or !$row.composed -or !$row.submitted -or !$row.gpu_composed -or
            $row.completed_passes -ne $effective -or $row.code -ne 1 -or $row.invalid_status -ne 0 -or
            $row.epoch -le $row.creation_epoch -or !$row.answer_resource -or [UInt64]$row.answer_resource -eq 0 -or
            !$row.model_hash -or !$row.output_hash -or $row.input_hash -eq $row.model_hash -or
            $row.reset -ne ($i -eq 2) -or $row.reference_valid -ne ($i -gt 2) -or
            ($row.reference_valid -and $row.changed_pixels -le 0) -or
            (!$row.reference_valid -and $row.changed_pixels -ne 0)){
            throw 'NR_QA_MODEL_PASS_PHYSICAL_RESULT'
        }
        for($j=1;$j -lt 7;++$j){
            if(!$row.chronology[$j] -or $row.chronology[$j] -gt $row.chronology[$j+1]){
                throw 'NR_QA_MODEL_PASS_CHRONOLOGY'
            }
        }
        if($i -gt 2 -and ($row.selected -ne 3 -or $row.selected_sequence -ne $i)){
            throw 'NR_QA_MODEL_PASS_PUBLICATION'
        }
    }
}
