param(
    [ValidateSet('Launch','Own','Query','Close','CpuChild')][string]$Mode='Launch',
    [ValidateSet('CPU','Native','NativeDiagnosticR4','NativeCoreInitControl')][string]$Lane='CPU',
    [string]$Attempt,[string]$Nonce,[string]$HelperHash,
    [switch]$SinkFault,[switch]$NativeApproved
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'HostSupport.ps1')
$work=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$base=Join-Path $work '.omo/evidence/obs-nr-native-filter-20260930/task-13/codex-continuation-20261004/physical-readiness-r3'
if($Lane -eq 'CPU'){$base=Join-Path $work '.omo/evidence/obs-nr-native-filter-20260930/task-13/codex-continuation-20261004/core-control-r6d-cpu/supervisors'}
$nativeLane=$Lane -in @('Native','NativeDiagnosticR4','NativeCoreInitControl')
$nativeEvidence=Join-Path $work '.omo/evidence/obs-nr-native-filter-20260930/task-13/verifier'
$nativeRoot=Join-Path $work 'out/task-13-verifier/host'
if($Lane -eq 'NativeDiagnosticR4') {
    if($Mode -eq 'CpuChild'){throw 'NR_QA_DIAGNOSTIC_CPU_MODE_REFUSED'}
    $nativeEvidence=Join-Path $work '.omo/evidence/obs-nr-native-filter-20260930/task-13/codex-continuation-20261004/diagnostic-r4-verified'
    $nativeRoot=Join-Path $work 'out/task-13-diagnostic-r4/verified-host'
    $base=Join-Path $work '.omo/evidence/obs-nr-native-filter-20260930/task-13/codex-continuation-20261004/readiness-r5/diagnostic-supervisors'
}
if($Lane -eq 'NativeCoreInitControl') {
    if($Mode -eq 'CpuChild'){throw 'NR_QA_CORE_CPU_MODE_REFUSED'}
    $nativeEvidence=Join-Path $work '.omo/evidence/obs-nr-native-filter-20260930/task-13/codex-continuation-20261004/core-control-r6d-physical'
    $nativeRoot=Join-Path $work 'out/task-13-core-control-r6d/verified-host'
    $base=Join-Path $work '.omo/evidence/obs-nr-native-filter-20260930/task-13/codex-continuation-20261004/core-control-r6d/supervisors'
}
if($nativeLane -and $Mode -eq 'Close'){throw 'NR_QA_NATIVE_CLOSE_REFUSED'}
$shell=Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
$self=[IO.Path]::GetFullPath($PSCommandPath)
if($Mode -eq 'Launch') {
    if($Attempt -or $Nonce -or $HelperHash){throw 'NR_QA_LAUNCH_ARGUMENT_REFUSED'}
    if($nativeLane -and (!$NativeApproved -or $SinkFault)){throw 'NR_QA_NATIVE_NOT_ADMITTED'}
    $Nonce=[Guid]::NewGuid().ToString('N')
    $Attempt=Join-Path $base ('attempt-'+$Nonce)
    if(Test-Path -LiteralPath $Attempt){throw 'NR_QA_EXISTING_ATTEMPT'}
    New-Item -ItemType Directory -Path $Attempt|Out-Null
    $HelperHash=(Get-FileHash -LiteralPath $self -Algorithm SHA256).Hash.ToLower()
} else {
    if($Nonce -notmatch '^[0-9a-f]{32}$'){throw 'NR_QA_NONCE_REFUSED'}
    if([IO.Path]::GetFullPath($Attempt) -ne [IO.Path]::GetFullPath((Join-Path $base ('attempt-'+$Nonce)))){throw 'NR_QA_ATTEMPT_REFUSED'}
    if($HelperHash -notmatch '^[0-9a-f]{64}$' -or (Get-FileHash -LiteralPath $self -Algorithm SHA256).Hash.ToLower() -ne $HelperHash){throw 'NR_QA_HELPER_CHANGED'}
}
$namespace='Local\nr-task13-r3-'+$Nonce
if($Lane -eq 'NativeDiagnosticR4'){$namespace='Local\nr-task13-r4-'+$Nonce}
if($Lane -eq 'NativeCoreInitControl'){$namespace='Local\nr-task13-core-r6d-'+$Nonce}
$ownership=Join-Path $Attempt 'ownership.json'
if($Mode -eq 'Launch') {
    $registered=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,($namespace+'-registered'))
    $argsList=@('-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+$self+'"'),'-Mode','Own','-Lane',$Lane,
        '-Attempt',('"'+$Attempt+'"'),'-Nonce',$Nonce,'-HelperHash',$HelperHash)
    if($SinkFault){$argsList+='-SinkFault'}
    if($NativeApproved){$argsList+='-NativeApproved'}
    $p=Start-Process -FilePath $shell -ArgumentList $argsList -WindowStyle Hidden -PassThru -WorkingDirectory $work
    try {
        $identity=Identity $p
        if(!$registered.WaitOne(15000)){throw 'NR_QA_DETACHED_REGISTRATION_TIMEOUT'}
        $record=Get-Content -Raw -LiteralPath $ownership|ConvertFrom-Json
        if(($identity|ConvertTo-Json -Compress) -ne ($record.supervisor|ConvertTo-Json -Compress)){throw 'NR_QA_DETACHED_IDENTITY_REFUSED'}
        $record|ConvertTo-Json -Depth 10|Set-Content -Encoding UTF8 (Join-Path $Attempt 'launch-return.json')
        $record|ConvertTo-Json -Depth 10
    } finally {$registered.Dispose();$p.Dispose()}
    return
}
if($Mode -in @('Query','Close')) {
    $record=Get-Content -Raw -LiteralPath $ownership|ConvertFrom-Json
    if($record.helper_sha256 -ne $HelperHash -or $record.nonce -ne $Nonce){throw 'NR_QA_OWNERSHIP_REFUSED'}
    if($Lane -in @('NativeDiagnosticR4','NativeCoreInitControl') -and $record.lane -ne $Lane){throw 'NR_QA_OWNERSHIP_REFUSED'}
    if($Mode -eq 'Query') {
        $receipt=if($record.lane -eq 'CPU'){Join-Path $Attempt 'supervisor.json'}else{
            $rows=@(Get-ChildItem -LiteralPath $nativeEvidence -Filter 'attempt-*-supervisor.json'|Where-Object{
                (Get-Content -Raw -LiteralPath $_.FullName|ConvertFrom-Json).supervisor.pid -eq $record.supervisor.pid
            })
            if($rows.Count -ne 1){throw 'NR_QA_NATIVE_SUPERVISOR_SET'}
            $rows[0].FullName
        }
        Request-RetainedHost -ReceiptPath $receipt|ConvertTo-Json -Depth 14
    } else {
        if($Lane -ne 'CPU' -or $record.lane -ne 'CPU'){throw 'NR_QA_NATIVE_CLOSE_REFUSED'}
        foreach($expected in @($record.supervisor,$record.host)) {
            $p=[Diagnostics.Process]::GetProcessById($expected.pid)
            try{if((Identity $p|ConvertTo-Json -Compress) -ne ($expected|ConvertTo-Json -Compress)){throw 'NR_QA_CLOSE_IDENTITY_REFUSED'}}finally{$p.Dispose()}
        }
        $close=[Threading.EventWaitHandle]::OpenExisting($namespace+'-cpu-close')
        try{$close.Set()|Out-Null}finally{$close.Dispose()}
        Write-Output 'CPU_COOPERATIVE_CLOSE_REQUESTED'
    }
    return
}
if($nativeLane -and ($Mode -ne 'Own' -or !$NativeApproved -or $SinkFault)){throw 'NR_QA_NATIVE_MODE_REFUSED'}
$controllerStdout=$null;$controllerStderr=$null
if($Mode -eq 'Own' -and !$nativeLane) {
    $controllerStdout=[IO.StreamWriter]::new((Join-Path $Attempt 'controller-stdout.log'),$false,[Text.UTF8Encoding]::new($false))
    $controllerStderr=[IO.StreamWriter]::new((Join-Path $Attempt 'controller-stderr.log'),$false,[Text.UTF8Encoding]::new($false))
    $controllerStdout.AutoFlush=$true;$controllerStderr.AutoFlush=$true
    [Console]::SetOut($controllerStdout);[Console]::SetError($controllerStderr)
}
try {
if($nativeLane) {
    $controller=[Diagnostics.Process]::GetCurrentProcess()
    try {
        @{lane=$Lane;nonce=$Nonce;helper=$self;helper_sha256=$HelperHash;supervisor=(Identity $controller);
            host=$null;events=$namespace;subscriptions='actual public Invoke-NrQa registration receipts'}|
            ConvertTo-Json -Depth 10|Set-Content -Encoding UTF8 $ownership
        $controllerStdout=[IO.StreamWriter]::new((Join-Path $Attempt 'controller-stdout.log'),$false,[Text.UTF8Encoding]::new($false))
        $controllerStderr=[IO.StreamWriter]::new((Join-Path $Attempt 'controller-stderr.log'),$false,[Text.UTF8Encoding]::new($false))
        $controllerStdout.AutoFlush=$true;$controllerStderr.AutoFlush=$true
        [Console]::SetOut($controllerStdout);[Console]::SetError($controllerStderr)
        $registered=[Threading.EventWaitHandle]::OpenExisting($namespace+'-registered')
        try{$registered.Set()|Out-Null}finally{$registered.Dispose()}
        & (Join-Path $work 'tools/Invoke-NrQa.ps1') -Scenario $(if($Lane -eq 'NativeCoreInitControl'){'native-model'}else{'lifecycle'}) -LifecycleNative:($Lane -ne 'NativeCoreInitControl') -LifecycleDiagnosticR4:($Lane -eq 'NativeDiagnosticR4') -CoreInitControl:($Lane -eq 'NativeCoreInitControl') -Evidence $nativeEvidence -HostRoot $nativeRoot -Runtime (Join-Path $env:ProgramFiles 'obs-studio/bin/64bit/nvngx_dlssnr.dll') | ForEach-Object {[Console]::Out.WriteLine([string]$_)}
    } finally {$controller.Dispose()}
    return
}
if($Mode -eq 'CpuChild') {
    $request=[Threading.EventWaitHandle]::OpenExisting($namespace+'-host-request')
    $close=[Threading.EventWaitHandle]::OpenExisting($namespace+'-cpu-close')
    try {
        $sequence=0
        for(;;) {
            $wake=[Threading.WaitHandle]::WaitAny([Threading.WaitHandle[]]@($close,$request))
            if($wake -eq 0){break}
            $request.Reset()|Out-Null;$sequence++
            [Console]::Out.WriteLine('CPU_RAW_DIAGNOSTIC request='+$sequence)
            [Console]::Error.WriteLine('CPU_ERROR_DIAGNOSTIC request='+$sequence)
            [Console]::Out.WriteLine('NR_QA_RETAINED_REPLY {"request":'+$sequence+',"normal_closed":false}')
        }
    } finally {$request.Dispose();$close.Dispose()}
    return
}
$registered=[Threading.EventWaitHandle]::OpenExisting($namespace+'-registered')
$request=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,($namespace+'-host-request'))
$reply=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,($namespace+'-host-reply'))
$close=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,($namespace+'-cpu-close'))
$controller=[Diagnostics.Process]::GetCurrentProcess()
$p=[Diagnostics.Process]::new()
$p.StartInfo.FileName=$shell
$p.StartInfo.Arguments='-NoProfile -ExecutionPolicy Bypass -File "'+$self+'" -Mode CpuChild -Lane CPU -Attempt "'+$Attempt+'" -Nonce '+$Nonce+' -HelperHash '+$HelperHash
$p.StartInfo.WorkingDirectory=$work;$p.StartInfo.UseShellExecute=$false;$p.StartInfo.CreateNoWindow=$true
$p.StartInfo.RedirectStandardOutput=$true;$p.StartInfo.RedirectStandardError=$true
$p.EnableRaisingEvents=$true;$exitSignal=[NrProcessExit]::new($p)
$sink=$null
if($SinkFault){$sink=[IO.StreamWriter]::new([IO.Stream]::Null);$sink.Dispose()}
$output=[NrHostOutput]::new($p,(Join-Path $Attempt 'cpu-child.log'),$sink)
if($SinkFault){[NrHostOutput].GetField('log',[Reflection.BindingFlags]'Instance,NonPublic').GetValue($output).Dispose()}
try {
    if(!$p.Start()){throw 'NR_QA_CPU_CHILD_START'}
    $identity=Identity $p
    $p.BeginOutputReadLine();$p.BeginErrorReadLine()
    @{lane='CPU';nonce=$Nonce;helper=$self;helper_sha256=$HelperHash;supervisor=(Identity $controller);host=$identity;
        events=$namespace;subscriptions_before_start=$true;sink_fault=$SinkFault.IsPresent;result='FAIL'}|
        ConvertTo-Json -Depth 10|Set-Content -Encoding UTF8 $ownership
    Wait-RetainedHost $p $identity @($registered,$request,$reply) $exitSignal $output ($namespace+'-diagnose') (Join-Path $Attempt 'supervisor.json')
    if(!$output.OutputClosed.WaitOne(15000) -or !$output.ErrorClosed.WaitOne(15000)){throw 'NR_QA_CPU_OUTPUT_DRAIN'}
    $released=@{result='FAIL';output_sink_failure=$output.SinkFailure;log_failures=$output.LogFailures;
        console_failures=$output.ConsoleFailures;stdout_closed=$true;stderr_closed=$true;child_exited=$p.HasExited;
        subscription_handles_released=$false;ipc_handles_released=$false}
} finally {
    if($p.HasExited){
        $output.Dispose();$exitSignal.Dispose();$p.Dispose();$registered.Dispose();$request.Dispose();$reply.Dispose();$close.Dispose();$controller.Dispose()
        if($released){$released.subscription_handles_released=$true;$released.ipc_handles_released=$true;
            $released|ConvertTo-Json -Depth 6|Set-Content -Encoding UTF8 (Join-Path $Attempt 'released.json')}
    }
}
} catch {
    if($Mode -eq 'Own'){[Console]::Error.WriteLine($_.ToString())}
    throw
} finally {
    if($controllerStdout){$controllerStdout.Dispose()}
    if($controllerStderr){$controllerStderr.Dispose()}
}
