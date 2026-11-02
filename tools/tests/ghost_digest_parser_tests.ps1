param([Parameter(Mandatory)][string]$Work)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
. (Join-Path $Work 'tools/host/GhostDigestSupport.ps1')
$script:checks=0
function Check([bool]$Value) { $script:checks++;if(!$Value){throw 'ghost digest parser assertion'} }
function Clone-GhostRecord([object]$Value) { return ($Value|ConvertTo-Json -Depth 16 -Compress|ConvertFrom-Json) }
function Reject([scriptblock]$Action) {
    $failed=$false;try{& $Action}catch{$failed=$true};Check $failed
}
$roles=@('canonical','encoded','keep','foreign','resolved','current_analysis','reference_analysis',
    'motion','vectors','cost','completed','current_raw','selected_raw','selected','program','preview','projector')
$current=[pscustomobject]@{instance=41;content_epoch=3;ingress=59}
$job=[pscustomobject]@{instance=41;content_epoch=3;ingress=59;history=100;generation=1;submission=12}
$reference=[pscustomobject]@{instance=41;content_epoch=3;ingress=58;history=100;generation=1;submission=11}
$hash='9dd576b9473e6bbe8291778e45a06ee0687c2a5996f1f68e64c7f0a66fef2307'
$packet=[pscustomobject]@{identity_digest_algorithm='sha256-chunk-root-v1';digest_mechanism='gpu_exact_chunks_cpu_ordered_root';schema=2;serial=1;tick=100;clock=1650000000;interval=16666666;
    preparation=$false;reset=$false;source_linear=$false;hdr_output=$false;model_evaluated=$true;
    model_mask=2047;graphics_mask=63;settings_revision=1;resource_generation=1;current_key=$current;
    model_key=$job;selected_key=$reference;selected_frame=$reference;reference_key=$reference;stages=@();digest_bytes=0}
for($index=0;$index -lt 17;$index++) {
    $format=if($index -le 4){10}elseif($index -in @(5,6)){87}elseif($index -eq 7){34}elseif($index -eq 8){38}elseif($index -eq 9){62}elseif($index -eq 14){87}else{28}
    $pixelBytes=if($format -eq 10){8}elseif($format -eq 62){1}else{4}
    $transfer=if($index -in @(1,3)){'model_proxy'}elseif($index -in @(5,6)){'srgb_analysis'}elseif($index -eq 7){
        'working_pixel_motion'}elseif($index -eq 8){'signed_1_32_native_flow'}elseif($index -eq 9){'native_cost'}else{'srgb'}
    $alpha=if($index -in @(7,8,9)){'none'}elseif($index -in @(5,6)){'opaque'}elseif($index -in @(1,3)){
        'model_proxy'}elseif($index -eq 14){'composited'}else{'source_preserved'}
    $domain=[pscustomobject]@{width=1920;height=1080;row_bytes=1920*$pixelBytes;rows=1080;storage_dxgi=$format;
        transfer=$transfer;primaries='rec709';alpha=$alpha;reference_white_nits=80;oracle_reference_white_nits=80;oracle_color=0;exposure=1}
    $bound=if($index -eq 6 -or $index -gt 11){$reference}else{$current}
    $stage=[pscustomobject]@{role=$roles[$index];present=$true;bytes=$domain.row_bytes*1080;domain=$domain;frame_key=$bound;digest=$hash}
    if($index -lt 11){$stage|Add-Member job_key $job}
    elseif($index -gt 11){$stage|Add-Member job_key $reference}
    $packet.stages+=,$stage;$packet.digest_bytes+=$stage.bytes
}
Assert-GhostStageDigests @($packet);Check $true
foreach($bad in @('fnv1a64','sha256','SHA256-CHUNK-ROOT-V1','sha256-chunk-root-v2','sha512','')) {
    $copy=Clone-GhostRecord $packet;$copy.identity_digest_algorithm=$bad;Reject {Assert-GhostStageDigests @($copy)}
}
$copy=Clone-GhostRecord $packet;$copy.schema=1;Reject {Assert-GhostStageDigests @($copy)}
$copy=Clone-GhostRecord $packet;$copy.digest_mechanism='cng_simd_multi';Reject {Assert-GhostStageDigests @($copy)}
foreach($role in 0..16) {
    $copy=Clone-GhostRecord $packet;$copy.stages[$role].digest='123';Reject {Assert-GhostStageDigests @($copy)}
    $copy=Clone-GhostRecord $packet;$copy.stages[$role].frame_key=Clone-GhostRecord $copy.stages[$role].frame_key
    $copy.stages[$role].frame_key.ingress++;Reject {Assert-GhostStageDigests @($copy)}
    $copy=Clone-GhostRecord $packet;$copy.stages[$role].domain.transfer='wrong';Reject {Assert-GhostStageDigests @($copy)}
    $copy=Clone-GhostRecord $packet;$copy.stages[$role].domain.exposure=2;Reject {Assert-GhostStageDigests @($copy)}
    $copy=Clone-GhostRecord $packet;$copy.stages[$role].bytes--;Reject {Assert-GhostStageDigests @($copy)}
    $copy=Clone-GhostRecord $packet;$copy.stages[$role].present=$false;Reject {Assert-GhostStageDigests @($copy)}
}
$copy=Clone-GhostRecord $packet;$copy.stages[0]=$copy.stages[1];Reject {Assert-GhostStageDigests @($copy)}
$copy=Clone-GhostRecord $packet;$copy.stages=$copy.stages[0..15];Reject {Assert-GhostStageDigests @($copy)}
$copy=Clone-GhostRecord $packet;$copy.stages+=,$copy.stages[0];Reject {Assert-GhostStageDigests @($copy)}
$copy=Clone-GhostRecord $packet;$copy.stages[16].role='unknown';Reject {Assert-GhostStageDigests @($copy)}
$copy=Clone-GhostRecord $packet;$copy.stages[0].digest=$hash.ToUpperInvariant();Reject {Assert-GhostStageDigests @($copy)}
$copy=Clone-GhostRecord $packet;$copy.stages[0].job_key=Clone-GhostRecord $copy.stages[0].job_key;$copy.stages[0].job_key.history++
Reject {Assert-GhostStageDigests @($copy)}
$copy=Clone-GhostRecord $packet;$copy.reference_key=Clone-GhostRecord $copy.reference_key;$copy.reference_key.ingress-=2
Reject {Assert-GhostStageDigests @($copy)}
Reject {Assert-GhostStageDigests @($packet,$packet)}
$reset=Clone-GhostRecord $packet;$reset.reset=$true;$reset.model_mask=1279
$reset.PSObject.Properties.Remove('reference_key')
$reset.stages[6].frame_key=$current;$reset.stages[6].job_key=$job
foreach($role in @(8,9)) {
    $reset.digest_bytes-=$reset.stages[$role].bytes
    $reset.stages[$role].present=$false;$reset.stages[$role].bytes=0;$reset.stages[$role].PSObject.Properties.Remove('digest')
}
Assert-GhostStageDigests @($reset);Check $true
$copy=Clone-GhostRecord $reset;$copy.stages[8]|Add-Member digest $hash;Reject {Assert-GhostStageDigests @($copy)}
$copy=Clone-GhostRecord $reset;$copy.stages[9].present=$true;Reject {Assert-GhostStageDigests @($copy)}
$warm=Clone-GhostRecord $reset;$warm.preparation=$true;$warm.graphics_mask=0
foreach($role in 11..16) {
    $warm.digest_bytes-=$warm.stages[$role].bytes;$warm.stages[$role].present=$false;$warm.stages[$role].bytes=0
    $warm.stages[$role].PSObject.Properties.Remove('digest')
}
Assert-GhostStageDigests @($warm);Check $true
# A complete-looking digest packet cannot green the physical runner without
# the native256/cadence/quality/codec/control/artifact proof.
Reject {Assert-GhostPublic ('NR_GHOST_STAGE_DIGEST '+($packet|ConvertTo-Json -Depth 16 -Compress)) $Work $Work}
$codec=@();$frames=@()
for($index=0;$index -lt 32;$index++) {
    $frameKey=[pscustomobject]@{instance=41;content_epoch=3;ingress=80+$index}
    $sourceClock=5000000000+[Int64]$index*16666666
    $frames+=,[pscustomobject]@{clock=$sourceClock;selected_frame=$frameKey;settings_revision=1;resource_generation=1}
    foreach($role in @('preencode','decoded')) {
        $codec+=,[pscustomobject]@{identity_digest_algorithm='sha256-chunk-root-v1';schema=2;digest_mechanism='gpu_exact_chunks_cpu_ordered_root';role=$role;index=$index;digest=$hash;
            bytes=3110400;timestamp=$sourceClock;source_clock=$sourceClock;tick=400+$index;
            width=1920;height=1080;format=2;format_tag='nv12';transfer=0;transfer_tag='sdr';full_range=$false;primaries='rec709';
            settings_revision=1;resource_generation=1;frame_key=$frameKey;planes=@(
                [pscustomobject]@{index=0;row_bytes=1920;rows=1080;bytes=2073600},
                [pscustomobject]@{index=1;row_bytes=1920;rows=540;bytes=1036800},
                [pscustomobject]@{index=2;row_bytes=0;rows=0;bytes=0})}
    }
}
Assert-GhostCodecDigests $codec $frames;Check $true
$copy=Clone-GhostRecord $codec;$copy[2].index=0;Reject {Assert-GhostCodecDigests $copy $frames}
$copy=Clone-GhostRecord $codec;$copy[0].frame_key=Clone-GhostRecord $copy[0].frame_key;$copy[0].frame_key.ingress++
Reject {Assert-GhostCodecDigests $copy $frames}
$copy=Clone-GhostRecord $codec;$copy[0].planes[0].bytes--;Reject {Assert-GhostCodecDigests $copy $frames}
$copy=Clone-GhostRecord $codec;$copy[0].transfer_tag='pq';Reject {Assert-GhostCodecDigests $copy $frames}
$copy=Clone-GhostRecord $codec;$copy[0].planes[1]=$copy[0].planes[0];Reject {Assert-GhostCodecDigests $copy $frames}
$copy=Clone-GhostRecord $codec;$copy[0].identity_digest_algorithm='fnv1a64';Reject {Assert-GhostCodecDigests $copy $frames}
$copy=Clone-GhostRecord $codec;$copy[0].identity_digest_algorithm='sha256';Reject {Assert-GhostCodecDigests $copy $frames}
$copy=Clone-GhostRecord $codec;$copy[0].schema=1;Reject {Assert-GhostCodecDigests $copy $frames}
$copy=Clone-GhostRecord $codec;$copy[0].digest_mechanism='cng_simd_multi';Reject {Assert-GhostCodecDigests $copy $frames}
Write-Output "PASS ghost_digest_parser checks=$script:checks physical_acceptance=false"
