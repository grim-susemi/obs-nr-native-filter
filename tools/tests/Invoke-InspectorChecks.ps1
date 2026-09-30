[CmdletBinding()]
param()
$ErrorActionPreference="Stop"
$root="C:/Users/USER/orca/projects/obs-nr-native-filter-wt/w1"
$evidence="$root/.omo/evidence/obs-nr-native-filter-20260930/task-1/inspector"
$script="$root/tools/Inspect-Inputs.ps1"
$runtime="C:/Program Files/obs-studio/bin/64bit/nvngx_dlssnr.dll"
$fixture="$evidence/fixtures"
$null=[IO.Directory]::CreateDirectory($fixture)
$runs=New-Object System.Collections.Generic.List[object]
function Assert([bool]$condition,[string]$code) { if (!$condition) { throw "Assertion failed: $code" } }
function Json([string]$path,$data) { [IO.File]::WriteAllText($path,($data | ConvertTo-Json -Depth 24),(New-Object Text.UTF8Encoding($false))) }
function Parse([string]$path) { Get-Content -LiteralPath $path -Raw | ConvertFrom-Json }
function Hash([string]$path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash }
function Run([string]$name,[string[]]$extra=@(),[string]$code="",[string]$model=$runtime) {
 $report="$evidence/$name.json"
 $arguments=@("-NoProfile","-ExecutionPolicy","Bypass","-File",$script,"-Report",$report,"-Runtime",$model)+$extra
 $output=@(& powershell.exe @arguments 2>&1); $exit=$LASTEXITCODE
 $literal="powershell.exe "+(($arguments | ForEach-Object { [char]34+$_+[char]34 }) -join " ")
 $data=Parse $report
 $runs.Add([ordered]@{name=$name;command=$literal;exit=$exit;stdout=($output -join [Environment]::NewLine);report=$report;report_sha256=(Hash $report)})
 Assert ($data.complete -eq $true -and $data.exit -eq $exit) "$name-machine-exit"
 Assert ($exit -eq 0 -or ($exit -eq 1 -and $data.status -eq "BLOCKED")) "$name-status"
 if ($code) { Assert ($exit -ne 0 -and @($data.reasons.code) -contains $code) "$name-refusal-code" }
 if ($data.protection.before.Count) { Assert (($data.protection.before | ConvertTo-Json -Depth 8 -Compress) -eq ($data.protection.after | ConvertTo-Json -Depth 8 -Compress)) "$name-protection" }
 return $data
}
$tokens=$null; $errors=$null
foreach ($path in @($script,$PSCommandPath)) {
 $null=[Management.Automation.Language.Parser]::ParseFile($path,[ref]$tokens,[ref]$errors)
 Assert ($errors.Count -eq 0) "AST-$path"
}
$spec=Parse "$root/provenance/inputs.spec.json"
$source=Parse "$root/provenance/source-lock.json"
$sdk=Parse "$root/provenance/sdk-lock.json"
$lockBefore=@((Hash "$root/provenance/source-lock.json"),(Hash "$root/provenance/sdk-lock.json"))
$manual=Run "inputs"
Assert ($manual.runtime.identity.sha256 -eq $spec.runtime.sha256 -and $manual.runtime.version -eq $spec.runtime.version -and $manual.runtime.static_pe.machine -eq "AMD64") "real-runtime-identities"
Assert (!$manual.runtime.loaded -and !$manual.runtime.evaluated) "static-only"
foreach ($name in $spec.runtime.required_exports) { Assert (@($manual.runtime.static_pe.exports) -contains $name) "actual-export-$name" }
foreach ($code in @("hash_drift","repository","source_lock","runtime_pe","runtime_identity","toolchain","toolchain_hash","input_changed","protected_changed")) { Assert (@($manual.reasons.code) -notcontains $code) "available-input-$code" }
Json "$evidence/manual.json" $runs[0]
Json "$evidence/protection.json" $manual.protection
function Clone($obj) { $obj | ConvertTo-Json -Depth 24 | ConvertFrom-Json }
function FixtureSpec([string]$name,$lock,[string]$family) {
 $copy=Clone $spec
 $relative=".omo/evidence/obs-nr-native-filter-20260930/task-1/inspector/fixtures/$name-lock.json"
 Json "$root/$relative" $lock
 $copy.$family=$relative
 $relative=".omo/evidence/obs-nr-native-filter-20260930/task-1/inspector/fixtures/$name-spec.json"
 Json "$root/$relative" $copy
 return $relative
}
$null=Run "wrong-root" @("-Root","C:/omo-research") "wrong_root"
$copy=Clone $sdk; $copy.obs.commit="0000000000000000000000000000000000000000"
$input=FixtureSpec "wrong-pin" $copy "sdk_lock"
$null=Run "wrong-pin" @("-InputSpec",$input) "wrong_pin"
$copy=Clone $sdk; $copy.nvofa.sdk_version="4.0"
$input=FixtureSpec "wrong-sdk" $copy "sdk_lock"
$null=Run "wrong-sdk" @("-InputSpec",$input) "sdk_package_version"
$copy=Clone $sdk; $copy.ngx.files[0].sha256="0000000000000000000000000000000000000000000000000000000000000000"
$input=FixtureSpec "sdk-hash-drift" $copy "sdk_lock"
$null=Run "sdk-hash-drift" @("-InputSpec",$input) "hash_drift"
$copy=Clone $source; $copy.files[0].sha256="0000000000000000000000000000000000000000000000000000000000000000"
$input=FixtureSpec "source-hash-drift" $copy "source_lock"
$null=Run "source-hash-drift" @("-InputSpec",$input) "hash_drift"
$badSpec=".omo/evidence/obs-nr-native-filter-20260930/task-1/inspector/fixtures/malformed-spec.json"
[IO.File]::WriteAllText("$root/$badSpec","{malformed")
$null=Run "malformed-spec" @("-InputSpec",$badSpec) "invalid_input"
$copy=Clone $sdk; $copy.ngx.files[0].path="../foreign-input"
$input=FixtureSpec "path-escape" $copy "sdk_lock"
$null=Run "path-escape" @("-InputSpec",$input) "sdk_lock"
# Tiny synthetic PE fixtures exercise the real parser through the public CLI.
# They are always refused as runtimes; no fabricated model/header is admitted.
function PeFixture([string]$name,[string]$fault) {
 $buffer=New-Object byte[] 1024
 function Put16([int]$at,[uint16]$value) { [BitConverter]::GetBytes($value).CopyTo($buffer,$at) }
 function Put32([int]$at,[uint32]$value) { [BitConverter]::GetBytes($value).CopyTo($buffer,$at) }
 Put16 0 0x5a4d; Put32 60 128; Put32 128 0x4550
 Put16 132 0x8664; Put16 134 1; Put16 148 240; Put16 152 0x20b
 Put32 212 512; Put32 260 16; Put32 264 4096; Put32 268 128
 Put32 400 512; Put32 404 4096; Put32 408 512; Put32 412 512
 Put32 532 1; Put32 536 1; Put32 540 4144; Put32 544 4148; Put32 548 4152
 Put32 560 4352; Put32 564 4160; Put16 568 0
 [Text.Encoding]::ASCII.GetBytes("FixtureOnly").CopyTo($buffer,576)
 switch ($fault) {
  "truncated" { Put32 60 2147483647 }
  "directory-overflow" { Put32 268 ([uint32]::MaxValue) }
  "ordinal" { Put16 568 5 }
  "count" { Put32 536 65537 }
  "unterminated" { for ($i=576;$i -lt 1024;$i++) { $buffer[$i]=65 } }
 }
 $path="$fixture/$name.dll"; [IO.File]::WriteAllBytes($path,$buffer)
 return $path
}
$model=PeFixture "missing-export" "none"
$modelBefore=Hash $model
$parsed=Run "runtime-export" @() "runtime_export" $model
Assert ($parsed.runtime.static_pe.machine -eq "AMD64") "synthetic-pe-parsed"
Assert ((Hash $model) -eq $modelBefore) "fixture-unchanged"
foreach ($fault in @("truncated","directory-overflow","ordinal","count","unterminated")) {
 $model=PeFixture $fault $fault; $modelBefore=Hash $model
 $null=Run "pe-$fault" @() "runtime_pe" $model
 Assert ((Hash $model) -eq $modelBefore) "pe-$fault-unchanged"
}
$common="$fixture/nvOpticalFlowCommon.h"
[IO.File]::WriteAllText($common,"#define NV_OF_API_MAJOR_VERSION 2")
$copy=Clone $sdk
$copy.nvofa.files=@([pscustomobject]@{path=".omo/evidence/obs-nr-native-filter-20260930/task-1/inspector/fixtures/nvOpticalFlowCommon.h";sha256=(Hash $common).ToLowerInvariant();bytes=(Get-Item $common).Length})
$input=FixtureSpec "common-api2" $copy "sdk_lock"
$null=Run "common-api2" @("-InputSpec",$input) "nvofa_d3d12"
# Seed artifacts at the exact interrupted/stale receipt surfaces, then re-enter.
# No timing race or process cancellation is used; unrelated process control is forbidden.
foreach ($attempt in @(1,2)) {
 [IO.File]::WriteAllText("$evidence/recovery.json",("{"+[char]34+"status"+[char]34+":"+[char]34+"ADMITTED"+[char]34+"}"))
 [IO.File]::WriteAllText("$evidence/recovery.json.partial","{interrupted")
 $recovered=Run "recovery" @("-Root","C:/omo-research") "wrong_root"
 Assert ($recovered.status -eq "BLOCKED" -and !(Test-Path "$evidence/recovery.json.partial")) "recovery-$attempt-stale-success-replaced"
}
Assert ($manual.repository.dirty.Count -gt 0) "dirty-worktree-observed"
$lockAfter=@((Hash "$root/provenance/source-lock.json"),(Hash "$root/provenance/sdk-lock.json"))
Assert (($lockBefore -join ",") -eq ($lockAfter -join ",")) "sibling-locks-preserved"
$positive="BLOCKED_INPUT"
if ($manual.exit -eq 0) { $positive="PASS" }
$summary=[ordered]@{
 schema_version=1;kind="nr-inspector-checks";independent_checks_pass=$true;cli_runs=$runs.Count;positive_all_inputs=$positive
 script_sha256=(Hash $script);tests_sha256=(Hash $PSCommandPath);spec_sha256=(Hash "$root/provenance/inputs.spec.json")
 manual_report="$evidence/manual.json";protection_report="$evidence/protection.json";runs=@($runs.ToArray())
 input_dependencies=@($manual.reasons);sibling_locks_before=$lockBefore;sibling_locks_after=$lockAfter
 adversarial=@{malformed_input="CLI PE/JSON/path/export fixtures";stale_state="Stale success receipt overwritten";dirty_worktree="Actual dirty W1 recorded, not reset";misleading_success_output="JSON exit/status/static-only fields asserted";cancel_resume_repeated_interruption="Two seeded partial/stale receipts recovered; no real process cancellation";other_classes_na="No GPU/app/network/install/parser beyond owned input files; no persistent host or process-control surface"}
 cleanup=@{persistent_resources=@();owned_processes_remaining=0;partial_files_remaining=@(Get-ChildItem -LiteralPath $evidence -Filter "*.partial").Count;retained="Small QA reports and refusal fixtures retained as evidence; no model/source/backups removed"}
 task1FullyDone=$false
}
Json "$evidence/checks.json" $summary
Write-Output ("INSPECTOR_CHECKS_PASS runs="+$runs.Count+" all_inputs="+$positive)
exit 0
