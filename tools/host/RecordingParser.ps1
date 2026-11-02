function Assert-NrRecordingChallenge($c,$row) {
    if(!$c -or $row.kind -ne 2 -or $row.genuine_nr){throw 'NR_RECORDING_CHALLENGE'}
    foreach($field in @('ready','eligible','evaluated','composed','submitted','fence_present')) {
        if($c.$field -isnot [bool] -or !$c.$field){throw 'NR_RECORDING_CHALLENGE'}
    }
    foreach($field in @('instance','content_epoch','ingress','history_epoch','resource_generation','submission_serial','module_base','completion_value','completed_value')) {
        if(($c.$field -isnot [long] -and $c.$field -isnot [int]) -or $c.$field -le 0){throw 'NR_RECORDING_CHALLENGE'}
    }
    if($c.instance -ne $row.instance -or $c.content_epoch -ne $row.content_epoch -or $c.ingress -ne $row.selected -or
        ($c.invalid -isnot [long] -and $c.invalid -isnot [int]) -or $c.invalid -ne 0 -or
        $c.module_path -isnot [string] -or !$c.module_path -or $c.completed_value -lt $c.completion_value){throw 'NR_RECORDING_CHALLENGE'}
}
# SPDX-License-Identifier: GPL-3.0-only
# UG03P3 (I3 Task16 B1/B2): every compared number must be a JSON number of the type the producer writes
# (obs_data_set_int / obs_data_set_double), so an array or a string can no longer make a comparison vacuous.
function Assert-NrRecordingNumbers($Row,[string[]]$Integers,[string[]]$Reals) {
    foreach($name in $Integers) {
        $value=$Row.$name
        if($value -isnot [int] -and $value -isnot [long]){throw 'NR_RECORDING_NUMBER'}
    }
    foreach($name in $Reals) {
        $value=$Row.$name
        if(($value -isnot [int] -and $value -isnot [long] -and $value -isnot [double] -and $value -isnot [decimal]) -or
            [double]::IsNaN([double]$value) -or [double]::IsInfinity([double]$value)){throw 'NR_RECORDING_NUMBER'}
    }
}
function Assert-NrRecordingRows($Frames,$Lanes,$Decoded,$Cleanup) {
    if(@($Lanes).Count -ne 4 -or @($Cleanup).Count -ne 4){throw 'NR_RECORDING_SET'}
    $frameIntegers=@('lane','tick','clock','interval','instance','ingress','selected','content_epoch','kind','source_id','filter_id',
        'program_id','width','height','alpha_errors','submission_serial','history_epoch','resource_generation','module_base',
        'fps_numerator','fps_denominator','settings_revision','transport_occurrence','wire_id')
    $frameReals=@('source_reference_white_nits','output_sdr_white_nits','output_peak_nits','same_key_nr_rms','settings_strength')
    $decodedIntegers=@('lane','width','height','bit_depth','transfer','codes_compared','code_errors','packet_timebase_numerator',
        'packet_timebase_denominator','wire_id','raw_wire_id','transport_occurrence','delivery_occurrence','tick','instance',
        'content_epoch','selected','history_epoch','resource_generation','submission_serial','render_clock_ns','source_id',
        'preencode_timestamp_ns','observed_predecessor_clock_ns','decoder_timestamp_ns','packet_pts','packet_dts')
    $decodedReals=@('decoded_peak_linear_nits','same_id_raw_decoded_nr_rms_codes')
    foreach($r in @($Frames)){Assert-NrRecordingNumbers $r $frameIntegers $frameReals}
    foreach($r in @($Decoded)){Assert-NrRecordingNumbers $r $decodedIntegers $decodedReals}
    foreach($r in @($Lanes)){Assert-NrRecordingNumbers $r @('lane','packets','decoded_frames') @()}
    foreach($r in @($Cleanup)){Assert-NrRecordingNumbers $r @('lane') @()}
    # Every row belongs to one of the four lanes; nothing outside them is silently ignored.
    foreach($r in @($Frames)+@($Decoded)+@($Lanes)+@($Cleanup)){if($r.lane -lt 0 -or $r.lane -gt 3){throw 'NR_RECORDING_SET'}}
    for($lane=0;$lane -lt 4;$lane++) {
        $hdr=$lane -ge 2;$off=($lane%2)-eq 0
        $summary=@($Lanes|Where-Object {$_.lane -eq $lane})
        $close=@($Cleanup|Where-Object {$_.lane -eq $lane})
        $group=@($Frames|Where-Object {$_.lane -eq $lane})
        $recorded=@($Decoded|Where-Object {$_.lane -eq $lane})
        # Row cap = kBoundedGraphSamples (recording_scenario.cpp) = bootstrap::max_content_id; keep the three in step.
        if($summary.Count -ne 1 -or $close.Count -ne 1 -or $group.Count -lt 8 -or $group.Count -gt 24 -or
            $summary[0].hdr -isnot [bool] -or $summary[0].hdr -ne $hdr -or
            $summary[0].nr_off -isnot [bool] -or $summary[0].nr_off -ne $off -or
            $summary[0].packets -ne 8 -or $summary[0].decoded_frames -ne 8 -or $recorded.Count -ne 8){throw 'NR_RECORDING_SET'}
        foreach($field in @('closed','worker_exit','retained')) {
            if($close[0].$field -isnot [bool] -or $close[0].$field -ne ($field -ne 'retained')){throw 'NR_RECORDING_CLOSURE'}
        }
        $previous=$null;$keys=@{};$wires=@{};$between=0;$positive=0
        foreach($r in $group) {
            foreach($field in @('hdr','nr_off','deterministic','genuine_nr','same_latch_views','between_views_completion','filter_order_preserved')) {
                if($r.$field -isnot [bool]){throw 'NR_RECORDING_BOOLEAN'}
            }
            if($r.native_producer_present -isnot [bool] -or $r.native_producer_present -eq $off -or
                $r.hdr -ne $hdr -or $r.nr_off -ne $off -or $r.deterministic -or !$r.same_latch_views -or
                !$r.filter_order_preserved -or $r.width -ne 1920 -or $r.height -ne 1080 -or
                $r.instance -le 0 -or $r.content_epoch -le 0 -or $r.settings_revision -le 0 -or
                $r.settings_strength -is [string] -or [double]::IsNaN($r.settings_strength) -or
                [double]::IsInfinity($r.settings_strength) -or $r.settings_strength -lt 0 -or $r.settings_strength -gt 1 -or
                $r.fps_numerator -ne 1 -or $r.fps_denominator -ne 1 -or $r.interval -ne 1000000000 -or
                $r.transport_occurrence -le 0 -or $r.alpha_errors -ne 0 -or
                $r.source_alpha -cne 'premultiplied' -or $r.source_reference_white_nits -ne 80 -or
                $r.output_sdr_white_nits -ne 300 -or $r.output_peak_nits -ne 1000 -or
                $r.filter_id -ne $r.source_id -or $r.program_id -ne $r.source_id -or
                $r.selected -le 0 -or $r.selected -ne $(if($off -or $r.kind -eq 1){$r.ingress}else{$r.ingress-1})){throw 'NR_RECORDING_LINEAGE'}
            foreach($field in @('raw_hash','current_raw_hash','filter_hash','program_hash','unrelated_hash','preview_hash','projector_hash')) {
                if($r.$field -isnot [string] -or $r.$field -cnotmatch '^[0-9]+$' -or [UInt64]$r.$field -eq 0){throw 'NR_RECORDING_IMAGE_HASH'}
            }
            if($r.preview_hash -cne $r.projector_hash){throw 'NR_RECORDING_BETWEEN_VIEWS'}
            if($previous -and ($r.tick -ne $previous.tick+1 -or $r.ingress -ne $previous.ingress+1 -or
                $r.transport_occurrence -ne $previous.transport_occurrence+1 -or $r.clock -ne $previous.clock+$r.interval -or
                $r.unrelated_hash -cne $previous.unrelated_hash -or $r.settings_revision -ne $previous.settings_revision -or
                $r.settings_strength -ne $previous.settings_strength)){throw 'NR_RECORDING_LINEAGE'}
            if($off) {
                if($r.kind -ne 0 -or $r.genuine_nr -or $r.submission_serial -ne 0 -or
                    $r.raw_hash -cne $r.current_raw_hash -or $r.same_key_nr_rms -lt 0 -or
                    $r.same_key_nr_rms -gt $(if($hdr){.001}else{.004})){throw 'NR_RECORDING_OFF'}
            }elseif($r.kind -eq 3) {
                # I3 Task16 B4: an NR-on row is genuine only above the lane's own raw tolerance and with filter bytes
                # different from raw (R22 recording_classifier.h:43-46 refuses both shapes).
                if(!$r.genuine_nr -or $r.submission_serial -le 0 -or $r.history_epoch -le 0 -or $r.resource_generation -le 0 -or
                    $r.module_base -le 0 -or !$r.module_path -or $r.same_key_nr_rms -is [string] -or
                    [double]::IsNaN($r.same_key_nr_rms) -or [double]::IsInfinity($r.same_key_nr_rms) -or $r.same_key_nr_rms -le 0 -or
                    $r.same_key_nr_rms -le $(if($hdr){.001}else{.004}) -or $r.raw_hash -ceq $r.filter_hash){throw 'NR_RECORDING_NR_NOOP'}
            }elseif($r.kind -notin @(1,2)){throw 'NR_RECORDING_KIND'}
            if($r.between_views_completion) {
                if($off -or $r.kind -ne 2 -or $r.genuine_nr){throw 'NR_RECORDING_BETWEEN_VIEWS'}
                Assert-NrRecordingChallenge $r.challenge_job $r
                $between++
            }
            $identity="$($r.transport_occurrence)"
            if($keys.ContainsKey($identity)){throw 'NR_RECORDING_DUPLICATE_KEY'}
            $phase=if($off){0}elseif($r.kind -eq 1){1}elseif($r.kind -eq 0){2}else{0}
            $expectedWire=(1 -shl 20) -bor ($r.selected -shl 4) -bor $phase
            if($r.wire_id -ne $expectedWire -or $wires.ContainsKey("$($r.wire_id)")){throw 'NR_RECORDING_WIRE'}
            $wires["$($r.wire_id)"]=$r
            $keys[$identity]=$r;$previous=$r
        }
        if(!$off -and $between -ne 1){throw 'NR_RECORDING_BETWEEN_VIEWS'}
        $previous=$null
        foreach($d in $recorded) {
            $key=$wires["$($d.wire_id)"]
            if(!$key -or $d.raw_wire_id -ne $d.wire_id -or $key.transport_occurrence -ne $d.transport_occurrence){throw 'NR_RECORDING_WIRE'}
            if(!$key -or $d.tick -ne $key.tick -or $d.instance -ne $key.instance -or $d.content_epoch -ne $key.content_epoch -or
                $d.selected -ne $key.selected -or $d.history_epoch -ne $key.history_epoch -or
                $d.resource_generation -ne $key.resource_generation -or $d.submission_serial -ne $key.submission_serial -or
                $d.render_clock_ns -ne $key.clock -or $d.source_id -ne $key.program_id -or
                $d.preencode_timestamp_ns -ne $d.observed_predecessor_clock_ns -or
                !$keys.ContainsKey("$($d.transport_occurrence-1)") -or
                $d.observed_predecessor_clock_ns -ne $keys["$($d.transport_occurrence-1)"].clock -or
                (!$off -and $key.kind -ne 3 -and !($key.kind -eq 2 -and $key.between_views_completion))){throw 'NR_RECORDING_DECODED_KEY'}
            # I3 Task16 B3: the row carries the transfer the decoder reports (nr_recording_decode.cpp frame->trc), not the
            # pre-encode tag. For the BT.709 SDR stream that is exactly VIDEO_TRC_SRGB (1), as on every genuine SDR decoded
            # row recorded so far (task-16 recording-window-r11..r33 native01/recording-host.log); DEFAULT (0), PQ (2)
            # and HLG (3) are refused.
            if($d.width -ne 1920 -or $d.height -ne 1080 -or $d.bit_depth -ne $(if($hdr){10}else{8}) -or
                ($hdr -and $d.transfer -ne 2) -or (!$hdr -and $d.transfer -ne 1) -or
                $d.full_range -isnot [bool] -or $d.full_range -or $d.primaries_matrix_preserved -isnot [bool] -or
                !$d.primaries_matrix_preserved -or $d.codes_compared -ne 3110400 -or $d.code_errors -ne 0 -or
                $d.packet_timebase_numerator -ne 1 -or $d.packet_timebase_denominator -ne 1 -or
                ($hdr -and $d.decoded_peak_linear_nits -le 200)){throw 'NR_RECORDING_CODEC_COLOR'}
            if($previous -and ($d.delivery_occurrence -ne $previous.delivery_occurrence+1 -or
                $d.transport_occurrence -ne $previous.transport_occurrence+1 -or $d.packet_pts -ne $previous.packet_pts+1 -or
                $d.packet_dts -ne $previous.packet_dts+1 -or $d.preencode_timestamp_ns -ne $previous.preencode_timestamp_ns+1000000000 -or
                [Math]::Abs($d.decoder_timestamp_ns-$previous.decoder_timestamp_ns-1000000000) -gt 1)){throw 'NR_RECORDING_TIMESTAMP'}
            if(!$off -and $key.genuine_nr -and $d.same_id_raw_decoded_nr_rms_codes -gt 0){$positive++}
            $previous=$d
        }
        if(!$off -and $positive -lt 3){throw 'NR_RECORDING_DECODED_NR_NOOP'}
    }
}
function Read-NrRecordingReceipts([string]$Output) {
    $sets=@{Frames=@();Lanes=@();Decoded=@();Cleanup=@()}
    foreach($line in ($Output -split [char]10)) {
        foreach($entry in @(@('NR_RECORDING_FRAME ','Frames'),@('NR_RECORDING_LANE ','Lanes'),
            @('NR_RECORDING_DECODED ','Decoded'),@('NR_RECORDING_CLEANUP ','Cleanup'))) {
            if($line.StartsWith($entry[0])){$sets[$entry[1]]+=@($line.Substring($entry[0].Length)|ConvertFrom-Json)}
        }
    }
    Assert-NrRecordingRows $sets.Frames $sets.Lanes $sets.Decoded $sets.Cleanup
    if($Output.Contains('NR_RECORDING_FAILURE') -or $Output.Contains('NR_RECORDING_RETAINED') -or
        !$Output.Contains('NR_RECORDING_SCOPE real_filter=true NR_off_current_raw=true Feature18=true SDR=true HDR=true recording=true independently_decoded=true quality=false performance=false cleanup=complete')){
        throw 'NR_RECORDING_SCOPE'
    }
    $sets
}
