param([Parameter(Mandatory)][string]$Work,
    [string]$GenuineLog='C:/Users/USER/orca/projects/obs-nr-native-filter-wt/w2/.omo/evidence/obs-nr-native-filter-20260930/task-16/recording-window-r33-exit-observer-20261015/native01/recording-host.log')
# UG03P3: Task16 public validator Read-NrRecordingReceipts (tools/host/RecordingParser.ps1) on real receipt text
# (ConvertFrom-Json, as the window passes it). Positive fixture = the sealed wire_parser_tests.ps1 shape; the sealed
# negative controls are replayed through JSON; then the I3 Task16 review blockers B1-B4 (ug07-w1-inputs-40437a2d)
# must each throw. Every admitted bad set is reported, so a RED run lists them all. A genuine recorded host log
# (pinned) supplies the decoded metadata the real decoder reports, which the validator must keep accepting.
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
. (Join-Path $Work 'tools/host/RecordingParser.ps1')
$checks=0
function Fixture([int]$Rows=12) {
    $s=@{Frames=@();Lanes=@();Decoded=@();Cleanup=@()}
    foreach($lane in 0..3) {
        $off=($lane%2)-eq 0;$hdr=$lane -ge 2
        $s.Lanes+=@{lane=$lane;hdr=$hdr;nr_off=$off;packets=8;decoded_frames=8}
        $s.Cleanup+=@{lane=$lane;closed=$true;worker_exit=$true;retained=$false}
        foreach($n in 1..$Rows) {
            $selected=if($off -or $n -eq 1){$n}else{$n-1}
            $challenge=!$off -and $n -eq 5
            $kind=if($off){0}elseif($n -eq 1){1}elseif($challenge){2}else{3}
            $r=@{lane=$lane;hdr=$hdr;nr_off=$off;deterministic=$false;genuine_nr=($kind -eq 3);same_latch_views=$true;
                between_views_completion=$challenge;filter_order_preserved=$true;transport_occurrence=$n;native_producer_present=(!$off);width=1920;height=1080;instance=10+$lane;content_epoch=1;
                settings_revision=1;settings_strength=.5;fps_numerator=1;fps_denominator=1;interval=1000000000;
                tick=101+$n;clock=[long](100+$n)*1000000000;alpha_errors=0;source_alpha='premultiplied';
                source_reference_white_nits=80;output_sdr_white_nits=300;output_peak_nits=1000;
                ingress=$n;selected=$selected;source_id=$selected;filter_id=$selected;program_id=$selected;kind=$kind;
                raw_hash='11';current_raw_hash='11';filter_hash='12';program_hash='13';unrelated_hash='14';preview_hash='15';projector_hash='15';
                submission_serial=$(if($kind -eq 3){$selected}else{0});history_epoch=$(if($kind -eq 3){1}else{0});
                resource_generation=$(if($kind -eq 3){1}else{0});module_base=$(if($kind -eq 3){100}else{0});
                module_path='C:/owned/not-loaded-runtime.dll';same_key_nr_rms=$(if($off){0}else{.01})}
            $r.challenge_job=@{instance=$r.instance;content_epoch=1;ingress=$selected;history_epoch=1;resource_generation=1;submission_serial=$selected;
                ready=$true;eligible=$true;evaluated=$true;composed=$true;submitted=$true;fence_present=$true;invalid=0;module_base=100;
                module_path="C:/owned/not-loaded-runtime.dll";completion_value=7;completed_value=7}
            $phase=if($off){0}elseif($kind -eq 1){1}else{0}
            $r.wire_id=(1 -shl 20) -bor ($selected -shl 4) -bor $phase
            $s.Frames+=@($r)
            if($n -ge 4 -and $n -le 11) {
                $s.Decoded+=@{lane=$lane;source_id=$selected;tick=$r.tick;instance=$r.instance;content_epoch=1;selected=$selected;
                    history_epoch=$r.history_epoch;resource_generation=$r.resource_generation;submission_serial=$r.submission_serial;
                    transport_occurrence=$n;delivery_occurrence=$n-3;render_clock_ns=$r.clock;wire_id=$r.wire_id;raw_wire_id=$r.wire_id;
                    observed_predecessor_clock_ns=$r.clock-1000000000;preencode_timestamp_ns=$r.clock-1000000000;decoder_timestamp_ns=[long]($n-4)*1000000000;packet_pts=$n-4;packet_dts=$n-4;
                    width=1920;height=1080;bit_depth=$(if($hdr){10}else{8});transfer=$(if($hdr){2}else{1});
                    full_range=$false;primaries_matrix_preserved=$true;codes_compared=3110400;code_errors=0;
                    packet_timebase_numerator=1;packet_timebase_denominator=1;decoded_peak_linear_nits=$(if($hdr){300}else{1});
                    same_id_raw_decoded_nr_rms_codes=$(if($off){0}else{1})}
            }
        }
    }
    $s
}
function Output($s) {
    $lines=@()
    foreach($entry in @(@('NR_RECORDING_FRAME ','Frames'),@('NR_RECORDING_LANE ','Lanes'),@('NR_RECORDING_DECODED ','Decoded'),@('NR_RECORDING_CLEANUP ','Cleanup'))) {
        foreach($row in $s[$entry[1]]){$lines+=$entry[0]+($row|ConvertTo-Json -Compress -Depth 8)}
    }
    $lines+='NR_RECORDING_SCOPE real_filter=true NR_off_current_raw=true Feature18=true SDR=true HDR=true recording=true independently_decoded=true quality=false performance=false cleanup=complete'
    ($lines -join [char]10)+[char]10
}
function Refused([scriptblock]$Mutation) {
    $s=Fixture;& $Mutation $s
    try{$null=Read-NrRecordingReceipts (Output $s)}catch{return $true}
    return $false
}
# Positive: the sealed fixture through real receipt text.
$parsed=Read-NrRecordingReceipts (Output (Fixture))
if(@($parsed.Frames).Count -ne 48 -or @($parsed.Decoded).Count -ne 32){throw 'positive fixture not parsed'}
$checks++
# Row budget: 24 rows per lane (the bounded source ID range) must parse; 25 must be refused.
$parsed24=Read-NrRecordingReceipts (Output (Fixture 24))
if(@($parsed24.Frames).Count -ne 96 -or @($parsed24.Decoded).Count -ne 32){throw 'positive 24-row fixture not parsed'}
$checks++
$refused25=$false
try{$null=Read-NrRecordingReceipts (Output (Fixture 25))}catch{$refused25=$_.Exception.Message -match 'NR_RECORDING_SET'}
if(!$refused25){throw '25-row lane admitted (expected NR_RECORDING_SET)'}
$checks++
$admitted=@()
# Sealed negative controls (wire_parser_tests.ps1) through JSON: must still be refused.
$sealed=@(
    {param($s)$s.Frames[16].projector_hash='99'},{param($s)$s.Frames[16].between_views_completion=$false},
    {param($s)$s.Frames[14].genuine_nr=$false},{param($s)$s.Frames[0].kind=3},{param($s)$s.Frames[1].selected--},
    {param($s)$s.Frames[1].unrelated_hash='99'},{param($s)$s.Frames[1].alpha_errors=1},{param($s)$s.Frames[1].deterministic=$true},
    {param($s)$s.Frames[1].settings_strength='0.5'},{param($s)$s.Frames[1].raw_hash='0'},{param($s)$s.Decoded[0].selected++},
    {param($s)$s.Decoded[1].transport_occurrence++},{param($s)$s.Decoded[1].delivery_occurrence++},{param($s)$s.Decoded[1].render_clock_ns++},
    {param($s)$s.Frames[0].native_producer_present=$true},{param($s)$s.Decoded[0].submission_serial++},
    {param($s)$s.Decoded[1].preencode_timestamp_ns++},{param($s)$s.Decoded[1].decoder_timestamp_ns+=2},{param($s)$s.Decoded[1].packet_pts++},
    {param($s)$s.Decoded[1].code_errors=1},{param($s)$s.Decoded[16].transfer=0},{param($s)$s.Cleanup[0].retained=$true},
    {param($s)$s.Lanes=$s.Lanes[0..2]},{param($s)$s.Frames[1].wire_id=$s.Frames[0].wire_id},{param($s)$s.Decoded[0].wire_id=12345},
    {param($s)$s.Decoded[0].raw_wire_id++},{param($s)$s.Decoded[0].wire_id=$s.Decoded[1].wire_id})
for($i=0;$i -lt $sealed.Count;$i++){if(!(Refused $sealed[$i])){$admitted+="sealed-control-$i"};$checks++}
# I3 Task16 blockers. Decoded rows: lane 0 = 0..7, lane 1 = 8..15, lane 2 (HDR off) = 16..23, lane 3 (HDR NR) = 24..31.
# Frames: lane 0 = 0..11, lane 1 = 12..23 (kind 3 rows 13..15,17..23), lane 2 = 24..35, lane 3 = 36..47.
$blockers=[ordered]@{
    'B1-hdr-metadata-empty-arrays'={param($s)foreach($d in $s.Decoded[24..31]){foreach($f in 'transfer','bit_depth','codes_compared','code_errors','decoded_peak_linear_nits'){$d[$f]=@()}}}
    'B1-alpha_errors-empty'={param($s)$s.Frames[1].alpha_errors=@()}
    'B1-raw_wire_id-empty'={param($s)$s.Decoded[0].raw_wire_id=@()}
    'B1-off-same_key_nr_rms-empty'={param($s)$s.Frames[0].same_key_nr_rms=@()}
    'B1-lane-packets-empty'={param($s)$s.Lanes[0].packets=@()}
    'B1-lane-decoded_frames-empty'={param($s)$s.Lanes[1].decoded_frames=@()}
    'B1-settings_revision-empty'={param($s)foreach($f in $s.Frames[0..11]){$f.settings_revision=@()}}
    'B1-decoded-timestamps-empty'={param($s)$s.Decoded[1].packet_pts=@();$s.Decoded[1].packet_dts=@();$s.Decoded[1].preencode_timestamp_ns=@();$s.Decoded[1].decoder_timestamp_ns=@()}
    'B1-code_errors-empty-sdr'={param($s)$s.Decoded[2].code_errors=@()}
    'B1-width-string'={param($s)$s.Frames[3].width='1920'}
    'B2a-frame-lane-4'={param($s)$extra=$s.Frames[0].Clone();$extra.lane=4;$s.Frames+=@($extra)}
    'B2a-decoded-lane-minus1'={param($s)$extra=$s.Decoded[0].Clone();$extra.lane=-1;$s.Decoded+=@($extra)}
    'B2a-frame-lane-string'={param($s)$extra=$s.Frames[0].Clone();$extra.lane='0';$s.Frames+=@($extra)}
    'B2b-off-rms-negative'={param($s)$s.Frames[0].same_key_nr_rms=-1}
    'B2b-off-rms-string'={param($s)$s.Frames[0].same_key_nr_rms='0.001'}
    'B3-sdr-transfer-hlg'={param($s)foreach($d in $s.Decoded[0..7]){$d.transfer=3}}
    'B3-sdr-transfer-default'={param($s)foreach($d in $s.Decoded[8..15]){$d.transfer=0}}
    'B3-sdr-transfer-pq'={param($s)foreach($d in $s.Decoded[0..7]){$d.transfer=2}}
    'B4-nr-on-filter-equals-raw'={param($s)$s.Frames[13].filter_hash=$s.Frames[13].raw_hash}
    'B4-nr-on-rms-inside-sdr-bound'={param($s)$s.Frames[13].same_key_nr_rms=.003}
    'B4-nr-on-rms-inside-hdr-bound'={param($s)$s.Frames[37].same_key_nr_rms=.0005}
}
foreach($name in $blockers.Keys){if(!(Refused $blockers[$name])){$admitted+=$name};$checks++}
# Accepted shapes the fix must keep (no false FAIL): integer-valued reals and [long] counters.
$keep=Fixture;foreach($f in $keep.Frames[12..23]){if($f.kind -eq 3){$f.same_key_nr_rms=1}};$null=Read-NrRecordingReceipts (Output $keep);$checks++
# Genuine decoded metadata (UG03P3 source review ITERATE): the SDR lane-0 rows of a recorded real run carry the values the
# decoder reports (transfer 1, bit_depth 8, measured peak, ...). Replayed into both SDR lanes they must be accepted.
$genuineSha='c4b445d6f1e6a38e588e7107959a10871e9e7710660a87a8febfcffb42c54959'
if((Get-FileHash -LiteralPath $GenuineLog -Algorithm SHA256).Hash.ToLowerInvariant() -cne $genuineSha){throw 'genuine recording log pin'}
$genuine=@(foreach($line in [IO.File]::ReadAllLines($GenuineLog)){$at=$line.IndexOf('NR_RECORDING_DECODED {');if($at -ge 0){$line.Substring($at+21)|ConvertFrom-Json}})
if($genuine.Count -ne 8 -or @($genuine|Where-Object {$_.lane -ne 0}).Count){throw 'genuine SDR lane-0 decoded rows'}
$replay=Fixture
foreach($lane in 0,1){for($i=0;$i -lt 8;$i++){
    $d=$replay.Decoded[$lane*8+$i];$g=$genuine[$i]
    foreach($f in 'width','height','bit_depth','transfer','full_range','primaries_matrix_preserved','codes_compared','code_errors',
        'decoded_peak_linear_nits','packet_timebase_numerator','packet_timebase_denominator'){$d[$f]=$g.$f}
    if($lane -eq 0){$d.same_id_raw_decoded_nr_rms_codes=$g.same_id_raw_decoded_nr_rms_codes}
}}
$null=Read-NrRecordingReceipts (Output $replay);$checks++
if($admitted.Count){throw ('recording parser admitted bad sets: '+($admitted -join ', '))}
Write-Output "PASS recording_parser_validator checks=$checks positive=true sealed_controls=$($sealed.Count) blockers=$($blockers.Count) genuine_sdr_decoded_replayed=true"
