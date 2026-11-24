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
try {
    Test-PushFlowOwnership
    Test-PushFlowStaging
    Test-PushFlowElevation
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
