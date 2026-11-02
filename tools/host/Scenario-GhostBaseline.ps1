# SPDX-License-Identifier: GPL-3.0-only
# Task27 ghost-baseline wrapper logic. Dot-sourced by tools/Invoke-NrQa.ps1 only for -Scenario ghost-baseline.
# Read-only with respect to the Task26 freeze: nothing here opens a calibration, controls or freeze file for writing.
function Get-GhostBaselineAcceptedBinding([string]$Work,[string]$CalibrationSha256,[string]$ControlsSha256) {
    # Late-bound final-hash argument: the caller names the accepted Task26 calibration/controls SHA-256 values.
    foreach($value in @($CalibrationSha256,$ControlsSha256)){
        if($value -isnot [string] -or $value -cnotmatch '^[0-9a-f]{64}$'){throw 'NR_QA_GHOST_BASELINE_ACCEPTED_HASH_REQUIRED'}
    }
    $task26=[IO.Path]::GetFullPath((Join-Path $Work '.omo/evidence/obs-nr-native-filter-20260930/task-26')).TrimEnd('\','/')
    $prefix=$task26+[IO.Path]::DirectorySeparatorChar
    $paths=@{}
    foreach($entry in @(@('calibration','calibration.json',$CalibrationSha256),@('controls','bad-controls.json',$ControlsSha256))) {
        $path=[IO.Path]::GetFullPath((Join-Path $task26 $entry[1]))
        if(!$path.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)){throw 'NR_QA_GHOST_BASELINE_ACCEPTED_PATH'}
        if(!(Test-Path -LiteralPath $path -PathType Leaf)){throw 'NR_QA_GHOST_BASELINE_ACCEPTED_MISSING'}
        if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $entry[2]){throw 'NR_QA_GHOST_BASELINE_ACCEPTED_HASH_MISMATCH'}
        $paths[$entry[0]]=$path.Replace('\','/')
    }
    $json=@{calibration_path=$paths.calibration;controls_path=$paths.controls;calibration_sha256=$CalibrationSha256;controls_sha256=$ControlsSha256}|
        ConvertTo-Json -Compress
    if($json.Length -ge 8192){throw 'NR_QA_GHOST_BASELINE_ACCEPTED_OVERSIZED'}
    return @{json=$json;calibration_path=$paths.calibration;controls_path=$paths.controls}
}
function Get-GhostBaselineBindingPaths() {
    # Extra identities bound into the receipt for ghost-baseline (project/ rows map to src/ and tools/host/ in W).
    return @('src/ghost_baseline.h','src/ghost_baseline.cpp','src/ghost_freeze.h','src/ghost_freeze.cpp',
        'tools/host/Scenario-GhostBaseline.ps1','tools/tests/ghost_baseline_tests.cpp','tools/tests/ghost_baseline_runner_tests.ps1',
        'tools/tests/ghost_raw_copy_tests.cpp','tools/tests/ghost_baseline_serialization_tests.ps1')
}
function Get-GhostBaselineScope([bool]$Accepted) {
    return @{class='physical-ghost-baseline';baseline_accepted=$Accepted;holdout_seed=2857;freeze_read_only=$true;
        rungs=@('raw','binary','unprotected_nr');NR=$true;Feature18=$false;recording=$true;HDR=$true;quality=$false;performance=$false;
        task28_input='DEFECTS'}
}
