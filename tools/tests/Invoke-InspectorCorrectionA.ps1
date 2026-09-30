[CmdletBinding()]
param([switch]$StableInputs)
$ErrorActionPreference="Stop"
if (!$StableInputs) { throw "Parent stable completed-lock signal is required before behavioral execution" }
$root="C:/Users/USER/orca/projects/obs-nr-native-filter-wt/w1"
$taskEvidence="$root/.omo/evidence/obs-nr-native-filter-20260930/task-1"
$evidence="$taskEvidence/inspector/correction-a"
$script="$root/tools/Inspect-Inputs.ps1"
$runtime="C:/Program Files/obs-studio/bin/64bit/nvngx_dlssnr.dll"
$null=[IO.Directory]::CreateDirectory($evidence)
$runs=New-Object System.Collections.Generic.List[object]
function Assert([bool]$ok,[string]$code) { if (!$ok) { throw "Assertion failed: $code" } }
function Save([string]$path,$data) { [IO.File]::WriteAllText($path,($data | ConvertTo-Json -Depth 24),(New-Object Text.UTF8Encoding($false))) }
function Identity([string]$path) {
 if (!(Test-Path -LiteralPath $path -PathType Leaf)) { return "ABSENT" }
 return ((Get-FileHash -LiteralPath $path).Hash+":"+(Get-Item -LiteralPath $path).Length)
}
function Run([string]$name,[string]$report,[string]$model=$runtime,[string[]]$extra=@(),[switch]$RefuseOutput) {
 $before=Identity $report
 $arguments=@("-NoProfile","-ExecutionPolicy","Bypass","-File",$script,"-Report",$report,"-Runtime",$model)+$extra
 try {
  $ErrorActionPreference="Continue"
  $output=@(& powershell.exe @arguments 2>&1); $exit=$LASTEXITCODE
 } finally { $ErrorActionPreference="Stop" }
 $literal="powershell.exe "+(($arguments | ForEach-Object { [char]34+$_+[char]34 }) -join " ")
 $runs.Add([ordered]@{name=$name;command=$literal;exit=$exit;output=($output -join [Environment]::NewLine);report=$report})
 Save "$evidence/runs.json" @($runs.ToArray())
 if ($RefuseOutput) {
  Assert ($exit -eq 2 -and (Identity $report) -eq $before) "$name-no-write-refusal"
  return
 }
 Assert ($exit -eq 0 -or $exit -eq 1) "$name-report-boundary"
 $data=Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
 Assert ($data.complete -and $data.exit -eq $exit) "$name-machine-exit"
 if (@($data.reasons.code) -contains "protected_changed" -or @($data.reasons.code) -contains "input_changed") { throw "CONCURRENT_INPUT_CHANGE: $name; receipt retained, no retry" }
 return $data
}
$lockPaths=@("$root/provenance/source-lock.json","$root/provenance/sdk-lock.json")
$locksBefore=@($lockPaths | ForEach-Object { Identity $_ })
$canonical=Run "canonical-task1" "$taskEvidence/inputs.json"
Assert ($canonical.runtime.owner_selected -eq $true -and @($canonical.reasons.code) -notcontains "runtime_identity") "canonical-owner-label"
$nested=Run "verifier-subdirectory" "$evidence/verifier/inputs.json"
Assert ($nested.runtime.owner_selected -eq $true) "verifier-owner-label"
# Retained tiny negative input, never a copied or loaded model.
$fake="$evidence/rejected-runtime.dll"
[IO.File]::WriteAllBytes($fake,[byte[]](77,90,0,0))
$fakeBefore=Identity $fake
$rejected=Run "rejected-override" "$evidence/rejected-override.json" $fake
Assert ($rejected.exit -eq 1 -and $rejected.runtime.owner_selected -eq $false -and @($rejected.reasons.code) -contains "runtime_identity") "rejected-override-not-owner"
Assert ((Identity $fake) -eq $fakeBefore) "rejected-runtime-unchanged"
$spec=Get-Content -LiteralPath "$root/provenance/inputs.spec.json" -Raw | ConvertFrom-Json
$spec.sdk_lock=".omo/evidence/obs-nr-native-filter-20260930/task-1/inspector/correction-a/nonexistent-sdk-lock.json"
$fixtureSpec="$evidence/sdk-blocked-spec.json"; Save $fixtureSpec $spec
$blocked=Run "owner-with-sdk-blocker" "$evidence/owner-sdk-blocked.json" $runtime @("-InputSpec",".omo/evidence/obs-nr-native-filter-20260930/task-1/inspector/correction-a/sdk-blocked-spec.json")
Assert ($blocked.exit -eq 1 -and $blocked.runtime.owner_selected -eq $true -and @($blocked.reasons.code) -contains "sdk_lock") "owner-label-independent-of-sdk"
$null=Run "traversal" "$evidence/../traversal-denied.json" $runtime @() -RefuseOutput
$null=Run "windows-traversal" ($evidence+[char]92+".."+[char]92+"windows-traversal-denied.json") $runtime @() -RefuseOutput
$null=Run "outside-task1" "$root/.omo/evidence/obs-nr-native-filter-20260930/outside-task1-denied.json" $runtime @() -RefuseOutput
$null=Run "source-output" "$root/provenance/inputs.spec.json" $runtime @() -RefuseOutput
$null=Run "foreign-output" "C:/omo-research/obs-mfg-v020-candidate/source/Inspector-correction-a-denied.json" $runtime @() -RefuseOutput
$null=Run "input-collision" $fixtureSpec $runtime @("-InputSpec",".omo/evidence/obs-nr-native-filter-20260930/task-1/inspector/correction-a/sdk-blocked-spec.json") -RefuseOutput
$locksAfter=@($lockPaths | ForEach-Object { Identity $_ })
if (($locksBefore -join ",") -ne ($locksAfter -join ",")) { throw "CONCURRENT_INPUT_CHANGE: sibling locks changed; no retry" }
Save "$evidence/checks.json" ([ordered]@{kind="nr-inspector-correction-a-checks";delta_checks_pass=$true;physical_positive=($canonical.exit -eq 0);runs=$runs.Count;script_sha256=(Get-FileHash $script).Hash;tests_sha256=(Get-FileHash $PSCommandPath).Hash;sibling_locks_before=$locksBefore;sibling_locks_after=$locksAfter;cleanup="Retained reports/tiny fixtures only; no deletion or persistent resources";task1FullyDone=$false})
Write-Output ("CORRECTION_A_CHECKS_PASS runs="+$runs.Count+" physical_positive="+($canonical.exit -eq 0))
exit 0
