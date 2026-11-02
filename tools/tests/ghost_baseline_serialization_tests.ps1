# SPDX-License-Identifier: GPL-3.0-only
# CPU contract test: real compiled write_summary/attribute_inherited -> public validator.
# Artifact bytes are scaffolding, not native/codec evidence. The summaries are never fabricated here.
param(
    [Parameter(Mandatory)][string]$BaselineExecutable,
    [Parameter(Mandatory)][string]$SupportDirectory,
    [Parameter(Mandatory)][string]$Scratch,
    [string]$ExpectedLegacyRefusal
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
. (Join-Path $SupportDirectory 'GhostDigestSupport.ps1')
$Scratch=Join-Path $Scratch ([Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $Scratch | Out-Null
$emit=Join-Path $Scratch 'emitted'
if(Test-Path -LiteralPath 'C:/Users/USER/orca/projects/obs-nr-native-filter-wt/w2/out/native-session-lock/ACTIVE'){
    throw 'NATIVE_LOCK_ACTIVE'
}
& $BaselineExecutable --emit $emit
if($LASTEXITCODE -ne 0){throw 'TEST_FAIL: compiled serializer executable failed'}
$script:checks=0
function Expect([bool]$Condition,[string]$Why) {
    $script:checks++
    if(!$Condition){throw "TEST_FAIL: $Why"}
}
function Expect-Throw([string]$Code,[scriptblock]$Body) {
    $script:checks++
    try{& $Body}catch{if($_.Exception.Message -ceq $Code){return};throw}
    throw "TEST_FAIL: expected $Code"
}
function Markers($Summary,[string]$Path) {
    $hash=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    $nr=if($Summary.unprotected_nr_pass){1}else{0}
    $cal=$Summary.accepted_calibration.calibration_sha256
    $ctl=$Summary.accepted_calibration.controls_sha256
    return ("NR_GHOST_BASELINE_ADMITTED holdout_seed=2857 accepted_calibration_sha256=$cal accepted_controls_sha256=$ctl freeze_read_only=true`n"+
        "NR_GHOST_BASELINE holdout_started=true freeze_written=false rungs=raw,binary,unprotected_nr`n"+
        "NR_GHOST_BASELINE_RESULT result=$($Summary.result) baseline_valid=1 raw_pass=1 binary_pass=1 controls_pass=1 unprotected_nr_pass=$nr defects=$(@($Summary.defects).Count) summary_sha256=$hash freeze_files_written=0`n")
}
function Validate($Summary,[string]$Directory) {
    $path=Join-Path $Directory 'ghost-baseline.json'
    Assert-GhostBaselinePublic (Markers $Summary $path) $Directory $Summary.accepted_calibration.calibration_sha256 $Summary.accepted_calibration.controls_sha256
}
foreach($variant in 'pass','defects') {
    $source=Join-Path $emit "ghost-baseline-$variant.json"
    $directory=Join-Path $Scratch $variant
    New-Item -ItemType Directory -Path $directory | Out-Null
    $path=Join-Path $directory 'ghost-baseline.json'
    Copy-Item -LiteralPath $source -Destination $path
    Expect ((Get-FileHash $source).Hash -ceq (Get-FileHash $path).Hash) 'production summary bytes changed'
    $summary=[IO.File]::ReadAllText($path)|ConvertFrom-Json
    foreach($rung in $summary.rungs) {
        $name=if($rung.rung -eq 'unprotected_nr'){'nr'}else{$rung.rung}
        $lane=Join-Path $directory "baseline-$name-rate-$($rung.rate_index)-mode-$($rung.mode)"
        New-Item -ItemType Directory -Path $lane | Out-Null
        foreach($file in 'source.bmp','filter.bmp','program.bmp','program.hevc'){
            [IO.File]::WriteAllBytes((Join-Path $lane $file),[byte[]](0x43,0x50,0x55,1))
        }
    }
    if($ExpectedLegacyRefusal) {
        Expect-Throw $ExpectedLegacyRefusal { Validate $summary $directory }
        "LEGACY_SERIALIZER_PUBLIC_RED variant=$variant error=$ExpectedLegacyRefusal ticks=$($summary.inherited_trail_attribution.ticks -join ',')"
        continue
    }
    $result=Validate $summary $directory
    Expect ($result.rungs -eq 24 -and $result.snapshots.Count -eq 96) 'all production rungs and raw artifacts admitted'
    Expect ($result.unprotected_nr_pass -eq ($variant -eq 'pass')) 'production verdict differs from public result'
    Expect (@($summary.inherited_trail_attribution).Count -eq 4) 'production serializer omitted inherited rows'
    Expect (@($summary.inherited_trail_attribution|Where-Object {$_.ticks -ne 255}).Count -eq 0) 'inherited measurements are not 1..255'
    "PRODUCTION_SERIALIZER_PUBLIC_GREEN variant=$variant ticks=$($summary.inherited_trail_attribution.ticks -join ',') snapshots=$($result.snapshots.Count)"
    # Both off-by-one directions are mutations of actual emitted rows, not alternate hand-written fixtures.
    foreach($count in 254,256) {
        $changed=[IO.File]::ReadAllText($source)|ConvertFrom-Json
        $changed.inherited_trail_attribution[0].ticks=$count
        $changed|ConvertTo-Json -Depth 64|Set-Content -LiteralPath $path -Encoding UTF8
        Expect-Throw 'NR_QA_GHOST_BASELINE_INHERITED' { Validate $changed $directory }
        "PRODUCTION_COUNT_RED variant=$variant scored_ticks=$count"
    }
    # NR and raw demonstrably differ in the real serializer. Copying the NR cohort must fail closed.
    $changed=[IO.File]::ReadAllText($source)|ConvertFrom-Json
    $raw=@($changed.rungs|Where-Object {$_.rung -eq 'raw'})[0]
    $nr=@($changed.rungs|Where-Object {$_.rung -eq 'unprotected_nr'})[0]
    Expect ($raw.kinds.current_source -eq 256 -and $nr.kinds.delayed_nr -eq 255 -and
        $raw.temporal.templates -ne $nr.temporal.templates) 'raw/NR key and oracle phases do not differ'
    $raw.kinds=$nr.kinds;$raw.temporal=$nr.temporal
    $changed|ConvertTo-Json -Depth 64|Set-Content -LiteralPath $path -Encoding UTF8
    Expect-Throw 'NR_QA_GHOST_BASELINE_RAW_CURRENT' { Validate $changed $directory }
    Copy-Item -LiteralPath $source -Destination $path
    Remove-Item -LiteralPath (Join-Path $directory 'baseline-raw-rate-0-mode-0/program.hevc')
    Expect-Throw 'NR_QA_GHOST_BASELINE_PHYSICAL_ARTIFACT' { Validate $summary $directory }
}
"GHOST_BASELINE_SERIALIZATION_TESTS_OK checks=$script:checks CPU_only=true physical_measurements=false synthetic_artifacts=true"
