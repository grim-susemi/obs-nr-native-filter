# SPDX-License-Identifier: GPL-3.0-only
# OBS Native NR installer and uninstaller (Windows PowerShell 5.1 or later).
# install.bat and uninstall.bat in the release zip run this script.
[CmdletBinding()]
param(
    [string]$ObsDir = '',
    [ValidateSet('', 'ko', 'en')][string]$Lang = '',
    [string]$Runtime = '',
    [string]$SrRuntime = '',
    [string]$VfgRuntimeDir = '',
    [string]$StageCleanupDir = '',
    [switch]$RuntimeDecided,
    [switch]$DownloadOptional,
    [switch]$SkipOptional,
    [switch]$Uninstall,
    [switch]$Yes,
    [switch]$NoPause,
    [string]$StateDir = ''
)
$ErrorActionPreference = 'Stop'
$Version = '0.2.1'
$RuntimeSha = 'e67dee209320cdafe0e93e45675d7aa34323a53acc57a72b2e40a181581c989a'
$RuntimeRel = 'bin\64bit\nvngx_dlssnr.dll'
$SrRel = 'bin\64bit\dlss\nvngx_dlss.dll'
$SrUrl = 'https://raw.githubusercontent.com/NVIDIA/DLSS/main/lib/Windows_x86_64/rel/nvngx_dlss.dll'
$VfgDirRel = 'bin\64bit\nvidia-vfg'
$VfgDlls = @('cudart64_12.dll', 'NVCVImage.dll', 'nvngxruntime.dll', 'NVVideoEffects.dll', 'nvVFXVideoFrameGeneration.dll')
$VfgWhlUrl = 'https://pypi.nvidia.com/nvidia-vfx/nvidia_vfx-0.2.0.0-cp311-cp311-win_amd64.whl'
$VfgWhlSha = '5aaf6a42bc6b6dbbf52fcb714194c994a6893cbbf7ada38bc2165a1f83e4a6fc'
$DefaultObs = 'C:\Program Files\obs-studio'
$Payload = Join-Path (Split-Path -Parent $PSScriptRoot) 'files'
if (-not $StateDir) { $StateDir = Join-Path $env:LOCALAPPDATA 'obs-nr-native-filter' }
try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch {}
try { $Host.UI.RawUI.WindowTitle = "OBS Native NR $Version" } catch {}

$Text = @{
    ko = @{
        title      = 'OBS Native NR {0} 설치'
        titleU     = 'OBS Native NR {0} 제거'
        sPayload   = '설치 파일 확인'
        sObs       = 'OBS 찾기'
        sRuntime   = 'NR 모델 파일 확인'
        sOpt       = '선택 확장 런타임 (DLSS 4.5 SR · NVIDIA VFG)'
        sCopy      = '파일 복사'
        sRemove    = '파일 제거'
        payloadBad = '설치 파일이 빠졌거나 손상됐습니다: {0}. 릴리즈 zip을 다시 받아 전부 풀어 주세요.'
        payloadOk  = '설치 파일 {0}개 확인'
        obsFound   = 'OBS 폴더: {0}'
        obsConfirm = '이 OBS에 진행할까요? (Y/n)'
        obsMany    = 'OBS가 여러 곳에 있습니다. 번호를 고르세요:'
        obsAsk     = 'OBS 설치 폴더 경로를 입력하세요 (예: C:\Program Files\obs-studio, 취소는 그냥 Enter)'
        obsNone    = 'OBS 폴더를 찾지 못했습니다.'
        obsInvalid = '이 폴더에는 bin\64bit\obs64.exe가 없습니다: {0}'
        obsVersion = 'OBS 버전 {0}'
        obsOld     = '주의: 이 플러그인은 OBS 32용입니다. 지금 OBS({0})에서는 동작하지 않을 수 있습니다.'
        obsRunning = 'OBS가 실행 중입니다. OBS를 완전히 종료한 뒤 Enter를 누르세요 (취소: Ctrl+C).'
        obsRunStop = 'OBS가 실행 중이라 중단합니다. OBS를 끄고 다시 실행하세요.'
        admin      = '관리자 권한이 필요합니다. 권한 확인 창에서 [예]를 눌러 주세요.'
        adminFail  = '관리자 권한을 받지 못해 중단했습니다.'
        adminDone  = '관리자 창에서 진행했습니다. 결과는 그 창에 표시됐습니다 (종료 코드 {0}).'
        rtOk       = '확인됨: {0}'
        rtMissing  = 'OBS에 NR 모델 파일(nvngx_dlssnr.dll)이 없습니다.'
        rtWrong    = 'OBS에 있는 nvngx_dlssnr.dll은 이 필터가 받는 판이 아닙니다 (SHA-256 {0}…). 이대로는 NR이 동작하지 않습니다.'
        rtNeed     = '필요한 파일: nvngx_dlssnr.dll 310.8.0.0 호환판, SHA-256 e67dee20…c989a (RTX 20~50 공용, 이 패키지에는 없음)'
        rtAsk      = '가지고 있는 nvngx_dlssnr.dll을 이 창에 끌어다 놓고 Enter (건너뛰기: 그냥 Enter)'
        rtNotFound = '파일이 없습니다: {0}'
        rtBadHash  = '이 파일은 다른 판입니다 (SHA-256 {0}…).'
        rtWillCopy = '이 파일을 OBS 폴더에 넣습니다: {0}'
        rtCopied   = 'NR 모델 파일을 넣었습니다: {0}'
        rtSkipped  = 'NR 모델 파일 없이 계속합니다. 나중에 여기에 넣어 주세요: {0}'
        srOk       = 'DLSS 4.5 SR 런타임 확인됨: {0}'
        srPrompt   = '1440p/4K DLSS 4.5 업스케일용 공식 nvngx_dlss.dll(약 56 MB)을 NVIDIA 공식 서버에서 자동 설치할까요? (Y/n)'
        srDown     = 'NVIDIA 공식 GitHub에서 nvngx_dlss.dll을 내려받는 중...'
        srSigOk    = 'NVIDIA 공식 서명 확인됨: nvngx_dlss.dll'
        srFail     = 'DLSS 4.5 SR 자동 설치 건너뜀({0}). 없어도 이중선형+CAS 업스케일은 동작합니다.'
        srCopied   = 'DLSS 4.5 SR 런타임을 넣었습니다: {0}'
        vfgOk      = 'NVIDIA VFG 런타임 확인됨: {0}'
        vfgLocal   = '로컬 Veyra의 공식 NVIDIA VFG DLL 5개를 재사용합니다.'
        vfgPrompt  = 'RTX 40/50 NVIDIA VFG(2x~4x 미리보기) 공식 런타임(약 415 MB wheel에서 5개 서명 DLL 추출)을 자동 설치할까요? (Y/n)'
        vfgDown    = 'NVIDIA 공식 PyPI에서 nvidia-vfx 0.2.0.0을 내려받는 중 (약 415 MB)...'
        vfgVerify  = 'wheel SHA-256 검증 및 VFG DLL 5개 추출 중...'
        vfgFail    = 'NVIDIA VFG 자동 설치 건너뜀({0}). NR/업스케일 필터는 정상 동작합니다.'
        vfgCopied  = 'NVIDIA VFG 런타임(5개 DLL)을 넣었습니다: {0}'
        backup     = '백업: {0}'
        copy       = '설치: {0}'
        verifyFail = '복사한 파일 확인에 실패했습니다: {0}'
        rolledBack = '설치를 되돌렸습니다.'
        done       = '설치 완료'
        next1      = '1. OBS를 실행합니다.'
        next2      = '2. 소스 우클릭 > 필터 > 효과 필터의 [+] > Native NR (미리보기 창에서 우클릭 메뉴 NVIDIA VFG 사용 가능)'
        next3      = '3. 필터 속성에 "상태: NR 적용 중"이 보이면 성공입니다.'
        pathNote   = 'OBS가 기본 위치가 아니라서 한 가지가 더 필요합니다. 필터 속성의 "NR 런타임 파일" 칸에 아래 경로를 넣고, 필터를 지웠다 다시 추가하세요:'
        uninstHint = '제거하려면 이 폴더의 uninstall.bat을 실행하세요.'
        uNoState   = '설치 기록이 없어 Native NR 파일만 찾아 지웁니다.'
        uRemoved   = '삭제: {0}'
        uChanged   = '남겨 둠 (설치 뒤 바뀐 파일): {0}'
        uRestored  = '복원: {0}'
        uDone      = '제거 완료'
        cancelled  = '취소했습니다.'
        error      = '오류: {0}'
        log        = '기록 파일: {0}'
        pause      = 'Enter를 누르면 창이 닫힙니다'
    }
    en = @{
        title      = 'OBS Native NR {0} setup'
        titleU     = 'OBS Native NR {0} uninstall'
        sPayload   = 'Checking package files'
        sObs       = 'Finding OBS'
        sRuntime   = 'Checking the NR model file'
        sOpt       = 'Optional runtimes (DLSS 4.5 SR & NVIDIA VFG)'
        sCopy      = 'Copying files'
        sRemove    = 'Removing files'
        payloadBad = 'A package file is missing or damaged: {0}. Download the release zip again and extract all of it.'
        payloadOk  = '{0} package files OK'
        obsFound   = 'OBS folder: {0}'
        obsConfirm = 'Use this OBS? (Y/n)'
        obsMany    = 'Found more than one OBS. Pick a number:'
        obsAsk     = 'Type the OBS install folder (for example C:\Program Files\obs-studio). Enter alone cancels'
        obsNone    = 'Could not find an OBS folder.'
        obsInvalid = 'No bin\64bit\obs64.exe in this folder: {0}'
        obsVersion = 'OBS version {0}'
        obsOld     = 'Note: this plugin is built for OBS 32. It may not load in OBS {0}.'
        obsRunning = 'OBS is running. Close OBS completely, then press Enter (Ctrl+C cancels).'
        obsRunStop = 'OBS is running, stopping here. Close OBS and run this again.'
        admin      = 'Admin rights are needed. Choose Yes in the Windows prompt.'
        adminFail  = 'Did not get admin rights, stopped.'
        adminDone  = 'Finished in the admin window, which showed the result (exit code {0}).'
        rtOk       = 'OK: {0}'
        rtMissing  = 'The NR model file (nvngx_dlssnr.dll) is not in this OBS.'
        rtWrong    = 'The nvngx_dlssnr.dll in this OBS is a different build (SHA-256 {0}...). NR will not run with it.'
        rtNeed     = 'Needed: nvngx_dlssnr.dll 310.8.0.0 compatibility build, SHA-256 e67dee20...c989a (RTX 20-50, not in this package)'
        rtAsk      = 'Drag your nvngx_dlssnr.dll into this window and press Enter (Enter alone skips)'
        rtNotFound = 'File not found: {0}'
        rtBadHash  = 'That file is a different build (SHA-256 {0}...).'
        rtWillCopy = 'Will copy this file into OBS: {0}'
        rtCopied   = 'NR model file copied: {0}'
        rtSkipped  = 'Continuing without the NR model file. Put it here later: {0}'
        srOk       = 'DLSS 4.5 SR runtime OK: {0}'
        srPrompt   = 'Download official nvngx_dlss.dll (~56 MB) from NVIDIA GitHub for 1440p/4K DLSS 4.5 SR? (Y/n)'
        srDown     = 'Downloading official nvngx_dlss.dll from NVIDIA GitHub...'
        srSigOk    = 'Verified NVIDIA signature on nvngx_dlss.dll'
        srFail     = 'Skipped DLSS 4.5 SR download ({0}). Bilinear+CAS upscaling still works.'
        srCopied   = 'DLSS 4.5 SR runtime copied: {0}'
        vfgOk      = 'NVIDIA VFG runtime OK: {0}'
        vfgLocal   = 'Reusing 5 NVIDIA-signed VFG DLLs from local Veyra install.'
        vfgPrompt  = 'Download official NVIDIA VFG runtime (extracts 5 signed DLLs from ~415 MB pypi.nvidia.com wheel, RTX 40/50 preview)? (Y/n)'
        vfgDown    = 'Downloading nvidia-vfx 0.2.0.0 from pypi.nvidia.com (~415 MB)...'
        vfgVerify  = 'Verifying wheel SHA-256 and extracting 5 VFG DLLs...'
        vfgFail    = 'Skipped NVIDIA VFG download ({0}). Native NR and upscaling still work.'
        vfgCopied  = 'NVIDIA VFG runtime (5 DLLs) copied: {0}'
        backup     = 'Backup: {0}'
        copy       = 'Installed: {0}'
        verifyFail = 'Check failed after copying: {0}'
        rolledBack = 'The install was rolled back.'
        done       = 'Install complete'
        next1      = '1. Start OBS.'
        next2      = '2. Right-click a source > Filters > Effect Filters [+] > Native NR (NVIDIA VFG available on preview window)'
        next3      = '3. When the filter properties show "Status: NR is being applied", it works.'
        pathNote   = 'OBS is not in the default folder, so one more step: put this path into "NR runtime file" in the filter properties, then remove the filter and add it again:'
        uninstHint = 'To uninstall, run uninstall.bat in this folder.'
        uNoState   = 'No install record, removing only Native NR files.'
        uRemoved   = 'Removed: {0}'
        uChanged   = 'Left in place (changed after install): {0}'
        uRestored  = 'Restored: {0}'
        uDone      = 'Uninstall complete'
        cancelled  = 'Cancelled.'
        error      = 'Error: {0}'
        log        = 'Log file: {0}'
        pause      = 'Press Enter to close'
    }
}

function T([string]$Key, [object[]]$Arg = @()) {
    $s = $Text[$script:L][$Key]
    if ($Arg.Count) { $s = $s -f $Arg }
    return $s
}
function Say([string]$Key, [object[]]$Arg = @(), [string]$Color = 'Gray') {
    Write-Host (T $Key $Arg) -ForegroundColor $Color
}
function Write-Title([string]$Line) {
    Write-Host ''
    Write-Host ('  ' + $Line) -ForegroundColor Cyan
    Write-Host '  github.com/grim-susemi/obs-nr-native-filter' -ForegroundColor DarkGray
}
function Write-Step([int]$N, [int]$Total, [string]$Label) {
    Write-Host ''
    Write-Host ('[{0}/{1}] {2}' -f $N, $Total, $Label) -ForegroundColor White
}
function Get-Sha([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Get-FullPath([string]$Path) {
    return [IO.Path]::GetFullPath(($Path.Trim().Trim('"') -replace '/', '\')).TrimEnd('\')
}
function Test-ObsDir([string]$Dir) {
    return [bool]($Dir -and (Test-Path -LiteralPath (Join-Path $Dir 'bin\64bit\obs64.exe') -PathType Leaf))
}
function Test-SamePath([string]$A, [string]$B) {
    return [string]::Equals($A.TrimEnd('\'), $B.TrimEnd('\'), [StringComparison]::OrdinalIgnoreCase)
}
function Test-NvidiaSigned([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    try {
        $sig = Get-AuthenticodeSignature -LiteralPath $Path
        return $sig.Status -eq 'Valid' -and $sig.SignerCertificate -and ($sig.SignerCertificate.Subject -match 'NVIDIA Corporation')
    } catch { return $false }
}
function Test-VfgDir([string]$Dir) {
    if (-not $Dir -or -not (Test-Path -LiteralPath $Dir -PathType Container)) { return $false }
    foreach ($n in $VfgDlls) {
        if (-not (Test-NvidiaSigned (Join-Path $Dir $n))) { return $false }
    }
    return $true
}
function Get-HttpFile([string]$Url, [string]$Dest) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Dest) | Out-Null
    $curl = Get-Command curl.exe -ErrorAction SilentlyContinue
    if ($curl) {
        & $curl.Source -fsSL --retry 2 -o $Dest $Url
        if ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $Dest -PathType Leaf)) { return }
    }
    Invoke-WebRequest -Uri $Url -OutFile $Dest -UseBasicParsing
}

function Get-Manifest {
    $file = Join-Path $Payload 'manifest.sha256'
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw (T 'payloadBad' @('files\manifest.sha256')) }
    $m = [ordered]@{}
    foreach ($line in [IO.File]::ReadAllLines($file)) {
        if ($line -match '^([0-9a-f]{64})\s+(\S.*)$') { $m[$Matches[2].Trim()] = $Matches[1] }
    }
    if ($m.Count -eq 0) { throw (T 'payloadBad' @('files\manifest.sha256')) }
    foreach ($rel in @($m.Keys)) {
        $src = Join-Path $Payload $rel
        if (-not (Test-Path -LiteralPath $src -PathType Leaf) -or (Get-Sha $src) -ne $m[$rel]) { throw (T 'payloadBad' @($rel)) }
    }
    return $m
}

function Find-ObsDirs {
    $candidates = New-Object System.Collections.Generic.List[string]
    foreach ($key in 'HKLM:\SOFTWARE\OBS Studio', 'HKLM:\SOFTWARE\WOW6432Node\OBS Studio', 'HKCU:\SOFTWARE\OBS Studio') {
        try { $v = (Get-ItemProperty -LiteralPath $key -ErrorAction Stop).'(default)'; if ($v) { $candidates.Add($v) } } catch {}
    }
    foreach ($key in 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\OBS Studio', 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\OBS Studio') {
        try {
            $p = Get-ItemProperty -LiteralPath $key -ErrorAction Stop
            if ($p.InstallLocation) { $candidates.Add($p.InstallLocation) }
            elseif ($p.UninstallString) { $candidates.Add((Split-Path -Parent ($p.UninstallString.Trim('"')))) }
        } catch {}
    }
    try {
        $steam = (Get-ItemProperty -LiteralPath 'HKCU:\Software\Valve\Steam' -ErrorAction Stop).SteamPath
        if ($steam) { $candidates.Add((Join-Path $steam 'steamapps\common\OBS Studio')) }
    } catch {}
    $candidates.Add($DefaultObs)
    $seen = @{}
    foreach ($c in $candidates) {
        try { $full = Get-FullPath $c } catch { continue }
        $k = $full.ToLowerInvariant()
        if ($seen.ContainsKey($k)) { continue }
        $seen[$k] = $true
        if (Test-ObsDir $full) { $full }
    }
}

function Select-Obs([string]$Hint) {
    if ($ObsDir) {
        $d = Get-FullPath $ObsDir
        if (-not (Test-ObsDir $d)) { throw (T 'obsInvalid' @($d)) }
        Say 'obsFound' @($d) 'Cyan'
        return $d
    }
    $list = @(if ($Hint) { $Hint } else { Find-ObsDirs })
    if ($list.Count -eq 1) {
        Say 'obsFound' @($list[0]) 'Cyan'
        if ($Yes) { return $list[0] }
        $a = (Read-Host (T 'obsConfirm')).Trim()
        if (-not $a -or $a -match '^(y|yes|ㅛ|예|네)$') { return $list[0] }
    } elseif ($list.Count -gt 1) {
        if ($Yes) { Say 'obsFound' @($list[0]) 'Cyan'; return $list[0] }
        Say 'obsMany' @() 'Cyan'
        for ($i = 0; $i -lt $list.Count; $i++) { Write-Host ('  {0}) {1}' -f ($i + 1), $list[$i]) }
        $n = (Read-Host '>').Trim()
        if ($n -match '^\d+$' -and [int]$n -ge 1 -and [int]$n -le $list.Count) { return $list[[int]$n - 1] }
    } else {
        Say 'obsNone' @() 'Yellow'
        if ($Yes) { throw (T 'obsNone') }
    }
    while ($true) {
        $p = (Read-Host (T 'obsAsk')).Trim()
        if (-not $p) { throw 'cancel' }
        $d = Get-FullPath $p
        if (Test-ObsDir $d) { return $d }
        Say 'obsInvalid' @($d) 'Red'
    }
}

function Show-ObsVersion([string]$Dir) {
    $v = ''
    try { $v = (Get-Item -LiteralPath (Join-Path $Dir 'bin\64bit\obs64.exe')).VersionInfo.ProductVersion } catch {}
    if ($v) {
        Say 'obsVersion' @($v) 'Gray'
        if ($v -notmatch '^32\.') { Say 'obsOld' @($v) 'Yellow' }
    }
}

# An OBS whose path cannot be read (for example one running as admin) counts as this OBS.
function Test-ObsRunning([string]$Dir) {
    $exe = Join-Path $Dir 'bin\64bit\obs64.exe'
    foreach ($p in @(Get-Process -Name obs64, obs32, obs-nr-fg-presenter -ErrorAction SilentlyContinue)) {
        $path = $null
        try { $path = $p.Path } catch {}
        if (-not $path -or (Test-SamePath $path $exe) -or $p.ProcessName -eq 'obs-nr-fg-presenter') { return $true }
    }
    return $false
}
function Wait-ObsClosed([string]$Dir) {
    while (Test-ObsRunning $Dir) {
        if ($Yes) { throw (T 'obsRunStop') }
        Say 'obsRunning' @() 'Yellow'
        [void](Read-Host)
    }
}

function Test-Writable([string]$Dir) {
    foreach ($sub in 'obs-plugins\64bit', 'bin\64bit') {
        $p = Join-Path $Dir $sub
        if (-not (Test-Path -LiteralPath $p)) { $p = $Dir }
        $probe = Join-Path $p ('.nr-write-test-' + [guid]::NewGuid().ToString('N'))
        try { [IO.File]::WriteAllText($probe, 'x'); [IO.File]::Delete($probe) } catch { return $false }
    }
    return $true
}

function Invoke-Elevated([string]$Dir, [string]$RuntimeSource, [string]$SrSource, [string]$VfgSourceDir, [string]$StageDir) {
    Say 'admin' @() 'Yellow'
    $quote = { param($s) '"' + $s + '"' }
    $argv = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (& $quote $PSCommandPath),
        '-ObsDir', (& $quote $Dir), '-Lang', $script:L, '-StateDir', (& $quote $StateDir), '-RuntimeDecided')
    if ($RuntimeSource) { $argv += @('-Runtime', (& $quote $RuntimeSource)) }
    if ($SrSource) { $argv += @('-SrRuntime', (& $quote $SrSource)) }
    if ($VfgSourceDir) { $argv += @('-VfgRuntimeDir', (& $quote $VfgSourceDir)) }
    if ($StageDir) { $argv += @('-StageCleanupDir', (& $quote $StageDir)) }
    if ($Uninstall) { $argv += '-Uninstall' }
    if ($Yes) { $argv += '-Yes' }
    if ($NoPause) { $argv += '-NoPause' }
    try { $p = Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList $argv -Wait -PassThru }
    catch { Say 'adminFail' @() 'Red'; return 1 }
    $script:Relaunched = $true
    Say 'adminDone' @($p.ExitCode) 'Gray'
    return $p.ExitCode
}

function Resolve-Runtime([string]$Dir) {
    $target = Join-Path $Dir $RuntimeRel
    if ($Runtime) {
        $p = Get-FullPath $Runtime
        if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { throw (T 'rtNotFound' @($p)) }
        $h = Get-Sha $p
        if ($h -ne $RuntimeSha) { throw ((T 'rtBadHash' @($h.Substring(0, 16))) + ' ' + (T 'rtNeed')) }
        if (Test-SamePath $p $target) { Say 'rtOk' @($target) 'Green'; return '' }
        Say 'rtWillCopy' @($p) 'Green'
        return $p
    }
    if (Test-Path -LiteralPath $target -PathType Leaf) {
        $h = Get-Sha $target
        if ($h -eq $RuntimeSha) { Say 'rtOk' @($target) 'Green'; return '' }
        Say 'rtWrong' @($h.Substring(0, 16)) 'Yellow'
    } else {
        Say 'rtMissing' @() 'Yellow'
    }
    Say 'rtNeed' @() 'Gray'
    if ($Yes -or $RuntimeDecided) { Say 'rtSkipped' @($target) 'Yellow'; return '' }
    for ($i = 0; $i -lt 3; $i++) {
        $answer = (Read-Host (T 'rtAsk')).Trim()
        if (-not $answer) { break }
        $p = Get-FullPath $answer
        if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { Say 'rtNotFound' @($p) 'Red'; continue }
        $h = Get-Sha $p
        if ($h -ne $RuntimeSha) { Say 'rtBadHash' @($h.Substring(0, 16)) 'Red'; continue }
        Say 'rtWillCopy' @($p) 'Green'
        return $p
    }
    Say 'rtSkipped' @($target) 'Yellow'
    return ''
}

function Resolve-OptionalRuntimes([string]$Dir, [string]$StageDir) {
    $out = @{ sr = ''; vfg = '' }
    if ($SrRuntime) {
        $p = Get-FullPath $SrRuntime
        if (Test-NvidiaSigned $p) { $out.sr = $p }
    }
    if ($VfgRuntimeDir) {
        $d = Get-FullPath $VfgRuntimeDir
        if (Test-VfgDir $d) { $out.vfg = $d }
    }
    if ($RuntimeDecided -or $SkipOptional) { return $out }

    $allowDownload = $DownloadOptional -or (-not $Yes)
    if (-not $out.sr) {
        $srTarget = Join-Path $Dir $SrRel
        if (Test-NvidiaSigned $srTarget) {
            Say 'srOk' @($srTarget) 'Green'
        } elseif ($allowDownload) {
            $doSr = $true
            if (-not $Yes) {
                $ans = (Read-Host (T 'srPrompt')).Trim()
                $doSr = (-not $ans) -or ($ans -match '^(y|yes|ㅛ|예|네)$')
            }
            if ($doSr) {
                try {
                    Say 'srDown' @() 'Cyan'
                    $dl = Join-Path $StageDir 'nvngx_dlss.dll'
                    Get-HttpFile $SrUrl $dl
                    if (-not (Test-NvidiaSigned $dl)) { throw 'Authenticode signature check failed' }
                    Say 'srSigOk' @() 'Green'
                    $out.sr = $dl
                } catch {
                    Say 'srFail' @($_.Exception.Message) 'Yellow'
                }
            }
        }
    }

    if (-not $out.vfg) {
        $vfgTarget = Join-Path $Dir $VfgDirRel
        if (Test-VfgDir $vfgTarget) {
            Say 'vfgOk' @($vfgTarget) 'Green'
        } else {
            $veyraCand = Join-Path $env:LOCALAPPDATA 'Programs\Veyra-2.1.9-NVIDIA-Virtual-Camera\Veyra-2.1.9-virtual-camera-NVIDIA-win64-portable\runtime\nvidia-vfg'
            if (Test-VfgDir $veyraCand) {
                Say 'vfgLocal' @() 'Green'
                $out.vfg = $veyraCand
            } elseif ($allowDownload) {
                $doVfg = $true
                if (-not $Yes) {
                    $ans = (Read-Host (T 'vfgPrompt')).Trim()
                    $doVfg = (-not $ans) -or ($ans -match '^(y|yes|ㅛ|예|네)$')
                }
                if ($doVfg) {
                    $whl = Join-Path $StageDir 'nvidia_vfx.whl'
                    try {
                        Say 'vfgDown' @() 'Cyan'
                        Get-HttpFile $VfgWhlUrl $whl
                        Say 'vfgVerify' @() 'Cyan'
                        if ((Get-Sha $whl) -ne $VfgWhlSha) { throw 'wheel SHA-256 mismatch' }
                        Add-Type -AssemblyName System.IO.Compression.FileSystem
                        $vfgStage = Join-Path $StageDir 'nvidia-vfg'
                        New-Item -ItemType Directory -Force -Path $vfgStage | Out-Null
                        $zip = [IO.Compression.ZipFile]::OpenRead($whl)
                        try {
                            foreach ($dll in $VfgDlls) {
                                $entry = $zip.GetEntry("nvvfx/libs/$dll")
                                if (-not $entry) { throw "Missing $dll inside wheel" }
                                $dst = Join-Path $vfgStage $dll
                                $inS = $entry.Open()
                                $outS = [IO.File]::Create($dst)
                                try { $inS.CopyTo($outS) } finally { $outS.Dispose(); $inS.Dispose() }
                                if (-not (Test-NvidiaSigned $dst)) { throw "Signature check failed for $dll" }
                            }
                        } finally { $zip.Dispose() }
                        $out.vfg = $vfgStage
                    } catch {
                        Say 'vfgFail' @($_.Exception.Message) 'Yellow'
                    } finally {
                        Remove-Item -LiteralPath $whl -Force -ErrorAction SilentlyContinue
                    }
                }
            }
        }
    }
    return $out
}

function Copy-WithDir([string]$From, [string]$To) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $To) | Out-Null
    Copy-Item -LiteralPath $From -Destination $To -Force
}

function Install-Files([string]$Dir, [string]$RuntimeSource, [string]$SrSource, [string]$VfgSourceDir, $Manifest) {
    $backupDir = Join-Path $StateDir ('backup-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
    $rels = @($Manifest.Keys)
    $backedUp = @(); $created = @(); $runtimeBackedUp = $false; $runtimeCopied = $false
    $srCopied = $false; $vfgCopied = @()
    $runtimeTarget = Join-Path $Dir $RuntimeRel
    $srTarget = Join-Path $Dir $SrRel
    $vfgTargetDir = Join-Path $Dir $VfgDirRel
    try {
        foreach ($rel in $rels) {
            $dst = Join-Path $Dir $rel
            if (Test-Path -LiteralPath $dst -PathType Leaf) {
                if ((Get-Sha $dst) -ne $Manifest[$rel]) {
                    Copy-WithDir $dst (Join-Path $backupDir $rel)
                    $backedUp += $rel
                    Say 'backup' @($rel) 'DarkGray'
                }
            } else {
                $created += $rel
            }
        }
        if ($RuntimeSource -and (Test-Path -LiteralPath $runtimeTarget -PathType Leaf)) {
            Copy-WithDir $runtimeTarget (Join-Path $backupDir $RuntimeRel)
            $runtimeBackedUp = $true
            Say 'backup' @($RuntimeRel) 'DarkGray'
        }
        foreach ($rel in $rels) {
            $dst = Join-Path $Dir $rel
            Copy-WithDir (Join-Path $Payload $rel) $dst
            if ((Get-Sha $dst) -ne $Manifest[$rel]) { throw (T 'verifyFail' @($rel)) }
            Say 'copy' @($rel) 'Gray'
        }
        if ($RuntimeSource) {
            Copy-WithDir $RuntimeSource $runtimeTarget
            if ((Get-Sha $runtimeTarget) -ne $RuntimeSha) { throw (T 'verifyFail' @($RuntimeRel)) }
            $runtimeCopied = $true
            Say 'rtCopied' @($runtimeTarget) 'Green'
        }
        if ($SrSource) {
            $srHash = Get-Sha $SrSource
            Copy-WithDir $SrSource $srTarget
            if ((Get-Sha $srTarget) -ne $srHash) { throw (T 'verifyFail' @($SrRel)) }
            $srCopied = $true
            Say 'srCopied' @($srTarget) 'Green'
        }
        if ($VfgSourceDir) {
            foreach ($dll in $VfgDlls) {
                $src = Join-Path $VfgSourceDir $dll
                $dst = Join-Path $vfgTargetDir $dll
                $h = Get-Sha $src
                Copy-WithDir $src $dst
                if ((Get-Sha $dst) -ne $h) { throw (T 'verifyFail' @("$VfgDirRel\$dll")) }
                $vfgCopied += $dll
            }
            Say 'vfgCopied' @($vfgTargetDir) 'Green'
        }
    } catch {
        foreach ($rel in $backedUp) { try { Copy-WithDir (Join-Path $backupDir $rel) (Join-Path $Dir $rel) } catch {} }
        foreach ($rel in $created) { Remove-Item -LiteralPath (Join-Path $Dir $rel) -Force -ErrorAction SilentlyContinue }
        if ($runtimeBackedUp) { try { Copy-WithDir (Join-Path $backupDir $RuntimeRel) $runtimeTarget } catch {} }
        elseif ($RuntimeSource) { Remove-Item -LiteralPath $runtimeTarget -Force -ErrorAction SilentlyContinue }
        if ($srCopied) { Remove-Item -LiteralPath $srTarget -Force -ErrorAction SilentlyContinue }
        foreach ($dll in $vfgCopied) { Remove-Item -LiteralPath (Join-Path $vfgTargetDir $dll) -Force -ErrorAction SilentlyContinue }
        Say 'rolledBack' @() 'Yellow'
        throw
    }
    $state = [ordered]@{
        version = $Version; obsDir = $Dir; installedAt = (Get-Date).ToString('s'); backupDir = $backupDir
        files = $rels; backedUp = $backedUp; runtimeCopied = $runtimeCopied; runtimeBackedUp = $runtimeBackedUp
        srCopied = $srCopied; vfgCopied = $vfgCopied
    }
    New-Item -ItemType Directory -Force -Path $StateDir | Out-Null
    ($state | ConvertTo-Json -Depth 4) | Set-Content -LiteralPath (Join-Path $StateDir 'install-state.json') -Encoding UTF8
}

function Read-State {
    $file = Join-Path $StateDir 'install-state.json'
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { return $null }
    try { return (Get-Content -LiteralPath $file -Raw | ConvertFrom-Json) } catch { return $null }
}

function Remove-Files([string]$Dir, $Manifest, $State) {
    $useState = [bool]($State -and $State.obsDir -and (Test-SamePath ([string]$State.obsDir) $Dir))
    if (-not $useState) { Say 'uNoState' @() 'Yellow' }
    foreach ($rel in @($Manifest.Keys)) {
        $dst = Join-Path $Dir $rel
        if (Test-Path -LiteralPath $dst -PathType Leaf) {
            if ((Get-Sha $dst) -eq $Manifest[$rel]) { Remove-Item -LiteralPath $dst -Force; Say 'uRemoved' @($rel) 'Gray' }
            else { Say 'uChanged' @($rel) 'Yellow' }
        }
    }
    if ($useState) {
        foreach ($rel in @($State.backedUp)) {
            if (-not $rel) { continue }
            $from = Join-Path $State.backupDir $rel
            $dst = Join-Path $Dir $rel
            if ((Test-Path -LiteralPath $from -PathType Leaf) -and -not (Test-Path -LiteralPath $dst)) {
                Copy-WithDir $from $dst
                Say 'uRestored' @($rel) 'Green'
            }
        }
        if ($State.runtimeCopied) {
            $rt = Join-Path $Dir $RuntimeRel
            if ((Test-Path -LiteralPath $rt -PathType Leaf) -and (Get-Sha $rt) -eq $RuntimeSha) {
                Remove-Item -LiteralPath $rt -Force
                Say 'uRemoved' @($RuntimeRel) 'Gray'
            }
            $from = Join-Path $State.backupDir $RuntimeRel
            if ($State.runtimeBackedUp -and (Test-Path -LiteralPath $from -PathType Leaf) -and -not (Test-Path -LiteralPath $rt)) {
                Copy-WithDir $from $rt
                Say 'uRestored' @($RuntimeRel) 'Green'
            }
        }
        if ($State.PSObject.Properties['srCopied'] -and $State.srCopied) {
            $sr = Join-Path $Dir $SrRel
            if (Test-Path -LiteralPath $sr -PathType Leaf) {
                Remove-Item -LiteralPath $sr -Force
                Say 'uRemoved' @($SrRel) 'Gray'
            }
        }
        if ($State.PSObject.Properties['vfgCopied'] -and $State.vfgCopied) {
            foreach ($dll in @($State.vfgCopied)) {
                $p = Join-Path (Join-Path $Dir $VfgDirRel) $dll
                if (Test-Path -LiteralPath $p -PathType Leaf) {
                    Remove-Item -LiteralPath $p -Force
                    Say 'uRemoved' @("$VfgDirRel\$dll") 'Gray'
                }
            }
        }
        Remove-Item -LiteralPath (Join-Path $StateDir 'install-state.json') -Force
    }
    foreach ($sub in 'data\obs-plugins\obs-nr-filter\locale', 'data\obs-plugins\obs-nr-filter', $VfgDirRel, 'bin\64bit\dlss') {
        $p = Join-Path $Dir $sub
        if ((Test-Path -LiteralPath $p) -and -not (Get-ChildItem -LiteralPath $p -Force | Select-Object -First 1)) {
            Remove-Item -LiteralPath $p -Force
        }
    }
}

function Invoke-Install {
    Write-Title (T 'title' @($Version))
    Write-Step 1 5 (T 'sPayload')
    $manifest = Get-Manifest
    Say 'payloadOk' @($manifest.Count) 'Green'
    Write-Step 2 5 (T 'sObs')
    $dir = Select-Obs ''
    Show-ObsVersion $dir
    Wait-ObsClosed $dir
    Write-Step 3 5 (T 'sRuntime')
    $runtimeSource = Resolve-Runtime $dir
    Write-Step 4 5 (T 'sOpt')
    $stageDir = Join-Path $env:TEMP ('obs-nr-stage-' + [guid]::NewGuid().ToString('N'))
    $opt = Resolve-OptionalRuntimes $dir $stageDir
    if (-not (Test-Writable $dir)) { return (Invoke-Elevated $dir $runtimeSource $opt.sr $opt.vfg $stageDir) }
    Write-Step 5 5 (T 'sCopy')
    try {
        Install-Files $dir $runtimeSource $opt.sr $opt.vfg $manifest
    } finally {
        if (Test-Path -LiteralPath $stageDir) { Remove-Item -LiteralPath $stageDir -Recurse -Force -ErrorAction SilentlyContinue }
        if ($StageCleanupDir -and (Test-Path -LiteralPath $StageCleanupDir) -and ([IO.Path]::GetFileName($StageCleanupDir) -like 'obs-nr-stage-*')) {
            Remove-Item -LiteralPath $StageCleanupDir -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
    Write-Host ''
    Say 'done' @() 'Green'
    Say 'next1' @() 'White'
    Say 'next2' @() 'White'
    Say 'next3' @() 'White'
    if (-not (Test-SamePath $dir $DefaultObs)) {
        Write-Host ''
        Say 'pathNote' @() 'Yellow'
        Write-Host ('  ' + (Join-Path $dir $RuntimeRel)) -ForegroundColor White
    }
    Write-Host ''
    Say 'uninstHint' @() 'DarkGray'
    return 0
}

function Invoke-Uninstall {
    Write-Title (T 'titleU' @($Version))
    Write-Step 1 3 (T 'sPayload')
    $manifest = Get-Manifest
    Say 'payloadOk' @($manifest.Count) 'Green'
    Write-Step 2 3 (T 'sObs')
    $state = Read-State
    $hint = ''
    if (-not $ObsDir -and $state -and (Test-ObsDir ([string]$state.obsDir))) { $hint = [string]$state.obsDir }
    $dir = Select-Obs $hint
    Wait-ObsClosed $dir
    if (-not (Test-Writable $dir)) { return (Invoke-Elevated $dir '' '' '' '') }
    Write-Step 3 3 (T 'sRemove')
    Remove-Files $dir $manifest $state
    Write-Host ''
    Say 'uDone' @() 'Green'
    return 0
}

$script:L = $Lang
$script:Relaunched = $false
if (-not $script:L) {
    if ($Yes) { $script:L = 'ko' }
    else {
        Write-Host ''
        Write-Host '  1) 한국어    2) English' -ForegroundColor Cyan
        $script:L = if ((Read-Host '  언어 / Language [1]').Trim() -eq '2') { 'en' } else { 'ko' }
    }
}
$code = 0
$log = ''
try {
    New-Item -ItemType Directory -Force -Path $StateDir | Out-Null
    $log = Join-Path $StateDir ('log-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + $(if ($Uninstall) { '-uninstall' } else { '' }) + '.txt')
    try { Start-Transcript -LiteralPath $log -Append | Out-Null } catch { $log = '' }
    if ($Uninstall) { $code = Invoke-Uninstall } else { $code = Invoke-Install }
} catch {
    if ($_.Exception.Message -eq 'cancel') { Say 'cancelled' @() 'Yellow'; $code = 2 }
    else { Say 'error' @($_.Exception.Message) 'Red'; $code = 1 }
} finally {
    if ($log) {
        try { Stop-Transcript | Out-Null } catch {}
        if (-not $script:Relaunched) { Say 'log' @($log) 'DarkGray' }
    }
}
if (-not $NoPause -and -not $script:Relaunched) { Write-Host ''; [void](Read-Host (T 'pause')) }
exit $code
