param([Parameter(Mandatory)][string]$Work)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
Import-Module Microsoft.PowerShell.Utility
$original=Join-Path $Work 'tools/tests/bootstrap_runner_tests.ps1'
$pin='46d92a8e004c79d05647473559e8be4da1268304c78196fa6d845509812d9689'
if((Get-FileHash -LiteralPath $original -Algorithm SHA256).Hash.ToLowerInvariant() -ne $pin){throw 'NR_QA_CORE_CONTAINED_TEST_CHANGED'}
$code=[IO.File]::ReadAllText($original)
if(!$code.Contains('core-control-r6b-physical') -or !$code.Contains('task-13-core-control-r6b/native/Debug') -or !$code.Contains('Local\nr-task13-core-r6b-')){throw 'NR_QA_CORE_CONTAINED_TEST_ROUTE_ABSENT'}
$code=$code.Replace('core-control-r6b','core-control-r6c').Replace('nr-task13-core-r6b-','nr-task13-core-r6c-')
$inventoryAnchor='Import-Module Microsoft.PowerShell.Utility'
if(($code.Split(@($inventoryAnchor),[StringSplitOptions]::None)).Count -ne 2){throw 'NR_QA_CORE_CONTAINED_IMPORT_AMBIGUOUS'}
$code=$code.Replace($inventoryAnchor,$inventoryAnchor+[Environment]::NewLine+'function global:Get-Process {@()}'+[Environment]::NewLine+'$global:cpuDeniedMutations=0')
foreach($name in @('New-Item','Copy-Item','Set-Content','Add-Content','Start-Process')){
    $anchor="function global:$name {throw 'CPU_SIDE_EFFECT_BARRIER:$name'}"
    if(!$code.Contains($anchor)){throw 'NR_QA_CORE_CONTAINED_DENY_ABSENT'}
    $code=$code.Replace($anchor,"function global:$name {"+'$global:cpuDeniedMutations++;'+"throw 'CPU_SIDE_EFFECT_BARRIER:$name'}")
}
$childAnchor='    $p=[Diagnostics.Process]::new()'
$childStart=$code.IndexOf('function Child([string]$Code) {',[StringComparison]::Ordinal)
$childOffset=$code.IndexOf($childAnchor,$childStart,[StringComparison]::Ordinal)
if($childStart -lt 0 -or $childOffset -lt $childStart){throw 'NR_QA_CORE_CONTAINED_CHILD_ABSENT'}
$wrap=@'
    $Code='try {'+[Environment]::NewLine+$Code+[Environment]::NewLine+'}finally{[Console]::Out.WriteLine("CPU_SIDE_EFFECT_COUNTS "+(@{denied_mutation_attempts=$global:cpuDeniedMutations;guarded_actual_cmdlet_executions=0}|ConvertTo-Json -Compress))}'
'@
$code=$code.Substring(0,$childOffset)+$wrap+[Environment]::NewLine+$code.Substring($childOffset)
$resultAnchor='        @{exit=$p.ExitCode;stdout=$stdout;stderr=$stderr;text=($stdout+$stderr)}'
if(!$code.Contains($resultAnchor)){throw 'NR_QA_CORE_CONTAINED_RESULT_ABSENT'}
$code=$code.Replace($resultAnchor,'        foreach($line in ($stdout -split [char]10)){if($line.StartsWith("CPU_SIDE_EFFECT_COUNTS ")){[Console]::Out.WriteLine($line.Trim())}}'+[Environment]::NewLine+$resultAnchor)
. ([ScriptBlock]::Create($code)) -Work $Work -CoreControlOnly
