# SPDX-License-Identifier: GPL-3.0-only
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ModifiedRoot,
    [Parameter(Mandatory = $true)][string]$BaselineRoot,
    [Parameter(Mandatory = $true)][string]$PushFlowRuntimeDir,
    [Parameter(Mandatory = $true)][string]$LogDir,
    [string]$LibObsDir = 'C:\Program Files\obs-studio\bin\64bit',
    [string]$Config = 'Release'
)
$ErrorActionPreference = 'Stop'

$ModifiedRoot = (Resolve-Path -LiteralPath $ModifiedRoot).Path
$BaselineRoot = (Resolve-Path -LiteralPath $BaselineRoot).Path
$PushFlowRuntimeDir = (Resolve-Path -LiteralPath $PushFlowRuntimeDir).Path
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$LogDir = (Resolve-Path -LiteralPath $LogDir).Path

$modBin = Join-Path $ModifiedRoot "out\build\windows-x64-release\$Config"
$baseBin = Join-Path $BaselineRoot "out\build\windows-x64-release\$Config"
$modPresenter = Join-Path $modBin 'obs-nr-fg-presenter.exe'
$basePresenter = Join-Path $baseBin 'obs-nr-fg-presenter.exe'
$modSmoke = Join-Path $modBin 'vfg_native_smoke_tests.exe'
$baseSmoke = Join-Path $baseBin 'vfg_native_smoke_tests.exe'
$abRunner = Join-Path $modBin 'vfg_ab_smoke_runner.exe'
$hudGpu = Join-Path $modBin 'vfg_hud_lock_gpu_tests.exe'
$anrTests = Join-Path $modBin 'anr_mock_dll_tests.exe'
$anrMockDll = Join-Path $modBin 'anr_mock_dll.dll'
$guidesShader = Join-Path $ModifiedRoot 'out\build\windows-x64-release\nr_guides.cso'
$filterTests = Join-Path $modBin 'vfg_filter_output_tests.exe'
$pluginDll = Join-Path $modBin 'obs-nr-filter.dll'
$pluginData = Join-Path $ModifiedRoot 'data'

foreach ($p in @($modPresenter, $basePresenter, $modSmoke, $baseSmoke, $abRunner, $hudGpu, $anrTests, $anrMockDll, $guidesShader, $filterTests, $pluginDll)) {
    if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { throw "Missing required binary: $p" }
}

function Invoke-Checked([string]$Label, [scriptblock]$Body) {
    Write-Host "=== [$Label] ==="
    & $Body
    if ($LASTEXITCODE -ne 0) { throw "Step '$Label' failed with exit code $LASTEXITCODE" }
}

function Get-Quantile([double[]]$Sorted, [double]$Q) {
    if (-not $Sorted.Count) { throw 'Empty quantile input' }
    $idx = [int][Math]::Ceiling($Q * $Sorted.Count) - 1
    if ($idx -lt 0) { $idx = 0 }
    if ($idx -ge $Sorted.Count) { $idx = $Sorted.Count - 1 }
    return $Sorted[$idx]
}

function Parse-ParityLog([string]$Path, [int]$Mult) {
    $lines = [IO.File]::ReadAllLines($Path)
    $init = @($lines | Where-Object { $_ -match '^event=initialized\b' })
    if (-not $init.Count -or $init[0] -notmatch 'backend=nvidia-vfg-native') {
        throw "Parity log $Path missing backend=nvidia-vfg-native"
    }
    if (@($lines | Where-Object { $_ -match '^event=error\b' }).Count -gt 0) {
        throw "Parity log $Path recorded event=error"
    }
    $latencies = [System.Collections.Generic.List[double]]::new()
    $origCount = 0
    $subCount = 0
    $dropCount = 0
    foreach ($line in $lines) {
        if ($line -notmatch '^event=(\S+)\s+.*\bb=(\d+)\b') { continue }
        $ev = $Matches[1]
        $serial = [int]$Matches[2]
        if ($serial -lt 17 -or $serial -gt 136) { continue }
        if ($ev -eq 'present_original') {
            $origCount++
            if ($line -match '\bb_arrival_ns=(\d+)\b') { $arr = [double]$Matches[1] } else { throw "Missing b_arrival_ns" }
            if ($line -match '\bqpc_ns=(\d+)\b') { $qpc = [double]$Matches[1] } else { throw "Missing qpc_ns" }
            $latencies.Add(($qpc - $arr) / 1e6)
        } elseif ($ev -eq 'present_vfg_subframe') {
            $subCount++
        } elseif ($ev -in @('drop_vfg_subframe_late', 'drop_vfg_pair_late', 'drop_vfg_present_busy', 'drop_original_late')) {
            $dropCount++
        }
    }
    if ($latencies.Count -eq 0) { throw "No measured present_original events in $Path" }
    $sorted = @($latencies.ToArray() | Sort-Object)
    $origRate = $origCount / 120.0
    $subRate = $subCount / (($Mult - 1) * [Math]::Max(1, $origCount - 1))
    return [pscustomobject]@{
        P50Ms        = Get-Quantile $sorted 0.50
        P95Ms        = Get-Quantile $sorted 0.95
        OrigCount    = $origCount
        SubCount     = $subCount
        DropCount    = $dropCount
        OrigRate     = $origRate
        SubRate      = $subRate
    }
}

# (a) Baseline ad4d8963 vs modified steady-state latency and presentation/drop parity (every individual trial + median summary)
$trials = 3
$baseSamples = @{ 2 = [System.Collections.Generic.List[object]]::new(); 4 = [System.Collections.Generic.List[object]]::new() }
$modSamples = @{ 2 = [System.Collections.Generic.List[object]]::new(); 4 = [System.Collections.Generic.List[object]]::new() }
for ($t = 1; $t -le $trials; $t++) {
    $baseParityDir = Join-Path $LogDir "parity-baseline-t$t"
    $modParityDir = Join-Path $LogDir "parity-modified-t$t"
    Invoke-Checked "parity-baseline-t$t" { & $baseSmoke $basePresenter $baseParityDir --parity }
    Invoke-Checked "parity-modified-t$t" { & $modSmoke $modPresenter $modParityDir --parity }
    foreach ($mult in @(2, 4)) {
        $bTrial = Parse-ParityLog (Join-Path $baseParityDir "parity-${mult}x.log") $mult
        $mTrial = Parse-ParityLog (Join-Path $modParityDir "parity-${mult}x.log") $mult
        $baseSamples[$mult].Add($bTrial)
        $modSamples[$mult].Add($mTrial)
        $tP50Delta = $mTrial.P50Ms - $bTrial.P50Ms
        $tP95Delta = $mTrial.P95Ms - $bTrial.P95Ms
        $tDropDelta = [int]($mTrial.DropCount - $bTrial.DropCount)
        $maxDropDelta = [Math]::Max(2, [int][Math]::Floor(0.02 * (120 * $mult)))
        Write-Host ("PARITY {0}x trial={1}: base(p50={2:N2}ms p95={3:N2}ms origRate={4:N3} subRate={5:N3} drop={6}) mod(p50={7:N2}ms p95={8:N2}ms origRate={9:N3} subRate={10:N3} drop={11}) delta(p50={12:+0.00;-0.00}ms p95={13:+0.00;-0.00}ms drop={14:+0;-0})" -f `
            $mult, $t, $bTrial.P50Ms, $bTrial.P95Ms, $bTrial.OrigRate, $bTrial.SubRate, [int]$bTrial.DropCount, `
            $mTrial.P50Ms, $mTrial.P95Ms, $mTrial.OrigRate, $mTrial.SubRate, [int]$mTrial.DropCount, $tP50Delta, $tP95Delta, $tDropDelta)
        if ($tP50Delta -gt 1.5) { throw "Parity ${mult}x trial $t p50 delta ${tP50Delta} ms > +1.5 ms" }
        if ($tP95Delta -gt 3.0) { throw "Parity ${mult}x trial $t p95 delta ${tP95Delta} ms > +3.0 ms" }
        if ($mTrial.OrigRate -lt ($bTrial.OrigRate - 0.02) -or $mTrial.OrigRate -lt 0.95) {
            throw "Parity ${mult}x trial $t orig_present_rate regression: mod=$($mTrial.OrigRate) base=$($bTrial.OrigRate)"
        }
        if ($mTrial.SubRate -lt ($bTrial.SubRate - 0.05)) {
            throw "Parity ${mult}x trial $t subframe_present_rate regression: mod=$($mTrial.SubRate) base=$($bTrial.SubRate)"
        }
        if ($tDropDelta -gt $maxDropDelta) {
            throw "Parity ${mult}x trial $t drop delta $tDropDelta > $maxDropDelta"
        }
    }
}

function Get-MedianMetric([object[]]$Items, [string]$Prop) {
    $vals = @($Items | ForEach-Object { [double]$_.$Prop } | Sort-Object)
    return $vals[[int][Math]::Floor($vals.Count / 2)]
}

foreach ($mult in @(2, 4)) {
    $bP50 = Get-MedianMetric $baseSamples[$mult] 'P50Ms'
    $bP95 = Get-MedianMetric $baseSamples[$mult] 'P95Ms'
    $bOrig = Get-MedianMetric $baseSamples[$mult] 'OrigRate'
    $bSub = Get-MedianMetric $baseSamples[$mult] 'SubRate'
    $bDrop = Get-MedianMetric $baseSamples[$mult] 'DropCount'
    $mP50 = Get-MedianMetric $modSamples[$mult] 'P50Ms'
    $mP95 = Get-MedianMetric $modSamples[$mult] 'P95Ms'
    $mOrig = Get-MedianMetric $modSamples[$mult] 'OrigRate'
    $mSub = Get-MedianMetric $modSamples[$mult] 'SubRate'
    $mDrop = Get-MedianMetric $modSamples[$mult] 'DropCount'
    $p50Delta = $mP50 - $bP50
    $p95Delta = $mP95 - $bP95
    $dropDelta = [int]($mDrop - $bDrop)
    Write-Host ("PARITY {0}x (median-of-{1}): base(p50={2:N2}ms p95={3:N2}ms origRate={4:N3} subRate={5:N3} drop={6}) mod(p50={7:N2}ms p95={8:N2}ms origRate={9:N3} subRate={10:N3} drop={11}) delta(p50={12:+0.00;-0.00}ms p95={13:+0.00;-0.00}ms drop={14:+0;-0})" -f `
        $mult, $trials, $bP50, $bP95, $bOrig, $bSub, [int]$bDrop, $mP50, $mP95, $mOrig, $mSub, [int]$mDrop, $p50Delta, $p95Delta, $dropDelta)
}

# (b) NativeVfg + VfgShotChange ON
Invoke-Checked 'ab-shot-change' { & $abRunner $modPresenter $LogDir shot-change }

# (c) NativeVfg + Live HUD Lock + combined hudLock=1 && gpuShotChange=1
Invoke-Checked 'ab-hud-lock' { & $abRunner $modPresenter $LogDir hud-lock }
Invoke-Checked 'vfg-hud-lock-gpu' { & $hudGpu }

# (d1) Same-PID live Pacing Repair OFF -> ON -> OFF on 17/50ms input (real libobs vfg_pacing_repair property + runner cross-check)
Invoke-Checked 'libobs-pacing-repair-ab' {
    Push-Location $LibObsDir
    $env:OBS_NR_VFG_PACING_LOG = (Join-Path $LogDir 'ab-pacing-repair-libobs.log')
    $env:OBS_NR_VFG_RUNTIME = (Join-Path $LibObsDir 'nvidia-vfg')
    try {
        & $filterTests $pluginDll $pluginData $modPresenter (Join-Path $LibObsDir 'libobs-d3d11.dll') (Join-Path $LogDir 'libobs-pacing-repair') pacing-repair-ab
    } finally {
        Remove-Item Env:OBS_NR_VFG_PACING_LOG -ErrorAction SilentlyContinue
        Remove-Item Env:OBS_NR_VFG_RUNTIME -ErrorAction SilentlyContinue
        Pop-Location
    }
}
$pacingOut = Invoke-Checked 'ab-pacing-repair' { & $abRunner $modPresenter $LogDir pacing-repair }
$pacingOut | ForEach-Object { Write-Host $_ }
$pacingSegs = @{}
foreach ($line in $pacingOut) {
    if ($line -match '^PACING_SEG seg=(\d+) repair=(\d+) first_serial=(\d+) last_serial=(\d+)') {
        $pacingSegs[[int]$Matches[1]] = @{ repair = [int]$Matches[2]; first = [int]$Matches[3]; last = [int]$Matches[4] }
    }
}
if ($pacingSegs.Count -ne 3) { throw 'Missing PACING_SEG ranges' }
$pacingLog = [IO.File]::ReadAllLines((Join-Path $LogDir 'ab-pacing-repair.log'))
foreach ($segIdx in @(1, 2, 3)) {
    $seg = $pacingSegs[$segIdx]
    $origQpc = [System.Collections.Generic.List[double]]::new()
    $subCount = 0
    $shiftNonZero = 0
    $flagMismatch = 0
    foreach ($line in $pacingLog) {
        if ($line -notmatch '^event=(\S+)\s+.*\bb=(\d+)\b') { continue }
        $ev = $Matches[1]; $serial = [int]$Matches[2]
        if ($serial -lt $seg.first -or $serial -gt $seg.last) { continue }
        if ($ev -eq 'present_original') {
            if ($line -match '\bqpc_ns=(\d+)\b') { $origQpc.Add([double]$Matches[1] / 1e6) }
            if ($line -match '\bpacing_repair=(\d+)\b' -and [int]$Matches[1] -ne $seg.repair) { $flagMismatch++ }
            if ($line -match '\bpacing_shift_ns=(-?\d+)\b' -and [int64]$Matches[1] -ne 0) { $shiftNonZero++ }
        } elseif ($ev -eq 'present_vfg_subframe') {
            $subCount++
        }
    }
    if ($origQpc.Count -le 20) { throw "Segment $segIdx had too few originals ($($origQpc.Count))" }
    $settled = $origQpc.GetRange(16, $origQpc.Count - 16)
    $errs = [System.Collections.Generic.List[double]]::new()
    for ($i = 1; $i -lt $settled.Count; $i++) {
        $dt = $settled[$i] - $settled[$i - 1]
        $errs.Add([Math]::Abs($dt - 33.333333))
    }
    $sortedErr = @($errs.ToArray() | Sort-Object)
    $p90 = Get-Quantile $sortedErr 0.90
    $maxErr = $sortedErr[$sortedErr.Count - 1]
    Write-Host ("PACING seg={0} repair={1} settled_dts={2} p90_err={3:N2}ms max_err={4:N2}ms subframes={5} shift_nz={6}" -f `
        $segIdx, $seg.repair, $sortedErr.Count, $p90, $maxErr, $subCount, $shiftNonZero)
    if ($seg.repair -eq 1) {
        if ($subCount -le 0 -or $shiftNonZero -le 0) { throw "Pacing ON segment missing subframes or shift" }
        if ($p90 -gt 5.0 -or $maxErr -gt 8.0) { throw "Pacing ON error too high: p90=$p90 max=$maxErr" }
    } else {
        if ($p90 -lt 10.0) { throw "Pacing OFF segment $segIdx expected judder p90 >= 10ms, got $p90" }
    }
}

# (d2) Same-PID live Adaptive Multiplier 30 -> 60 -> 30 fps (eff_mult 4 -> 2 -> 4)
$adaptOut = Invoke-Checked 'ab-adaptive' { & $abRunner $modPresenter $LogDir adaptive }
$adaptOut | ForEach-Object { Write-Host $_ }
$adaptSegs = @{}
foreach ($line in $adaptOut) {
    if ($line -match '^ADAPTIVE_SEG seg=(\d+) fps=(\d+) first_serial=(\d+) last_serial=(\d+)') {
        $adaptSegs[[int]$Matches[1]] = @{ fps = [int]$Matches[2]; first = [int]$Matches[3]; last = [int]$Matches[4] }
    }
}
if ($adaptSegs.Count -ne 3) { throw 'Missing ADAPTIVE_SEG ranges' }
$adaptLog = [IO.File]::ReadAllLines((Join-Path $LogDir 'ab-adaptive.log'))
$expectedMult = @{ 1 = 4; 2 = 2; 3 = 4 }
foreach ($segIdx in @(1, 2, 3)) {
    $seg = $adaptSegs[$segIdx]
    $want = $expectedMult[$segIdx]
    $tailStart = $seg.last - 15
    $origTail = @($adaptLog | Where-Object { $_ -match '^event=present_original\b' -and $_ -match '\bb=(\d+)\b' -and [int]$Matches[1] -ge $tailStart -and [int]$Matches[1] -le $seg.last })
    $subTail = @($adaptLog | Where-Object { $_ -match '^event=present_vfg_subframe\b' -and $_ -match '\bb=(\d+)\b' -and [int]$Matches[1] -ge $tailStart -and [int]$Matches[1] -le $seg.last })
    $origWant = @($origTail | Where-Object { $_ -match "\beff_mult=$want\b" }).Count
    $subWant = @($subTail | Where-Object { $_ -match "\beff_mult=$want\b" }).Count
    Write-Host "ADAPTIVE seg=$segIdx fps=$($seg.fps) want_mult=$want orig_tail=$($origTail.Count) orig_want=$origWant sub_tail=$($subTail.Count) sub_want=$subWant"
    if ($origWant -lt 10 -or $subWant -lt 10) {
        throw "Adaptive segment $segIdx failed to settle on eff_mult=$want (origWant=$origWant subWant=$subWant)"
    }
}

# (e, e1, e2, f1) PushFlow E2E matrix + crash/Esc lifecycle + empty runtime fallback
$env:OBS_NR_PUSHFLOW_RUNTIME = $PushFlowRuntimeDir
try {
    Invoke-Checked 'ab-pushflow-smoke' { & $abRunner $modPresenter $LogDir pushflow-smoke }
    Invoke-Checked 'ab-pushflow-stalled-esc' { & $abRunner $modPresenter $LogDir pushflow-stalled-esc }
    Invoke-Checked 'ab-pushflow-userstop-stalled' { & $abRunner $modPresenter $LogDir pushflow-userstop-stalled }
    Invoke-Checked 'ab-stalled-log-esc' { & $abRunner $modPresenter $LogDir stalled-log-esc }
    Invoke-Checked 'ab-watchdog-stalled-pipe-log' { & $abRunner $modPresenter $LogDir watchdog-stalled-pipe-log }
    Invoke-Checked 'ab-pushflow-esc-stalled-pipe-log' { & $abRunner $modPresenter $LogDir pushflow-esc-stalled-pipe-log }
    Invoke-Checked 'ab-pushflow-userstop-stalled-pipe-log' { & $abRunner $modPresenter $LogDir pushflow-userstop-stalled-pipe-log }
    Invoke-Checked 'ab-pushflow-midrun-crash' { & $abRunner $modPresenter $LogDir pushflow-midrun-crash }
} finally {
    Remove-Item Env:OBS_NR_PUSHFLOW_RUNTIME -ErrorAction SilentlyContinue
}
Invoke-Checked 'ab-pushflow-empty' { & $abRunner $modPresenter $LogDir pushflow-empty }

# (f2) Real libobs vfg_filter_output_tests (all 17 close/recovery scenarios + immediate Esc reopen + render tick < 5 ms)
Invoke-Checked 'libobs-vfg-filter-output' {
    Push-Location $LibObsDir
    $env:OBS_NR_TEST_IMMEDIATE_REOPEN_RUNTIME = $PushFlowRuntimeDir
    try {
        & $filterTests $pluginDll $pluginData $modPresenter (Join-Path $LibObsDir 'libobs-d3d11.dll') (Join-Path $LogDir 'libobs-vfg-output')
    } finally {
        Remove-Item Env:OBS_NR_TEST_IMMEDIATE_REOPEN_RUNTIME -ErrorAction SilentlyContinue
        Pop-Location
    }
}

# (g) ANR contract + live A/B HUD toggle & reopen ShotChange + optional real B15 check
$env:OBS_NR_GUIDES_SHADER = $guidesShader
$env:OBS_NR_TEST_ANR_MOCK_DLL = $anrMockDll
try {
    Invoke-Checked 'anr-mock-dll' { & $anrTests }
    Invoke-Checked 'ab-anr-mock-ab' { & $abRunner $modPresenter $LogDir anr-ab }
} finally {
    Remove-Item Env:OBS_NR_GUIDES_SHADER -ErrorAction SilentlyContinue
    Remove-Item Env:OBS_NR_TEST_ANR_MOCK_DLL -ErrorAction SilentlyContinue
}
if ($env:OBS_NR_TEST_ANR_B15_DLL) {
    Invoke-Checked 'anr-real-b15' { & $anrTests --real-b15-dll $env:OBS_NR_TEST_ANR_B15_DLL }
} else {
    Write-Host 'IS-3 real B15 interpolation: UNVERIFIED (complete user-supplied B15 addon not provided)'
}

Write-Host 'ALL A/B SMOKE MATRIX GATES PASSED'
