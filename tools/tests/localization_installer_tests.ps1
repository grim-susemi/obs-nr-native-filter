# SPDX-License-Identifier: GPL-3.0-only
param([Parameter(Mandatory)][string]$Work)
$ErrorActionPreference = 'Stop'
$root = Join-Path ([IO.Path]::GetTempPath()) ('nr-locale-' + [guid]::NewGuid().ToString('N'))
$scripts = Join-Path $root 'package/scripts'
$files = Join-Path $root 'package/files'
$obs = Join-Path $root 'fakeOBS'
$state = Join-Path $root 'testState'
New-Item -ItemType Directory -Path $scripts, $files, (Join-Path $obs 'bin/64bit') -Force | Out-Null
try {
    Copy-Item (Join-Path $Work 'packaging/windows/install.ps1') $scripts
    $relative = 'data\obs-plugins\obs-nr-filter\locale\ru-RU.ini'
    $payload = Join-Path $files $relative
    New-Item -ItemType Directory -Path (Split-Path $payload) -Force | Out-Null
    Copy-Item (Join-Path $Work 'data/locale/ru-RU.ini') $payload
    [IO.File]::WriteAllText((Join-Path $obs 'bin/64bit/obs64.exe'), 'fixture')
    $hash = (Get-FileHash $payload -Algorithm SHA256).Hash.ToLowerInvariant()
    [IO.File]::WriteAllText((Join-Path $files 'manifest.sha256'), "$hash  $relative`n")
    $installer = Join-Path $scripts 'install.ps1'
    foreach ($lang in 'ru', 'en', 'ko', '') {
        $argsForRun = @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', $installer,
            '-ObsDir', $obs, '-StateDir', $state, '-Yes', '-NoPause')
        if ($lang) { $argsForRun += @('-Lang', $lang) }
        $output = & powershell.exe @argsForRun 2>&1
        if ($LASTEXITCODE -ne 0) { throw "Install failed for $lang : $output" }
        if ((Get-FileHash (Join-Path $obs $relative)).Hash -ne (Get-FileHash $payload).Hash) { throw "Installed locale differs: $lang" }
        $output = & powershell.exe @argsForRun -Uninstall 2>&1
        if ($LASTEXITCODE -ne 0) { throw "Uninstall failed for $lang : $output" }
        if (Test-Path (Join-Path $obs $relative)) { throw "Uninstall left locale: $lang" }
    }
    $ErrorActionPreference = 'Continue'
    $output = & powershell.exe -NoProfile -NonInteractive -File $installer -Lang invalid -Yes -NoPause -StateDir $state 2>&1
    $ErrorActionPreference = 'Stop'
    if ($LASTEXITCODE -eq 0) { throw 'Invalid language was accepted' }
    $output = & powershell.exe -NoProfile -NonInteractive -File $installer -Lang ru -ObsDir (Join-Path $root 'missing') -Yes -NoPause -StateDir $state 2>&1
    if ($LASTEXITCODE -ne 1) { throw "Invalid OBS path not rejected: $output" }
    $tokens = $null; $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($installer, [ref]$tokens, [ref]$errors)
    $assign = $ast.Find({ param($n) $n -is [Management.Automation.Language.AssignmentStatementAst] -and $n.Left.Extent.Text -eq '$Text' }, $true)
    $tables = Invoke-Expression $assign.Right.Extent.Text
    $interactiveState = Join-Path $root 'interactiveState'
    $output = '3' | & powershell.exe -NoProfile -File $installer -ObsDir (Join-Path $root 'missing') -NoPause -StateDir $interactiveState 2>&1
    if ($LASTEXITCODE -ne 1) { throw "Interactive bad path exit: $output" }
    $log = Get-ChildItem $interactiveState -Filter '*.txt' | Select-Object -First 1
    if (-not ([IO.File]::ReadAllText($log.FullName).Contains(($tables.ru.title -f '0.1.1')))) { throw 'Interactive choice 3 did not select ru' }
    $output = & powershell.exe -NoProfile -NonInteractive -File $installer '-?' 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Help failed: $output" }
    Write-Output 'PASS real installer/uninstaller ru en ko default, choice 3, help, invalid language and OBS path'
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force
}
