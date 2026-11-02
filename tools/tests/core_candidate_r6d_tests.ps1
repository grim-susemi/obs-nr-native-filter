param([Parameter(Mandatory)][string]$Work)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
Import-Module Microsoft.PowerShell.Utility
$original=Join-Path $Work 'tools/tests/core_candidate_r6c_tests.ps1'
if((Get-FileHash -LiteralPath $original -Algorithm SHA256).Hash.ToLowerInvariant() -ne 'fc9071261b4604f6c334c3e341fa49e922b357260d610e20bb128221d42a8833'){throw 'NR_QA_R6C_TEST_CHANGED'}
$code=[IO.File]::ReadAllText($original)
$code=$code.Replace('core-control-r6c','core-control-r6d').Replace('nr-task13-core-r6c-','nr-task13-core-r6d-')
$execution='. ([ScriptBlock]::Create($code)) -Work $Work -CoreControlOnly'
$adapt=@'
$marker='    $ownedRoute=@'+[char]39
$lines=@(
 '$controlledPath = ''@ACTUALHELPER@''',
 '$controlledSource = [IO.File]::ReadAllText($controlledPath)',
 'if((Get-FileHash -LiteralPath $controlledPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne ''@ACTUALHASH@''){throw ''CONTROLLED_HELPER_HASH_CHANGED''}',
 '$stdoutAnchor = ''        $controllerStdout=[IO.StreamWriter]::new((Join-Path $Attempt ''''controller-stdout.log''''),$false,[Text.UTF8Encoding]::new($false))''',
 '$stderrAnchor = ''        $controllerStderr=[IO.StreamWriter]::new((Join-Path $Attempt ''''controller-stderr.log''''),$false,[Text.UTF8Encoding]::new($false))''',
 'foreach($anchor in @($stdoutAnchor,$stderrAnchor)){if($controlledSource.Split(@($anchor),[StringSplitOptions]::None).Count -ne 2){throw ''CONTROLLED_SINK_ANCHOR_NOT_EXACT''}}',
 '$controlledSource=$controlledSource.Replace($stdoutAnchor,''        $controllerStdout=[IO.StreamWriter]::new([IO.Stream]::Null)'').Replace($stderrAnchor,''        $controllerStderr=[IO.StreamWriter]::new([IO.Stream]::Null)'')',
 '$controlledSource=$controlledSource.Replace(''. (Join-Path $PSScriptRoot ''''HostSupport.ps1'''')'',''. ''''@SUPPORT@'''''')',
 '$controlledSource=$controlledSource.Replace(''$work=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot ''''../..''''))'',''$work=''''@WORK@'''''')',
 '$controlledSource=$controlledSource.Replace(''$self=[IO.Path]::GetFullPath($PSCommandPath)'',''$self=''''@ACTUALHELPER@'''''')',
 '[Console]::Out.WriteLine(''CONTROLLED_OWN_ROUTE actual_helper_sha256=@ACTUALHASH@ sink_substitutions=2 path_bindings=3 native_execution=none'')',
 '$controlledConsoleOut=[Console]::Out; $controlledConsoleError=[Console]::Error',
 'try { & ([ScriptBlock]::Create($controlledSource))'
)
$insert='    $controlledOwn = ('+(@($lines|ForEach-Object {"'"+$_.Replace("'","''")+"'"}) -join ',')+' -join ''; '')'+[Environment]::NewLine
$insert+='    $controlledOwn=$controlledOwn.Replace(''@ACTUALHELPER@'',$helper.Replace("''","''''")).Replace(''@ACTUALHASH@'',$helperHash).Replace(''@SUPPORT@'',(Join-Path $Work ''tools/host/HostSupport.ps1'').Replace("''","''''")).Replace(''@WORK@'',$Work.Replace("''","''''"))'+[Environment]::NewLine
$insert+='    $controlledOwn+=$own.Substring(("& ''"+$helper+"''").Length)'+[Environment]::NewLine
if($code.Split(@($marker),[StringSplitOptions]::None).Count -ne 2){throw 'CONTROLLED_OWN_CASE_AMBIGUOUS'}
$code=$code.Replace($marker,$insert+$marker)
$call="@OWN@ -NativeApproved; throw 'CPU_OWN_UNEXPECTED_RETURN'"
if($code.Split(@($call),[StringSplitOptions]::None).Count -ne 2){throw 'CONTROLLED_OWN_CALL_AMBIGUOUS'}
$code=$code.Replace($call,"@OWN@ -NativeApproved } finally { [Console]::SetOut(`$controlledConsoleOut); [Console]::SetError(`$controlledConsoleError) }; throw 'CPU_OWN_UNEXPECTED_RETURN'")
$pair="@('@OWN@',`$own)"
if($code.Split(@($pair),[StringSplitOptions]::None).Count -ne 2){throw 'CONTROLLED_OWN_PAIR_AMBIGUOUS'}
$code=$code.Replace($pair,"@('@OWN@',`$controlledOwn)")
'@
if($code.Split(@($execution),[StringSplitOptions]::None).Count -ne 2){throw 'CONTROLLED_EXECUTION_AMBIGUOUS'}
$code=$code.Replace($execution,$adapt+[Environment]::NewLine+$execution)
. ([ScriptBlock]::Create($code)) -Work $Work
