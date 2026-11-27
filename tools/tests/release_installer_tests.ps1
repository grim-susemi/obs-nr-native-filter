$ErrorActionPreference = 'Stop'
$installer = Join-Path $PSScriptRoot '..\..\packaging\windows\install.ps1'
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile((Resolve-Path $installer), [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw $errors[0] }
foreach ($f in $ast.FindAll({ param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] }, $true)) {
    . ([scriptblock]::Create($f.Extent.Text))
}
function Say { }
function Assert-Equal($actual, $expected, $message) {
    if ($actual -cne $expected) { throw "$message : expected '$expected', got '$actual'" }
}
$root = Join-Path $env:TEMP ('nr-installer-test-' + [guid]::NewGuid().ToString('N'))
$script:StateDir = Join-Path $root 'state'
$script:Payload = Join-Path $root 'payload'
$script:Version = 'test'
$script:RuntimeRel = 'bin\64bit\nvngx_dlssnr.dll'
$script:SrRel = 'bin\64bit\dlss\nvngx_dlss.dll'
$script:VfgDirRel = 'bin\64bit\nvidia-vfg'
$script:VfgDlls = @('one.dll', 'two.dll')
$dir = Join-Path $root 'obs'
$rel = 'obs-plugins\64bit\obs-nr-filter.dll'
function Put($path, $value) { New-Item -ItemType Directory -Force (Split-Path -Parent $path) | Out-Null; [IO.File]::WriteAllText($path, $value) }
function Assert-Rejected([scriptblock]$Action, [string]$Reason) {
    try { & $Action } catch {
        Assert-Equal $_.Exception.Message $Reason 'unexpected rejection reason'
        return
    }
    throw "expected rejection: $Reason"
}
function Test-PushFlowOwnership {
    $obs = Join-Path $root 'pushflow-obs'
    $StateDir = Join-Path $root 'pushflow-ownership-state'
    $runtime = Join-Path $root 'pushflow-runtime'
    $models = Join-Path $root 'pushflow-models'
    $push = @{ runtime = $runtime; models = $models }
    $manifest = [ordered]@{}
    $dll = 'bin\64bit\pushflow\DirectML.dll'
    $model = 'bin\64bit\pushflow\models\pushflow-fast.onnx'
    $facts = "$model.txt"
    $ownedFiles = @('bin\64bit\pushflow\onnxruntime.dll', $dll, $model, $facts)
    function Test-MicrosoftSigned { return $true } # Authenticode is exercised separately from ownership.
    Put (Join-Path $runtime 'onnxruntime.dll') 'ORT'
    Put (Join-Path $runtime 'DirectML.dll') 'DML'
    Put (Join-Path $models 'pushflow-fast.onnx') 'first'
    Put (Join-Path $models 'pushflow-fast.onnx.txt') 'metadata'

    Install-Files $obs '' '' '' $manifest $push
    $state = Read-State
    Assert-Equal (($state.ownedFiles.relative | Sort-Object) -join ',') (($ownedFiles | Sort-Object) -join ',') 'PushFlow owned files'
    foreach ($entry in $state.ownedFiles) {
        Assert-Equal $entry.hash (Get-Sha (Join-Path $obs $entry.relative)) 'receipt must record installed bytes'
    }
    Remove-Files $obs $manifest $state
    Assert-Equal (Test-Path -LiteralPath (Join-Path $obs 'bin\64bit\pushflow')) $false 'clean uninstall must remove model child and runtime parent'
    Write-Host 'PASS PushFlow receipt and child-first uninstall'

    Put (Join-Path $obs $dll) 'foreign DML'
    Install-Files $obs '' '' '' $manifest $push
    Put (Join-Path $obs $model) 'external model edit'
    Put (Join-Path $models 'pushflow-fast.onnx') 'second'
    Put (Join-Path $models 'pushflow-fast.onnx.txt') 'updated metadata'
    Install-Files $obs '' '' '' $manifest $push
    Assert-Equal ([IO.File]::ReadAllText((Join-Path $obs $model))) 'external model edit' 'upgrade must preserve changed model'
    Assert-Equal ([IO.File]::ReadAllText((Join-Path $obs $facts))) 'updated metadata' 'upgrade must replace unchanged metadata'
    Remove-Files $obs $manifest (Read-State)
    Assert-Equal ([IO.File]::ReadAllText((Join-Path $obs $model))) 'external model edit' 'uninstall must preserve changed model'
    Assert-Equal ([IO.File]::ReadAllText((Join-Path $obs $dll))) 'foreign DML' 'uninstall must restore foreign runtime'
    Write-Host 'PASS PushFlow upgrade and external edit preservation'

    Install-Files $obs '' '' '' $manifest $push
    $statePath = Join-Path $StateDir 'install-state.json'
    $oldReceipt = Get-Sha $statePath
    $priorHashes = @($ownedFiles | ForEach-Object { Get-Sha (Join-Path $obs $_) })
    Put (Join-Path $runtime 'DirectML.dll') 'new DML'
    Put (Join-Path $models 'new.onnx') 'new model'
    Put (Join-Path $models 'pushflow-fast.onnx.txt') 'revised metadata'
    $realCopy = ${function:Copy-WithDir}
    $failedSource = Join-Path $models 'new.onnx'
    function Copy-WithDir([string]$From, [string]$To) {
        if ($From -eq $failedSource) {
            Put $To 'partial'
            throw 'fixture PushFlow copy failure'
        }
        & $realCopy $From $To
    }
    Assert-Rejected { Install-Files $obs '' '' '' $manifest $push } 'fixture PushFlow copy failure'
    Assert-Equal (Get-Sha $statePath) $oldReceipt 'rollback must leave receipt intact'
    Assert-Equal (Test-Path -LiteralPath (Join-Path $obs 'bin\64bit\pushflow\models\new.onnx')) $false 'rollback must remove partial new model'
    Assert-Equal (($ownedFiles | ForEach-Object { Get-Sha (Join-Path $obs $_) }) -join ',') ($priorHashes -join ',') 'rollback must restore the complete installed set'
    Assert-Equal @(Get-ChildItem -LiteralPath $StateDir -Filter 'rollback-*').Count 0 'rollback must clean staging'
    Write-Host 'PASS PushFlow mid-copy rollback'

    $state = Read-State
    $state.ownedFiles += [pscustomobject]@{ relative = 'bin\64bit\pushflow\models\..\sentinel.onnx'; hash = 'unused'; backup = '' }
    Assert-Rejected { Remove-Files $obs $manifest $state } 'Invalid installer receipt path'
    Assert-Equal (($ownedFiles | ForEach-Object { Get-Sha (Join-Path $obs $_) }) -join ',') ($priorHashes -join ',') 'receipt rejection must precede all file changes'
    Assert-Equal (Get-Sha $statePath) $oldReceipt 'receipt rejection must not delete or replace state'
    Write-Host 'PASS PushFlow receipt path containment'
}

function Test-PushFlowStaging {
    $stage = Join-Path $root 'pushflow-staging'
    $assets = Join-Path $root 'pushflow-fixtures'
    New-Item -ItemType Directory -Path $assets | Out-Null
    $Yes = $true; $DownloadPushFlow = $true
    $PushFlowRuntimeDir = ''; $PushFlowModelsDir = ''
    $PushFlowModelZipUrl = ''; $PushFlowModelZipSha = ''
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    function New-FixtureZip($file, $members) {
        $archive = [IO.Compression.ZipFile]::Open($file, 'Create')
        try {
            foreach ($member in $members) {
                $writer = [IO.StreamWriter]::new($archive.CreateEntry($member).Open())
                try { $writer.Write("fixture $member") } finally { $writer.Dispose() }
            }
        } finally { $archive.Dispose() }
    }
    $ort = 'microsoft.ml.onnxruntime.directml.1.20.1.nupkg'
    $dml = 'microsoft.ai.directml.1.15.4.nupkg'
    New-FixtureZip (Join-Path $assets $ort) @('runtimes/win-x64/native/onnxruntime.dll', 'ignored/readme.txt')
    New-FixtureZip (Join-Path $assets $dml) @('bin/x64-win/DirectML.dll')
    New-FixtureZip (Join-Path $assets 'companion.zip') @('models/custom.onnx', 'models/custom.onnx.txt', 'RIFE-MIT.txt')
    $realSha = ${function:Get-Sha}
    function Get-Sha([string]$path) {
        switch ([IO.Path]::GetFileName($path)) {
            $ort { return '6763468507b7cfc777b1334b3e174c11a540ddacb7bd4354bc2e0ec89e56eec2' }
            $dml { return '4e7cb7ddce8cf837a7a75dc029209b520ca0101470fcdf275c1f49736a3615b9' }
            'rife_v4.25_lite.7z' { return '7d53e29fff5e67345b19f4ce97dfd1e34b490eedc752df2236904fda1a13842c' }
            'rife_v4.26.7z' { return 'dfdabd84a2a3db773f87604b8cc255e94a6a72f13550d910ccd3b4ee2606cd4f' }
            default { return (& $realSha $path) }
        }
    }
    function Get-HttpFile([string]$Url, [string]$Dest) {
        $uri = [uri]$Url
        if ($uri.Host -eq 'github.com' -and $uri.AbsolutePath.StartsWith('/AmusementClub/vs-mlrt/releases/download/external-models/')) {
            Put $Dest "fixture $($uri.Segments[-1])"
        } elseif ($uri.Host -in 'api.nuget.org', 'fixture.invalid') {
            Copy-WithDir (Join-Path $assets $uri.Segments[-1]) $Dest
        } else { throw 'unexpected fixture download' }
    }
    $signatureStatus = 'Valid'
    function Get-AuthenticodeSignature([string]$LiteralPath) {
        return @{ Status = $signatureStatus; SignerCertificate = @{ Subject = 'CN=Microsoft Corporation, O=Microsoft Corporation, C=US' } }
    }
    function Expand-PinnedRifeModel([string]$Archive, [string]$Destination, $Model) {
        $fixture = [IO.File]::ReadAllText($Archive)
        Put (Join-Path $Destination $Model.output) $fixture
    }

    $resolved = Resolve-PushFlow (Join-Path $stage 'default')
    Assert-Equal ([IO.File]::ReadAllText((Join-Path $resolved.runtime 'DirectML.dll'))) 'fixture bin/x64-win/DirectML.dll' 'only x64 DML member'
    Assert-Equal ([IO.File]::ReadAllText((Join-Path $resolved.runtime 'onnxruntime.dll'))) 'fixture runtimes/win-x64/native/onnxruntime.dll' 'only x64 ORT member'
    Assert-Equal ([IO.File]::ReadAllText((Join-Path $resolved.models 'pushflow-fast.onnx'))) 'fixture rife_v4.25_lite.7z' 'default fast model'
    Assert-Equal ([IO.File]::ReadAllText((Join-Path $resolved.models 'pushflow-quality.onnx'))) 'fixture rife_v4.26.7z' 'default quality model'
    Write-Host 'PASS PushFlow optional runtime and default model staging (mocked downloads)'

    $DownloadPushFlow = $false
    $PushFlowModelZipUrl = 'https://fixture.invalid/companion.zip'
    $PushFlowModelZipSha = '0' * 64
    Assert-Rejected { Resolve-PushFlow (Join-Path $stage 'bad-hash') } 'PushFlow download SHA-256 mismatch'
    Assert-Equal (Test-Path -LiteralPath (Join-Path $stage 'bad-hash\models')) $false 'hash failure must write no extracted model'
    $PushFlowModelZipSha = Get-Sha (Join-Path $assets 'companion.zip')
    $companion = Resolve-PushFlow (Join-Path $stage 'companion')
    Assert-Equal ((Get-ChildItem -LiteralPath $companion.models -Name | Sort-Object) -join ',') 'custom.onnx,custom.onnx.txt' 'ZIP ships only models and sidecars to OBS'
    Assert-Equal ([IO.File]::ReadAllText((Join-Path $companion.models 'custom.onnx'))) 'fixture models/custom.onnx' 'benign control must extract the model bytes'

    $rejectedStage = Join-Path $stage 'rejected-companion'
    $rejectedModels = Join-Path $rejectedStage 'models'
    $sentinel = Join-Path $rejectedModels 'keep.onnx'
    Put $sentinel 'pre-existing model'
    & {
        $validatePath = ${function:Assert-ArchivePath}
        function Assert-ArchivePath([string]$Name) {
            # Reject the ordinary notice after both model entries have been seen.
            if ($Name -eq 'RIFE-MIT.txt') { throw 'fixture archive path rejected' }
            & $validatePath $Name
        }
        Assert-Rejected { Resolve-PushFlow $rejectedStage } 'fixture archive path rejected'
    }
    Assert-Equal ((Get-ChildItem -LiteralPath $rejectedModels -Force -Recurse -Name) -join ',') 'keep.onnx' 'rejected ZIP must produce no model or sidecar'
    Assert-Equal ([IO.File]::ReadAllText($sentinel)) 'pre-existing model' 'rejected ZIP must leave destination bytes unchanged'
    Write-Host 'PASS PushFlow benign ZIP control and extraction-boundary rejection'

    foreach ($name in @('/sandbox/readme.txt', 'models/../readme.txt')) {
        Assert-Rejected { Assert-ArchivePath $name } 'Invalid archive entry path'
    }
    Write-Host 'PASS PushFlow model ZIP pin and in-memory archive path rejection'

    $PushFlowModelZipUrl = ''; $PushFlowModelZipSha = ''
    $DownloadPushFlow = $true; $signatureStatus = 'NotSigned'
    Assert-Rejected { Resolve-PushFlow (Join-Path $stage 'bad-signature') } 'PushFlow runtime Microsoft signature verification failed'
    Assert-Equal (Test-Path -LiteralPath (Join-Path $stage 'bad-signature\models')) $false 'signature failure must not stage models'
    Write-Host 'PASS PushFlow Microsoft Authenticode gate (mocked signature status)'
}

function Test-PushFlowElevation {
    $dir = Join-Path $root 'OBS with spaces'
    $runtime = Join-Path $root 'runtime stage with spaces'
    $models = Join-Path $root 'model stage with spaces'
    $optional = @{ pushflow = @{ runtime = $runtime; models = $models } }
    $script:L = 'en'
    function Show-Screen { }
    function Start-Process($FilePath, $Verb, $ArgumentList, [switch]$Wait, [switch]$PassThru) {
        Assert-Equal $Verb 'RunAs' 'actual elevation is never invoked in this fixture'
        $forwarded = @{}
        $quotedPaths = [regex]::Matches(($ArgumentList -join ' '), ' (-PushFlowRuntimeDir|-PushFlowModelsDir) "([^"]+)"')
        foreach ($path in $quotedPaths) { $forwarded[$path.Groups[1].Value] = $path.Groups[2].Value }
        Assert-Equal $forwarded['-PushFlowRuntimeDir'] $runtime 'runtime path must reach elevated child as one argument'
        Assert-Equal $forwarded['-PushFlowModelsDir'] $models 'models path must reach elevated child as one argument'
        return @{ ExitCode = 42 }
    }
    # Invoke-Elevated catches launch errors, so an assertion in the mock returns 1 instead of 42.
    Assert-Equal (Invoke-Elevated $dir '' $optional (Join-Path $root 'staging root')) 42 'mocked elevated child must receive both staged directories'
    Write-Host 'PASS PushFlow UAC argument forwarding (mocked Start-Process)'
}
function Test-NrRuntimeAdmission {
    $sha3108 = 'e67dee209320cdafe0e93e45675d7aa34323a53acc57a72b2e40a181581c989a'
    $sha3109 = 'cdfa6c1ec784867182164c7836f324f087b5c9f374d0c9fe46f54999feabf3ed'
    $shaThird = '1111111111111111111111111111111111111111111111111111111111111111'
    $allowed = Get-AllowedRuntimeShas
    Assert-Equal $allowed.Count 2 'allowed runtime table must pin both 310.8 and 310.9'
    Assert-Equal $allowed[$sha3108] '310.8.0.0' '310.8 label'
    Assert-Equal $allowed[$sha3109] '310.9.16.0 (Model B)' '310.9 label'
    Assert-Equal $allowed.ContainsKey($shaThird) $false 'third hash must not be allowed'

    $nrRoot = Join-Path $root 'nr-admission'
    $obs = Join-Path $nrRoot 'obs'
    $StateDir = Join-Path $nrRoot 'state'
    $Payload = Join-Path $nrRoot 'payload'
    $sources = Join-Path $nrRoot 'sources'
    $rt3108 = Join-Path $sources 'nr-310.8.dll'
    $rt3109 = Join-Path $sources 'nr-310.9.dll'
    $rtThird = Join-Path $sources 'nr-third.dll'
    $rt221 = Join-Path $nrRoot 'localappdata\Programs\Veyra (NVIDIA)\versions\2.2.1\runtime\experimental\nr-ampere\nvngx_dlssnr.dll'
    $rt210 = Join-Path $nrRoot 'localappdata\Programs\Veyra (NVIDIA)\versions\2.10.0\runtime\experimental\nr-ampere\nvngx_dlssnr.dll'
    $rt211Bad = Join-Path $nrRoot 'localappdata\Programs\Veyra (NVIDIA)\versions\2.11.0\runtime\experimental\nr-ampere\nvngx_dlssnr.dll'

    Put (Join-Path $obs 'bin\64bit\obs64.exe') 'mock-obs'
    Put (Join-Path $Payload $rel) 'plugin-bytes'
    $manifest = [ordered]@{}
    $manifest[$rel] = (Get-Sha (Join-Path $Payload $rel))
    Put $rt3108 'fixture-310.8.0.0'
    Put $rt3109 'fixture-310.9.16.0'
    Put $rtThird 'fixture-third-build'
    Put $rt221 'veyra-2.2.1'
    Put $rt210 'veyra-2.10.0'
    Put $rt211Bad 'veyra-2.11.0-bad'

    $realSha = ${function:Get-Sha}
    $hashOverrides = @{
        $rt3108   = $sha3108
        $rt3109   = $sha3109
        $rtThird  = $shaThird
        $rt221    = $sha3109
        $rt210    = $sha3109
        $rt211Bad = $shaThird
    }
    function Get-Sha([string]$Path) {
        $full = Get-FullPath $Path
        if ($hashOverrides.ContainsKey($full)) { return $hashOverrides[$full] }
        $bytes = [IO.File]::ReadAllText($full)
        switch ($bytes) {
            'fixture-310.8.0.0'  { return $sha3108 }
            'fixture-310.9.16.0' { return $sha3109 }
            'veyra-2.10.0'       { return $sha3109 }
            'fixture-third-build'{ return $shaThird }
            default { return (& $realSha $Path) }
        }
    }

    $obsRt = Join-Path $obs $RuntimeRel
    $Runtime = ''; $Yes = $true; $RuntimeDecided = $false

    $prevLocalApp = $env:LOCALAPPDATA
    try {
        $env:LOCALAPPDATA = Join-Path $nrRoot 'localappdata'
        $pickedLocal = Find-LocalVeyraNrRuntime
        Assert-Equal (Get-FullPath $pickedLocal) (Get-FullPath $rt210) 'numeric version sort must pick 2.10.0 over 2.2.1 and skip unverified 2.11.0'
        Assert-Equal (Get-FullPath (Resolve-Runtime $obs)) (Get-FullPath $rt210) 'Resolve-Runtime must reuse verified local Veyra 310.9 when OBS lacks nvngx_dlssnr.dll'
        Write-Host 'PASS NR local Veyra runtime numeric version sort (2.10.0 > 2.2.1) and SHA-256 verification'
    } finally {
        $env:LOCALAPPDATA = $prevLocalApp
    }

    foreach ($case in @(
        @{ label = '310.8.0.0'; src = $rt3108; sha = $sha3108; expectedText = 'fixture-310.8.0.0' },
        @{ label = '310.9.16.0'; src = $rt3109; sha = $sha3109; expectedText = 'fixture-310.9.16.0' }
    )) {
        Put $obsRt 'foreign-nr-dll'
        $Runtime = $case.src
        $resolved = Resolve-Runtime $obs
        Assert-Equal (Get-FullPath $resolved) (Get-FullPath $case.src) "$($case.label) must pass Resolve-Runtime"
        Install-Files $obs $resolved '' '' $manifest @{}
        Assert-Equal ([IO.File]::ReadAllText($obsRt)) $case.expectedText "$($case.label) bytes must be installed"
        Assert-Equal (Get-Sha $obsRt) $case.sha "$($case.label) installed hash must match pinned hash"
        $receiptRt = @((Read-State).ownedFiles | Where-Object { $_.relative -eq $RuntimeRel })[0]
        Assert-Equal $receiptRt.hash $case.sha "$($case.label) receipt must record pinned hash"
        $Runtime = ''
        Assert-Equal (Resolve-Runtime $obs) '' "$($case.label) installed in OBS must validate in-place"
        Remove-Files $obs $manifest (Read-State)
        Assert-Equal ([IO.File]::ReadAllText($obsRt)) 'foreign-nr-dll' "$($case.label) uninstall must restore original foreign NR DLL"
        Write-Host "PASS NR runtime $($case.label) accepted and uninstall rollback verified"
    }

    Put $obsRt 'foreign-nr-dll'
    $Runtime = $rtThird
    $threwThird = $false
    try { [void](Resolve-Runtime $obs) } catch { $threwThird = $_.Exception.Message -match 'rtBadHash' }
    Assert-Equal $threwThird $true 'Resolve-Runtime must reject third SHA-256'
    $threwInstallThird = $false
    try { Install-Files $obs $rtThird '' '' $manifest @{} } catch { $threwInstallThird = $_.Exception.Message -match 'rtBadHash' }
    Assert-Equal $threwInstallThird $true 'Install-Files must reject third SHA-256'
    Assert-Equal ([IO.File]::ReadAllText($obsRt)) 'foreign-nr-dll' 'rejected third hash must leave mock OBS untouched'

    $Runtime = Join-Path $sources 'missing-truncated.dll'
    $threwMissing = $false
    try { [void](Resolve-Runtime $obs) } catch { $threwMissing = $_.Exception.Message -match 'rtNotFound' }
    Assert-Equal $threwMissing $true 'Resolve-Runtime must reject missing/invalid DLL path'
    Assert-Equal ([IO.File]::ReadAllText($obsRt)) 'foreign-nr-dll' 'invalid DLL path must leave mock OBS untouched'
    Write-Host 'PASS NR third hash and invalid path rejected with mock OBS untouched'

    $Runtime = $rt3109
    $realCopy = ${function:Copy-WithDir}
    function Copy-WithDir([string]$From, [string]$To) {
        if ($From -eq $rt3109) {
            & $realCopy $From $To
            throw 'injected failure after NR copy'
        }
        & $realCopy $From $To
    }
    Assert-Rejected { Install-Files $obs $rt3109 '' '' $manifest @{} } 'injected failure after NR copy'
    function Copy-WithDir([string]$From, [string]$To) { & $realCopy $From $To }
    Assert-Equal ([IO.File]::ReadAllText($obsRt)) 'foreign-nr-dll' 'mid-install rollback must restore foreign NR DLL'
    Assert-Equal (Test-Path -LiteralPath (Join-Path $obs $rel)) $false 'mid-install rollback must remove newly copied plugin'
    Assert-Equal @(Get-ChildItem -LiteralPath $StateDir -Filter 'rollback-*').Count 0 'rollback must clean staging'

    # Stale state rerun after rollback must succeed cleanly and preserve rollback target.
    Install-Files $obs $rt3109 '' '' $manifest @{}
    Assert-Equal (Get-Sha $obsRt) $sha3109 'rerun after rollback must install 310.9'
    Remove-Files $obs $manifest (Read-State)
    Assert-Equal ([IO.File]::ReadAllText($obsRt)) 'foreign-nr-dll' 'uninstall after post-rollback rerun must restore foreign NR DLL'
    Write-Host 'PASS NR rollback and stale-state rerun verified'
}

function Test-BundledNrRuntime {
    $sha3109 = 'cdfa6c1ec784867182164c7836f324f087b5c9f374d0c9fe46f54999feabf3ed'
    $shaThird = '1111111111111111111111111111111111111111111111111111111111111111'
    $nrRoot = Join-Path $root 'bundled-admission'
    $obs = Join-Path $nrRoot 'obs'
    $StateDir = Join-Path $nrRoot 'state'
    $Payload = Join-Path $nrRoot 'payload'
    $localApp = Join-Path $nrRoot 'localappdata'
    $veyra = Join-Path $localApp 'Programs\Veyra (NVIDIA)\versions\2.2.1\runtime\experimental\nr-ampere\nvngx_dlssnr.dll'
    $manifestFile = Join-Path $Payload 'manifest.sha256'
    Put (Join-Path $obs 'bin\64bit\obs64.exe') 'mock-obs'
    foreach ($rel in @(Get-PackagePaths)) { Put (Join-Path $Payload $rel) "fixture-$rel" }
    Put (Join-Path $Payload $RuntimeRel) 'fixture-bundled-310.9'
    Put $veyra 'veyra-310.9'

    $realSha = ${function:Get-Sha}
    $shaOverrides = @{
        (Get-FullPath (Join-Path $Payload $RuntimeRel)) = $sha3109
        (Get-FullPath $veyra) = $sha3109
    }
    function Get-Sha([string]$Path) {
        $full = Get-FullPath $Path
        if ($shaOverrides.ContainsKey($full)) { return $shaOverrides[$full] }
        if ([IO.File]::ReadAllText($full) -eq 'fixture-bundled-310.9') { return $sha3109 }
        return (& $realSha $Path)
    }
    function Write-FixtureManifest([bool]$withRuntime, [string]$runtimeHash = 'cdfa6c1ec784867182164c7836f324f087b5c9f374d0c9fe46f54999feabf3ed') {
        $rels = @(Get-PackagePaths)
        if ($withRuntime) { $rels += $RuntimeRel }
        $lines = foreach ($rel in $rels) { (Get-Sha (Join-Path $Payload $rel)) + '  ' + $rel }
        if ($withRuntime -and $runtimeHash -ne $sha3109) {
            $lines = @($lines | Where-Object { -not $_.EndsWith("  $RuntimeRel") })
            $lines += "$runtimeHash  $RuntimeRel"
        }
        [IO.File]::WriteAllLines($manifestFile, [string[]]$lines)
    }
    function Read-Host { throw 'bundled runtime must not prompt' }

    Assert-Equal (Test-PackagePath $RuntimeRel) $true 'bundled runtime is an allowed package path'
    Assert-Equal (Test-PackagePath 'bin\64bit\unapproved.dll') $false 'unapproved DLL is not a package path'

    Write-FixtureManifest $true
    $m = Get-Manifest
    Assert-Equal ([string]$m[$RuntimeRel]) $sha3109 'bundled runtime hash must come from the manifest'

    $prevLocalApp = $env:LOCALAPPDATA
    try {
        $env:LOCALAPPDATA = $localApp
        $Runtime = ''; $Yes = $false; $RuntimeDecided = $false
        $resolved = Resolve-Runtime $obs $m
        Assert-Equal (Get-FullPath $resolved) (Get-FullPath (Join-Path $Payload $RuntimeRel)) 'bundled runtime must win over local Veyra with no prompt'
    } finally { $env:LOCALAPPDATA = $prevLocalApp }

    Put (Join-Path $obs $RuntimeRel) 'foreign-nr'
    Install-Files $obs $resolved '' '' $m @{}
    Assert-Equal ([IO.File]::ReadAllText((Join-Path $obs $RuntimeRel))) 'fixture-bundled-310.9' 'bundled runtime bytes must be installed'
    $receipt = @((Read-State).ownedFiles | Where-Object { $_.relative -eq $RuntimeRel })[0]
    Assert-Equal ([string]$receipt.hash) $sha3109 'receipt must record the bundled hash'
    Remove-Files $obs $m (Read-State)
    Assert-Equal ([IO.File]::ReadAllText((Join-Path $obs $RuntimeRel))) 'foreign-nr' 'uninstall must restore the foreign NR runtime'
    Write-Host 'PASS bundled runtime selected, installed, and restored on uninstall'

    Write-FixtureManifest $true $shaThird
    $failure = $null
    try { [void](Get-Manifest) } catch { $failure = $_.Exception.Message }
    if ($failure -notmatch 'payloadBad') { throw "third bundled hash must be rejected as payloadBad, got '$failure'" }
    Write-FixtureManifest $false
    $m2 = Get-Manifest
    Assert-Equal $m2.Contains($RuntimeRel) $false 'lightweight manifest must not contain the runtime'
    $obsZero = Join-Path $nrRoot 'obs-zero'
    Put (Join-Path $obsZero 'bin\64bit\obs64.exe') 'mock-obs'
    $emptyLocal = Join-Path $nrRoot 'empty-localappdata'
    New-Item -ItemType Directory -Force -Path $emptyLocal | Out-Null
    $prevLocalApp = $env:LOCALAPPDATA
    try {
        $env:LOCALAPPDATA = $emptyLocal
        $Runtime = ''; $Yes = $true; $RuntimeDecided = $false
        Assert-Equal (Resolve-Runtime $obsZero $m2) '' 'lightweight package without a local runtime must skip it'
    } finally { $env:LOCALAPPDATA = $prevLocalApp }
    Write-Host 'PASS bundled third hash rejected and zero-runtime fallback retained'
}

function Test-ReleasePackaging {
    $makeRelease = (Resolve-Path (Join-Path $PSScriptRoot '..\..\packaging\windows\make-release.ps1')).Path
    $tokens = $null; $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($makeRelease, [ref]$tokens, [ref]$errors)
    if ($errors.Count) { throw $errors[0] }
    foreach ($f in $ast.FindAll({ param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] }, $true)) {
        . ([scriptblock]::Create($f.Extent.Text))
    }
    $plugin = 'files/obs-plugins/64bit/obs-nr-filter.dll'
    $runtime = 'files/bin/64bit/nvngx_dlssnr.dll'
    Assert-ReleaseArtifacts @($plugin, 'files/bin/64bit/nr_codec.dxil') $plugin '' 'zero-runtime fixture'
    Assert-ReleaseArtifacts @($plugin, $runtime) $plugin $runtime 'bundled fixture'
    foreach ($bad in @('files/bin/64bit/other.dll', 'files/models/model.onnx', 'files/model.7z', 'files/model.lib')) {
        $message = $null
        try { Assert-ReleaseArtifacts @($plugin, $bad) $plugin '' 'fixture' } catch { $message = $_.Exception.Message }
        if ($message -notmatch 'forbidden release artifact') { throw "release allowlist must reject $bad (got '$message')" }
    }
    $message = $null
    try { Assert-ReleaseArtifacts @($plugin) $plugin $runtime 'fixture' } catch { $message = $_.Exception.Message }
    if ($message -notmatch 'exactly one') { throw "bundled mode must require its runtime (got '$message')" }
    $message = $null
    try { Assert-ReleaseArtifacts @($plugin, $runtime) $plugin '' 'fixture' } catch { $message = $_.Exception.Message }
    if ($message -notmatch 'forbidden release artifact') { throw "zero-runtime mode must reject a stray runtime (got '$message')" }
    Write-Host 'PASS release artifact allowlist (plugin plus approved runtime only)'

    function New-FakeBuild([string]$buildDir) {
        Put (Join-Path $buildDir 'Release\obs-nr-filter.dll') 'fixture-plugin'
        Put (Join-Path $buildDir 'Release\obs-nr-fg-presenter.exe') 'fixture-presenter'
        foreach ($shader in 'nr_codec.dxil', 'nr_color.dxil', 'nr_stabilize.dxil', 'nr_cas.dxil', 'nr_guides.cso', 'nr_interop_vertex.cso', 'nr_interop_pixel.cso', 'nr_obs_analysis.cso') {
            Put (Join-Path $buildDir $shader) "fixture-$shader"
        }
    }
    function Invoke-MakeRelease([string[]]$ExtraArgs) {
        $argsList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $makeRelease) + $ExtraArgs
        & powershell.exe @argsList
        if ($LASTEXITCODE -ne 0) { throw "make-release failed with exit code $LASTEXITCODE" }
    }
    function Install-Package([string]$packageRoot, [string]$obs, [string]$state, [switch]$Uninstall) {
        $argsList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $packageRoot 'scripts\install.ps1'),
            '-ObsDir', $obs, '-StateDir', $state, '-Lang', 'en', '-Yes', '-NoPause', '-SkipOptional', '-SkipPushFlow')
        if ($Uninstall) { $argsList += '-Uninstall' }
        & powershell.exe @argsList
        if ($LASTEXITCODE -ne 0) { throw "packaged installer failed with exit code $LASTEXITCODE" }
    }

    $zeroRoot = Join-Path $root 'package-zero'
    $zeroBuild = Join-Path $zeroRoot 'build'
    $zeroOut = Join-Path $zeroRoot 'out'
    New-FakeBuild $zeroBuild
    Invoke-MakeRelease @('-BuildDir', $zeroBuild, '-OutDir', $zeroOut, '-Version', '0.3.0-zero', '-NoBundleNrDll')
    $zeroName = 'obs-nr-native-filter-0.3.0-zero-windows-x64'
    $zeroZip = Join-Path $zeroOut "$zeroName.zip"
    Assert-Equal (Test-Path -LiteralPath $zeroZip) $true 'zero-runtime zip must exist'
    $zeroSums = [IO.File]::ReadAllText((Join-Path $zeroOut 'SHA256SUMS.txt'))
    if ($zeroSums -notmatch [regex]::Escape("$((Get-Sha $zeroZip))  $zeroName.zip")) { throw 'SHA256SUMS must pin the zero-runtime zip' }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($zeroZip)
    try { $names = @($archive.Entries.FullName) } finally { $archive.Dispose() }
    if (@($names | Where-Object { $_ -match 'nvngx_dlssnr\.dll$' }).Count) { throw 'zero-runtime zip must not contain nvngx_dlssnr.dll' }
    $zeroExtract = Join-Path $zeroRoot 'extracted'
    [IO.Compression.ZipFile]::ExtractToDirectory($zeroZip, $zeroExtract)
    $zeroPkg = Join-Path $zeroExtract $zeroName
    $zeroObs = Join-Path $zeroRoot 'obs'
    Put (Join-Path $zeroObs 'bin\64bit\obs64.exe') 'fixture-obs'
    $zeroState = Join-Path $zeroRoot 'state'
    $emptyLocal = Join-Path $zeroRoot 'empty-localappdata'
    New-Item -ItemType Directory -Force -Path $emptyLocal | Out-Null
    $prevLocalApp = $env:LOCALAPPDATA
    try {
        $env:LOCALAPPDATA = $emptyLocal
        Install-Package $zeroPkg $zeroObs $zeroState
        Assert-Equal (Test-Path -LiteralPath (Join-Path $zeroObs 'obs-plugins\64bit\obs-nr-filter.dll')) $true 'zero-runtime package must install the plugin'
        Assert-Equal (Test-Path -LiteralPath (Join-Path $zeroObs 'bin\64bit\nvngx_dlssnr.dll')) $false 'zero-runtime package must leave the NR runtime absent'
        Install-Package $zeroPkg $zeroObs $zeroState -Uninstall
        Assert-Equal (Test-Path -LiteralPath (Join-Path $zeroObs 'obs-plugins\64bit\obs-nr-filter.dll')) $false 'zero-runtime uninstall must remove the plugin'
    } finally { $env:LOCALAPPDATA = $prevLocalApp }
    Write-Host 'PASS zero-runtime ZIP generation, install, and uninstall'

    $badDll = Join-Path $root 'bad-runtime\nvngx_dlssnr.dll'
    Put $badDll 'unapproved-nr-runtime'
    $badRejected = $false
    try {
        Invoke-MakeRelease @('-BuildDir', $zeroBuild, '-OutDir', (Join-Path $zeroRoot 'bad-out'), '-Version', '0.3.0-bad', '-BundleNrDll', $badDll)
    } catch { $badRejected = $true }
    Assert-Equal $badRejected $true 'make-release must reject an unapproved NR runtime hash'
    Assert-Equal (Test-Path -LiteralPath (Join-Path $zeroRoot 'bad-out\obs-nr-native-filter-0.3.0-bad-windows-x64.zip')) $false 'rejected runtime must produce no ZIP'
    Write-Host 'PASS unapproved bundled runtime rejected before staging'

    $realNr = Find-VeyraNrRuntime
    if (-not $realNr) {
        Write-Host 'SKIP bundled ZIP roundtrip (no verified local Veyra nvngx_dlssnr.dll)'
        return
    }
    $realNrSha = Get-NrRuntimeSha $realNr
    $bundleRoot = Join-Path $root 'package-bundled'
    $bundleBuild = Join-Path $bundleRoot 'build'
    $bundleOut = Join-Path $bundleRoot 'out'
    New-FakeBuild $bundleBuild
    Invoke-MakeRelease @('-BuildDir', $bundleBuild, '-OutDir', $bundleOut, '-Version', '0.3.0-bundle', '-BundleNrDll', $realNr)
    $bundleName = 'obs-nr-native-filter-0.3.0-bundle-windows-x64'
    $bundleZip = Join-Path $bundleOut "$bundleName.zip"
    Assert-Equal (Test-Path -LiteralPath $bundleZip) $true 'bundled zip must exist'
    $bundleSums = [IO.File]::ReadAllText((Join-Path $bundleOut 'SHA256SUMS.txt'))
    if ($bundleSums -notmatch [regex]::Escape("$((Get-Sha $bundleZip))  $bundleName.zip")) { throw 'SHA256SUMS must pin the bundled zip' }
    $manifestText = ''
    $archive = [IO.Compression.ZipFile]::OpenRead($bundleZip)
    try {
        $names = @($archive.Entries.FullName | ForEach-Object { $_.Replace('\', '/') })
        Assert-Equal @($names | Where-Object { $_ -match '/files/bin/64bit/nvngx_dlssnr\.dll$' }).Count 1 'bundled zip must contain one NR runtime'
        Assert-Equal @($names | Where-Object { $_ -match '\.dll$' }).Count 2 'bundled zip must contain only the plugin and approved NR DLL'
        $manifestEntry = @($archive.Entries | Where-Object { $_.FullName.Replace('\', '/') -match '/files/manifest\.sha256$' })[0]
        $reader = [IO.StreamReader]::new($manifestEntry.Open())
        try { $manifestText = $reader.ReadToEnd() } finally { $reader.Dispose() }
    } finally { $archive.Dispose() }
    $runtimeLine = @($manifestText -split "`r?`n" | Where-Object { $_ -eq "$realNrSha  bin\64bit\nvngx_dlssnr.dll" })
    Assert-Equal $runtimeLine.Count 1 'bundled manifest must pin the NR runtime hash'
    $bundleExtract = Join-Path $bundleRoot 'extracted'
    [IO.Compression.ZipFile]::ExtractToDirectory($bundleZip, $bundleExtract)
    $bundlePkg = Join-Path $bundleExtract $bundleName
    $emptyLocal = Join-Path $bundleRoot 'empty-localappdata'
    New-Item -ItemType Directory -Force -Path $emptyLocal | Out-Null
    $prevLocalApp = $env:LOCALAPPDATA
    try {
        $env:LOCALAPPDATA = $emptyLocal
        $obsForeign = Join-Path $bundleRoot 'obs-foreign'
        Put (Join-Path $obsForeign 'bin\64bit\obs64.exe') 'fixture-obs'
        Put (Join-Path $obsForeign 'bin\64bit\nvngx_dlssnr.dll') 'foreign-nr'
        $stateForeign = Join-Path $bundleRoot 'state-foreign'
        Install-Package $bundlePkg $obsForeign $stateForeign
        Assert-Equal (Get-Sha (Join-Path $obsForeign 'bin\64bit\nvngx_dlssnr.dll')) $realNrSha 'bundled install must write the pinned NR runtime'
        Install-Package $bundlePkg $obsForeign $stateForeign -Uninstall
        Assert-Equal ([IO.File]::ReadAllText((Join-Path $obsForeign 'bin\64bit\nvngx_dlssnr.dll'))) 'foreign-nr' 'bundled uninstall must restore the foreign runtime'
        $obsClean = Join-Path $bundleRoot 'obs-clean'
        Put (Join-Path $obsClean 'bin\64bit\obs64.exe') 'fixture-obs'
        $stateClean = Join-Path $bundleRoot 'state-clean'
        Install-Package $bundlePkg $obsClean $stateClean
        Assert-Equal (Get-Sha (Join-Path $obsClean 'bin\64bit\nvngx_dlssnr.dll')) $realNrSha 'bundled install must write the runtime on a clean OBS'
        Install-Package $bundlePkg $obsClean $stateClean -Uninstall
        Assert-Equal (Test-Path -LiteralPath (Join-Path $obsClean 'bin\64bit\nvngx_dlssnr.dll')) $false 'bundled uninstall must remove the runtime it copied'
    } finally { $env:LOCALAPPDATA = $prevLocalApp }
    Write-Host "PASS bundled ZIP generation, hash install, and uninstall ($realNrSha)"
}

try {
    Test-NrRuntimeAdmission
    Test-BundledNrRuntime
    Test-PushFlowOwnership
    Test-PushFlowStaging
    Test-PushFlowElevation
    Test-ReleasePackaging
    $target = Join-Path $dir $rel
    $src = Join-Path $Payload $rel
    $failures = @()
    try {
        Put $src 'upgrade-one'; $m = [ordered]@{}; $m[$rel] = Get-Sha $src
        $installedHash = $m[$rel]
        Put $target 'original-foreign'
        Install-Files $dir '' '' '' $m
        Put $target 'external-edits'
        Put $src 'upgrade-two'; $m[$rel] = Get-Sha $src
        Install-Files $dir '' '' '' $m
        Assert-Equal ([IO.File]::ReadAllText($target)) 'external-edits' 'upgrade must preserve externally modified owned file'
        $receiptEntry = @((Read-State).ownedFiles | Where-Object { $_.relative -eq $rel })[0]
        Assert-Equal ([IO.File]::ReadAllText($receiptEntry.backup)) 'original-foreign' 'upgrade must retain original backup'
        Assert-Equal $receiptEntry.hash $installedHash 'skipped upgrade must retain installed hash'
        Remove-Files $dir $m (Read-State)
        Assert-Equal ([IO.File]::ReadAllText($target)) 'external-edits' 'uninstall must preserve external edits after upgrade'
    } catch { $failures += $_.Exception.Message }
    Remove-Item -LiteralPath $StateDir, $dir -Recurse -Force -ErrorAction SilentlyContinue
    try {
        Put $src 'unmanaged-identical'; $m = [ordered]@{}; $m[$rel] = Get-Sha $src
        Put $target 'unmanaged-identical'
        Install-Files $dir '' '' '' $m
        Remove-Files $dir $m (Read-State)
        Remove-Files $dir $m (Read-State)
        if (-not (Test-Path -LiteralPath $target)) { throw 'repeated uninstall deleted unmanaged identical file' }
    } catch { $failures += $_.Exception.Message }
    Remove-Item -LiteralPath $StateDir, $dir -Recurse -Force -ErrorAction SilentlyContinue
    if ($failures.Count) { throw ($failures -join '; ') }

    Put $target 'foreign'; Put $src 'version-one'
    $m = [ordered]@{}; $m[$rel] = Get-Sha $src
    Install-Files $dir '' '' '' $m
    Put $src 'version-two'; $m[$rel] = Get-Sha $src
    Install-Files $dir '' '' '' $m
    Put $src 'version-three'; $m[$rel] = Get-Sha $src
    Remove-Files $dir $m (Read-State)
    Assert-Equal ([IO.File]::ReadAllText($target)) 'foreign' 'upgrade must restore original foreign file even from later package'

    Put $target 'legacy-version'; Put $src 'modern-version'; $m[$rel] = Get-Sha $src
    $legacyBackup = Join-Path $StateDir 'backup-20261120-010100'
    Put (Join-Path $legacyBackup $rel) 'pre-legacy'
    Put (Join-Path $StateDir 'install-state.json') (@{
        obsDir = $dir; files = @($rel); backedUp = @($rel); backupDir = $legacyBackup
    } | ConvertTo-Json)
    Install-Files $dir '' '' '' $m
    Assert-Equal ([IO.File]::ReadAllText($target)) 'modern-version' 'upgrade from a legacy receipt must install new bytes'
    Remove-Files $dir $m (Read-State)
    Assert-Equal ([IO.File]::ReadAllText($target)) 'pre-legacy' 'legacy update must retain original backup'

    Put $target 'modern-version'
    Put (Join-Path $StateDir 'install-state.json') (@{
        obsDir = $dir; files = @($rel); backedUp = @($rel); backupDir = $legacyBackup
    } | ConvertTo-Json)
    Install-Files $dir '' '' '' $m
    Remove-Files $dir $m (Read-State)
    Assert-Equal ([IO.File]::ReadAllText($target)) 'pre-legacy' 'legacy backup must survive an identical upgrade'

    $unmanaged = 'bin\64bit\obs64.exe'
    $unmanagedFile = Join-Path $dir $unmanaged
    Put $unmanagedFile 'OBS'
    $invalid = [pscustomobject]@{ obsDir = $dir; ownedFiles = @(
        [pscustomobject]@{ relative = $unmanaged; hash = Get-Sha $unmanagedFile; backup = '' }
    ) }
    $refused = $false
    try { Remove-Files $dir $m $invalid } catch { $refused = $_.Exception.Message -eq 'Invalid installer receipt path' }
    Assert-Equal $refused $true 'uninstall must reject receipt targeting an unmanaged OBS file'
    Assert-Equal ([IO.File]::ReadAllText($unmanagedFile)) 'OBS' 'unmanaged OBS file must be preserved'

    $sr = Join-Path $dir $SrRel; $srSource = Join-Path $root 'source\sr.dll'
    Put $sr 'foreign-sr'; Put $srSource 'installed-sr'
    Install-Files $dir '' $srSource '' $m
    Put $sr 'user-modified'
    Remove-Files $dir $m (Read-State)
    Assert-Equal ([IO.File]::ReadAllText($sr)) 'user-modified' 'modified optional runtime must survive uninstall'

    Put $sr 'foreign-sr'
    Install-Files $dir '' $srSource '' $m
    Remove-Files $dir $m (Read-State)
    Assert-Equal ([IO.File]::ReadAllText($sr)) 'foreign-sr' 'unmodified optional runtime must restore foreign bytes'

    Put $sr 'installed-sr'
    Install-Files $dir '' $srSource '' $m
    Remove-Files $dir $m (Read-State)
    Assert-Equal ([IO.File]::ReadAllText($sr)) 'installed-sr' 'identical pre-existing optional runtime is not owned'

    $vfgSource = Join-Path $root 'vfg-source'
    $vfgTarget = Join-Path $dir $VfgDirRel
    Put (Join-Path $vfgSource 'one.dll') 'new-one'
    Put (Join-Path $vfgSource 'two.dll') 'new-two'
    Put (Join-Path $vfgTarget 'one.dll') 'foreign-one'
    Put $sr 'foreign-sr'
    $originalCopy = ${function:Copy-WithDir}
    function Copy-WithDir([string]$From, [string]$To) {
        if ($From -eq (Join-Path $vfgSource 'two.dll')) {
            Put $To 'partial'
            throw 'injected copy failure'
        }
        & $originalCopy $From $To
    }
    $failed = $false
    try { Install-Files $dir '' $srSource $vfgSource $m } catch { $failed = $true }
    if (-not $failed) { throw 'injected failure did not occur' }
    Assert-Equal ([IO.File]::ReadAllText($sr)) 'foreign-sr' 'rollback must restore optional SR'
    Assert-Equal ([IO.File]::ReadAllText((Join-Path $vfgTarget 'one.dll'))) 'foreign-one' 'rollback must restore optional VFG'
    if (Test-Path (Join-Path $vfgTarget 'two.dll')) { throw 'rollback left partial optional VFG file' }
    if (Get-ChildItem -LiteralPath $StateDir -Filter 'rollback-*') { throw 'rollback staging was left behind' }

    $package = Join-Path $root 'package'
    $packageScript = Join-Path $package 'scripts\install.ps1'
    $packagePayload = Join-Path $package 'files'
    $realObs = Join-Path $root 'entry-obs'
    $realState = Join-Path $root 'entry-state'
    Put (Join-Path $realObs 'bin\64bit\obs64.exe') 'sandbox'
    Put (Join-Path $realObs $rel) 'entry-foreign'
    Put (Join-Path $packagePayload $rel) 'entry-v1'
    New-Item -ItemType Directory -Force (Split-Path -Parent $packageScript) | Out-Null
    Copy-Item -LiteralPath $installer -Destination $packageScript
    Copy-Item -LiteralPath (Join-Path (Split-Path -Parent $installer) 'uninstall.bat') -Destination $package
    function Write-Manifest {
        $lines = foreach ($required in (Get-PackagePaths)) {
            $file = Join-Path $packagePayload $required
            if (-not (Test-Path -LiteralPath $file)) { Put $file "fixture-$required" }
            (Get-Sha $file) + '  ' + $required
        }
        [IO.File]::WriteAllLines((Join-Path $packagePayload 'manifest.sha256'), [string[]]$lines)
    }
    function Run-Installer([bool]$uninstall) {
        $argsList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $packageScript, '-ObsDir', $realObs, '-StateDir', $realState, '-Lang', 'en', '-Yes', '-NoPause', '-SkipOptional')
        if ($uninstall) { $argsList += '-Uninstall' }
        $output = & powershell.exe @argsList | Out-String
        if ($LASTEXITCODE -ne 0) { throw "installer process failed: $LASTEXITCODE`n$output" }
    }
    Write-Manifest
    $manifestFile = Join-Path $packagePayload 'manifest.sha256'
    $complete = [IO.File]::ReadAllLines($manifestFile)
    [IO.File]::WriteAllLines($manifestFile, [string[]]($complete | Where-Object { -not $_.EndsWith('nr_cas.dxil') }))
    $partialResult = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $packageScript -ObsDir $realObs -StateDir $realState -Lang en -Yes -NoPause -SkipOptional | Out-String
    Assert-Equal $LASTEXITCODE 1 'installer must reject a manifest missing a required shader'
    if ($partialResult -notmatch 'nr_cas\.dxil') { throw "installer must identify the missing shader: $partialResult" }
    Assert-Equal ([IO.File]::ReadAllText((Join-Path $realObs $rel))) 'entry-foreign' 'incomplete manifest must not write the OBS plugin'
    [IO.File]::WriteAllLines($manifestFile, $complete)
    $badRuntimeFile = Join-Path $root 'bad-runtime.dll'
    Put $badRuntimeFile 'truncated-or-arbitrary-runtime'
    $realRuntimeTarget = Join-Path $realObs $RuntimeRel
    Put $realRuntimeTarget 'entry-foreign-nr'
    $badHashResult = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $packageScript -ObsDir $realObs -StateDir $realState -Lang en -Yes -NoPause -SkipOptional -SkipPushFlow -Runtime $badRuntimeFile | Out-String
    Assert-Equal $LASTEXITCODE 1 'real installer CLI must reject arbitrary third SHA-256 non-zero'
    if ($badHashResult -notmatch '310\.8\.0\.0' -or $badHashResult -notmatch '310\.9\.16\.0') {
        throw "real installer error must describe both allowed builds: $badHashResult"
    }
    Assert-Equal ([IO.File]::ReadAllText((Join-Path $realObs $rel))) 'entry-foreign' 'third-hash rejection must leave mock OBS plugin untouched'
    Assert-Equal ([IO.File]::ReadAllText($realRuntimeTarget)) 'entry-foreign-nr' 'third-hash rejection must leave mock OBS runtime untouched'

    $missingRuntimePath = Join-Path $root 'nonexistent\nvngx_dlssnr.dll'
    $missingPathResult = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $packageScript -ObsDir $realObs -StateDir $realState -Lang en -Yes -NoPause -SkipOptional -SkipPushFlow -Runtime $missingRuntimePath | Out-String
    Assert-Equal $LASTEXITCODE 1 'real installer CLI must reject missing runtime path non-zero'
    Assert-Equal ([IO.File]::ReadAllText($realRuntimeTarget)) 'entry-foreign-nr' 'missing runtime path must leave mock OBS runtime untouched'
    Write-Host 'PASS real installer CLI rejected third hash and missing path non-zero with mock OBS untouched'

    Run-Installer $false
    Put (Join-Path $packagePayload $rel) 'entry-v2'
    Write-Manifest; Run-Installer $false
    Assert-Equal ([IO.File]::ReadAllText((Join-Path $realObs $rel))) 'entry-v2' 'real reinstall must upgrade an unchanged owned file'
    Put (Join-Path $packagePayload $rel) 'entry-v3'
    Write-Manifest; Run-Installer $true
    Assert-Equal ([IO.File]::ReadAllText((Join-Path $realObs $rel))) 'entry-foreign' 'real entry point must restore original across upgrades'
    Put (Join-Path $realObs $rel) 'entry-v3'
    Run-Installer $false
    foreach ($attempt in 1..2) {
        $output = & (Join-Path $package 'uninstall.bat') -ObsDir $realObs -StateDir $realState -Lang en -Yes -NoPause | Out-String
        if ($LASTEXITCODE -ne 0) { throw "uninstall wrapper failed: $LASTEXITCODE`n$output" }
        Assert-Equal ([IO.File]::ReadAllText((Join-Path $realObs $rel))) 'entry-v3' "unmanaged identical bytes must survive uninstall $attempt"
    }
    Write-Host 'PASS release installer ownership'
} finally {
    if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
}
