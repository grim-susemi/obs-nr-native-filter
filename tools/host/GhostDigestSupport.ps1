# SPDX-License-Identifier: GPL-3.0-only
# Ghost-only machine schema. Generic numeric/FNV scenario validators are unchanged.
function Assert-GhostSha256([object]$Value) {
    if($Value -isnot [string] -or $Value -cnotmatch '^[0-9a-f]{64}$') { throw 'NR_QA_GHOST_SHA256_ENCODING' }
}
function Assert-GhostKey([object]$Key,[bool]$Job) {
    if(!$Key){throw 'NR_QA_GHOST_KEY_MISSING'}
    foreach($name in @('instance','content_epoch','ingress')) {
        if(!$Key.PSObject.Properties[$name] -or [UInt64]$Key.$name -eq 0){throw 'NR_QA_GHOST_FRAME_KEY'}
    }
    if($Job) {
        foreach($name in @('history','generation','submission')) {
            if(!$Key.PSObject.Properties[$name] -or [UInt64]$Key.$name -eq 0){throw 'NR_QA_GHOST_JOB_KEY'}
        }
    }
}
function Assert-GhostSameKey([object]$Actual,[object]$Expected,[bool]$Job) {
    Assert-GhostKey $Actual $Job;Assert-GhostKey $Expected $Job
    $names=@('instance','content_epoch','ingress')
    if($Job){$names+=@('history','generation','submission')}
    foreach($name in $names) {
        if([UInt64]$Actual.$name -ne [UInt64]$Expected.$name){throw 'NR_QA_GHOST_STALE_KEY'}
    }
}
function Assert-GhostStageDigests([object[]]$Packets) {
    if($Packets.Count -eq 0){throw 'NR_QA_GHOST_DIGEST_ROWS_MISSING'}
    $roles=@('canonical','encoded','keep','foreign','resolved','current_analysis','reference_analysis',
        'motion','vectors','cost','completed','current_raw','selected_raw','selected','program','preview','projector')
    $previous=@{}
    foreach($packet in $Packets) {
        if($packet.identity_digest_algorithm -cne 'sha256-chunk-root-v1' -or $packet.schema -ne 2 -or
            $packet.digest_mechanism -cne 'gpu_exact_chunks_cpu_ordered_root'){throw 'NR_QA_GHOST_DIGEST_ALGORITHM'}
        Assert-GhostKey $packet.current_key $false
        if([UInt64]$packet.settings_revision -eq 0 -or [UInt64]$packet.resource_generation -eq 0){throw 'NR_QA_GHOST_REVISION'}
        if($packet.stages.Count -ne 17){throw 'NR_QA_GHOST_STAGE_COUNT'}
        $instance=[string]$packet.current_key.instance
        if($previous.ContainsKey($instance)) {
            $old=$previous[$instance]
            if([UInt64]$packet.serial -le [UInt64]$old.serial -or
                [UInt64]$packet.current_key.ingress -ne [UInt64]$old.current_key.ingress+1 -or
                [UInt64]$packet.tick -ne [UInt64]$old.tick+1){throw 'NR_QA_GHOST_REORDERED_PACKET'}
        }
        $previous[$instance]=$packet
        $modelMask=[UInt64]$packet.model_mask;$graphicsMask=[UInt64]$packet.graphics_mask
        if($modelMask -gt 2047 -or $graphicsMask -gt 63){throw 'NR_QA_GHOST_STAGE_MASK'}
        if(!$packet.preparation -and $graphicsMask -ne 63){throw 'NR_QA_GHOST_GRAPHIC_STAGE_MISSING'}
        if($packet.model_evaluated) {
            $required=if($packet.reset){1279}else{2047}
            if($modelMask -ne $required){throw 'NR_QA_GHOST_MODEL_STAGE_MISSING'}
            Assert-GhostKey $packet.model_key $true
            Assert-GhostSameKey $packet.model_key $packet.current_key $false
        }elseif($modelMask -ne 0){throw 'NR_QA_GHOST_UNBOUND_MODEL_STAGES'}
        if(!$packet.reset -and $packet.model_evaluated) {
            Assert-GhostKey $packet.reference_key $true
            if([UInt64]$packet.reference_key.ingress+1 -ne [UInt64]$packet.model_key.ingress -or
                [UInt64]$packet.reference_key.instance -ne [UInt64]$packet.model_key.instance -or
                [UInt64]$packet.reference_key.history -ne [UInt64]$packet.model_key.history -or
                [UInt64]$packet.reference_key.generation -ne [UInt64]$packet.model_key.generation){throw 'NR_QA_GHOST_REFERENCE_KEY'}
        }
        [UInt64]$total=0
        for($index=0;$index -lt 17;$index++) {
            $stage=$packet.stages[$index]
            if($stage.role -cne $roles[$index]){throw 'NR_QA_GHOST_DUPLICATE_MISSING_EXTRA_ROLE'}
            $exists=if($index -lt 11){($modelMask -band ([UInt64]1 -shl $index)) -ne 0}else{
                ($graphicsMask -band ([UInt64]1 -shl ($index-11))) -ne 0}
            if($stage.present -isnot [bool] -or $stage.present -ne $exists){throw 'NR_QA_GHOST_ROLE_PRESENCE'}
            $domain=$stage.domain
            if(!$domain -or $domain.width -le 0 -or $domain.height -le 0 -or $domain.rows -ne $domain.height -or
                $domain.row_bytes -le 0 -or $domain.primaries -cne 'rec709' -or $domain.exposure -ne 1 -or
                $domain.oracle_reference_white_nits -le 0){throw 'NR_QA_GHOST_DOMAIN'}
            $expectedTransfer=if($index -in @(1,3)){'model_proxy'}elseif($index -in @(5,6)){'srgb_analysis'}elseif($index -eq 7){
                'working_pixel_motion'}elseif($index -eq 8){'signed_1_32_native_flow'}elseif($index -eq 9){'native_cost'}elseif($index -eq 14){
                if($packet.hdr_output){'scrgb_linear'}else{'srgb'}}else{if($packet.source_linear){'scrgb_linear'}else{'srgb'}}
            if($domain.transfer -cne $expectedTransfer -or
                $domain.reference_white_nits -ne $(if($index -eq 14 -and $packet.hdr_output){300}else{80})){
                throw 'NR_QA_GHOST_TRANSFER_WHITE_DOMAIN'
            }
            $expectedAlpha=if($index -in @(7,8,9)){'none'}elseif($index -in @(5,6)){'opaque'}elseif($index -in @(1,3)){
                'model_proxy'}elseif($index -eq 14){'composited'}else{'source_preserved'}
            if($domain.alpha -cne $expectedAlpha){throw 'NR_QA_GHOST_ALPHA_DOMAIN'}
            $expectedFormat=if($index -le 4){10}elseif($index -in @(5,6)){87}elseif($index -eq 7){34}elseif($index -eq 8){38}elseif($index -eq 9){
                $domain.storage_dxgi}elseif($index -eq 14){if($packet.hdr_output){10}else{87}}else{if($packet.source_linear){10}else{28}}
            if($domain.storage_dxgi -ne $expectedFormat){throw 'NR_QA_GHOST_STORAGE_DOMAIN'}
            $pixelBytes=switch([int]$domain.storage_dxgi){10{8}28{4}87{4}34{4}38{4}62{1}57{2}42{4}default{throw 'NR_QA_GHOST_FORMAT'}}
            if([UInt64]$domain.row_bytes -ne [UInt64]$domain.width*$pixelBytes){throw 'NR_QA_GHOST_LOGICAL_ROW'}
            if($index -notin @(8,9) -and ($domain.width -ne 1920 -or $domain.height -ne 1080)){throw 'NR_QA_GHOST_FULL_WORKING_EXTENT'}
            $expectedFrame=$packet.current_key;$expectedJob=$null
            if($index -lt 11 -and $packet.PSObject.Properties['model_key']) {
                $expectedJob=$packet.model_key
                $expectedFrame=$expectedJob
                if($index -eq 6 -and $packet.PSObject.Properties['reference_key']){$expectedFrame=$packet.reference_key}
            }elseif($index -gt 11 -and $packet.PSObject.Properties['selected_frame']) {
                $expectedFrame=$packet.selected_frame
                if($packet.PSObject.Properties['selected_key']){$expectedJob=$packet.selected_key}
            }
            Assert-GhostSameKey $stage.frame_key $expectedFrame $false
            if($expectedJob){Assert-GhostSameKey $stage.job_key $expectedJob $true}
            elseif($stage.PSObject.Properties['job_key']){throw 'NR_QA_GHOST_UNEXPECTED_JOB'}
            if($exists) {
                if([UInt64]$stage.bytes -ne [UInt64]$domain.row_bytes*[UInt64]$domain.rows -or $stage.bytes -le 0){
                    throw 'NR_QA_GHOST_TRUNCATED_PAYLOAD'
                }
                Assert-GhostSha256 $stage.digest;$total+=[UInt64]$stage.bytes
            }elseif($stage.bytes -ne 0 -or $stage.PSObject.Properties['digest']){throw 'NR_QA_GHOST_ABSENT_IMAGE_DIGEST'}
        }
        if([UInt64]$packet.digest_bytes -ne $total){throw 'NR_QA_GHOST_DIGEST_BYTE_SUM'}
    }
}
function Assert-GhostCodecDigests([object[]]$Rows,[object[]]$Frames=@()) {
    if($Rows.Count -ne 64){throw 'NR_QA_GHOST_CODEC_DIGEST_COUNT'}
    $seen=@{}
    $last=@{}
    foreach($row in $Rows) {
        if($row.identity_digest_algorithm -cne 'sha256-chunk-root-v1' -or $row.schema -ne 2 -or
            $row.digest_mechanism -cne 'gpu_exact_chunks_cpu_ordered_root' -or $row.role -notin @('preencode','decoded')){throw 'NR_QA_GHOST_CODEC_ALGORITHM'}
        Assert-GhostSha256 $row.digest;Assert-GhostKey $row.frame_key $false
        $key=$row.role+':'+$row.index
        if($row.index -lt 0 -or $row.index -ge 32 -or $seen.ContainsKey($key)){throw 'NR_QA_GHOST_CODEC_DUPLICATE'}
        $seen[$key]=$true
        if($last.ContainsKey($row.role)) {
            if($row.index -ne $last[$row.role].index+1 -or
                [UInt64]$row.frame_key.ingress -ne [UInt64]$last[$row.role].frame_key.ingress+1){
                throw 'NR_QA_GHOST_CODEC_REORDER_OR_STALE'
            }
        }elseif($row.index -ne 0){throw 'NR_QA_GHOST_CODEC_FIRST_INDEX'}
        $last[$row.role]=$row
        if($Frames.Count -gt 0) {
            $bound=@($Frames|Where-Object {$_.clock -eq $row.source_clock})
            if($bound.Count -ne 1){throw 'NR_QA_GHOST_CODEC_SOURCE_CLOCK'}
            Assert-GhostSameKey $row.frame_key $bound[0].selected_frame $false
            if($row.settings_revision -ne $bound[0].settings_revision -or
                $row.resource_generation -ne $bound[0].resource_generation){throw 'NR_QA_GHOST_CODEC_SETTINGS_DOMAIN'}
            if($bound[0].PSObject.Properties['selected_key']){Assert-GhostSameKey $row.job_key $bound[0].selected_key $true}
            elseif($row.PSObject.Properties['job_key']){throw 'NR_QA_GHOST_CODEC_UNEXPECTED_JOB'}
        }
        if($row.width -ne 1920 -or $row.height -ne 1080 -or $row.full_range -or $row.planes.Count -ne 3){throw 'NR_QA_GHOST_CODEC_DOMAIN'}
        if($row.format_tag -cnotin @('nv12','p010','i420','i010')){throw 'NR_QA_GHOST_CODEC_FORMAT'}
        $ten=$row.format_tag -cin @('p010','i010');$planar=$row.format_tag -cin @('i420','i010')
        if($row.transfer_tag -cne $(if($ten){'pq'}else{'sdr'}) -or $row.primaries -cne $(if($ten){'rec2020'}else{'rec709'})){
            throw 'NR_QA_GHOST_CODEC_TRANSFER_PRIMARIES'
        }
        [UInt64]$bytes=0
        for($plane=0;$plane -lt 3;$plane++) {
            $p=$row.planes[$plane]
            if($p.index -ne $plane -or [UInt64]$p.bytes -ne [UInt64]$p.row_bytes*[UInt64]$p.rows){throw 'NR_QA_GHOST_CODEC_PLANE_ORDER_SIZE'}
            $expectedRow=if($plane -eq 0 -or !$planar){1920*$(if($ten){2}else{1})}else{960*$(if($ten){2}else{1})}
            $expectedRows=if($plane -eq 0){1080}elseif($plane -eq 2 -and !$planar){0}else{540}
            if($expectedRows -eq 0){$expectedRow=0}
            if($p.row_bytes -ne $expectedRow -or $p.rows -ne $expectedRows){throw 'NR_QA_GHOST_CODEC_PLANE_DOMAIN'}
            $bytes+=[UInt64]$p.bytes
        }
        if($bytes -eq 0 -or $bytes -ne [UInt64]$row.bytes){throw 'NR_QA_GHOST_CODEC_PAYLOAD'}
    }
}
# UG03P3 (architect decision recorded in ug03p3: ALLOWED_WITH_CONDITIONS; I3 Task26 B1/B2): the holdout lanes
# (passes 1-2, holdout seed) run inside Task26 only after the lock. That order is checked from the observed rows,
# not from the host's marker text alone; rows are attributed to their lane by position, and the lock must equal
# the values captured before the freeze, with every holdout lane judged by that lock.
function Get-GhostRowInstance([string]$Type,[object]$Row) {
    $key=switch($Type){'stageRows'{'current_key'}'frames'{'selected_frame'}'codec'{'frame_key'}'physicalControls'{'current_key'}'off'{'current_key'}default{$null}}
    if($key) {
        $p=$Row.PSObject.Properties[$key]
        if(!$p -or $null -eq $p.Value -or !$p.Value.PSObject.Properties['instance']){throw 'NR_QA_GHOST_ROW_SEGMENT'}
        return [UInt64]$p.Value.instance
    }
    if($Type -in @('guides','edits')) {
        $p=$Row.PSObject.Properties['instance'];if(!$p){throw 'NR_QA_GHOST_ROW_SEGMENT'}
        return [UInt64]$p.Value
    }
    return $null
}
# A JSON integer scalar: arrays, strings and reals never pass the comparisons below by accident.
function Test-GhostInteger([object]$Value){return ($Value -is [int] -or $Value -is [long])}
function Assert-GhostFreezeOrder([object[]]$Ordered,[int[]]$GhostLines,[int[]]$FreezeLines,[bool]$FreezeExact,[object]$Policy) {
    $laneRows=@($Ordered|Where-Object {$_.type -ceq 'lanes'})
    if($laneRows.Count -ne 24 -or [UInt64]$Policy.calibration_seed -eq [UInt64]$Policy.holdout_seed){throw 'NR_QA_GHOST_LANE_ORDER'}
    $instances=@{}
    for($k=0;$k -lt 24;$k++) {
        $lane=$laneRows[$k].value
        foreach($name in @('pass','rate_index','mode','seed','instance')){
            if(!$lane.PSObject.Properties[$name] -or !(Test-GhostInteger $lane.$name)){throw 'NR_QA_GHOST_LANE_ORDER'}
        }
        $seed=if($k -lt 8){$Policy.calibration_seed}else{$Policy.holdout_seed}
        if($lane.pass -ne [Math]::Floor($k/8) -or $lane.rate_index -ne [Math]::Floor(($k%8)/4) -or $lane.mode -ne $k%4 -or
            [UInt64]$lane.seed -ne [UInt64]$seed){throw 'NR_QA_GHOST_LANE_ORDER'}
        $id=[string][UInt64]$lane.instance
        if($id -ceq '0' -or $instances.ContainsKey($id)){throw 'NR_QA_GHOST_LANE_ORDER'}
        $instances[$id]=$k
    }
    # Exactly one freeze marker, between the eighth (last calibration) and the ninth (first holdout) lane rows.
    $bounds=@($laneRows|ForEach-Object {$_.index})
    $freeze=if($FreezeLines.Count -eq 1){$FreezeLines[0]}else{-1}
    if($freeze -lt 0 -or !$FreezeExact -or $freeze -le $bounds[7] -or $freeze -ge $bounds[8]){throw 'NR_QA_GHOST_FREEZE_ORDER'}
    # Rows are attributed to lane k by position: segment k is (lane row k-1, lane row k], the first holdout segment
    # starting at the freeze marker. A row that carries an instance carries lane k's instance; calibration-only rows
    # (planted controls on the calibration seed, OFF proofs) lie in calibration segments; nothing follows the last lane.
    $segments=@(0..23|ForEach-Object {@{frames=0;cleanup=0;decoded=0;physicalControls=0;off=0}})
    foreach($row in $Ordered) {
        if($row.type -ceq 'lanes'){continue}
        $k=-1
        for($j=0;$j -lt 24;$j++){if($row.index -lt $bounds[$j]){$k=$j;break}}
        if($k -lt 0 -or ($k -eq 8 -and $row.index -lt $freeze)){throw 'NR_QA_GHOST_ROW_SEGMENT'}
        $instance=Get-GhostRowInstance $row.type $row.value
        if($null -ne $instance -and $instance -ne [UInt64]$laneRows[$k].value.instance){throw 'NR_QA_GHOST_ROW_SEGMENT'}
        if($row.type -in @('physicalControls','off') -and $k -ge 8){throw 'NR_QA_GHOST_ROW_SEGMENT'}
        if($row.type -ceq 'physicalControls') {
            $seed=$row.value.PSObject.Properties['seed']
            if(!$seed -or $null -eq $seed.Value -or [UInt64]$seed.Value -ne [UInt64]$Policy.calibration_seed){throw 'NR_QA_GHOST_CONTROL_SEED'}
        }
        if($segments[$k].ContainsKey($row.type)){$segments[$k][$row.type]++}
    }
    # The freeze marker is the first ghost line of any kind after the eighth lane row.
    if(@($GhostLines|Where-Object {$_ -gt $bounds[7] -and $_ -lt $freeze}).Count){throw 'NR_QA_GHOST_FREEZE_ORDER'}
    return @{lanes=@($laneRows|ForEach-Object {$_.value});segments=$segments}
}
# Per-lane row counts inside each segment (architect check 4): 256 frames, one cleanup and 32 decoded frames per lane;
# nine planted controls and the lane's own OFF samples per calibration lane, none in holdout lanes.
function Assert-GhostSegmentCounts([object[]]$Segments,[object]$Calibration) {
    $locked=@($Calibration.lanes)
    for($k=0;$k -lt 24;$k++) {
        $segment=$Segments[$k]
        $controls=if($k -lt 8){9}else{0}
        $off=0;if($k -lt 8){$off=$locked[$k].off_current_samples} # direct assignment keeps a JSON array an array
        if($segment.frames -ne 256 -or $segment.cleanup -ne 1 -or $segment.decoded -ne 32 -or
            $segment.physicalControls -ne $controls -or !(Test-GhostInteger $off) -or $segment.off -ne $off){throw 'NR_QA_GHOST_ROW_SEGMENT'}
    }
}
function Test-GhostSameValue([object]$A,[object]$B) {
    if($null -eq $A -or $null -eq $B -or $A -is [string] -or $B -is [string] -or $A -is [bool] -or $B -is [bool] -or
        $A -is [array] -or $B -is [array]){return $false}
    $x=[double]$A;$y=[double]$B
    if([double]::IsNaN($x) -or [double]::IsNaN($y) -or [double]::IsInfinity($x) -or [double]::IsInfinity($y)){return $false}
    # Both writers print 17 significant digits; the tolerance only absorbs text-to-double noise.
    return [Math]::Abs($x-$y) -le 1e-12+1e-9*[Math]::Max([Math]::Abs($x),[Math]::Abs($y))
}
function Assert-GhostLockBinding([object[]]$Lanes,[object]$Calibration) {
    $locked=@($Calibration.lanes)
    if($locked.Count -ne 8 -or $Lanes.Count -ne 24){throw 'NR_QA_GHOST_LOCK_BINDING'}
    $pairs=@(@('format_bound','declared_format_bound'),@('raw_maximum','raw_maximum'),@('epsilon','epsilon'),
        @('guide_mismatch','guide_mismatch'),@('absolute_vector_error','absolute_vector_error'),@('relative_vector_error','relative_vector_error'))
    for($i=0;$i -lt 8;$i++) {
        $lock=$locked[$i];$capture=$Lanes[$i]
        if(!(Test-GhostInteger $lock.rate_index) -or !(Test-GhostInteger $lock.mode) -or !(Test-GhostInteger $lock.raw_samples) -or
            $lock.rate_index -ne [Math]::Floor($i/4) -or $lock.mode -ne $i%4 -or $lock.raw_samples -ne 256 -or
            !(Test-GhostSameValue $lock.model_samples $capture.model_samples)){throw 'NR_QA_GHOST_LOCK_BINDING'}
        foreach($pair in $pairs){if(!(Test-GhostSameValue $lock.($pair[0]) $capture.($pair[1]))){throw 'NR_QA_GHOST_LOCK_BINDING'}}
    }
    for($k=8;$k -lt 24;$k++) {
        $lane=$Lanes[$k];$lock=$locked[[int]($lane.rate_index*4+$lane.mode)]
        foreach($pair in $pairs){if(!(Test-GhostSameValue $lock.($pair[0]) $lane.($pair[1]))){throw 'NR_QA_GHOST_HOLDOUT_LOCK'}}
    }
}
function Get-GhostReceiptText([byte[]]$Bytes) {
    return [Text.Encoding]::UTF8.GetString($Bytes).TrimStart([char]0xFEFF)
}
function Get-GhostBytesSha256([byte[]]$Bytes) {
    $sha=[Security.Cryptography.SHA256]::Create()
    try{return ([BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace('-','').ToLowerInvariant()}finally{$sha.Dispose()}
}
# I3 Task26 B3: the receipts are a closed schema with JSON booleans, as the Task27 reader requires
# (ghost_baseline.cpp flag() and the nine controls in frozen order).
function Assert-GhostReceiptSchema([object]$Calibration,[object]$Controls) {
    foreach($name in @('holdout_disjoint','raw_only_frozen','all_prior_templates','image_decoded_source_identity','actual_loaded_runtime','normal_cleanup')) {
        $p=$Calibration.PSObject.Properties[$name]
        if(!$p -or $p.Value -isnot [bool] -or !$p.Value){throw 'NR_QA_GHOST_RECEIPT_SCHEMA'}
    }
    $q=$Calibration.PSObject.Properties['qualification_allowed']
    if(!$q -or $q.Value -isnot [bool] -or $q.Value){throw 'NR_QA_GHOST_RECEIPT_SCHEMA'}
    foreach($lane in @($Calibration.lanes)) {
        $m=$lane.PSObject.Properties['measured']
        if(!$m -or $m.Value -isnot [bool] -or !$m.Value){throw 'NR_QA_GHOST_RECEIPT_SCHEMA'}
    }
    $q=$Controls.PSObject.Properties['qualification_allowed']
    if(!$q -or $q.Value -isnot [bool] -or !$q.Value){throw 'NR_QA_GHOST_RECEIPT_SCHEMA'}
    $names=@('blend_2percent','blur_1pixel','frozen','extra_delay','dropped','stale_guide','relabeled_stale_guide','reordered_completion','poststamp_only')
    $list=@($Controls.controls)
    if($list.Count -ne 9){throw 'NR_QA_GHOST_RECEIPT_SCHEMA'}
    for($i=0;$i -lt 9;$i++) {
        $c=$list[$i]
        if($c.name -isnot [string] -or $c.name -cne $names[$i] -or $c.actual_pixels -isnot [bool] -or !$c.actual_pixels -or $c.rejected -isnot [bool] -or !$c.rejected){
            throw 'NR_QA_GHOST_RECEIPT_SCHEMA'
        }
    }
}
function Assert-GhostPublic([string]$Output,[string]$ArtifactRoot,[string]$Evidence,[object]$Policy,[string]$HostHash,[string]$SourceHash) {
    $stageRows=@();$frames=@();$lanes=@();$codec=@();$guides=@();$edits=@();$decoded=@();$cleanup=@();$physicalControls=@();$off=@()
    $ordered=New-Object System.Collections.Generic.List[object];$freezeLines=New-Object System.Collections.Generic.List[int]
    $freezeText='NR_GHOST_FREEZE raw_only=true actual_sdk=true both_rational_rates=true holdout_not_started=true'
    $freezeExact=$true;$lineIndex=-1;$ghostLines=New-Object System.Collections.Generic.List[int]
    foreach($line in ($Output -split [char]10)) {
        $lineIndex++
        if($line.StartsWith('NR_GHOST_')){$ghostLines.Add($lineIndex)}
        if($line.StartsWith('NR_GHOST_FREEZE')){$freezeLines.Add($lineIndex);if($line.TrimEnd([char]13) -cne $freezeText){$freezeExact=$false}}
        foreach($entry in @(
            @('NR_GHOST_STAGE_DIGEST ','stageRows'),@('NR_GHOST_FRAME ','frames'),@('NR_GHOST_LANE ','lanes'),
            @('NR_GHOST_CODEC_DIGEST ','codec'),@('NR_GHOST_GUIDE ','guides'),@('NR_GHOST_SELECTED_EDIT ','edits'),
            @('NR_GHOST_DECODED ','decoded'),@('NR_GHOST_CLEANUP ','cleanup'),
            @('NR_GHOST_PHYSICAL_CONTROL ','physicalControls'),@('NR_GHOST_OFF ','off'))) {
            if($line.StartsWith($entry[0])) {
                $value=$line.Substring($entry[0].Length)|ConvertFrom-Json
                $ordered.Add([pscustomobject]@{type=$entry[1];index=$lineIndex;value=$value})
                switch($entry[1]){
                    stageRows{$stageRows+=,$value}frames{$frames+=,$value}lanes{$lanes+=,$value}codec{$codec+=,$value}
                    guides{$guides+=,$value}edits{$edits+=,$value}decoded{$decoded+=,$value}cleanup{$cleanup+=,$value}
                    physicalControls{$physicalControls+=,$value}
                    off{$off+=,$value}
                }
            }
        }
    }
    if($Output.Contains('NR_GHOST_FAILURE') -or $Output.Contains('NR_GHOST_RETAINED') -or
        $Output.Contains('NR_GHOST_DIGEST_FAILURE') -or $Output.Contains('NR_GHOST_DETACHED_FAILURE')){
        throw 'NR_QA_GHOST_ACTUAL_FAILURE'
    }
    Assert-GhostStageDigests $stageRows
    if($lanes.Count -ne 24 -or $frames.Count -ne 6144 -or $cleanup.Count -ne 24){throw 'NR_QA_GHOST_COMPLETE_CORPUS'}
    if(!$Output.Contains('NR_GHOST_FREEZE raw_only=true actual_sdk=true both_rational_rates=true holdout_not_started=true')){
        throw 'NR_QA_GHOST_RAW_ONLY_FREEZE'
    }
    if(!$Policy){throw 'NR_QA_GHOST_PUBLIC_POLICY_BINDING'}
    foreach($name in @('calibration.json','bad-controls.json')){
        if(!(Test-Path -LiteralPath (Join-Path $ArtifactRoot $name))){throw 'NR_QA_GHOST_PHYSICAL_CALIBRATION_MISSING'}
    }
    # The accepted lock bytes are read once: these exact bytes are parsed, checked and their sha256 returned.
    $calibrationBytes=[IO.File]::ReadAllBytes((Join-Path $ArtifactRoot 'calibration.json'))
    $controlsBytes=[IO.File]::ReadAllBytes((Join-Path $ArtifactRoot 'bad-controls.json'))
    $calibration=(Get-GhostReceiptText $calibrationBytes)|ConvertFrom-Json
    $controls=(Get-GhostReceiptText $controlsBytes)|ConvertFrom-Json
    $order=Assert-GhostFreezeOrder $ordered.ToArray() $ghostLines.ToArray() $freezeLines.ToArray() $freezeExact $Policy
    Assert-GhostLockBinding $order.lanes $calibration
    Assert-GhostSegmentCounts $order.segments $calibration
    $seen=@{}
    foreach($lane in $lanes) {
        $key="$($lane.pass):$($lane.rate_index):$($lane.mode)"
        if($seen.ContainsKey($key) -or $lane.pass -notin @(0,1,2) -or $lane.rate_index -notin @(0,1) -or
            $lane.mode -notin @(0,1,2,3)){throw 'NR_QA_GHOST_LANE_DUPLICATE'}
        $seen[$key]=$true
        $num=if($lane.rate_index -eq 0){60}else{60000};$den=if($lane.rate_index -eq 0){1}else{1001}
        if($lane.rate_num -ne $num -or $lane.rate_den -ne $den -or $lane.raw_samples -ne 256 -or
            !$lane.closed -or !$lane.worker_exit -or $lane.retained -or
            $lane.seed -ne $(if($lane.pass -eq 0){2583}else{2857}) -or
            $lane.deterministic -ne ($lane.pass -eq 1) -or $lane.calibration -ne ($lane.pass -eq 0) -or
            $lane.source_linear -ne ($lane.mode -ge 2) -or $lane.hdr_output -ne ($lane.mode -ge 2)){
            throw 'NR_QA_GHOST_LANE_DOMAIN'
        }
        if($lane.declared_format_bound -lt 0 -or $lane.raw_maximum -lt 0 -or
            $lane.epsilon -ne [Math]::Max($lane.declared_format_bound,$lane.raw_maximum)){throw 'NR_QA_GHOST_EPSILON'}
        $owned=@($frames|Where-Object {$_.selected_frame.instance -eq $lane.instance})
        if($owned.Count -ne 256){throw 'NR_QA_GHOST_INGRESS_DENOMINATOR'}
        for($at=0;$at -lt $owned.Count;$at++) {
            $row=$owned[$at]
            if($row.identity_digest_algorithm -cne 'sha256-chunk-root-v1' -or $row.schema -ne 2){throw 'NR_QA_GHOST_FRAME_ALGORITHM'}
            foreach($name in @('raw_hash','selected_hash','program_hash')){Assert-GhostSha256 $row.$name}
            Assert-GhostKey $row.selected_frame $false
            if([UInt64]$row.selected_frame.ingress -ne [UInt64]$row.selected -or $row.source_tick -lt 0 -or
                $row.source_tick -ge 256 -or $row.content_id -le 0 -or $row.interval -le 0 -or
                [UInt64]$row.tick -ne [UInt64][Math]::Floor($row.clock/$row.interval)+1){throw 'NR_QA_GHOST_FRAME_BINDING'}
            if(($row.kind -eq 1 -and $row.selected -ne $row.ingress) -or
                ($row.kind -in @(2,3) -and $row.selected+1 -ne $row.ingress) -or
                $row.kind -notin @(1,2,3)){throw 'NR_QA_GHOST_SELECTION'}
            if($at -gt 0 -and ($row.tick -ne $owned[$at-1].tick+1 -or $row.ingress -ne $owned[$at-1].ingress+1)){
                throw 'NR_QA_GHOST_FRAME_ORDER'
            }
            if($lane.deterministic -and $row.raw_hash -cne $row.selected_hash){throw 'NR_QA_GHOST_DETERMINISTIC_BYTES'}
            $packet=@($stageRows|Where-Object {!$_.preparation -and $_.current_key.instance -eq $lane.instance -and $_.tick -eq $row.tick})
            if($packet.Count -ne 1 -or $packet[0].serial -ne $row.serial -or
                $packet[0].stages[12].digest -cne $row.raw_hash -or $packet[0].stages[13].digest -cne $row.selected_hash -or
                $packet[0].stages[14].digest -cne $row.program_hash){throw 'NR_QA_GHOST_FRAME_STAGE_DIGEST'}
        }
        if(!$lane.deterministic) {
            $positive=@($edits|Where-Object {$_.instance -eq $lane.instance -and $_.binary_g -eq 1 -and
                $_.restore_reference_valid -and $_.same_JobKey_D_before_g -and $_.static_positive -and $_.moving_positive -and
                $_.static_rms -gt 8*$lane.epsilon -and $_.moving_rms -gt 8*$lane.epsilon -and
                $_.static_retention -ge .8 -and $_.moving_retention -ge .8})
            if($positive.Count -eq 0 -or $lane.static_positive_samples -le 0 -or $lane.moving_positive_samples -le 0){
                throw 'NR_QA_GHOST_ORDINARY_SELECTED_EDIT_POSITIVES'
            }
            foreach($edit in $positive) {
                $selected=@($owned|Where-Object {$_.selected -eq $edit.selected_ingress -and $_.PSObject.Properties['selected_key'] -and
                    $_.selected_key.submission -eq $edit.selected_serial})
                if($selected.Count -ne 1){throw 'NR_QA_GHOST_SELECTED_D_BINDING'}
                Assert-GhostSameKey $edit.D_key $selected[0].selected_key $true
            }
            $ordinary=@($guides|Where-Object {$_.instance -eq $lane.instance -and !$_.reset -and $_.reference_present -and
                $_.labeled_points -gt 0 -and $_.sign_units_rectangle_asserted})
            if($ordinary.Count -eq 0){throw 'NR_QA_GHOST_ORDINARY_GUIDE_POSITIVES'}
        }
        Assert-GhostCodecDigests @($codec|Where-Object {$_.frame_key.instance -eq $lane.instance}) $owned
        $directory=Join-Path $ArtifactRoot "pass-$($lane.pass)-rate-$($lane.rate_index)-mode-$($lane.mode)"
        foreach($name in @('source.bmp','filter.bmp','program.bmp','decoded.bmp','program.hevc','templates.tsv')){
            $path=Join-Path $directory $name
            if(!(Test-Path -LiteralPath $path) -or (Get-Item -LiteralPath $path).Length -le 0){throw 'NR_QA_GHOST_PHYSICAL_ARTIFACT'}
        }
    }
    foreach($row in $cleanup){if(!$row.closed -or !$row.worker_exit -or !$row.consumer_event -or $row.retained){throw 'NR_QA_GHOST_CLEANUP'}}
    if($decoded.Count -ne 768){throw 'NR_QA_GHOST_DECODED_COUNT'}
    foreach($row in $decoded) {
        if($row.code_errors -ne 0 -or $row.codes_compared -ne 3110400 -or !$row.packet_preencode_decoder_rational_order){
            throw 'NR_QA_GHOST_REAL_CODEC_PROOF'
        }
    }
    if($calibration.identity_digest_algorithm -cne 'sha256-chunk-root-v1' -or $calibration.schema -ne 2 -or $calibration.evidence_class -cne 'physical' -or
        !$calibration.raw_only_frozen -or !$calibration.holdout_disjoint -or $calibration.off_current_samples -le 0 -or
        !$calibration.all_prior_templates -or !$calibration.image_decoded_source_identity -or
        !$calibration.actual_loaded_runtime -or !$calibration.normal_cleanup){throw 'NR_QA_GHOST_CALIBRATION_PROOF'}
    if(!$Policy){throw 'NR_QA_GHOST_PUBLIC_POLICY_BINDING'}
    Assert-GhostPhysicalMeasurements $calibration $controls $Policy
    Assert-GhostOffProof $calibration $off @($lanes|Where-Object {$_.calibration})
    if($physicalControls.Count -ne 72){throw 'NR_QA_GHOST_PHYSICAL_CONTROL_ROWS'}
    Assert-GhostControlRows $physicalControls $stageRows
    foreach($lane in @($lanes|Where-Object {$_.calibration})) {
        foreach($name in @('blend_2percent','blur_1pixel','frozen','extra_delay','dropped','stale_guide','relabeled_stale_guide','reordered_completion','poststamp_only')) {
            $found=@($physicalControls|Where-Object {$_.current_key.instance -eq $lane.instance -and $_.name -ceq $name})
            if($found.Count -ne 1 -or $found[0].mode -ne $lane.mode -or $found[0].rate_num -ne $lane.rate_num -or
                $found[0].rate_den -ne $lane.rate_den){throw 'NR_QA_GHOST_CONTROL_COHORT_BINDING'}
        }
    }
    if($calibration.host_sha256 -cne $HostHash -or $calibration.host_source_sha256 -cne $SourceHash){
        throw 'NR_QA_GHOST_PUBLIC_SOURCE_BINDING'
    }
    $required=@('blend_2percent','blur_1pixel','frozen','extra_delay','dropped','stale_guide','reordered_completion','poststamp_only')
    if($controls.evidence_class -cne 'physical' -or $controls.identity_digest_algorithm -cne 'sha256-chunk-root-v1' -or $controls.schema -ne 2 -or
        $controls.positive_anchors -le 0){throw 'NR_QA_GHOST_PHYSICAL_CONTROL_CLASS'}
    foreach($name in $required) {
        $found=@($controls.controls|Where-Object {$_.name -ceq $name})
        if($found.Count -ne 1 -or !$found[0].rejected -or !$found[0].actual_pixels -or $found[0].cases -le 0){
            throw 'NR_QA_GHOST_PLANTED_CONTROL'
        }
    }
    return @{stage_packets=$stageRows.Count;frames=$frames.Count;lanes=$lanes.Count;codec_digests=$codec.Count;physical=$true;
        calibration_sha256=(Get-GhostBytesSha256 $calibrationBytes);controls_sha256=(Get-GhostBytesSha256 $controlsBytes)}
}

# The receipt's host_source_sha256 must name the exact bytes compiled into the host image: the sealed snapshot row whose
# digest the entry pins (the host embeds the same digest as ghost::compiled_host_source_sha256). W lookups are never used.
function Get-GhostSealedSourceSha256([string]$SealedSource,[string]$PinnedSha256) {
    Assert-GhostSha256 $PinnedSha256
    if(!$SealedSource -or !(Test-Path -LiteralPath $SealedSource -PathType Leaf)){throw 'NR_QA_GHOST_SEALED_SOURCE_ABSENT'}
    $actual=(Get-FileHash -LiteralPath $SealedSource -Algorithm SHA256).Hash.ToLowerInvariant()
    if($actual -cne $PinnedSha256){throw 'NR_QA_GHOST_SEALED_SOURCE_BINDING'}
    return $actual
}

# Exact immutable policy bytes are separate from physical result receipts.
function Assert-GhostFreezeDocuments([object]$Calibration,[object]$Holdout,[object]$Masks,[object]$Epsilon,[object]$Thresholds) {
    if($Calibration.schema -ne 1 -or $Holdout.schema -ne 1 -or $Masks.schema -ne 1 -or $Epsilon.schema -ne 1 -or $Thresholds.schema -ne 1){throw 'NR_QA_GHOST_POLICY_SCHEMA'}
    if($Calibration.seed -ne 2583 -or $Holdout.seed -ne 2857 -or $Calibration.seed -eq $Holdout.seed -or
        $Holdout.calibration_seed -ne $Calibration.seed -or !$Holdout.disjoint -or $Holdout.candidate_tuning_allowed -or
        $Calibration.ticks -ne 256 -or $Holdout.ticks -ne 256){throw 'NR_QA_GHOST_POLICY_COHORT'}
    if((ConvertTo-Json -InputObject $Calibration.rates -Depth 8 -Compress) -cne '[[60,1],[60000,1001]]' -or
        ($Calibration.colors|ConvertTo-Json -Compress) -cne '["SDR-linear-709","dark-SDR-linear-709","HDR-linear-709","HDR-linear-2020"]' -or
        ($Holdout.colors|ConvertTo-Json -Compress) -cne '[0,1,2,3]'){throw 'NR_QA_GHOST_POLICY_LANES'}
    if(($Masks.working_geometry|ConvertTo-Json -Compress) -cne '[192,96]' -or
        ($Masks.physical_canvas|ConvertTo-Json -Compress) -cne '[1920,1080]' -or $Masks.rescale_geometry -or
        ($Masks.visibility.x_half_open|ConvertTo-Json -Compress) -cne '[0,192]' -or
        ($Masks.visibility.y_half_open|ConvertTo-Json -Compress) -cne '[8,96]' -or
        $Masks.physical_masks.static_edit.x -ne 256 -or $Masks.physical_masks.static_edit.y -ne 128 -or
        $Masks.physical_masks.static_edit.width -ne 32 -or $Masks.physical_masks.static_edit.height -ne 32 -or
        $Masks.physical_masks.moving_edit_inset -ne 8 -or
        (ConvertTo-Json -InputObject $Masks.physical_masks.detail_regions -Depth 8 -Compress) -cne '[[0,8,192,88],[256,128,32,32],[0,79,1920,3]]') {throw 'NR_QA_GHOST_POLICY_MASKS'}
    if($Thresholds.numeric.temporal_epsilon_factor -ne 2 -or $Thresholds.numeric.detail_epsilon_factor -ne 2 -or
        $Thresholds.numeric.edit_epsilon_factor -ne 8 -or $Thresholds.numeric.edit_retention_min -ne .8 -or
        $Thresholds.edit_energy_retained_min -ne .8 -or $Epsilon.candidate_output_may_set_epsilon -or
        !$Epsilon.physical_values_required_before_qualification -or !$Epsilon.codec_error_is_separate) {throw 'NR_QA_GHOST_POLICY_THRESHOLDS'}
    if($Calibration.qualification_allowed -or $Epsilon.qualification_allowed -or $Thresholds.qualification_allowed){throw 'NR_QA_GHOST_CPU_POLICY_IS_NOT_PHYSICAL'}
}
function Read-GhostFreezePolicy([string]$PolicyRoot,[string]$ManifestPath) {
    $names=@('calibration','holdout','masks','epsilon','thresholds')
    $pins=@('083846ff95bc9bd819cd2d0424d98acd86554ca6023a6296e1b7acc200f6f881','d15c5110cc727142e290b5d7723eb8abaad08fd2c61ea4b638ca6c8cc681c2ac','3a0bbba94fb75f18e5d77f61f6469809e4d4af61ed46549dab46b37cd99dd16f','2562e4187b776da35d5fb84ca384b2b7fbcad81fd5a86c3e4fd5fe8bd1730411','60c0504d36ed3f4687584f72f1c8292525cae7e52f265c131aad8b1a86cb6e61')
    $docs=@();$hashes=@()
    for($i=0;$i -lt 5;$i++) {
        $path=Join-Path $PolicyRoot ($names[$i]+'.json')
        $hash=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        if($hash -cne $pins[$i]){throw 'NR_QA_GHOST_POLICY_HASH'}
        $docs+=,[IO.File]::ReadAllText($path)|ConvertFrom-Json
        $hashes+=,$hash
    }
    Assert-GhostFreezeDocuments $docs[0] $docs[1] $docs[2] $docs[3] $docs[4]
    $manifest=(Get-FileHash -LiteralPath $ManifestPath -Algorithm SHA256).Hash.ToLowerInvariant()
    foreach($index in @(0,1,2)) {if($docs[$index].manifest_sha256 -cne $manifest){throw 'NR_QA_GHOST_POLICY_MANIFEST'}}
    $m=$docs[2];$t=$docs[4].numeric
    return [ordered]@{policy_root=[IO.Path]::GetFullPath($PolicyRoot);manifest_path=[IO.Path]::GetFullPath($ManifestPath);
        hash0=$hashes[0];hash1=$hashes[1];hash2=$hashes[2];hash3=$hashes[3];hash4=$hashes[4];manifest_hash=$manifest;
        calibration_seed=$docs[0].seed;holdout_seed=$docs[1].seed;ticks=$docs[0].ticks;
        rate0_num=$docs[0].rates[0][0];rate0_den=$docs[0].rates[0][1];rate1_num=$docs[0].rates[1][0];rate1_den=$docs[0].rates[1][1];
        width=$m.working_geometry[0];height=$m.working_geometry[1];visible_top=$m.visibility.y_half_open[0];
        physical_width=$m.physical_canvas[0];physical_height=$m.physical_canvas[1];
        static_x=$m.physical_masks.static_edit.x;static_y=$m.physical_masks.static_edit.y;
        static_width=$m.physical_masks.static_edit.width;static_height=$m.physical_masks.static_edit.height;
        moving_inset=$m.physical_masks.moving_edit_inset;temporal_factor=$t.temporal_epsilon_factor;
        detail_factor=$t.detail_epsilon_factor;edit_factor=$t.edit_epsilon_factor;edit_retention=$t.edit_retention_min}
}
function Assert-GhostPhysicalMeasurements([object]$Calibration,[object]$Controls,[object]$Policy) {
    Assert-GhostReceiptSchema $Calibration $Controls
    foreach($receipt in @($Calibration,$Controls)) {
        if($receipt.schema -ne 2 -or $receipt.evidence_class -cne 'physical' -or
            $receipt.identity_digest_algorithm -cne 'sha256-chunk-root-v1' -or $receipt.measurement_status -cne 'measured') {throw 'NR_QA_GHOST_UNMEASURED_PHYSICAL'}
        Assert-GhostSha256 $receipt.host_sha256;Assert-GhostSha256 $receipt.host_source_sha256
        $names=@('calibration.json','holdout.json','masks.json','epsilon.json','thresholds.json')
        for($i=0;$i -lt 5;$i++) {if($receipt.policy_hashes.($names[$i]) -cne $Policy.('hash'+$i)){throw 'NR_QA_GHOST_RECEIPT_POLICY_BINDING'}}
        if($receipt.manifest_sha256 -cne $Policy.manifest_hash){throw 'NR_QA_GHOST_RECEIPT_MANIFEST'}
    }
    if($Calibration.host_sha256 -cne $Controls.host_sha256 -or $Calibration.host_source_sha256 -cne $Controls.host_source_sha256){throw 'NR_QA_GHOST_RECEIPT_SOURCE_BINDING'}
    if($Calibration.calibration_seed -ne $Policy.calibration_seed -or $Calibration.holdout_seed -ne $Policy.holdout_seed -or
        !$Calibration.raw_only_frozen -or !$Calibration.holdout_disjoint -or $Calibration.lanes.Count -ne 8){throw 'NR_QA_GHOST_RECEIPT_COHORT'}
    $seen=@{};$offTotal=0
    foreach($lane in $Calibration.lanes) {
        $key="$($lane.rate_index)/$($lane.mode)"
        if($lane.rate_index -notin @(0,1) -or $lane.mode -notin @(0,1,2,3) -or $seen.ContainsKey($key)){throw 'NR_QA_GHOST_RECEIPT_LANES'}
        $seen[$key]=$true
        if($lane.off_current_samples -le 0 -or $lane.off_instance -le 0 -or $lane.off_ingress -le 0 -or $lane.off_tick -le 0){
            throw 'NR_QA_GHOST_OFF_MEASUREMENT_MISSING'
        }
        $offTotal+=$lane.off_current_samples
        if(!$lane.measured -or $lane.raw_samples -ne 256 -or $lane.guide_samples -le 0 -or $lane.guide_points -le 0 -or
            $lane.model_samples -le 0 -or $lane.codec_frames -ne 32 -or $null -eq $lane.codec_code_errors -or
            $lane.codec_code_errors -ne 0){throw 'NR_QA_GHOST_UNMEASURED_LANE'}
        foreach($name in @('format_bound','raw_maximum','epsilon','guide_mismatch','absolute_vector_error','relative_vector_error')) {
            $v=$lane.$name
            if($null -eq $v -or $v -is [string] -or [double]::IsNaN([double]$v) -or [double]::IsInfinity([double]$v) -or $v -lt 0){throw 'NR_QA_GHOST_UNMEASURED_VALUE'}
        }
        if($lane.epsilon -ne [Math]::Max($lane.format_bound,$lane.raw_maximum)){throw 'NR_QA_GHOST_CANDIDATE_EPSILON'}
    }
    if($Calibration.off_current_samples -ne $offTotal){throw 'NR_QA_GHOST_OFF_COUNT_MISMATCH'}
    if($Controls.positive_anchors -ne 8){throw 'NR_QA_GHOST_CONTROL_ANCHOR'}
    foreach($name in @('blend_2percent','blur_1pixel','frozen','extra_delay','dropped','stale_guide','relabeled_stale_guide','reordered_completion','poststamp_only')) {
        $found=@($Controls.controls|Where-Object {$_.name -ceq $name})
        if($found.Count -ne 1 -or $found[0].cases -ne 8 -or $found[0].rejected -isnot [bool] -or
            !$found[0].rejected -or !$found[0].actual_pixels){throw 'NR_QA_GHOST_UNMEASURED_CONTROL'}
    }
}
function Assert-GhostOffProof([object]$Calibration,[object[]]$Rows,[object[]]$Lanes) {
    if($Calibration.off_current_samples -le 0 -or $Rows.Count -ne $Calibration.off_current_samples -or $Lanes.Count -ne 8){
        throw 'NR_QA_GHOST_OFF_PROOF_MISSING'
    }
    $seen=@{}
    foreach($row in $Rows) {
        if($row.schema -ne 2 -or $row.evidence_class -cne 'physical_disabled_filter' -or
            $row.identity_digest_algorithm -cne 'sha256' -or $row.kind -cne 'current-source' -or
            $row.configuration_enabled -isnot [bool] -or $row.configuration_enabled -or
            $row.filter_enabled -isnot [bool] -or $row.filter_enabled -or !$row.current_source_pixels_proved -or
            $row.settings_revision -le 0 -or $row.width -ne 1920 -or $row.height -ne 1080 -or
            $row.current_tick -le 0 -or $row.current_tick -ne $row.output_tick){
            throw 'NR_QA_GHOST_OFF_DISABLED_CURRENT_OUTPUT'
        }
        Assert-GhostSameKey $row.output_key $row.current_key $false
        Assert-GhostSha256 $row.current_hash;Assert-GhostSha256 $row.output_hash
        if($row.current_hash -cne $row.output_hash){throw 'NR_QA_GHOST_OFF_PIXEL_MISMATCH'}
        $id="$($row.current_key.instance)/$($row.current_key.content_epoch)/$($row.current_key.ingress)/$($row.current_tick)"
        if($seen.ContainsKey($id)){throw 'NR_QA_GHOST_OFF_STALE_DUPLICATE'}
        $seen[$id]=$true
        $lane=@($Lanes|Where-Object {$_.instance -eq $row.current_key.instance -and $_.mode -eq $row.mode -and
            $_.rate_num -eq $row.rate_num -and $_.rate_den -eq $row.rate_den})
        if($lane.Count -ne 1){throw 'NR_QA_GHOST_OFF_LANE_BINDING'}
    }
    foreach($lane in $Lanes) {
        $model=@($Calibration.lanes|Where-Object {$_.mode -eq $lane.mode -and $_.rate_index -eq $lane.rate_index})
        $rowsForLane=@($Rows|Where-Object {$_.current_key.instance -eq $lane.instance})
        if($model.Count -ne 1 -or $rowsForLane.Count -ne $model[0].off_current_samples -or
            $rowsForLane.Count -le 0 -or $model[0].off_instance -ne $lane.instance -or
            $model[0].off_tick -ne $rowsForLane[-1].current_tick -or $model[0].off_ingress -ne $rowsForLane[-1].current_key.ingress){
            throw 'NR_QA_GHOST_OFF_WRITER_READER_BINDING'
        }
    }
}
function Assert-GhostControlRows([object[]]$Controls,[object[]]$Packets) {
    foreach($control in $Controls) {
        if($control.schema -ne 2 -or $control.evidence_class -cne 'physical_source_pixel_mutation' -or
            !$control.actual_pixels -or $control.gpu_submission -or $control.rejected -isnot [bool] -or !$control.rejected -or
            $control.seed -ne 2583 -or $control.source_tick -lt 32 -or $control.source_tick -ge 64){throw 'NR_QA_GHOST_PHYSICAL_CONTROL_ROW'}
        $packet=@($Packets|Where-Object {$_.serial -eq $control.serial -and $_.current_key.instance -eq $control.current_key.instance})
        if($packet.Count -ne 1){throw 'NR_QA_GHOST_CONTROL_PACKET_BINDING'}
        Assert-GhostSameKey $control.current_key $packet[0].model_key $true
        Assert-GhostSameKey $control.reference_key $packet[0].reference_key $true
        Assert-GhostSameKey $control.completed_key $packet[0].selected_key $true
        foreach($pair in @(@('current_hash',11),@('prior_hash',12),@('current_guide_hash',5),@('prior_guide_hash',6))) {
            Assert-GhostSha256 $control.($pair[0])
            if($control.($pair[0]) -cne $packet[0].stages[$pair[1]].digest){throw 'NR_QA_GHOST_CONTROL_PIXEL_BINDING'}
        }
    }
}

# ---- Task27 ghost-baseline (appended; everything above is byte-identical to the base row 1b681f49) ----
function Get-GhostBaselineMember([object]$Object,[string]$Name) {
    if($null -eq $Object -or !$Object.PSObject.Properties[$Name]){throw 'NR_QA_GHOST_BASELINE_FIELD_MISSING'}
    return $Object.$Name
}
function Assert-GhostBaselineFlag([object]$Value,[bool]$Expected) {
    if($Value -isnot [bool] -or $Value -ne $Expected){throw 'NR_QA_GHOST_BASELINE_FLAG'}
}
function Read-GhostBaselineSummary([string]$ArtifactRoot) {
    # Best effort for the FAIL receipt: the machine DEFECTS list and flags are kept even when the baseline is invalid.
    $path=Join-Path $ArtifactRoot 'ghost-baseline.json'
    if(!(Test-Path -LiteralPath $path)){return $null}
    try{return [IO.File]::ReadAllText($path)|ConvertFrom-Json}catch{return $null}
}
function Assert-GhostBaselinePublic([string]$Output,[string]$ArtifactRoot,[string]$AcceptedCalibrationSha256,[string]$AcceptedControlsSha256) {
    Assert-GhostSha256 $AcceptedCalibrationSha256;Assert-GhostSha256 $AcceptedControlsSha256
    foreach($marker in @('NR_GHOST_FAILURE','NR_GHOST_RETAINED','NR_GHOST_DIGEST_FAILURE','NR_GHOST_DETACHED_FAILURE')){
        if($Output.Contains($marker)){throw 'NR_QA_GHOST_BASELINE_ACTUAL_FAILURE'}
    }
    $lines=@($Output -split [char]10|ForEach-Object {$_.TrimEnd([char]13)})
    $admitted=@($lines|Where-Object {$_.StartsWith('NR_GHOST_BASELINE_ADMITTED ')})
    $scope=@($lines|Where-Object {$_ -ceq 'NR_GHOST_BASELINE holdout_started=true freeze_written=false rungs=raw,binary,unprotected_nr'})
    $results=@($lines|Where-Object {$_.StartsWith('NR_GHOST_BASELINE_RESULT ')})
    if($admitted.Count -ne 1 -or $scope.Count -ne 1 -or $results.Count -ne 1){throw 'NR_QA_GHOST_BASELINE_MARKERS'}
    $a=[regex]::Match($admitted[0],'^NR_GHOST_BASELINE_ADMITTED holdout_seed=([0-9]+) accepted_calibration_sha256=([0-9a-f]{64}) accepted_controls_sha256=([0-9a-f]{64}) freeze_read_only=true$')
    if(!$a.Success -or $a.Groups[1].Value -cne '2857'){throw 'NR_QA_GHOST_BASELINE_ADMISSION'}
    if($a.Groups[2].Value -cne $AcceptedCalibrationSha256 -or $a.Groups[3].Value -cne $AcceptedControlsSha256){
        throw 'NR_QA_GHOST_BASELINE_ACCEPTED_HASH'
    }
    $r=[regex]::Match($results[0],'^NR_GHOST_BASELINE_RESULT result=(BASELINE_VALID_NR_PASS|BASELINE_VALID_NR_DEFECTS|BASELINE_INVALID) baseline_valid=([01]) raw_pass=([01]) binary_pass=([01]) controls_pass=([01]) unprotected_nr_pass=([01]) defects=([0-9]+) summary_sha256=([0-9a-f]{64}) freeze_files_written=0$')
    if(!$r.Success){throw 'NR_QA_GHOST_BASELINE_RESULT_LINE'}
    foreach($index in 2,3,4,5){if($r.Groups[$index].Value -cne '1'){throw 'NR_QA_GHOST_BASELINE_CONTROL_RUNG_FAILED'}}
    $nrPass=$r.Groups[6].Value -ceq '1'
    $label=$r.Groups[1].Value
    if($label -cne $(if($nrPass){'BASELINE_VALID_NR_PASS'}else{'BASELINE_VALID_NR_DEFECTS'})){throw 'NR_QA_GHOST_BASELINE_LABEL'}
    $summaryPath=Join-Path $ArtifactRoot 'ghost-baseline.json'
    if(!(Test-Path -LiteralPath $summaryPath)){throw 'NR_QA_GHOST_BASELINE_SUMMARY_MISSING'}
    $summarySha=(Get-FileHash -LiteralPath $summaryPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if($summarySha -cne $r.Groups[8].Value){throw 'NR_QA_GHOST_BASELINE_SUMMARY_HASH'}
    $summary=[IO.File]::ReadAllText($summaryPath)|ConvertFrom-Json
    if((Get-GhostBaselineMember $summary 'schema') -ne 1 -or (Get-GhostBaselineMember $summary 'kind') -cne 'GHOST_BASELINE' -or
        (Get-GhostBaselineMember $summary 'evidence_class') -cne 'physical' -or
        (Get-GhostBaselineMember $summary 'identity_digest_algorithm') -cne 'sha256-chunk-root-v1' -or
        (Get-GhostBaselineMember $summary 'holdout_seed') -ne 2857){throw 'NR_QA_GHOST_BASELINE_SCHEMA'}
    Assert-GhostBaselineFlag $summary.test_not_release $true
    Assert-GhostBaselineFlag $summary.calibration_seed_measured_here $false
    Assert-GhostSha256 $summary.host_sha256;Assert-GhostSha256 $summary.host_source_sha256
    if($summary.freeze.read_only -ne $true -or $summary.freeze.files_written -ne 0){throw 'NR_QA_GHOST_BASELINE_FREEZE_WRITTEN'}
    foreach($name in @('calibration.json','holdout.json','masks.json','epsilon.json','thresholds.json')){
        Assert-GhostSha256 (Get-GhostBaselineMember $summary.freeze.policy_hashes $name)
    }
    Assert-GhostSha256 $summary.freeze.manifest_sha256
    if($summary.accepted_calibration.calibration_sha256 -cne $AcceptedCalibrationSha256 -or
        $summary.accepted_calibration.controls_sha256 -cne $AcceptedControlsSha256){throw 'NR_QA_GHOST_BASELINE_ACCEPTED_HASH'}
    if($summary.result -cne $label){throw 'NR_QA_GHOST_BASELINE_LABEL'}
    Assert-GhostBaselineFlag $summary.baseline_valid $true
    Assert-GhostBaselineFlag $summary.raw_pass $true;Assert-GhostBaselineFlag $summary.binary_pass $true
    Assert-GhostBaselineFlag $summary.controls_pass $true;Assert-GhostBaselineFlag $summary.unprotected_nr_pass $nrPass
    if(@($summary.faults).Count -ne 0){throw 'NR_QA_GHOST_BASELINE_FAULT_LABELS'}
    $rungs=@($summary.rungs)
    if($rungs.Count -ne 24){throw 'NR_QA_GHOST_BASELINE_RUNG_COUNT'}
    $seen=@{};$snapshots=@()
    foreach($rung in $rungs) {
        if($rung.rung -cnotin @('raw','binary','unprotected_nr') -or $rung.rate_index -notin @(0,1) -or $rung.mode -notin @(0,1,2,3)){
            throw 'NR_QA_GHOST_BASELINE_RUNG_DOMAIN'
        }
        $key="$($rung.rung):$($rung.rate_index):$($rung.mode)"
        if($seen.ContainsKey($key)){throw 'NR_QA_GHOST_BASELINE_RUNG_DUPLICATE'}
        $seen[$key]=$true
        if($rung.ticks -ne 256 -or $rung.key_anomalies -ne 0 -or [UInt64]$rung.instance -eq 0 -or $rung.pass -isnot [bool]){
            throw 'NR_QA_GHOST_BASELINE_RUNG_CORPUS'
        }
        if($rung.rung -cne 'unprotected_nr' -and !$rung.pass){throw 'NR_QA_GHOST_BASELINE_CONTROL_RUNG_FAILED'}
        if($rung.rung -ceq 'unprotected_nr' -and $nrPass -and !$rung.pass){throw 'NR_QA_GHOST_BASELINE_LABEL'}
        if($rung.rung -ceq 'raw') {
            if($rung.kinds.current_source -ne 256 -or $rung.kinds.priming_raw -ne 0 -or
                $rung.kinds.delayed_raw -ne 0 -or $rung.kinds.delayed_nr -ne 0 -or
                $rung.raw_transport.off_current_samples -ne 256 -or $rung.raw_transport.checks -ne 256 -or
                $rung.raw_transport.failures -ne 0 -or $rung.last_ingress - $rung.first_ingress -ne 255){
                throw 'NR_QA_GHOST_BASELINE_RAW_CURRENT'
            }
        }
        if($rung.codec.required -ne $true -or $rung.codec.frames -ne 32 -or $rung.codec.code_errors -ne 0){
            throw 'NR_QA_GHOST_BASELINE_CODEC'
        }
        $directory=Join-Path $ArtifactRoot ('baseline-'+$(if($rung.rung -ceq 'raw'){'raw'}elseif($rung.rung -ceq 'binary'){'binary'}else{'nr'})+'-rate-'+$rung.rate_index+'-mode-'+$rung.mode)
        foreach($name in @('source.bmp','filter.bmp','program.bmp','program.hevc')) {
            $path=Join-Path $directory $name
            if(!(Test-Path -LiteralPath $path) -or (Get-Item -LiteralPath $path).Length -le 0){throw 'NR_QA_GHOST_BASELINE_PHYSICAL_ARTIFACT'}
            $snapshots+=,@{key="$($rung.rung):rate$($rung.rate_index):mode$($rung.mode):$name";path=$path;
                sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant();bytes=(Get-Item -LiteralPath $path).Length}
        }
    }
    $controls=@($summary.bad_controls)
    if($controls.Count -ne 9 -or $summary.positive_anchors -ne 8){throw 'NR_QA_GHOST_BASELINE_CONTROLS'}
    $names=@{}
    foreach($control in $controls) {
        if($names.ContainsKey([string]$control.name) -or $control.cases -ne 8 -or $control.actual_pixels -ne $true -or $control.rejected -ne $true){
            throw 'NR_QA_GHOST_BASELINE_PLANTED_CONTROL'
        }
        $names[[string]$control.name]=$true
    }
    $inherited=@($summary.inherited_trail_attribution)
    if($inherited.Count -ne 4){throw 'NR_QA_GHOST_BASELINE_INHERITED'}
    $inheritedModes=@{}
    foreach($row in $inherited){
        # PLAN:91: 256 episode ticks, but X_t=.9C_t+.1C_(t-1) has a predecessor only at ticks 1..255.
        # Keep the emitter's actual 255 measurements; never relabel tick 0 as a scored sample.
        if($row.raw_equals_total -ne $true -or $row.attributed_to_plugin -ne $false -or $row.ticks -ne 255 -or
            $row.episode_ticks -ne 256 -or $row.first_scored_tick -ne 1 -or $row.last_scored_tick -ne 255 -or
            $row.templates -le 0 -or $row.mode -notin @(0,1,2,3) -or $inheritedModes.ContainsKey([string]$row.mode)){
            throw 'NR_QA_GHOST_BASELINE_INHERITED'
        }
        $inheritedModes[[string]$row.mode]=$true
    }
    $defects=@($summary.defects)
    if($defects.Count -ne [int]$r.Groups[7].Value){throw 'NR_QA_GHOST_BASELINE_DEFECT_COUNT'}
    if($nrPass -and @($defects|Where-Object {$_.for_task28}).Count -ne 0){throw 'NR_QA_GHOST_BASELINE_DEFECTS_ON_PASS'}
    if(!$nrPass -and $defects.Count -eq 0){throw 'NR_QA_GHOST_BASELINE_DEFECTS_MISSING'}
    foreach($defect in $defects){
        foreach($name in @('id','rung','category','detail')){
            if([string]::IsNullOrEmpty([string](Get-GhostBaselineMember $defect $name))){throw 'NR_QA_GHOST_BASELINE_DEFECT_SCHEMA'}
        }
        if($defect.rung -cne 'unprotected_nr' -or $defect.for_task28 -isnot [bool]){throw 'NR_QA_GHOST_BASELINE_DEFECT_SCHEMA'}
    }
    return @{result=$label;baseline_valid=$true;unprotected_nr_pass=$nrPass;DEFECTS=$defects;defect_count=$defects.Count;
        summary_sha256=$summarySha;rungs=$rungs.Count;bad_controls=$controls.Count;snapshots=$snapshots;physical=$true}
}
