param([Parameter(Mandatory)][string]$Work,[string]$Runner,[string]$PathReport,[switch]$UnsafeControl)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
# CPU-only route control for handled-faults/shutdown: the stage is derived from the evidence root's window
# root identity, never from a literal. Fixtures are stub stages created and removed under the runner's tree.
if(!$Runner){$Runner = Join-Path $Work 'tools/Invoke-NrQa.ps1'}
$runnerDir = Split-Path -Parent ([IO.Path]::GetFullPath($Runner))
$tree = [IO.Path]::GetFullPath((Join-Path $runnerDir '../.omo/evidence/obs-nr-native-filter-20260930'))
$task = Join-Path $tree 'task-17'
$runtime = 'C:/Program Files/obs-studio/bin/64bit/nvngx_dlssnr.dll'
$sha = 'a' * 64
$dispatcher = @'
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Scenario,[Parameter(Mandatory)][string]$Evidence,
    [Parameter(Mandatory)][string]$HostRoot,[Parameter(Mandatory)][string]$Runtime,
    [Parameter(Mandatory)][string]$CandidateManifest,[Parameter(Mandatory)][string]$CandidateSha256)
$actual=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'INPUTS.json') -Algorithm SHA256).Hash.ToLowerInvariant()
if($actual -cne $CandidateSha256){throw 'WINDOW_INPUTS_BINDING'}
if(!(Get-Variable WindowSession -Scope Script -ErrorAction SilentlyContinue)){throw 'WINDOW_ORIGINAL_SESSION_REQUIRED'}
$i=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'INPUTS.json') -Raw -Encoding UTF8|ConvertFrom-Json
if(!$script:WindowSession -or $script:WindowSession.inputsPin.sha256 -cne $CandidateSha256){throw 'WINDOW_ORIGINAL_SESSION_REQUIRED'}
$route=@($i.generatorSources|Where-Object {(Split-Path -Leaf $_.path) -ceq 'Invoke-WindowScenario.ps1'})
if($route.Count -ne 1 -or (Get-FileHash -LiteralPath $route[0].path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $route[0].sha256){throw 'WINDOW_STALE_PIN'}
. $route[0].path -Scenario $Scenario -Evidence $Evidence -HostRoot $HostRoot -Runtime $Runtime `
    -CandidateManifest $CandidateManifest -CandidateSha256 $CandidateSha256
'@
$launch = @'
param($Runner,$Scenario,$Evidence,$HostRoot,$Runtime,$CandidateManifest,$CandidateSha256)
$ErrorActionPreference='Stop'
$inputs=Get-Content -LiteralPath $CandidateManifest -Raw|ConvertFrom-Json
$script:WindowSession=@{inputsPin=@{sha256=$CandidateSha256};active=@($inputs.scenarios|Where-Object {$_.scenario -ceq $Scenario})[0]}
. $Runner -Scenario $Scenario -Evidence $Evidence -HostRoot $HostRoot -Runtime $Runtime `
    -Task17CandidateManifest $CandidateManifest -Task17CandidateSha256 $CandidateSha256
'@
$stub = @'
[CmdletBinding()]
param($Scenario,$Evidence,$HostRoot,$Runtime,$CandidateManifest,$CandidateSha256)
. $script:WindowSession.active.runner.path
. $script:WindowSession.active.validator.path
@{stage=(Join-Path $PSScriptRoot 'Invoke-Task17Window.ps1');Scenario=$Scenario;Evidence=$Evidence;HostRoot=$HostRoot;Runtime=$Runtime;
    CandidateManifest=$CandidateManifest;CandidateSha256=$CandidateSha256} | ConvertTo-Json -Compress |
    Set-Content -LiteralPath (Join-Path $PSScriptRoot 'stage-called.json') -Encoding UTF8
'@
function Invoke-Runner([string]$Scenario, [string]$Evidence) {
    $child = [Diagnostics.Process]::new()
    $child.StartInfo.FileName = "$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe"
    $fixtureInputs=Join-Path $winA 'INPUTS.json'
    $window=Split-Path -Parent ([IO.Path]::GetFullPath($Evidence).TrimEnd('\','/'))
    if(Test-Path -LiteralPath (Join-Path $window 'INPUTS.json')){$fixtureInputs=Join-Path $window 'INPUTS.json'}
    $fixtureSha=(Get-FileHash -LiteralPath $fixtureInputs -Algorithm SHA256).Hash.ToLowerInvariant()
    $child.StartInfo.Arguments = '-NoProfile -ExecutionPolicy Bypass -File "' + (Join-Path $winA 'Launch.ps1') + '" -Runner "' + $Runner + '" -Scenario "' + $Scenario + '" -Evidence "' + $Evidence + '" -HostRoot "' + (Join-Path $Work 'out/task-17-fixture/host') + '" -Runtime "' + $runtime + '" -CandidateManifest "' + $fixtureInputs + '" -CandidateSha256 "' + $fixtureSha + '"'
    $child.StartInfo.UseShellExecute = $false
    $child.StartInfo.CreateNoWindow = $true
    $child.StartInfo.RedirectStandardOutput = $true
    $child.StartInfo.RedirectStandardError = $true
    try {
        if (!$child.Start()) { throw 'route test child start failed' }
        $out = $child.StandardOutput.ReadToEndAsync()
        $err = $child.StandardError.ReadToEndAsync()
        if (!$child.WaitForExit(60000)) { $child.Kill(); throw 'route test child timed out' }
        @{exit = $child.ExitCode; text = ($out.GetAwaiter().GetResult() + $err.GetAwaiter().GetResult())}
    } finally { $child.Dispose() }
}
function Expect-Refusal([string]$Scenario, [string]$Evidence, [string]$Token) {
    $r = Invoke-Runner $Scenario $Evidence
    if (!$r.exit -or !$r.text.Contains($Token)) { throw ('Wrong refusal for ' + $Scenario + ' ' + $Evidence + ': ' + $r.text) }
    Write-Output ('PASS route refusal ' + $Token)
}
$created = [Collections.Generic.List[string]]::new()
function New-Fixture([string]$Path, [bool]$WithStage) {
    if (Test-Path -LiteralPath $Path) { throw ('Fixture path must be absent: ' + $Path) }
    [void][IO.Directory]::CreateDirectory($Path)
    $created.Add($Path)
    if ($WithStage) {
        $utf8=[Text.UTF8Encoding]::new($false)
        [IO.File]::WriteAllText((Join-Path $Path 'Invoke-Task17Window.ps1'),($dispatcher+"`n"),$utf8)
        [IO.File]::WriteAllText((Join-Path $Path 'Launch.ps1'),$launch,$utf8)
        [IO.File]::WriteAllText((Join-Path $Path 'Invoke-WindowScenario.ps1'),$stub,$utf8)
        [IO.File]::WriteAllText((Join-Path $Path 'Runner.ps1'),'function Invoke-D7FixtureRunner {}',$utf8)
        [IO.File]::WriteAllText((Join-Path $Path 'Validator.ps1'),'function Assert-D7FixtureValidator {}',$utf8)
        $pins=@{}
        foreach($name in @('Invoke-WindowScenario.ps1','Runner.ps1','Validator.ps1')) {
            $file=Join-Path $Path $name
            $pins[$name]=@{path=$file;bytes=(Get-Item -LiteralPath $file).Length;sha256=(Get-FileHash -LiteralPath $file).Hash.ToLowerInvariant()}
        }
        $pins['Runner.ps1'].function='Invoke-D7FixtureRunner'
        $pins['Validator.ps1'].function='Assert-D7FixtureValidator'
        @{generatorSources=@($pins['Invoke-WindowScenario.ps1']);scenarios=@(foreach($scenario in @('handled-faults','shutdown')){
            @{scenario=$scenario;runner=$pins['Runner.ps1'];validator=$pins['Validator.ps1']}
        })}|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $Path 'INPUTS.json') -Encoding UTF8
    }
}
$pathResults=[Collections.Generic.List[object]]::new()
function Check-PathCase([string]$Id,[string]$Window,[string]$Token,[string]$Marker) {
    foreach($scenario in @('handled-faults','shutdown')) {
        if(Test-Path -LiteralPath $Marker){Remove-Item -LiteralPath $Marker}
        $r=Invoke-Runner $scenario (Join-Path $Window 'native01')
        if($UnsafeControl) {
            if($r.exit -ne 0 -or !(Test-Path -LiteralPath $Marker)){throw ("Old-wrapper exploit not reproduced: $Id "+$r.text)}
        } elseif(!$r.exit -or !$r.text.Contains($Token) -or (Test-Path -LiteralPath $Marker)){
            throw ("Unsafe path admitted or wrong refusal: $Id "+$r.text)
        }
        $pathResults.Add(@{id=$Id;scenario=$scenario;exit=$r.exit;stageExecuted=(Test-Path -LiteralPath $Marker);
            expected=$(if($UnsafeControl){'unsafe-old-wrapper'}else{$Token});text=$r.text})
        Write-Output ("PASS path-control $Id $scenario unsafe="+[bool]$UnsafeControl)
    }
}
try {
    $winA = Join-Path $task 'recording-window-r901-d7fixture-20261015'
    $winB = Join-Path $task 'recording-window-r902-d7fixture-20261015'
    $winNoStage = Join-Path $task 'recording-window-r903-d7fixture-20261015'
    $badLeaf = Join-Path $task 'recording-window-R904-d7fixture-20261015'
    $badDate = Join-Path $task 'recording-window-r905-d7fixture-2026'
    foreach ($p in @($winA, $winB)) { New-Fixture $p $true }
    New-Fixture $winNoStage $false
    New-Fixture $badLeaf $true
    New-Fixture $badDate $true
    foreach ($scenario in @('handled-faults', 'shutdown')) {
        foreach ($win in @($winA, $winB)) {
            $evidence = Join-Path $win 'native01'
            $marker = Join-Path $win 'stage-called.json'
            $r = Invoke-Runner $scenario $evidence
            if ($r.exit -ne 0 -or !(Test-Path -LiteralPath $marker)) { throw ('Window stage was not reached for ' + $scenario + ' in ' + $win + ': ' + $r.text) }
            $seen = Get-Content -LiteralPath $marker -Raw | ConvertFrom-Json
            if ([IO.Path]::GetFullPath($seen.stage) -ine [IO.Path]::GetFullPath((Join-Path $win 'Invoke-Task17Window.ps1')) -or
                $seen.Scenario -cne $scenario -or $seen.Evidence -cne $evidence -or $seen.Runtime -cne $runtime -or
                $seen.HostRoot -cne (Join-Path $Work 'out/task-17-fixture/host') -or
                $seen.CandidateManifest -cne (Join-Path $win 'INPUTS.json') -or
                $seen.CandidateSha256 -cne (Get-FileHash -LiteralPath (Join-Path $win 'INPUTS.json')).Hash.ToLowerInvariant()) { throw ('Window stage arguments changed: ' + ($seen | ConvertTo-Json -Compress)) }
            Remove-Item -LiteralPath $marker
            Write-Output ('PASS routed ' + $scenario + ' to its own window stage ' + (Split-Path -Leaf $win))
        }
    }
    foreach ($scenario in @('handled-faults', 'shutdown')) {
        Expect-Refusal $scenario $task 'NR_FAULT_PARENT_ADMISSION_REQUIRED'
        Expect-Refusal $scenario (Join-Path $winA 'native01/nested') 'NR_FAULT_PARENT_ADMISSION_REQUIRED'
        Expect-Refusal $scenario (Join-Path $badLeaf 'native01') 'NR_FAULT_PARENT_ADMISSION_REQUIRED'
        Expect-Refusal $scenario (Join-Path $badDate 'native01') 'NR_FAULT_PARENT_ADMISSION_REQUIRED'
        Expect-Refusal $scenario (Join-Path $tree 'recording-window-r901-d7fixture-20261015/native01') 'NR_FAULT_PARENT_ADMISSION_REQUIRED'
        Expect-Refusal $scenario (Join-Path $Work 'foreign/recording-window-r901-d7fixture-20261015/native01') 'NR_FAULT_PARENT_ADMISSION_REQUIRED'
        Expect-Refusal $scenario (Join-Path $winNoStage 'native01') 'NR_QA_T17_WINDOW_STAGE_MISSING'
    }
    # Real directory reparse points, not string lookalikes. Junction targets stay in this
    # private source copy but outside its owned evidence tree, exactly as the review probe.
    $foreign=Join-Path $Work 'out/d7-path-foreign'
    New-Fixture $foreign $true
    $junction=Join-Path $task 'recording-window-r906-d7junction-20261015'
    $null=New-Item -ItemType Junction -Path $junction -Target $foreign
    try {
        Check-PathCase 'junction-window' $junction 'NR_QA_T17_REPARSE_PATH' (Join-Path $foreign 'stage-called.json')
    } finally {[IO.Directory]::Delete($junction)}
    $foreignTask=Join-Path $Work 'out/d7-path-foreign-task'
    $foreignWindow=Join-Path $foreignTask 'recording-window-r907-d7ancestor-20261015'
    New-Fixture $foreignWindow $true
    $taskJunction=Join-Path $tree 'task-998'
    $null=New-Item -ItemType Junction -Path $taskJunction -Target $foreignTask
    try {
        Check-PathCase 'junction-task-ancestor' (Join-Path $taskJunction (Split-Path -Leaf $foreignWindow)) `
            'NR_QA_T17_REPARSE_PATH' (Join-Path $foreignWindow 'stage-called.json')
    } finally {[IO.Directory]::Delete($taskJunction)}
    if(!$UnsafeControl) {
        $sourceLink=Join-Path $winB 'source-link'
        $null=New-Item -ItemType Junction -Path $sourceLink -Target $foreign
        try {
            foreach($kind in @('generic','runner','validator')) {
                $inputPath=Join-Path $winB 'INPUTS.json'
                $saved=[IO.File]::ReadAllBytes($inputPath)
                try {
                    $inputs=Get-Content -LiteralPath $inputPath -Raw|ConvertFrom-Json
                    if($kind -ceq 'generic'){$inputs.generatorSources[0].path=Join-Path $sourceLink 'Invoke-WindowScenario.ps1'}
                    else {foreach($scenario in $inputs.scenarios){$scenario.$kind.path=Join-Path $sourceLink $(if($kind -ceq 'runner'){'Runner.ps1'}else{'Validator.ps1'})}}
                    $inputs|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $inputPath -Encoding UTF8
                    Check-PathCase ('junction-'+$kind+'-ancestor') $winB 'NR_QA_T17_REPARSE_PATH' (Join-Path $winB 'stage-called.json')
                } finally {[IO.File]::WriteAllBytes($inputPath,$saved)}
            }
        } finally {[IO.Directory]::Delete($sourceLink)}
        foreach($name in @('Invoke-Task17Window.ps1','Invoke-WindowScenario.ps1','Runner.ps1','Validator.ps1')) {
            $tamper=Join-Path $winB $name
            $saved=[IO.File]::ReadAllBytes($tamper)
            try {
                [IO.File]::AppendAllText($tamper,"`n# unreviewed replacement")
                Check-PathCase ('changed-'+$name) $winB 'WINDOW_STALE_PIN' (Join-Path $winB 'stage-called.json')
            } finally {[IO.File]::WriteAllBytes($tamper,$saved)}
        }
        # A reviewed pin cannot authorize a stage outside this wrapper's owned root.
        $inputPath=Join-Path $winB 'INPUTS.json'
        $saved=[IO.File]::ReadAllBytes($inputPath)
        try {
            $inputs=Get-Content -LiteralPath $inputPath -Raw|ConvertFrom-Json
            foreach($scenario in $inputs.scenarios){$scenario.runner.path=Join-Path (Split-Path -Parent $Work) 'foreign-runner.ps1'}
            $inputs|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $inputPath -Encoding UTF8
            Check-PathCase 'outside-owned-root' $winB 'NR_QA_T17_OUTSIDE_OWNED_ROOT' (Join-Path $winB 'stage-called.json')
        } finally {[IO.File]::WriteAllBytes($inputPath,$saved)}
        # Try to replace both an already-checked script and its directory at the actual
        # downstream use boundary. This must fail because route handles still own them.
        $generic=Join-Path $winB 'Invoke-WindowScenario.ps1'
        $savedGeneric=[IO.File]::ReadAllBytes($generic)
        $savedInputs=[IO.File]::ReadAllBytes($inputPath)
        try {
            $attempt=@'
$writeBlocked=$false;$renameBlocked=$false
try{[IO.File]::AppendAllText($script:WindowSession.active.runner.path,"`n# unreviewed")}catch [IO.IOException]{$writeBlocked=$true}
try{[IO.Directory]::Move($PSScriptRoot,($PSScriptRoot+'-replaced'))}catch [IO.IOException]{$renameBlocked=$true}
if(!$writeBlocked -or !$renameBlocked){throw ('WINDOW_USE_BOUNDARY_UNLOCKED writeBlocked='+$writeBlocked+' renameBlocked='+$renameBlocked)}
'@
            $changed=$stub.Replace('. $script:WindowSession.active.runner.path',($attempt+"`n"+'. $script:WindowSession.active.runner.path'))
            [IO.File]::WriteAllText($generic,$changed,[Text.UTF8Encoding]::new($false))
            $inputs=Get-Content -LiteralPath $inputPath -Raw|ConvertFrom-Json
            $inputs.generatorSources[0].bytes=(Get-Item -LiteralPath $generic).Length
            $inputs.generatorSources[0].sha256=(Get-FileHash -LiteralPath $generic).Hash.ToLowerInvariant()
            $inputs|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $inputPath -Encoding UTF8
            foreach($scenario in @('handled-faults','shutdown')) {
                $r=Invoke-Runner $scenario (Join-Path $winB 'native01')
                if($r.exit -ne 0 -or !(Test-Path -LiteralPath (Join-Path $winB 'stage-called.json'))){throw ('Use-boundary lock failed: '+$r.text)}
                Remove-Item -LiteralPath (Join-Path $winB 'stage-called.json')
                $pathResults.Add(@{id='hash-at-use-write-and-rename-denied';scenario=$scenario;exit=$r.exit;stageExecuted=$true})
                Write-Output ('PASS hash-at-use file write and ancestor rename denied '+$scenario)
            }
        } finally {[IO.File]::WriteAllBytes($generic,$savedGeneric);[IO.File]::WriteAllBytes($inputPath,$savedInputs)}
    }
    $text = Get-Content -LiteralPath $Runner -Raw
    foreach ($literal in @('public-parent-r4-closed-audit-source', 'Invoke-Task17Private.ps1')) {
        if ($text.Contains($literal)) { throw ('Old-root route literal still present: ' + $literal) }
    }
    Write-Output 'PASS no old-root route literal in the wrapper'
    Write-Output 'TASK17_WINDOW_ROUTE_CPU_PASS native=false'
} finally {
    if($PathReport){$pathResults.ToArray()|ConvertTo-Json -Depth 6|Set-Content -LiteralPath $PathReport -Encoding UTF8}
    foreach ($p in $created) { if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Recurse -Force } }
}
