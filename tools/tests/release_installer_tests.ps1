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
try {
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
    $packageScript = Join-Path $package 'windows\install.ps1'
    $packagePayload = Join-Path $package 'files'
    $realObs = Join-Path $root 'entry-obs'
    $realState = Join-Path $root 'entry-state'
    Put (Join-Path $realObs 'bin\64bit\obs64.exe') 'sandbox'
    Put (Join-Path $realObs $rel) 'entry-foreign'
    Put (Join-Path $packagePayload $rel) 'entry-v1'
    New-Item -ItemType Directory -Force (Split-Path -Parent $packageScript) | Out-Null
    Copy-Item -LiteralPath $installer -Destination $packageScript
    function Write-Manifest {
        $file = Join-Path $packagePayload $rel
        [IO.File]::WriteAllText((Join-Path $packagePayload 'manifest.sha256'), ((Get-Sha $file) + '  ' + $rel))
    }
    function Run-Installer([bool]$uninstall) {
        $argsList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $packageScript, '-ObsDir', $realObs, '-StateDir', $realState, '-Lang', 'en', '-Yes', '-NoPause', '-SkipOptional')
        if ($uninstall) { $argsList += '-Uninstall' }
        $output = & powershell.exe @argsList | Out-String
        if ($LASTEXITCODE -ne 0) { throw "installer process failed: $LASTEXITCODE`n$output" }
    }
    Write-Manifest; Run-Installer $false
    Put (Join-Path $packagePayload $rel) 'entry-v2'
    Write-Manifest; Run-Installer $false
    Assert-Equal ([IO.File]::ReadAllText((Join-Path $realObs $rel))) 'entry-v2' 'real reinstall must upgrade an unchanged owned file'
    Put (Join-Path $packagePayload $rel) 'entry-v3'
    Write-Manifest; Run-Installer $true
    Assert-Equal ([IO.File]::ReadAllText((Join-Path $realObs $rel))) 'entry-foreign' 'real entry point must restore original across upgrades'
    Write-Host 'PASS release installer ownership'
} finally {
    if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
}
