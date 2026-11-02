param([Parameter(Mandatory)][string]$Work,[ValidateSet('Red','Green')][string]$Expected,[Parameter(Mandatory)][string]$Label,[switch]$SinkFault)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$e=Join-Path $Work '.omo/evidence/obs-nr-native-filter-20260930/task-13/codex-continuation-20261004/core-control-r6d-cpu'
$helper=Join-Path $Work 'tools/host/Start-NrLifecycleSupervisor.ps1'
$shell=Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
$base=if($Expected -eq 'Red'){Join-Path (Split-Path $e) 'physical-readiness-r3'}else{Join-Path $e 'supervisors'}
$before=if(Test-Path $base){@(Get-ChildItem -LiteralPath $base -Directory|ForEach-Object FullName)}else{@()}
$sha=(Get-FileHash -LiteralPath $helper -Algorithm SHA256).Hash.ToLowerInvariant()
$children=[Collections.Generic.List[object]]::new()
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class R6dExit {
 [DllImport("kernel32.dll", SetLastError=true)] static extern bool GetExitCodeProcess(IntPtr handle,out uint code);
 [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool QueryFullProcessImageName(IntPtr handle,uint flags,System.Text.StringBuilder path,ref uint count);
 public static string Image(IntPtr handle) { uint count=32768;var path=new System.Text.StringBuilder((int)count);if(!QueryFullProcessImageName(handle,0,path,ref count))throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());return path.ToString(); }
 public static uint Code(IntPtr handle) { uint code;if(!GetExitCodeProcess(handle,out code))throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());return code; }
}
'@
function Identity($p){@{pid=$p.Id;creation=$p.StartTime.ToUniversalTime().ToString('o');image=[R6dExit]::Image($p.Handle)}}
function Save($name,$v){[IO.File]::WriteAllText((Join-Path $e $name),($v|ConvertTo-Json -Depth 16),[Text.UTF8Encoding]::new($false))}
function StartCaptured([string]$arguments){
 $p=[Diagnostics.Process]::new();$p.StartInfo.FileName=$shell;$p.StartInfo.Arguments=$arguments
 $p.StartInfo.WorkingDirectory=$Work;$p.StartInfo.UseShellExecute=$false;$p.StartInfo.CreateNoWindow=$true
 $p.StartInfo.RedirectStandardOutput=$true;$p.StartInfo.RedirectStandardError=$true
 if(!$p.Start()){throw 'CPU_EXTERNAL_START'}
 $o=$p.StandardOutput.ReadToEndAsync();$r=$p.StandardError.ReadToEndAsync()
 $x=@{process=$p;stdout=$o;stderr=$r;identity=(Identity $p);arguments=$arguments};$children.Add($x);return $x
}
function Finish($x,$ms){
 if(!$x.process.WaitForExit($ms)){throw 'CPU_COMMAND_UNRESOLVED_NO_CONTROL'}
 if(![Threading.Tasks.Task]::WaitAll([Threading.Tasks.Task[]]@($x.stdout,$x.stderr),15000)){throw 'CPU_EOF_UNRESOLVED_NO_CONTROL'}
 $v=@{identity=$x.identity;arguments=$x.arguments;exit=$x.process.ExitCode;stdout=[string]$x.stdout.Result;stderr=[string]$x.stderr.Result;terminal=$true;reaped=$true;stdoutEOF=$true;stderrEOF=$true}
 $x.process.Dispose();return $v
}
$sw=[Diagnostics.Stopwatch]::StartNew()
$command='& "'+$shell+'" -NoProfile -ExecutionPolicy Bypass -File "'+$helper+'" -Mode Launch -Lane CPU'+$(if($SinkFault){' -SinkFault'}else{''})
$launch=StartCaptured ('-NoProfile -ExecutionPolicy Bypass -Command '+[char]34+$command.Replace('"','\"')+[char]34)
$returned=$launch.process.WaitForExit(20000)
$eof=$returned -and [Threading.Tasks.Task]::WaitAll([Threading.Tasks.Task[]]@($launch.stdout,$launch.stderr),1000)
$elapsed=$sw.ElapsedMilliseconds
$attempts=@(Get-ChildItem -LiteralPath $base -Directory|Where-Object {$_.FullName -notin $before})
if($attempts.Count -ne 1){Save ($Label+'-unresolved.json') @{external=$launch.identity;attempts=@($attempts.FullName);returned=$returned;EOF=$eof};throw 'CPU_OWN_ATTEMPT_SET_UNRESOLVED'}
$attempt=$attempts[0].FullName
$record=[IO.File]::ReadAllText((Join-Path $attempt 'ownership.json'))|ConvertFrom-Json
if($record.helper_sha256 -ne $sha -or $record.lane -ne 'CPU'){throw 'CPU_OWNERSHIP_REFUSED'}
$owned=@(foreach($id in @($record.supervisor,$record.host)){
 $p=[Diagnostics.Process]::GetProcessById($id.pid);$now=Identity $p
 if(($now|ConvertTo-Json -Compress) -ne ($id|ConvertTo-Json -Compress)){
  if($now.pid -ne $id.pid -or $now.creation -ne $id.creation -or $now.image -ne $id.image){throw 'CPU_CURRENT_IDENTITY_REFUSED'}
 }
 @{expected=$id;current=$now;process=$p;handle=$p.Handle;aliveBeforeCleanup=(!$p.HasExited)}
})
Save ($Label+'-held.json') @{helperSha256=$sha;nonce=$record.nonce;attempt=$attempt;external=$launch.identity;command=$command;externalReturned=$returned;externalEOF=$eof;elapsedMs=$elapsed;owned=@(foreach($row in $owned){@{expected=$row.expected;current=$row.current;aliveBeforeCleanup=$row.aliveBeforeCleanup}})}
$tail=' -Lane CPU -Attempt "'+$attempt+'" -Nonce '+$record.nonce+' -HelperHash '+$sha
$query=StartCaptured ('-NoProfile -ExecutionPolicy Bypass -File "'+$helper+'" -Mode Query'+$tail)
$queryReceipt=Finish $query 25000
Save ($Label+'-query.json') $queryReceipt
$close=StartCaptured ('-NoProfile -ExecutionPolicy Bypass -File "'+$helper+'" -Mode Close'+$tail)
$closeReceipt=Finish $close 20000
Save ($Label+'-close.json') $closeReceipt
$launchReceipt=Finish $launch 25000
Save ($Label+'-launch.json') $launchReceipt
$cleanup=@(foreach($row in $owned){
 if(!$row.process.WaitForExit(20000)){throw 'CPU_OWN_FIXTURE_UNRESOLVED_NO_CONTROL'}
 $exit=[R6dExit]::Code($row.handle);$row.process.Dispose()
 $absent=$false;try{$p=[Diagnostics.Process]::GetProcessById($row.expected.pid);try{$now=Identity $p;$absent=$now.creation -ne $row.expected.creation -or $now.image -ne $row.expected.image}finally{$p.Dispose()}}catch [ArgumentException]{$absent=$true}
 @{identity=$row.expected;exit=$exit;terminal=$true;reaped=$true;exactAbsent=$absent}
})
$released=[IO.File]::ReadAllText((Join-Path $attempt 'released.json'))|ConvertFrom-Json
$pass=$returned -and $eof -and @($owned|Where-Object {!$_.aliveBeforeCleanup}).Count -eq 0 -and $queryReceipt.exit -eq 0 -and $closeReceipt.exit -eq 0 -and $launchReceipt.exit -eq 0 -and @($cleanup|Where-Object {!$_.exactAbsent}).Count -eq 0 -and $released.stdout_closed -and $released.stderr_closed -and $released.child_exited -and $released.subscription_handles_released -and $released.ipc_handles_released
$newFiles=@(Get-ChildItem -LiteralPath $attempt -Recurse -File|ForEach-Object {@{path=$_.FullName;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}})
Save ($Label+'-result.json') @{pass=$pass;expected=$Expected;helperSha256=$sha;attempt=$attempt;nonce=$record.nonce;heldExternalReturned=$returned;heldExternalEOF=$eof;heldElapsedMs=$elapsed;ownCleanup=$cleanup;released=$released;newFiles=$newFiles;terminal=$true;commandsRunning=0;normalRetirement=$false;nativeCommands=0}
Write-Output ('CPU_DETACHED '+$Label+' pass='+$pass+' externalReturned='+$returned+' EOF='+$eof+' heldMs='+$elapsed+' exactCleanup='+$cleanup.Count)
if(!$pass){exit 1}
exit 0
