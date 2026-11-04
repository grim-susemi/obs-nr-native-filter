param([Parameter(Mandatory)][string]$Work,[Parameter(Mandatory)][string]$Scratch)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
. (Join-Path $Work 'tools/host/HostSupport.ps1')
$producer=Join-Path $Work 'tools/tests/nr_retained_producer.ps1'
$dir=Join-Path $Scratch ([Guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path $dir -Force)
try {
    foreach($mode in @('retained','ordinary')) {
        $nonce=[Guid]::NewGuid().ToString('N')
        $readyName='Local\nr-cost-test-'+$nonce+'-ready'
        $startName='Local\nr-cost-test-'+$nonce+'-start'
        $stopName='Local\nr-cost-test-'+$nonce+'-stop'
        $notifyName='Local\nr-cost-retained-'+$nonce
        $ready=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,$readyName)
        $start=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,$startName)
        $stop=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,$stopName)
        $notify=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,$notifyName)
        $receiverEvent=Open-NrCostRetainedEvent $notifyName
        $p=[Diagnostics.Process]::new()
        $p.StartInfo.FileName="$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe"
        $p.StartInfo.Arguments='-NoProfile -ExecutionPolicy Bypass -File "'+$producer+'" -Ready "'+$readyName+'" -Start "'+$startName+'" -Stop "'+$stopName+'" -Mode '+$mode
        $p.StartInfo.UseShellExecute=$false;$p.StartInfo.CreateNoWindow=$true
        $p.StartInfo.RedirectStandardOutput=$true;$p.StartInfo.RedirectStandardError=$true
        $p.EnableRaisingEvents=$true
        $exit=[NrProcessExit]::new($p)
        $output=[NrHostOutput]::new($p,(Join-Path $dir ($mode+'.log')),[IO.StringWriter]::new(),$receiverEvent)
        $started=$false
        try {
            if(!$p.Start()){throw 'CPU_PRODUCER_START'}
            $started=$true
            $p.BeginOutputReadLine();$p.BeginErrorReadLine()
            if([Threading.WaitHandle]::WaitAny([Threading.WaitHandle[]]@($ready,$exit.Exited),15000) -ne 0){throw 'CPU_PRODUCER_READY'}
            [void]$start.Set()
            if($mode -eq 'retained' -and [Threading.WaitHandle]::WaitAny([Threading.WaitHandle[]]@($notify,$exit.Exited),15000) -ne 0){throw 'CPU_RETAINED_NOT_DELIVERED'}
            if(!$output.Reply.WaitOne(15000) -or $output.ReplySequence -ne 73){throw 'CPU_OUTPUT_BARRIER'}
            if($p.HasExited){throw 'CPU_PRODUCER_EXITED_BEFORE_ACK'}
            if($notify.WaitOne(0) -ne ($mode -eq 'retained')){throw 'CPU_RETAINED_WRONG_STATE'}
            if($output.SinkFailed){throw ('CPU_NOTIFICATION_SINK_FAILURE '+$output.SinkFailure)}
        } finally {
            [void]$stop.Set()
            try {
                if($started -and (!$exit.Exited.WaitOne(20000) -or !$p.WaitForExit(5000))){throw 'CPU_PRODUCER_CLEANUP_UNRESOLVED'}
                if($started -and (!$output.OutputClosed.WaitOne(5000) -or !$output.ErrorClosed.WaitOne(5000))){throw 'CPU_PRODUCER_STREAMS_UNRESOLVED'}
                if($started -and $p.ExitCode -ne 0){throw "CPU_PRODUCER_EXIT $($p.ExitCode)"}
            } finally {
                if(!$started -or $p.HasExited){$output.Dispose();$exit.Dispose();$p.Dispose();$receiverEvent.Dispose()}
                $ready.Dispose();$start.Dispose();$stop.Dispose();$notify.Dispose()
            }
        }
    }
    Write-Output 'PASS retained_notification CPU_only=true'
} finally {
    Remove-Item -LiteralPath $dir -Recurse -Force
}
