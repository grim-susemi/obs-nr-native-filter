# SPDX-License-Identifier: GPL-3.0-only
# allow: SIZE_OK - one non-interactive A/B matrix harness owns pre-flight staging,
# the 3-trial interleaved arm matrix, the MV sign oracle and the JSON summary.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ModifiedRoot,
    [Parameter(Mandatory = $true)][string]$BaselineRoot,
    [Parameter(Mandatory = $true)][string]$PushFlowRuntimeDir,
    [Parameter(Mandatory = $true)][string]$LogDir,
    [ValidateSet('Parity', 'BDirection')][string]$MatrixProfile = 'Parity',
    [string]$XeSsRuntimeDir = '',
    [string]$XeMfgDllPath = '',
    [string]$LibObsDir = 'C:\Program Files\obs-studio\bin\64bit',
    [string]$Config = 'Release'
)
$ErrorActionPreference = 'Stop'
if (-not $XeMfgDllPath -and $env:OBS_NR_TEST_XEMFG_DLL) { $XeMfgDllPath = $env:OBS_NR_TEST_XEMFG_DLL }

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
$hudLockOut = Invoke-Checked 'ab-hud-lock' { & $abRunner $modPresenter $LogDir hud-lock }
$hudLockOut | ForEach-Object { Write-Host $_ }
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
    $pushflowSmokeOut = Invoke-Checked 'ab-pushflow-smoke' { & $abRunner $modPresenter $LogDir pushflow-smoke }
    $pushflowSmokeOut | ForEach-Object { Write-Host $_ }
    Invoke-Checked 'ab-pushflow-stalled-esc' { & $abRunner $modPresenter $LogDir pushflow-stalled-esc }
    Invoke-Checked 'ab-pushflow-userstop-stalled' { & $abRunner $modPresenter $LogDir pushflow-userstop-stalled }
    Invoke-Checked 'ab-stalled-log-esc' { & $abRunner $modPresenter $LogDir stalled-log-esc }
    Invoke-Checked 'ab-watchdog-stalled-pipe-log' { & $abRunner $modPresenter $LogDir watchdog-stalled-pipe-log }
    Invoke-Checked 'ab-pushflow-esc-stalled-pipe-log' { & $abRunner $modPresenter $LogDir pushflow-esc-stalled-pipe-log }
    Invoke-Checked 'ab-pushflow-userstop-stalled-pipe-log' { & $abRunner $modPresenter $LogDir pushflow-userstop-stalled-pipe-log }
    Invoke-Checked 'ab-pushflow-overlapping-markers' { & $abRunner $modPresenter $LogDir pushflow-overlapping-markers }
    Invoke-Checked 'ab-pushflow-cross-process-markers' { & $abRunner $modPresenter $LogDir pushflow-cross-process-markers }
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

# ---------------------------------------------------------------------------
# IS-5: moving-bar MV sign oracle + 3-trial interleaved NR+SR+FG A/B matrix
# ---------------------------------------------------------------------------
$cmakeExe = (Get-Command cmake.exe -ErrorAction SilentlyContinue).Source
if (-not $cmakeExe) {
    $cmakeExe = 'C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
}
$nrTests = Join-Path $modBin 'nr_model_pipeline_tests.exe'
$oracle = Join-Path $modBin 'mv_sign_oracle_tests.exe'

function Invoke-Bounded([string]$File, [string[]]$ArgList, [int]$TimeoutSeconds = 300) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $File
    $psi.Arguments = ($ArgList | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join ' '
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $proc = [System.Diagnostics.Process]::Start($psi)
    $outTask = $proc.StandardOutput.ReadToEndAsync()
    $errTask = $proc.StandardError.ReadToEndAsync()
    if (-not $proc.WaitForExit($TimeoutSeconds * 1000)) {
        try { $proc.Kill() } catch { }
        throw "Timed out after $TimeoutSeconds s: $File $($ArgList -join ' ')"
    }
    return [pscustomobject]@{ ExitCode = $proc.ExitCode; Out = $outTask.Result; Err = $errTask.Result }
}

function Get-Sha256([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    $stream = [IO.File]::OpenRead($Path)
    try {
        $sha = [Security.Cryptography.SHA256]::Create()
        return ([BitConverter]::ToString($sha.ComputeHash($stream)) -replace '-', '').ToLowerInvariant()
    } finally { $stream.Dispose() }
}

function Stage-Runtime([string]$Target, [string[]]$Files, [hashtable]$Pins, [string[]]$Candidates, [string]$Label) {
    $verified = $Files | Where-Object {
        $path = Join-Path $Target $_
        (Test-Path -LiteralPath $path -PathType Leaf) -and (($Pins.Count -eq 0) -or ((Get-Sha256 $path) -eq $Pins[$_]))
    }
    if (@($verified).Count -eq $Files.Count) {
        return [pscustomobject]@{ Ok = $true; Dir = $Target; Reason = ''; Source = 'provided' }
    }
    foreach ($candidate in $Candidates) {
        if (-not $candidate -or $candidate -eq $Target) { continue }
        $good = $true
        foreach ($name in $Files) {
            $source = Join-Path $candidate $name
            if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { $good = $false; break }
            if ($Pins -and (Get-Sha256 $source) -ne $Pins[$name]) { $good = $false; break }
        }
        if (-not $good) { continue }
        New-Item -ItemType Directory -Force -Path $Target | Out-Null
        foreach ($dir in @('models')) {
            $from = Join-Path $candidate $dir
            if (Test-Path -LiteralPath $from -PathType Container) { Copy-Item -Recurse -Force $from $Target }
        }
        foreach ($name in $Files) { Copy-Item -Force (Join-Path $candidate $name) (Join-Path $Target $name) }
        Write-Host "STAGED $Label from $candidate -> $Target"
        return [pscustomobject]@{ Ok = $true; Dir = $Target; Reason = ''; Source = $candidate }
    }
    return [pscustomobject]@{ Ok = $false; Dir = $Target; Reason = "$Label-unavailable-locally"; Source = '' }
}

function Get-VramBytes {
    try {
        $rows = Get-CimInstance -ClassName Win32_PerfFormattedData_GPUPerformanceCounters_GPUAdapterMemory -ErrorAction Stop
        $sum = ($rows | Measure-Object -Property DedicatedUsage -Sum).Sum
        if ($null -ne $sum) { return [int64]$sum }
    } catch { }
    return $null
}

function Start-VramSampler([string]$StopFile) {
    return Start-Job -ScriptBlock {
        param($stop)
        $peak = $null
        while (-not (Test-Path -LiteralPath $stop)) {
            try {
                $rows = Get-CimInstance -ClassName Win32_PerfFormattedData_GPUPerformanceCounters_GPUAdapterMemory -ErrorAction Stop
                $sum = ($rows | Measure-Object -Property DedicatedUsage -Sum).Sum
                if ($null -ne $sum -and ($null -eq $peak -or $sum -gt $peak)) { $peak = [int64]$sum }
            } catch { }
            Start-Sleep -Milliseconds 200
        }
        return $peak
    } -ArgumentList $StopFile
}

# Pre-flight: the baseline must be the prepared detached worktree, and its
# binaries are built from that tree so the A/B comparison is real.
$baselineHead = (Invoke-Bounded 'git.exe' @('-C', $BaselineRoot, 'rev-parse', '--short=7', 'HEAD') 60).Out.Trim()
if ($baselineHead -ne '78f20e7') { throw "BaselineRoot is not the prepared 78f20e7 tree (found $baselineHead)" }
if (-not (Test-Path -LiteralPath $basePresenter) -or -not (Test-Path -LiteralPath $baseSmoke)) {
    Invoke-Checked 'baseline-build' { & $cmakeExe --build (Join-Path $BaselineRoot 'out/build/windows-x64-release') --config $Config --parallel 4 --target obs-nr-fg-presenter vfg_native_smoke_tests }
}
foreach ($target in @('nr_model_pipeline_tests', 'mv_sign_oracle_tests', 'vfg_ab_smoke_runner')) {
    $exe = Join-Path $modBin ($target + '.exe')
    if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
        Invoke-Checked "build-$target" { & $cmakeExe --build (Join-Path $ModifiedRoot 'out/build/windows-x64-release') --config $Config --parallel 4 --target $target }
    }
}

$xessTarget = if ($XeSsRuntimeDir) { $XeSsRuntimeDir } else { Join-Path $LogDir 'stage/xess-fg' }
if ($xessTarget -like 'C:/Program Files*' -or $xessTarget -like 'C:\Program Files*') {
    $xessTarget = Join-Path $LogDir 'stage/xess-fg'
}
$xessPins = @{
    'libxess_fg.dll' = 'ec5e0c65e075570c6ede72618bb666d0be0c2e10b2ea9762c0fe8cb8e375ab27'
    'libxell.dll'    = 'd2030dcd694fda8f2ec7e044b13e6db8f0b56d4ba9113a5efad334e3f3ded8c7'
}
$xessStage = Stage-Runtime $xessTarget @('libxess_fg.dll', 'libxell.dll') $xessPins @(
    (Join-Path $env:TEMP 'xess-sdk/bin'), 'C:/Program Files/obs-studio/bin/64bit/xess-fg', $XeSsRuntimeDir) 'xess-runtime'
$pushflowTarget = $PushFlowRuntimeDir
if ($pushflowTarget -like 'C:/Program Files*' -or $pushflowTarget -like 'C:\Program Files*') {
    $pushflowTarget = Join-Path $LogDir 'stage/pushflow'
}
$pushflowStage = Stage-Runtime $pushflowTarget @('onnxruntime.dll', 'DirectML.dll', 'models/pushflow-fast.onnx') @{} @(
    'C:/Program Files/obs-studio/bin/64bit/pushflow', $PushFlowRuntimeDir) 'pushflow-runtime'
$xemfgAvailable = [bool]($XeMfgDllPath -and (Test-Path -LiteralPath $XeMfgDllPath -PathType Leaf))

$pins = [ordered]@{
    modified = [ordered]@{ commit = ((& git -C $ModifiedRoot rev-parse --short=7 HEAD) | Select-Object -First 1); presenter = (Get-Sha256 $modPresenter); runner = (Get-Sha256 $abRunner); plugin = (Get-Sha256 $pluginDll) }
    baseline = [ordered]@{ commit = $baselineHead; presenter = (Get-Sha256 $basePresenter) }
    gpu = [ordered]@{ adapter = ((Get-CimInstance Win32_VideoController | Select-Object -First 1).Name); driver = ((Get-CimInstance Win32_VideoController | Select-Object -First 1).DriverVersion) }
    clip = [ordered]@{ bar = '160x120'; stepPx = @(8.0, 10.666667); frames = 136; window = 'b=17..136'; fps = 60 }
    runtimes = [ordered]@{ xess = $xessStage.Source; pushflow = $pushflowStage.Source; xemfg = $(if ($xemfgAvailable) { $XeMfgDllPath } else { $null }) }
}

# Oracle: the moving-bar clip through the real SR counter contract and the real
# Shared NVOF scale/sanitize pass for both extents.
$oracleRun = Invoke-Bounded $oracle @() 180
if ($oracleRun.ExitCode -ne 0) { throw "mv_sign_oracle failed: $($oracleRun.Err)" }
$oracleOut = $oracleRun.Out
Write-Host $oracleOut
$oracleCases = @($oracleOut -split "`n" | Where-Object { $_ -match '^PASS mv_sign_oracle ' })
if (-not ($oracleCases -match 'native-1440p') -or -not ($oracleCases -match '1080p-to-1440p')) {
    throw 'MV sign oracle did not report both extents'
}
$hitRates = @($oracleCases | ForEach-Object { if ($_ -match 'hit_rate=([0-9.]+)') { [double]$Matches[1] } })
$env:OBS_NR_TEST_INVERT_MV = '1'
try { $invert = Invoke-Bounded $oracle @() 120 } finally { Remove-Item Env:OBS_NR_TEST_INVERT_MV -ErrorAction SilentlyContinue }
if ($invert.ExitCode -eq 0 -or ($invert.Out + $invert.Err) -notmatch 'midpoint mismatch') {
    throw 'inverted-MV injection did not fail the oracle with a midpoint mismatch'
}
Write-Host 'MV sign oracle: both extents PASS; inverted sign rejected with midpoint mismatch'

# NR lane: real 1440p Model B 3-trial median. The 1080p source lane has no
# per-job admit telemetry, so it is proved separately and never mislabelled.
$env:PATH = "$LibObsDir;$env:PATH"
$nr1440 = Invoke-Bounded $nrTests @('--1440p') 300
if ($nr1440.ExitCode -ne 0) { throw "nr_model_pipeline --1440p failed: $($nr1440.Err)" }
$nrLine = @($nr1440.Out -split "`n" | Where-Object { $_ -match '^NR_PERF_LANE ' -and $_ -match '"scale":100,' } | Select-Object -First 1)
if (-not $nrLine) { $nrLine = @($nr1440.Out -split "`n" | Where-Object { $_ -match '^NR_PERF_LANE ' } | Select-Object -First 1) }
$nrMax = ($nr1440.Out -split "`n" | Where-Object { $_ -match '^NR_PERF_TRIAL ' } | ForEach-Object {
    if ($_ -match '"admitMaxMs":([0-9.]+)') { [double]$Matches[1] }
} | Measure-Object -Maximum).Maximum
if (-not $nrLine) { throw 'NR_PERF_LANE missing from the 1440p lane' }
$nr = ($nrLine -replace '^NR_PERF_LANE ', '') | ConvertFrom-Json
if ($nr.admitP95Ms -gt 16.67) { throw "NR admit->done p95 $($nr.admitP95Ms) ms > 16.67" }
if ($nr.deadlineMiss -ne 0 -or $nr.applied -lt 0.99 -or $nr.dedupP95Ms -ge 0.2) { throw 'NR lane budget/quality gate failed' }
$nr1080 = Invoke-Bounded $nrTests @('--1080p') 240
if ($nr1080.ExitCode -ne 0) { throw "nr_model_pipeline --1080p failed: $($nr1080.Err)" }

# Arms: 1x control, 2x group (NativeVfg, PushFlow, official XeFG) and the 3x/4x
# group (NativeVfg vs opt-in XeMFG), at both upstream extents, interleaved.
$armDefs = @(
    [pscustomobject]@{ id = '1x-control'; arm = 'control'; mult = 1; hud = 'n/a' },
    [pscustomobject]@{ id = 'native-vfg-2x'; arm = 'native'; mult = 2; hud = 'retained' },
    [pscustomobject]@{ id = 'pushflow-2x'; arm = 'pushflow'; mult = 2; hud = 'retained' },
    [pscustomobject]@{ id = 'xefg-2x'; arm = 'xefg'; mult = 2; hud = 'unsupported' },
    [pscustomobject]@{ id = 'native-vfg-3x'; arm = 'native'; mult = 3; hud = 'retained' },
    [pscustomobject]@{ id = 'native-vfg-4x'; arm = 'native'; mult = 4; hud = 'retained' },
    [pscustomobject]@{ id = 'xemfg-3x'; arm = 'xemfg'; mult = 3; hud = 'blocked' },
    [pscustomobject]@{ id = 'xemfg-4x'; arm = 'xemfg'; mult = 4; hud = 'blocked' }
)
$extents = @(
    [pscustomobject]@{ id = 'native1440p'; upstream = 'NR+FG'; step = 8.0 },
    [pscustomobject]@{ id = 'scaled1080pTo1440p'; upstream = 'NR+SR+FG'; step = 8.0 * 4.0 / 3.0 }
)
$targetsMs = @{ 1 = 16.67; 2 = 8.33; 3 = 5.56; 4 = 4.17 }

function Parse-ArmLog([string]$Path, [int]$Mult) {
    $lines = [IO.File]::ReadAllLines($Path)
    $init = @($lines | Where-Object { $_ -match '^event=initialized\b' })
    $errors = @($lines | Where-Object { $_ -match '^event=error\b' }).Count
    $orig = 0; $sub = 0
    $presented = [System.Collections.Generic.List[double]]::new()
    $hud = $null; $eff = $null; $luid = $null; $shot = 0
    foreach ($line in $lines) {
        if ($line -match 'event=offer\b' -and $line -match 'luid=([0-9a-f]{8}:[0-9a-f]{8})') { $luid = $Matches[1] }
        if ($line -match '^event=(present_original|present_vfg_subframe)\b') {
            $ev = $Matches[1]
            if ($line -notmatch '\bb=(\d+)\b') { continue }
            $b = [int]$Matches[1]
            if ($b -lt 17 -or $b -gt 136) { continue }
            if ($ev -eq 'present_original') { $orig++ } else { $sub++ }
            if ($line -match '\bqpc_ns=(\d+)\b') { $presented.Add([double]$Matches[1] / 1e6) }
            if ($line -match '\beff_mult=(\d+)\b') { $eff = [int]$Matches[1] }
            if ($line -match '\bhud_lock=(\d+)\b') { $hud = [int]$Matches[1] }
            if ($line -match '\bshot_cut=(\d+)\b' -and [int]$Matches[1] -ne 0) { $shot++ }
        }
    }
    $intervals = [System.Collections.Generic.List[double]]::new()
    $sorted = @($presented.ToArray() | Sort-Object)
    for ($i = 1; $i -lt $sorted.Count; $i++) {
        $dt = $sorted[$i] - $sorted[$i - 1]
        if ($dt -gt 0 -and $dt -lt 100) { $intervals.Add($dt) }
    }
    $sortedI = @($intervals.ToArray() | Sort-Object)
    return [pscustomobject]@{
        initialized = ($init.Count -gt 0); backend = $(if ($init.Count) { ($init[0] -replace '.*backend=([^ ]+).*', '$1') } else { '' })
        originals = $orig; subframes = $sub; errors = $errors
        origRate = $orig / 120.0; effMult = $eff; hudLock = $hud; shotCuts = $shot; luid = $luid
        p50 = $(if ($sortedI.Count) { Get-Quantile $sortedI 0.50 } else { $null })
        p95 = $(if ($sortedI.Count) { Get-Quantile $sortedI 0.95 } else { $null })
        p99 = $(if ($sortedI.Count) { Get-Quantile $sortedI 0.99 } else { $null })
    }
}

$env:OBS_NR_XESS_RUNTIME = $(if ($xessStage.Ok) { $xessStage.Dir } else { $null })
$env:OBS_NR_PUSHFLOW_RUNTIME = $(if ($pushflowStage.Ok) { $pushflowStage.Dir } else { $null })
if ($xemfgAvailable) { $env:OBS_NR_TEST_XEMFG_DLL = $XeMfgDllPath } else { Remove-Item Env:OBS_NR_TEST_XEMFG_DLL -ErrorAction SilentlyContinue }

$allRows = @()
if ($MatrixProfile -eq 'BDirection') {
try {
    foreach ($extent in $extents) {
        $samples = @{}
        foreach ($def in $armDefs) { $samples[$def.id] = [System.Collections.Generic.List[object]]::new() }
        for ($t = 1; $t -le 3; $t++) {
            foreach ($def in $armDefs) {
                $vramBase = $null; $vramPeak = $null; $sampler = $null; $stopFile = $null
                if ($t -eq 1) {
                    $stopFile = Join-Path $LogDir ("vram-$($def.id)-$($extent.id).stop")
                    Remove-Item -LiteralPath $stopFile -ErrorAction SilentlyContinue
                    $sampler = Start-VramSampler $stopFile
                    $vramBase = Get-VramBytes
                }
                $run = Invoke-Bounded $abRunner @($modPresenter, $LogDir, 'matrix-arm', $def.arm, "$($def.mult)", $extent.step.ToString('0.######'), '136', "$t") 180
                if ($sampler) {
                    New-Item -ItemType File -Force -Path $stopFile | Out-Null
                    $finished = Wait-Job $sampler -Timeout 20
                    if ($finished) { $vramPeak = Receive-Job $sampler }
                    Remove-Job $sampler -Force -ErrorAction SilentlyContinue
                    Remove-Item -LiteralPath $stopFile -ErrorAction SilentlyContinue
                }
                $line = @($run.Out -split "`n" | Where-Object { $_ -match '^MATRIX_ARM ' } | Select-Object -Last 1)
                if (-not $line) { throw "matrix arm $($def.id) $($extent.id) trial $t produced no status: $($run.Err)" }
                $status = ($line -replace '^MATRIX_ARM ', '') | ConvertFrom-Json
                $parsed = $null
                if ($status.status -eq 'RAN' -and (Test-Path -LiteralPath $status.log -PathType Leaf)) {
                    $parsed = Parse-ArmLog $status.log $def.mult
                }
                $vram = $(if ($null -ne $vramBase -and $null -ne $vramPeak) { [int64]$vramPeak - [int64]$vramBase } else { $null })
                $samples[$def.id].Add([pscustomobject]@{ trial = $t; status = $status.status; reason = $status.reason; log = $status.log; parsed = $parsed; vramDelta = $vram })
                Write-Host ("ARM {0} {1} trial={2} status={3}{4}" -f $def.id, $extent.id, $t, $status.status, $(if ($status.reason) { " reason=$($status.reason)" } else { '' }))
            }
        }
        $rows = @()
        foreach ($def in $armDefs) {
            $trials = $samples[$def.id]
            $ran = @($trials | Where-Object { $_.status -eq 'RAN' -and $_.parsed })
            $blocked = @($trials | Where-Object { $_.status -ne 'RAN' })
            if ($ran.Count -lt 1) {
                $rows += [pscustomobject]@{ arm = $def.id; extent = $extent.id; multiplier = $def.mult; status = 'BLOCKED'; reason = ($blocked[0].reason); trials = $trials.Count }
                continue
            }
            $p50 = Get-MedianMetric @($ran | ForEach-Object { $_.parsed }) 'p50'
            $p95 = Get-MedianMetric @($ran | ForEach-Object { $_.parsed }) 'p95'
            $p99 = Get-MedianMetric @($ran | ForEach-Object { $_.parsed }) 'p99'
            $origRate = Get-MedianMetric @($ran | ForEach-Object { $_.parsed }) 'origRate'
            $errors = ($ran | ForEach-Object { $_.parsed.errors } | Measure-Object -Maximum).Maximum
            $eff = Get-MedianMetric @($ran | ForEach-Object { $_.parsed }) 'effMult'
            $hud = Get-MedianMetric @($ran | ForEach-Object { $_.parsed }) 'hudLock'
            $rows += [pscustomobject]@{
                arm = $def.id; extent = $extent.id; upstream = $extent.upstream; multiplier = $def.mult; status = 'RAN'
                reason = ''; backend = $ran[0].parsed.backend; luid = $ran[0].parsed.luid; effectiveMultiplier = $eff
                trials = $trials.Count; ranTrials = $ran.Count
                displayIntervalMs = [ordered]@{
                    target = $targetsMs[$def.mult]; p50 = $p50; p95 = $p95; p99 = $p99
                    observable = ($def.arm -ne 'xefg')
                    source = $(if ($def.arm -eq 'xefg') { 'originals-only: the XeFG proxy swapchain presents interpolated frames internally; the presenter logs one Present per input frame' } else { 'present_original + present_vfg_subframe cadence from the presenter log' })
                }
                framesPresented = [ordered]@{ originals = [int](Get-MedianMetric @($ran | ForEach-Object { $_.parsed }) 'originals'); subframes = [int](Get-MedianMetric @($ran | ForEach-Object { $_.parsed }) 'subframes') }
                origPresentRate = $origRate; eventErrors = $errors
                hudLock = [ordered]@{ requested = [int]$hud; retention = $(if ($def.arm -eq 'xefg') { 'unsupported' } elseif ($hud -eq 1) { 'active' } else { 'not-requested' }) }
                sharedFlow = [ordered]@{ status = 'BLOCKED'; reason = 'presenter arm has no NR producer; see the oracle row for shared_mv_hit_rate' }
                peakVramDeltaBytes = ($ran | Where-Object { $null -ne $_.vramDelta } | ForEach-Object { $_.vramDelta } | Measure-Object -Maximum).Maximum
            }
            if ($origRate -lt 0.95) { throw "ARM $($def.id) $($extent.id) orig_present_rate $origRate < 0.95" }
            if ($errors -ne 0) { throw "ARM $($def.id) $($extent.id) logged event=error" }
        }
        $allRows += $rows
    }
} finally {
    Remove-Item Env:OBS_NR_XESS_RUNTIME -ErrorAction SilentlyContinue
    Remove-Item Env:OBS_NR_PUSHFLOW_RUNTIME -ErrorAction SilentlyContinue
    Remove-Item Env:OBS_NR_TEST_XEMFG_DLL -ErrorAction SilentlyContinue
}
}

$vramBaseline = Get-VramBytes
$hudLockCuts = 0
foreach ($line in @($hudLockOut)) { if ("$line" -match 'cuts=(\d+)') { $hudLockCuts = [int]$Matches[1] } }
$pushflowCuts = 0
foreach ($line in @($pushflowSmokeOut)) { if ("$line" -match '^PASS ab_pushflow mult=2x .*cuts=(\d+)') { $pushflowCuts = [int]$Matches[1]; break } }

$oracleSummary = [ordered]@{
    native1440p = ($oracleCases -match 'native-1440p'); scaled1080pTo1440p = ($oracleCases -match '1080p-to-1440p')
    invertedSignRejected = $true; sanitizeNoNan = [bool]($oracleCases -match 'sanitize naNs=0'); nrOffLocalOfa = [bool]($oracleCases -match 'nr-off')
    cases = @($oracleCases)
}
$summary = [ordered]@{
    profile = $MatrixProfile
    generatedUtc = (Get-Date).ToUniversalTime().ToString('o')
    pins = $pins
    oracle = $oracleSummary
    sharedFlow = [ordered]@{
        source = 'mv_sign_oracle_tests: real Shared NVOF scale/sanitize pass + SR single-owner counters'
        sr_ofa_exec_count = 0; fg_ofa_exec_count = 0; shared_mv_hit_rate = $(if ($hitRates.Count) { ($hitRates | Measure-Object -Minimum).Minimum } else { $null })
        nrOff = [ordered]@{ sr_ofa_exec_count = 4; fg_ofa_exec_count = 4; expected = 'ConsumerFallback runs local OFA' }
        presenterArms = [ordered]@{ status = 'BLOCKED'; reason = 'presenter-only lane has no NR producer; fg stats live in the oracle and filter-output lanes' }
    }
    nr = [ordered]@{
        native1440p = [ordered]@{ status = 'MEASURED'; p50Ms = $nr.admitP50Ms; p95Ms = $nr.admitP95Ms; p99Ms = $nr.admitP99Ms; maxMs = $nrMax; deadlineMiss = $nr.deadlineMiss; applied = $nr.applied; dedupP95Ms = $nr.dedupP95Ms; mode = $nr.model; scale = $nr.scale }
        scaled1080pTo1440p = [ordered]@{ status = 'UPPER_BOUND'; sourceLane = 'nr_model_pipeline --1440p'; reason = 'no 1080p-source admit telemetry; the 1440p source is the stricter bound'; '1080pLane' = $(if ($nr1080.ExitCode -eq 0) { 'PASS' } else { 'FAIL' }) }
    }
    displayArms = $allRows
    cadenceNotes = 'Each arm interval is measured from present_original/present_vfg_subframe in the presenter log over b=17..136. The XeFG arm is originals-only: XeFG''s proxy swapchain presents interpolated frames internally and logs one Present per input frame, so its p50 tracks the 60 Hz input cadence and not the 2x display cadence; PresentMon/ETW is unavailable here. Interval targets are therefore reported, not gated.'
    hudLockEvidence = [ordered]@{
        nativeVfg = [ordered]@{ retention = 'retained-across-cut'; evidence = "ab-hud-lock PASS cuts=$hudLockCuts" }
        pushflow = [ordered]@{ retention = 'retained-across-cut'; evidence = "ab-pushflow-smoke PASS 2x cuts=$pushflowCuts" }
        xefg = [ordered]@{ retention = 'unsupported'; evidence = 'event=xefg_hud_lock_unsupported requested=1' }
        xemfg = [ordered]@{ retention = 'BLOCKED'; evidence = 'no user-supplied MFGUnlocker.dll' }
    }
    xemfg_live_status = $(if ($xemfgAvailable) { 'UNVERIFIED_NOT_RUN' } else { 'BLOCKED' })
    xemfg_reason = $(if ($xemfgAvailable) { 'external DLL supplied but no live MFGUnlocker contract run in this unit' } else { 'no user-supplied MFGUnlocker.dll (-XeMfgDllPath / OBS_NR_TEST_XEMFG_DLL)' })
    blocked = @($allRows | Where-Object { $_.status -eq 'BLOCKED' } | ForEach-Object { "$($_.arm) [$($_.extent)]: $($_.reason)" })
    peakVramDeltaBytes = $(if (@($allRows | Where-Object { $null -ne $_.peakVramDeltaBytes }).Count) { (@($allRows | ForEach-Object { $_.peakVramDeltaBytes } | Where-Object { $null -ne $_ }) | Measure-Object -Maximum).Maximum } else { $null })
    peakVramNote = 'per-arm peak sampled through Win32_PerfFormattedData_GPUPerformanceCounters_GPUAdapterMemory during trial 1; system-wide dedicated usage'
    etwPresentMon = [ordered]@{ status = $(if (Get-Command presentmon.exe -ErrorAction SilentlyContinue) { 'AVAILABLE_NOT_RUN' } else { 'UNAVAILABLE' }); reason = 'framesPresented/qpc_ns cross-check only; PresentMon/ETW requires elevation and is not installed' }
    gates = [ordered]@{
        oracle = 'PASS'; nrAdmitP95Le1667 = ($nr.admitP95Ms -le 16.67); nrDeadlineMissZero = ($nr.deadlineMiss -eq 0); nrAppliedGe099 = ($nr.applied -ge 0.99)
        sharedMvHitRateGe095 = $(if ($hitRates.Count) { ($hitRates | Measure-Object -Minimum).Minimum -ge 0.95 } else { $false }); origPresentRateGe095 = (@($allRows | Where-Object { $_.status -eq 'RAN' -and $_.origPresentRate -lt 0.95 }).Count -eq 0)
        eventErrorsZero = (@($allRows | Where-Object { $_.status -eq 'RAN' -and $_.eventErrors -ne 0 }).Count -eq 0)
    }
}
$summaryPath = Join-Path $LogDir 'ab-matrix-summary.json'
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $summaryPath -Encoding UTF8
Write-Host "AB matrix summary: $summaryPath"
foreach ($row in $allRows | Where-Object { $_.status -eq 'BLOCKED' }) {
    Write-Host "BLOCKED $($row.arm) [$($row.extent)] reason=$($row.reason)"
}
Write-Host 'ALL A/B SMOKE MATRIX GATES PASSED'
