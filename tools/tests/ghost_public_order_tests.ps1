param([Parameter(Mandatory)][string]$Work)
# UG03P3: Task26 public validator (tools/host/GhostDigestSupport.ps1) against the I3 Task26 review blockers
# B1 (lock values bound to the captured pass-0 rows; holdout lanes judged by the lock), B2 (single freeze marker
# between the 8 calibration lanes and the first holdout lane; rows inside their lane's segment) and B3 (closed
# receipt schema, typed booleans), following the architect decision recorded in ug03p3 (ALLOWED_WITH_CONDITIONS).
# Part A drives the real Assert-GhostPublic with a compact corpus whose every lane row carries closed=false: a
# validator that accepts the ordering/lock evidence reaches its existing lane-domain check (NR_QA_GHOST_LANE_DOMAIN);
# one that refuses it stops earlier with the expected code. Part B drives Assert-GhostPhysicalMeasurements directly.
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
. (Join-Path $Work 'tools/host/GhostDigestSupport.ps1')
$checks=0;$wrong=@()
$hash='9dd576b9473e6bbe8291778e45a06ee0687c2a5996f1f68e64c7f0a66fef2307'
$freeze='NR_GHOST_FREEZE raw_only=true actual_sdk=true both_rational_rates=true holdout_not_started=true'
function J($o){$o|ConvertTo-Json -Compress -Depth 20}
function Packet([int64]$Instance) {
    $roles=@('canonical','encoded','keep','foreign','resolved','current_analysis','reference_analysis',
        'motion','vectors','cost','completed','current_raw','selected_raw','selected','program','preview','projector')
    $current=[ordered]@{instance=$Instance;content_epoch=3;ingress=59}
    $job=[ordered]@{instance=$Instance;content_epoch=3;ingress=59;history=100;generation=1;submission=12}
    $reference=[ordered]@{instance=$Instance;content_epoch=3;ingress=58;history=100;generation=1;submission=11}
    $packet=[ordered]@{identity_digest_algorithm='sha256-chunk-root-v1';digest_mechanism='gpu_exact_chunks_cpu_ordered_root';schema=2;serial=1;tick=100;
        clock=1650000000;interval=16666666;preparation=$false;reset=$false;source_linear=$false;hdr_output=$false;model_evaluated=$true;
        model_mask=2047;graphics_mask=63;settings_revision=1;resource_generation=1;current_key=$current;model_key=$job;selected_key=$reference;
        selected_frame=$reference;reference_key=$reference;stages=@();digest_bytes=0}
    for($index=0;$index -lt 17;$index++) {
        $format=if($index -le 4){10}elseif($index -in @(5,6)){87}elseif($index -eq 7){34}elseif($index -eq 8){38}elseif($index -eq 9){62}elseif($index -eq 14){87}else{28}
        $pixelBytes=if($format -eq 10){8}elseif($format -eq 62){1}else{4}
        $transfer=if($index -in @(1,3)){'model_proxy'}elseif($index -in @(5,6)){'srgb_analysis'}elseif($index -eq 7){
            'working_pixel_motion'}elseif($index -eq 8){'signed_1_32_native_flow'}elseif($index -eq 9){'native_cost'}else{'srgb'}
        $alpha=if($index -in @(7,8,9)){'none'}elseif($index -in @(5,6)){'opaque'}elseif($index -in @(1,3)){'model_proxy'}elseif($index -eq 14){'composited'}else{'source_preserved'}
        $domain=[ordered]@{width=1920;height=1080;row_bytes=1920*$pixelBytes;rows=1080;storage_dxgi=$format;transfer=$transfer;primaries='rec709';
            alpha=$alpha;reference_white_nits=80;oracle_reference_white_nits=80;oracle_color=0;exposure=1}
        $bound=if($index -eq 6 -or $index -gt 11){$reference}else{$current}
        $stage=[ordered]@{role=$roles[$index];present=$true;bytes=$domain.row_bytes*1080;domain=$domain;frame_key=$bound;digest=$hash}
        if($index -lt 11){$stage.job_key=$job}elseif($index -gt 11){$stage.job_key=$reference}
        $packet.stages+=,$stage;$packet.digest_bytes+=$stage.bytes
    }
    $packet
}
function Lane([int]$k) {
    $pass=[Math]::Floor($k/8);$rate=[Math]::Floor(($k%8)/4);$mode=$k%4
    $lock=Lock (($rate*4)+$mode)
    [ordered]@{pass=$pass;mode=$mode;instance=41+$k;rate_index=$rate;rate_num=$(if($rate -eq 0){60}else{60000});rate_den=$(if($rate -eq 0){1}else{1001});
        source_linear=($mode -ge 2);hdr_output=($mode -ge 2);deterministic=($pass -eq 1);calibration=($pass -eq 0);
        seed=$(if($pass -eq 0){2583}else{2857});raw_samples=256;model_samples=$lock.model_samples;selected_edit_samples=4;
        static_positive_samples=2;moving_positive_samples=2;declared_format_bound=$lock.format_bound;raw_maximum=$lock.raw_maximum;
        epsilon=$lock.epsilon;guide_mismatch=$lock.guide_mismatch;absolute_vector_error=$lock.absolute_vector_error;
        relative_vector_error=$lock.relative_vector_error;static_edit_rms=.5;moving_edit_rms=.5;static_retention=.9;moving_retention=.9;
        closed=$false;worker_exit=$true;retained=$false}
}
function Lock([int]$i) {
    $fb=0.00390625+$i/1024.0;$rm=0.0012+$i/4096.0
    [ordered]@{rate_index=[Math]::Floor($i/4);mode=$i%4;measured=$true;raw_samples=256;guide_samples=40;off_current_samples=2;
        off_instance=41+$i;off_ingress=7;off_tick=9;guide_points=900;model_samples=200+$i;codec_frames=32;codec_code_errors=0;
        format_bound=$fb;raw_maximum=$rm;epsilon=[Math]::Max($fb,$rm);guide_mismatch=0.125+$i/64.0;absolute_vector_error=0.25+$i/32.0;
        relative_vector_error=0.0625+$i/128.0}
}
function Calibration {
    [ordered]@{identity_digest_algorithm='sha256-chunk-root-v1';schema=2;evidence_class='physical';calibration_seed=2583;holdout_seed=2857;
        holdout_disjoint=$true;raw_only_frozen=$true;measurement_status='measured';qualification_allowed=$false;all_prior_templates=$true;
        image_decoded_source_identity=$true;actual_loaded_runtime=$true;normal_cleanup=$true;off_current_samples=16;lanes=@(0..7|ForEach-Object {Lock $_})}
}
# Rows of lane k: one stage packet (lane 0 only, it must be valid), 256 frames, 32 decoded frames, one cleanup;
# calibration lanes also carry the nine planted-control rows (calibration seed) and their two OFF rows
# (off_current_samples in Lock). Lane row last, freeze marker right after lane 7 (producer order, nr_ghost_scenario.cpp).
$controlNames=@('blend_2percent','blur_1pixel','frozen','extra_delay','dropped','stale_guide','relabeled_stale_guide','reordered_completion','poststamp_only')
function Corpus([hashtable]$Change=@{}) {
    $order=if($Change.ContainsKey('order')){$Change.order}else{0..23}
    $lines=New-Object System.Collections.Generic.List[string]
    foreach($k in $order) {
        $lane=Lane $k
        if($Change.ContainsKey('lane')){& $Change.lane $k $lane}
        if($k -eq 0){$lines.Add('NR_GHOST_STAGE_DIGEST '+(J (Packet 41)))}
        for($f=0;$f -lt 256;$f++){$lines.Add('NR_GHOST_FRAME {"selected_frame":{"instance":'+(41+$k)+',"content_epoch":1,"ingress":'+($f+1)+'}}')}
        if($k -lt 8){
            foreach($name in $controlNames){$lines.Add('NR_GHOST_PHYSICAL_CONTROL {"name":"'+$name+'","seed":2583,"current_key":{"instance":'+(41+$k)+',"content_epoch":1,"ingress":40}}')}
            for($o=0;$o -lt 2;$o++){$lines.Add('NR_GHOST_OFF {"current_key":{"instance":'+(41+$k)+',"content_epoch":1,"ingress":'+(7+$o)+'}}')}
        }
        for($d=0;$d -lt 32;$d++){$lines.Add('NR_GHOST_DECODED {"index":'+$d+'}')}
        $lines.Add('NR_GHOST_CLEANUP {"closed":true,"worker_exit":true,"consumer_event":true,"retained":false}')
        $lines.Add('NR_GHOST_LANE '+(J $lane))
        if($k -eq 7 -and !$Change.ContainsKey('noFreeze')){$lines.Add($freeze)}
    }
    if($Change.ContainsKey('extra')){& $Change.extra $lines}
    ($lines -join [char]10)+[char]10
}
$root=Join-Path ([IO.Path]::GetTempPath()) ('nr-ghost-order-'+$PID+'-'+[Environment]::TickCount)
[void][IO.Directory]::CreateDirectory($root)
function Run([string]$Output,[object]$Calibration=$(Calibration)) {
    [IO.File]::WriteAllText((Join-Path $root 'calibration.json'),(J $Calibration))
    [IO.File]::WriteAllText((Join-Path $root 'bad-controls.json'),'{"schema":2}')
    $policy=@{calibration_seed=2583;holdout_seed=2857}
    try{$null=Assert-GhostPublic $Output $root $root $policy ('a'*64) ('b'*64);return 'ACCEPT'}catch{return $_.Exception.Message}
}
function LaneLine($l,[int]$k){$seen=-1;for($i=0;$i -lt $l.Count;$i++){if($l[$i].StartsWith('NR_GHOST_LANE ')){$seen++;if($seen -eq $k){return $i}}};throw 'no lane line'}
function Expect([string]$Name,[string]$Actual,[string]$Want){$script:checks++;if($Actual -cne $Want){$script:wrong+="${Name}: got $Actual want $Want"}}
try {
    Expect 'positive-reaches-lane-domain' (Run (Corpus)) 'NR_QA_GHOST_LANE_DOMAIN'
    Expect 'positive-crlf-reaches-lane-domain' (Run ((Corpus) -replace "`n","`r`n")) 'NR_QA_GHOST_LANE_DOMAIN'
    # B2: freeze marker position/uniqueness and lane order.
    Expect 'B2-freeze-last' (Run (Corpus @{noFreeze=$true;extra={param($l)$l.Add($freeze)}})) 'NR_QA_GHOST_FREEZE_ORDER'
    Expect 'B2-freeze-twice' (Run (Corpus @{extra={param($l)$l.Add($freeze)}})) 'NR_QA_GHOST_FREEZE_ORDER'
    Expect 'B2-freeze-early' (Run (Corpus @{noFreeze=$true;extra={param($l)$l.Insert(0,$freeze)}})) 'NR_QA_GHOST_FREEZE_ORDER'
    Expect 'B2-holdout-lanes-first' (Run (Corpus @{order=@(8..23)+@(0..7)})) 'NR_QA_GHOST_LANE_ORDER'
    Expect 'B2-lane-out-of-order' (Run (Corpus @{order=@(1,0)+@(2..23)})) 'NR_QA_GHOST_LANE_ORDER'
    $moved='NR_GHOST_FRAME {"selected_frame":{"instance":49,"content_epoch":1,"ingress":1}}'
    Expect 'B2-holdout-frame-before-freeze' (Run (Corpus @{extra={param($l)$null=$l.Remove($moved);$l.Insert($l.IndexOf($freeze),$moved)}})) 'NR_QA_GHOST_ROW_SEGMENT'
    Expect 'B2-control-after-freeze' (Run (Corpus @{extra={param($l)$l.Add('NR_GHOST_PHYSICAL_CONTROL {"current_key":{"instance":41,"content_epoch":1,"ingress":40}}')}})) 'NR_QA_GHOST_ROW_SEGMENT'
    $cleanupLine='NR_GHOST_CLEANUP {"closed":true,"worker_exit":true,"consumer_event":true,"retained":false}'
    Expect 'B2-row-after-last-lane' (Run (Corpus @{extra={param($l)$null=$l.Remove($cleanupLine);$l.Add($cleanupLine)}})) 'NR_QA_GHOST_ROW_SEGMENT'
    Expect 'B2-unparsed-ghost-row-before-freeze' (Run (Corpus @{extra={param($l)$l.Insert($l.IndexOf($freeze),'NR_GHOST_SERVICE {"credit_released":true}')}})) 'NR_QA_GHOST_FREEZE_ORDER'
    Expect 'B2-foreign-instance-row' (Run (Corpus @{extra={param($l)$l.Insert((LaneLine $l 2),'NR_GHOST_GUIDE {"instance":999}')}})) 'NR_QA_GHOST_ROW_SEGMENT'
    Expect 'B2-guide-in-other-lane-segment' (Run (Corpus @{extra={param($l)$l.Insert((LaneLine $l 2),'NR_GHOST_GUIDE {"instance":41}')}})) 'NR_QA_GHOST_ROW_SEGMENT'
    $decodedLine='NR_GHOST_DECODED {"index":0}'
    Expect 'B2-decoded-moved-between-lanes' (Run (Corpus @{extra={param($l)$null=$l.Remove($decodedLine);$l.Insert((LaneLine $l 1),$decodedLine)}})) 'NR_QA_GHOST_ROW_SEGMENT'
    Expect 'B2-cleanup-moved-between-lanes' (Run (Corpus @{extra={param($l)$null=$l.Remove($cleanupLine);$l.Insert((LaneLine $l 1),$cleanupLine)}})) 'NR_QA_GHOST_ROW_SEGMENT'
    $offCount=Calibration;$offCount.lanes[1].off_current_samples=3;$offCount.lanes[2].off_current_samples=1
    Expect 'B2-off-count-not-lane-own' (Run (Corpus) $offCount) 'NR_QA_GHOST_ROW_SEGMENT'
    $holdoutControl='NR_GHOST_PHYSICAL_CONTROL {"name":"frozen","seed":2583,"current_key":{"instance":50,"content_epoch":1,"ingress":40}}'
    Expect 'B2-control-in-holdout-segment' (Run (Corpus @{extra={param($l)$l.Insert((LaneLine $l 9),$holdoutControl)}})) 'NR_QA_GHOST_ROW_SEGMENT'
    $wrongSeed='NR_GHOST_PHYSICAL_CONTROL {"name":"frozen","seed":2583,"current_key":{"instance":43,"content_epoch":1,"ingress":40}}'
    Expect 'B2-control-holdout-seed' (Run (Corpus @{extra={param($l)$at=$l.IndexOf($wrongSeed);$l[$at]=$wrongSeed.Replace('"seed":2583','"seed":2857')}})) 'NR_QA_GHOST_CONTROL_SEED'
    Expect 'B2-lane-mode-array' (Run (Corpus @{lane={param($k,$x)if($k -eq 3){$x.mode=@(3)}}})) 'NR_QA_GHOST_LANE_ORDER'
    Expect 'B2-lane-instance-string' (Run (Corpus @{lane={param($k,$x)if($k -eq 10){$x.instance=[string](41+$k)}}})) 'NR_QA_GHOST_LANE_ORDER'
    Expect 'B2-holdout-seed-wrong' (Run (Corpus @{lane={param($k,$x)if($k -eq 12){$x.seed=2583}}})) 'NR_QA_GHOST_LANE_ORDER'
    # B1: lock values bound to the pass-0 capture; holdout lanes judged by the lock.
    $wide=Calibration;$wide.lanes[0].format_bound=0.5;$wide.lanes[0].raw_maximum=0.5;$wide.lanes[0].epsilon=0.5
    $rawArray=Calibration;$rawArray.lanes[0].raw_samples=@(256)
    Expect 'B1-lock-raw-samples-array' (Run (Corpus) $rawArray) 'NR_QA_GHOST_LOCK_BINDING'
    $offArray=Calibration;$offArray.lanes[4].off_current_samples=@(2)
    Expect 'B2-off-count-array' (Run (Corpus) $offArray) 'NR_QA_GHOST_ROW_SEGMENT'
    Expect 'B1-widened-lock' (Run (Corpus) $wide) 'NR_QA_GHOST_LOCK_BINDING'
    $samples=Calibration;$samples.lanes[3].model_samples=999
    Expect 'B1-lock-model-samples' (Run (Corpus) $samples) 'NR_QA_GHOST_LOCK_BINDING'
    $swapped=Calibration;$tmp=$swapped.lanes[1];$swapped.lanes[1]=$swapped.lanes[2];$swapped.lanes[2]=$tmp
    Expect 'B1-lock-lane-order' (Run (Corpus) $swapped) 'NR_QA_GHOST_LOCK_BINDING'
    Expect 'B1-holdout-zero-bounds' (Run (Corpus @{lane={param($k,$x)if($k -ge 16){$x.declared_format_bound=0;$x.raw_maximum=0;$x.epsilon=0}}})) 'NR_QA_GHOST_HOLDOUT_LOCK'
    Expect 'B1-holdout-guide-widened' (Run (Corpus @{lane={param($k,$x)if($k -eq 9){$x.guide_mismatch=9}}})) 'NR_QA_GHOST_HOLDOUT_LOCK'
    Expect 'B1-capture-differs-from-lock' (Run (Corpus @{lane={param($k,$x)if($k -eq 5){$x.raw_maximum=0.75;$x.epsilon=0.75}}})) 'NR_QA_GHOST_LOCK_BINDING'
} finally { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
# Part B: Assert-GhostPhysicalMeasurements receipt schema (B3).
$policyHashes=[ordered]@{'calibration.json'='1'*64;'holdout.json'='2'*64;'masks.json'='3'*64;'epsilon.json'='4'*64;'thresholds.json'='5'*64}
$policy=@{hash0='1'*64;hash1='2'*64;hash2='3'*64;hash3='4'*64;hash4='5'*64;manifest_hash='6'*64;calibration_seed=2583;holdout_seed=2857}
$names=@('blend_2percent','blur_1pixel','frozen','extra_delay','dropped','stale_guide','relabeled_stale_guide','reordered_completion','poststamp_only')
function Receipts {
    $cal=Calibration;$cal.host_sha256='a'*64;$cal.host_source_sha256='b'*64;$cal.policy_hashes=$policyHashes;$cal.manifest_sha256='6'*64
    $ctl=[ordered]@{schema=2;evidence_class='physical';identity_digest_algorithm='sha256-chunk-root-v1';measurement_status='measured';
        host_sha256='a'*64;host_source_sha256='b'*64;policy_hashes=$policyHashes;manifest_sha256='6'*64;positive_anchors=8;
        physical_pixel_mutations_not_gpu_submissions=$true;qualification_allowed=$true;controls=@($names|ForEach-Object {[ordered]@{name=$_;cases=8;actual_pixels=$true;rejected=$true}})}
    @((J $cal|ConvertFrom-Json),(J $ctl|ConvertFrom-Json))
}
function Test-Receipts([scriptblock]$Mutation) {
    $r=Receipts;& $Mutation $r[0] $r[1]
    try{Assert-GhostPhysicalMeasurements $r[0] $r[1] $policy;return 'ACCEPT'}catch{return $_.Exception.Message}
}
Expect 'B3-positive-receipts' (Test-Receipts {param($c,$t)}) 'ACCEPT'
Expect 'B3-extra-passing-control' (Test-Receipts {param($c,$t)$t.controls+=,[pscustomobject]@{name='blend_2percent_retry';cases=8;actual_pixels=$true;rejected=$false}}) 'NR_QA_GHOST_RECEIPT_SCHEMA'
Expect 'B3-controls-reordered' (Test-Receipts {param($c,$t)$t.controls=@($t.controls[1],$t.controls[0])+@($t.controls[2..8])}) 'NR_QA_GHOST_RECEIPT_SCHEMA'
Expect 'B3-holdout_disjoint-string' (Test-Receipts {param($c,$t)$c.holdout_disjoint='false'}) 'NR_QA_GHOST_RECEIPT_SCHEMA'
Expect 'B3-raw_only_frozen-string' (Test-Receipts {param($c,$t)$c.raw_only_frozen='true'}) 'NR_QA_GHOST_RECEIPT_SCHEMA'
Expect 'B3-normal_cleanup-number' (Test-Receipts {param($c,$t)$c.normal_cleanup=1}) 'NR_QA_GHOST_RECEIPT_SCHEMA'
Expect 'B3-lane-measured-string' (Test-Receipts {param($c,$t)$c.lanes[2].measured='true'}) 'NR_QA_GHOST_RECEIPT_SCHEMA'
Expect 'B3-actual_pixels-string' (Test-Receipts {param($c,$t)$t.controls[4].actual_pixels='true'}) 'NR_QA_GHOST_RECEIPT_SCHEMA'
Expect 'B3-calibration-qualification-true' (Test-Receipts {param($c,$t)$c.qualification_allowed=$true}) 'NR_QA_GHOST_RECEIPT_SCHEMA'
Expect 'B3-control-name-array' (Test-Receipts {param($c,$t)$t.controls[0].name=@('blend_2percent')}) 'NR_QA_GHOST_RECEIPT_SCHEMA'
Expect 'B3-controls-qualification-false' (Test-Receipts {param($c,$t)$t.qualification_allowed=$false}) 'NR_QA_GHOST_RECEIPT_SCHEMA'
if($wrong.Count){throw ('ghost public validator: '+($wrong -join '; '))}
Write-Output "PASS ghost_public_order checks=$checks lock_before_holdout=true lock_bound_to_capture=true receipt_schema_closed=true"
