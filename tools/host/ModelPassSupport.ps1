# SPDX-License-Identifier: GPL-2.0-or-later
function Assert-ModelChurnRows($Rows,$Modules,$Contexts,$Changes,$Summary,$Cleanup) {
    if($Rows.Count -ne 27 -or !$Modules.Count -or $Contexts.Count -ne 1 -or $Changes.Count -ne 20 -or
        $Summary.Count -ne 1 -or $Cleanup.Count -ne 1){throw 'NR_QA_MODEL_CHURN_SET'}
    $context=$Contexts[0];$summary=$Summary[0];$close=$Cleanup[0]
    if([UInt64]$context.device -eq 0 -or $context.instance -ne 111 -or $context.width -ne 1280 -or $context.height -ne 720 -or
        $context.frame_interval_ns -lt 16000000 -or $context.frame_interval_ns -gt 17000000){
        throw 'NR_QA_MODEL_CHURN_CONTEXT'
    }
    if($summary.changes -ne 20 -or $summary.elapsed_ms -le 0 -or $summary.creations -ne 22 -or
        $summary.requested -ne 1 -or $summary.effective -ne 1 -or $summary.generation -ne 21 -or
        $close.closed -isnot [bool] -or !$close.closed -or $close.worker_exit -isnot [bool] -or !$close.worker_exit -or
        $close.retained -isnot [bool] -or $close.retained){throw 'NR_QA_MODEL_CHURN_SUMMARY'}
    for($i=0;$i -lt 27;++$i) {
        $r=$Rows[$i]
        foreach($field in @('created','evaluated','composed','submitted','gpu_composed','capture_pin_open')){
            if($r.$field -isnot [bool]){throw 'NR_QA_MODEL_CHURN_BOOLEAN'}
        }
        if($r.lane -ne 0 -or $r.step -ne $i -or $r.instance_id -ne 111 -or
            $r.width -ne 1280 -or $r.height -ne 720 -or $r.physical_done -ne 1 -or
            !$r.capture_pin_open -or $r.producer -le 0 -or $r.producer_done -lt $r.producer -or
            !$r.raw_hash -or $r.raw_hash -ne $r.analysis_hash){throw 'NR_QA_MODEL_CHURN_IDENTITY'}
        if($i -ge 5 -and $i -lt 25) {
            $change=$i-4;$c=$Changes[$change-1]
            $passes=if($change%2){2}else{1};$scale=if($change%2){50}else{100}
            if($c.change -ne $change -or $c.revision -ne $change+1 -or $c.generation -ne $change+1 -or
                $c.passes -ne $passes -or $c.scale -ne $scale -or $c.creation_jobs -ne $change+2 -or
                $c.created -isnot [bool] -or !$c.created -or $c.physical_complete -isnot [bool] -or !$c.physical_complete -or
                $r.settings_revision -ne $c.revision -or $r.resource_generation -ne $c.generation -or
                $r.requested_passes -ne $passes -or $r.work_scale -ne $scale -or
                !$r.created -or !$r.submitted -or $r.creation_pass -ne 1 -or $r.gpu_composed -or
                $r.completed_passes -ne 0){throw 'NR_QA_MODEL_CHURN_CHANGE'}
        } elseif($i -lt 2) {
            if(!$r.created -or !$r.submitted -or $r.creation_pass -ne $i+1 -or $r.requested_passes -ne 2){
                throw 'NR_QA_MODEL_CHURN_INITIAL'
            }
        } else {
            $wanted=if($i -lt 5){2}else{1}
            if(!$r.evaluated -or !$r.composed -or !$r.submitted -or !$r.gpu_composed -or
                $r.completed_passes -ne $wanted -or $r.code -ne 1 -or $r.invalid_status -ne 0 -or
                [UInt64]$r.answer_resource -eq 0 -or !$r.model_hash -or !$r.output_hash){
                throw 'NR_QA_MODEL_CHURN_RESULT'
            }
        }
    }
}
function Assert-ModelPairRows($Rows,$Modules,$Contexts,$Status,$Close) {
    if($Rows.Count -ne 11 -or !$Modules.Count -or $Contexts.Count -ne 1 -or $Status.Count -ne 2 -or $Close.Count -ne 2){
        throw 'NR_QA_MODEL_PAIR_SET'
    }
    $context=$Contexts[0];$live=$Status[0];$after=$Status[1];$a=$Close[0];$b=$Close[1]
    if([UInt64]$context.processor_a -eq 0 -or [UInt64]$context.processor_b -eq 0 -or
        $context.processor_a -eq $context.processor_b -or [UInt64]$context.device_a -eq 0 -or
        $context.device_a -ne $context.device_b){throw 'NR_QA_MODEL_PAIR_CONTEXT'}
    if($live.stage -cne 'live' -or $live.a_requested -ne 2 -or $live.a_effective -ne 2 -or
        $live.b_requested -ne 1 -or $live.b_effective -ne 1 -or $live.a_creations -ne 2 -or $live.b_creations -ne 1 -or
        $after.stage -cne 'after_a' -or $after.b_requested -ne 1 -or $after.b_effective -ne 1 -or
        $after.b_creations -ne 1 -or $after.b_completed -ne 5){throw 'NR_QA_MODEL_PAIR_STATUS'}
    if($a.lane -cne 'A' -or $b.lane -cne 'B' -or !$a.session -or $a.session -ne $b.session -or
        $a.sdk -ne 2 -or $a.owners_at_detachment -ne 2 -or $b.sdk -ne 4 -or $b.owners_at_detachment -ne 1 -or
        $a.feature_closed -isnot [bool] -or !$a.feature_closed -or
        $b.feature_closed -isnot [bool] -or !$b.feature_closed){throw 'NR_QA_MODEL_PAIR_CLOSE'}
    for($i=0;$i -lt 11;++$i) {
        $r=$Rows[$i]
        $lane=if($i -eq 10){1}else{$i%2}
        $step=if($i -eq 10){5}else{[Math]::Floor($i/2)}
        $wanted=if($lane -eq 0){2}else{1}
        foreach($field in @('created','evaluated','composed','submitted','gpu_composed','capture_pin_open')){
            if($r.$field -isnot [bool]){throw 'NR_QA_MODEL_PAIR_BOOLEAN'}
        }
        if($r.lane -ne $lane -or $r.step -ne $step -or $r.instance_id -ne 111+$lane -or
            $r.width -ne 1920 -or $r.height -ne 1080 -or $r.work_scale -ne 100 -or
            $r.requested_passes -ne $wanted -or $r.creation_jobs -ne [Math]::Min($step+1,$wanted) -or
            $r.physical_done -ne 1 -or !$r.capture_pin_open -or $r.producer -le 0 -or
            $r.producer_done -lt $r.producer -or !$r.raw_hash -or $r.raw_hash -ne $r.analysis_hash){
            throw 'NR_QA_MODEL_PAIR_IDENTITY'
        }
        if($step -lt $wanted) {
            if(!$r.created -or !$r.submitted -or $r.creation_pass -ne $step+1 -or $r.gpu_composed -or $r.completed_passes -ne 0){
                throw 'NR_QA_MODEL_PAIR_CREATION'
            }
        } elseif(!$r.evaluated -or !$r.composed -or !$r.submitted -or !$r.gpu_composed -or
            $r.completed_passes -ne $wanted -or $r.code -ne 1 -or $r.invalid_status -ne 0 -or
            [UInt64]$r.answer_resource -eq 0 -or !$r.model_hash -or !$r.output_hash) {
            throw 'NR_QA_MODEL_PAIR_RESULT'
        }
    }
}
function Assert-ModelHdrRows($Rows,$Contexts,$Cleanup,$Modules,$Status,$HdrRows,$GuideRows) {
    if($Rows.Count -ne 6 -or $Contexts.Count -ne 1 -or $Cleanup.Count -ne 1 -or !$Modules.Count -or
        $Status.Count -ne 1 -or $HdrRows.Count -ne 4 -or $GuideRows.Count -ne 6){throw 'NR_QA_MODEL_HDR_SET'}
    $context=$Contexts[0];$close=$Cleanup[0];$summary=$Status[0]
    if($context.adapter -ne 'RTX 4090' -or $context.software -isnot [bool] -or $context.software -or
        $context.private_direct_queue -isnot [bool] -or !$context.private_direct_queue -or
        [UInt64]$context.device -eq 0 -or [UInt64]$context.queue -eq 0 -or
        $context.renderer_luid_high -ne $context.worker_luid_high -or $context.renderer_luid_low -ne $context.worker_luid_low){
        throw 'NR_QA_MODEL_HDR_CONTEXT'
    }
    if($summary.requested -ne 2 -or $summary.effective -ne 2 -or $summary.failed_pass -ne 0 -or
        $summary.code -ne 0 -or $summary.creation_jobs -ne 2 -or $summary.reason -ne 'none' -or
        $close.worker_exit -isnot [bool] -or !$close.worker_exit -or $close.closed -isnot [bool] -or !$close.closed -or
        $close.retained -isnot [bool] -or $close.retained -or $close.applied -ne 3){throw 'NR_QA_MODEL_HDR_SUMMARY'}
    for($i=0;$i -lt 6;++$i) {
        $r=$Rows[$i]
        $guide=$GuideRows[$i]
        foreach($field in @('created','evaluated','composed','submitted','gpu_composed','reset','reference_valid','capture_pin_open')){
            if($r.$field -isnot [bool]){throw 'NR_QA_MODEL_HDR_BOOLEAN'}
        }
        if($r.lane -ne 0 -or $r.step -ne $i -or $r.width -ne 1920 -or $r.height -ne 1080 -or
            $r.work_scale -ne 50 -or $r.source_format -ne 10 -or $r.requested_passes -ne 2 -or
            $r.creation_jobs -ne [Math]::Min($i+1,2) -or $r.physical_done -ne 1 -or
            !$r.capture_pin_open -or $r.producer -le 0 -or $r.producer_done -lt $r.producer -or
            !$r.raw_hash -or !$r.analysis_hash -or $r.raw_hash -eq $r.analysis_hash){
            throw 'NR_QA_MODEL_HDR_IDENTITY'
        }
        if($guide.step -ne $i -or $guide.format -ne 87 -or $guide.dimension -ne 3 -or
            $guide.width -ne 1920 -or $guide.height -ne 1080 -or $guide.depth -ne 1 -or
            $guide.mips -ne 1 -or $guide.samples -ne 1 -or
            $guide.resource_device_match -isnot [bool] -or !$guide.resource_device_match -or
            $guide.fence_device_match -isnot [bool] -or !$guide.fence_device_match -or
            $guide.fence_required -le 0 -or $guide.fence_done -lt $guide.fence_required){
            throw 'NR_QA_MODEL_HDR_GUIDE_INPUT'
        }
        if($i -lt 2){
            if(!$r.created -or !$r.submitted -or $r.creation_pass -ne $i+1 -or $r.completed_passes -ne 0 -or
                $r.evaluated -or $r.composed -or $r.gpu_composed -or $r.selected -eq 3){
                throw 'NR_QA_MODEL_HDR_PRIMING'
            }
            continue
        }
        $h=$HdrRows[$i-2]
        if(!$r.evaluated -or !$r.composed -or !$r.submitted -or !$r.gpu_composed -or
            $r.completed_passes -ne 2 -or $r.answer_width -ne 960 -or $r.answer_height -ne 540 -or
            [UInt64]$r.answer_resource -eq 0 -or !$r.model_hash -or !$r.output_hash -or
            $r.code -ne 1 -or $r.invalid_status -ne 0 -or $h.step -ne $i -or
            $h.encoded_bad -ne 0 -or $h.output_nonfinite -ne 0 -or $h.source_max -le 1 -or
            $h.encoded_min -lt 0 -or $h.encoded_max -gt 1 -or $h.encoded_min -gt $h.encoded_max){
            throw 'NR_QA_MODEL_HDR_RESULT'
        }
    }
}
function Assert-ModelQualityRows($Rows,$Contexts,$Cleanup,$Modules,$Status,[string]$PassCase) {
    if($PassCase -cnotmatch '^quality([12])$'){throw 'NR_QA_MODEL_QUALITY_CASE'}
    $n=[int]$Matches[1]
    if($Rows.Count -ne $n+6 -or $Contexts.Count -ne 1 -or $Cleanup.Count -ne 1 -or !$Modules.Count -or $Status.Count -ne 1){
        throw 'NR_QA_MODEL_QUALITY_SET'
    }
    $context=$Contexts[0];$close=$Cleanup[0];$status=$Status[0]
    if($context.adapter -ne 'RTX 4090' -or $context.software -isnot [bool] -or $context.software -or
        $context.private_direct_queue -isnot [bool] -or !$context.private_direct_queue -or
        [UInt64]$context.device -eq 0 -or [UInt64]$context.queue -eq 0 -or
        $context.renderer_luid_high -ne $context.worker_luid_high -or $context.renderer_luid_low -ne $context.worker_luid_low){
        throw 'NR_QA_MODEL_QUALITY_CONTEXT'
    }
    if($status.requested -ne $n -or $status.effective -ne $n -or $status.creation_jobs -ne $n -or
        $close.worker_exit -isnot [bool] -or !$close.worker_exit -or $close.closed -isnot [bool] -or !$close.closed -or
        $close.retained -isnot [bool] -or $close.retained -or $close.applied -ne 4){
        throw 'NR_QA_MODEL_QUALITY_SUMMARY'
    }
    for($i=0;$i -lt $Rows.Count;++$i){
        $r=$Rows[$i]
        foreach($field in @('created','evaluated','composed','submitted','gpu_composed','reset','reference_valid','capture_pin_open')){
            if($r.$field -isnot [bool]){throw 'NR_QA_MODEL_QUALITY_BOOLEAN'}
        }
        if($r.lane -ne 0 -or $r.step -ne $i -or $r.width -ne 1920 -or $r.height -ne 1080 -or
            $r.work_scale -ne 100 -or $r.requested_passes -ne $n -or $r.creation_jobs -ne [Math]::Min($i+1,$n) -or
            $r.physical_done -ne 1 -or !$r.capture_pin_open -or $r.producer -le 0 -or
            $r.producer_done -lt $r.producer -or !$r.raw_hash -or $r.raw_hash -ne $r.analysis_hash){
            throw 'NR_QA_MODEL_QUALITY_IDENTITY'
        }
        if($i -lt $n){
            if(!$r.created -or !$r.submitted -or $r.creation_pass -ne $i+1 -or $r.completed_passes -ne 0 -or
                $r.evaluated -or $r.composed -or $r.gpu_composed -or $r.selected -eq 3){
                throw 'NR_QA_MODEL_QUALITY_PRIMING'
            }
            continue
        }
        $reset=$i -eq $n -or $i -eq $n+3
        if(!$r.evaluated -or !$r.composed -or !$r.submitted -or !$r.gpu_composed -or
            $r.completed_passes -ne $n -or $r.reset -ne $reset -or $r.reference_valid -eq $reset -or
            $r.answer_width -ne 1920 -or $r.answer_height -ne 1080 -or [UInt64]$r.answer_resource -eq 0 -or
            !$r.model_hash -or !$r.output_hash -or $r.code -ne 1 -or $r.invalid_status -ne 0){
            throw 'NR_QA_MODEL_QUALITY_RESULT'
        }
        if(!$reset -and ($r.selected -ne 3 -or $r.selected_sequence -ne $i)){
            throw 'NR_QA_MODEL_QUALITY_PUBLICATION'
        }
    }
}
function Assert-ModelPassRows($Rows,$Contexts,$Cleanup,$Modules,$Status,[string]$PassCase) {
    if($PassCase -in @('scale50','odd50','align100','align50')) {
        $w=if($PassCase -eq 'odd50'){1281}else{1920}
        $h=if($PassCase -eq 'odd50'){721}else{1080}
        $ww=if($PassCase -eq 'odd50'){641}elseif($PassCase -eq 'align100'){1920}else{960}
        $wh=if($PassCase -eq 'odd50'){361}elseif($PassCase -eq 'align100'){1080}else{540}
        $scale=if($PassCase -eq 'align100'){100}else{50}
        if($Rows.Count -ne 6 -or $Contexts.Count -ne 1 -or $Cleanup.Count -ne 1 -or !$Modules.Count -or $Status.Count){
            throw 'NR_QA_MODEL_SCALED_SET'
        }
        $context=$Contexts[0];$close=$Cleanup[0]
        if($context.lane -ne 0 -or $context.software -isnot [bool] -or $context.software -or
            $context.private_direct_queue -isnot [bool] -or !$context.private_direct_queue -or
            $context.adapter -ne 'RTX 4090' -or [UInt64]$context.device -eq 0 -or [UInt64]$context.queue -eq 0 -or
            $context.renderer_luid_high -ne $context.worker_luid_high -or $context.renderer_luid_low -ne $context.worker_luid_low){
            throw 'NR_QA_MODEL_SCALED_CONTEXT'
        }
        if($close.worker_exit -isnot [bool] -or !$close.worker_exit -or $close.closed -isnot [bool] -or !$close.closed -or
            $close.retained -isnot [bool] -or $close.retained -or $close.applied -ne 3){throw 'NR_QA_MODEL_SCALED_CLEANUP'}
        for($i=0;$i -lt 6;++$i){
            $row=$Rows[$i]
            foreach($field in @('created','evaluated','composed','submitted','gpu_composed','reset','reference_valid','capture_pin_open')){
                if($row.$field -isnot [bool]){throw 'NR_QA_MODEL_SCALED_BOOLEAN'}
            }
            if($row.lane -ne 0 -or $row.step -ne $i -or $row.width -ne $w -or $row.height -ne $h -or
                $row.work_scale -ne $scale -or $row.requested_passes -ne 1 -or $row.creation_jobs -ne 1 -or
                $row.physical_done -ne 1 -or !$row.capture_pin_open -or $row.producer -le 0 -or
                $row.producer_done -lt $row.producer -or !$row.raw_hash -or $row.raw_hash -ne $row.analysis_hash){
                throw 'NR_QA_MODEL_SCALED_IDENTITY'
            }
            if($i -eq 0){
                if(!$row.created -or !$row.submitted -or $row.creation_pass -ne 1 -or $row.gpu_composed -or $row.selected -eq 3){
                    throw 'NR_QA_MODEL_SCALED_CREATE'
                }
            } elseif(!$row.evaluated -or !$row.composed -or !$row.submitted -or !$row.gpu_composed -or
                $row.completed_passes -ne 1 -or $row.answer_width -ne $ww -or $row.answer_height -ne $wh -or
                [UInt64]$row.answer_resource -eq 0 -or !$row.model_hash -or !$row.output_hash -or
                $row.code -ne 1 -or $row.invalid_status -ne 0) {
                throw 'NR_QA_MODEL_SCALED_RESULT'
            }
            if($PassCase -like 'align*' -and $i -in @(2,3) -and
                ($row.reset -or !$row.reference_valid -or $row.selected -ne 3 -or $row.selected_sequence -ne $i)){
                throw 'NR_QA_MODEL_ALIGNMENT_HISTORY'
            }
        }
        return
    }
    $evaluationFailure=$PassCase -eq 'eval2'
    if($Rows.Count -ne $(if($evaluationFailure){7}else{6}) -or $Contexts.Count -ne 1 -or $Cleanup.Count -ne 1 -or !$Modules.Count -or
        $Status.Count -ne 1 -or $PassCase -notin @('two','create2','eval2')){throw 'NR_QA_MODEL_PASS_SET'}
    $creationFailure=$PassCase -eq 'create2'
    $failed=$creationFailure -or $evaluationFailure
    $effective=if($failed){1}else{2}
    $code=if($evaluationFailure){0xe018}elseif($creationFailure){0xe008}else{0}
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
    for($i=0;$i -lt $Rows.Count;++$i) {
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
                $row.created -ne (!$creationFailure -or $i -eq 0) -or $row.selected -eq 3){
                throw 'NR_QA_MODEL_PASS_EARLY_PUBLICATION'
            }
            if($creationFailure -and $i -eq 1 -and ($row.code -ne 0xe008 -or $row.submitted)){
                throw 'NR_QA_MODEL_PASS_CREATE_INJECTION'
            }
            continue
        }
        if($evaluationFailure -and $i -eq 2) {
            if(!$row.evaluated -or $row.composed -or $row.submitted -or $row.gpu_composed -or
                $row.completed_passes -ne 1 -or $row.job_failed_pass -ne 2 -or $row.code -ne 0xe018 -or
                $row.selected -eq 3){throw 'NR_QA_MODEL_PASS_EVALUATION_DISCARD'}
            continue
        }
        $firstInference=if($evaluationFailure){3}else{2}
        if(!$row.evaluated -or !$row.composed -or !$row.submitted -or !$row.gpu_composed -or
            $row.completed_passes -ne $effective -or $row.code -ne 1 -or $row.invalid_status -ne 0 -or
            $row.epoch -le $row.creation_epoch -or !$row.answer_resource -or [UInt64]$row.answer_resource -eq 0 -or
            !$row.model_hash -or !$row.output_hash -or $row.input_hash -eq $row.model_hash -or
            $row.reset -ne ($i -eq $firstInference) -or $row.reference_valid -ne ($i -gt $firstInference) -or
            ($row.reference_valid -and $row.changed_pixels -le 0) -or
            (!$row.reference_valid -and $row.changed_pixels -ne 0)){
            throw 'NR_QA_MODEL_PASS_PHYSICAL_RESULT'
        }
        for($j=1;$j -lt 7;++$j){
            if(!$row.chronology[$j] -or $row.chronology[$j] -gt $row.chronology[$j+1]){
                throw 'NR_QA_MODEL_PASS_CHRONOLOGY'
            }
        }
        if($i -gt $firstInference -and ($row.selected -ne 3 -or $row.selected_sequence -ne $i)){
            throw 'NR_QA_MODEL_PASS_PUBLICATION'
        }
    }
}
