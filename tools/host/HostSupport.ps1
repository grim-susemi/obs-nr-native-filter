# SPDX-License-Identifier: GPL-2.0-or-later
Add-Type -TypeDefinition @'
using System;
using System.Diagnostics;
using System.Threading;
public sealed class NrProcessExit : IDisposable {
    [System.Runtime.InteropServices.DllImport("kernel32.dll", CharSet=System.Runtime.InteropServices.CharSet.Unicode, SetLastError=true)]
    private static extern bool QueryFullProcessImageName(IntPtr process, uint flags, System.Text.StringBuilder path, ref uint size);
    public static string Image(Process process) {
        uint size=32768;
        var path=new System.Text.StringBuilder((int)size);
        if(!QueryFullProcessImageName(process.Handle,0,path,ref size))
            throw new System.ComponentModel.Win32Exception(System.Runtime.InteropServices.Marshal.GetLastWin32Error());
        return path.ToString();
    }
    public readonly ManualResetEvent Exited = new ManualResetEvent(false);
    private readonly object exitGate = new object();
    private readonly Process process;
    private readonly EventHandler exitHandler;
    private bool disposed;
    public NrProcessExit(Process process) {
        this.process=process;
        exitHandler=(sender, args) => { lock(exitGate) { if(!disposed) Exited.Set(); } };
        process.Exited+=exitHandler;
    }
    public void Dispose() {
        lock(exitGate) {
            if(disposed)return;
            disposed=true;
            process.Exited-=exitHandler;
            Exited.Dispose();
        }
    }
}
public sealed class NrHostOutput : IDisposable {
    private readonly System.Text.StringBuilder output = new System.Text.StringBuilder();
    private readonly System.Text.StringBuilder error = new System.Text.StringBuilder();
    private readonly System.IO.StreamWriter log;
    private readonly System.IO.TextWriter console;
    private string sinkFailure;
    private int logFailures,consoleFailures;
    public int LogFailures { get { lock(gate) { return logFailures; } } }
    public int ConsoleFailures { get { lock(gate) { return consoleFailures; } } }
    public bool SinkFailed { get { lock(gate) { return sinkFailure!=null; } } }
    public string SinkFailure { get { lock(gate) { return sinkFailure; } } }
    private readonly object gate = new object();
    public readonly ManualResetEvent OutputClosed = new ManualResetEvent(false);
    public readonly ManualResetEvent ErrorClosed = new ManualResetEvent(false);
    public readonly ManualResetEvent Reply = new ManualResetEvent(false);
    private long replySequence;
    public long ReplySequence { get { lock(gate) { return replySequence; } } }
    public NrHostOutput(Process process, string path, System.IO.TextWriter consoleSink=null) {
        console=consoleSink ?? Console.Out;
        log = new System.IO.StreamWriter(path, false, new System.Text.UTF8Encoding(false));
        log.AutoFlush = true;
        process.OutputDataReceived += (sender, args) => {
            if (args.Data == null) { OutputClosed.Set(); return; }
            lock (gate) {
                output.AppendLine(args.Data);
                try { log.WriteLine("OUT " + args.Data); } catch(Exception failure) { logFailures++;RememberFailure(failure); }
                const string prefix="NR_QA_RETAINED_REPLY {\"request\":";
                if(args.Data.StartsWith(prefix, StringComparison.Ordinal)) {
                    int comma=args.Data.IndexOf(',',prefix.Length);
                    long sequence;
                    if(comma>0 && long.TryParse(args.Data.Substring(prefix.Length,comma-prefix.Length),out sequence)) {
                        replySequence=sequence;Reply.Set();
                    }
                }
            }
            try { console.WriteLine(args.Data); } catch(Exception failure) { lock(gate) { consoleFailures++;RememberFailure(failure); } }
        };
        process.ErrorDataReceived += (sender, args) => {
            if (args.Data == null) { ErrorClosed.Set(); return; }
            lock (gate) {
                error.AppendLine(args.Data);
                try { log.WriteLine("ERR " + args.Data); } catch(Exception failure) { logFailures++;RememberFailure(failure); }
            }
            try { console.WriteLine(args.Data); } catch(Exception failure) { lock(gate) { consoleFailures++;RememberFailure(failure); } }
        };
    }
    public string Text { get { lock (gate) { return output.ToString() + error.ToString(); } } }
    private void RememberFailure(Exception failure) {
        if(sinkFailure!=null)return;
        string text=failure.GetType().Name+": "+failure.Message;
        sinkFailure=text.Substring(0,Math.Min(512,text.Length));
    }
    public void Dispose() { log.Dispose(); OutputClosed.Dispose(); ErrorClosed.Dispose(); Reply.Dispose(); }
}
'@
function Assert-OwnedRoot([string]$Candidate) {
    if ([IO.Path]::GetFullPath($Candidate).TrimEnd('\','/') -ne $owned) { throw 'NR_QA_FOREIGN_ROOT' }
    if (Test-Path $Candidate) { throw 'NR_QA_EXISTING_ROOT' }
}
function Identity($Process) {
    [ordered]@{pid=$Process.Id; creation=$Process.StartTime.ToUniversalTime().ToString('o'); image=[NrProcessExit]::Image($Process)}
}
function Read-LifecycleClosure([string]$Output) {
    foreach($line in ($Output -split [char]10)) {
        if($line.StartsWith('NR_LIFECYCLE_CLOSURE ')) {
            $line.Substring(21) | ConvertFrom-Json
        }
    }
}
function Assert-LifecycleClosure($Rows) {
    $final=@($Rows | Where-Object {$_.stage -eq 'post-source-destruction'})
    if($final.Count -ne 1){throw 'NR_QA_LIFECYCLE_CLOSURE_SET'}
    $s=$final[0]
    if($s.normal_closed -isnot [bool] -or !$s.normal_closed -or $s.stop -isnot [bool] -or !$s.stop -or
        $s.closed -isnot [bool] -or !$s.closed -or $s.worker_exit -isnot [bool] -or !$s.worker_exit -or
        $s.block -ne 0 -or $s.pipeline -ne 2 -or $s.retiring_count -ne 0){throw 'NR_QA_LIFECYCLE_CLOSURE'}
    if(@($s.generations).Count -ne 3){throw 'NR_QA_LIFECYCLE_CLOSURE_GENERATION_SET'}
    foreach($g in $s.generations) {
        if(!$g.id){continue}
        if(!$g.closed -or $g.bridge -ne 2 -or $g.generation -ne 2 -or $g.cutoff_leases -ne 0 -or
            [UInt64]$g.graphics_done -eq [UInt64]::MaxValue -or [UInt64]$g.graphics_done -lt [UInt64]$g.graphics){
            throw 'NR_QA_LIFECYCLE_CLOSURE_GENERATION'
        }
        foreach($slot in $g.slots) {
            if(!$slot.occupied){continue}
            if(!$slot.ready -or !$slot.returned -or $slot.pin){throw 'NR_QA_LIFECYCLE_CLOSURE_SLOT'}
            foreach($fence in @('producer','worker','consumer')) {
                $done=$fence+'_done'
                if([UInt64]$slot.$done -eq [UInt64]::MaxValue -or [UInt64]$slot.$done -lt [UInt64]$slot.$fence){
                    throw 'NR_QA_LIFECYCLE_CLOSURE_FENCE'
                }
            }
        }
        if($g.has_model) {
            foreach($field in @('model_settlement','feature_release','ofa_release','module_release','core_close')) {
                if($g.$field -ne 2){throw 'NR_QA_LIFECYCLE_CLOSURE_MODEL'}
            }
            if(!$g.coherent -or $g.exception -or $g.module_error -or $g.core_remaining -or
                ($g.module_required -and $g.module_present) -or
                ($g.core_called -and $g.core_result -ne 1)){throw 'NR_QA_LIFECYCLE_CLOSURE_MODEL'}
        }
    }
}
function New-RetainedHostEvents([string]$Scenario,[bool]$CoreInitControl,[string]$StopName) {
    $ownedEvents=@()
    try {
        if($Scenario -eq 'lifecycle' -or $CoreInitControl) {
            foreach($suffix in @('.host-diagnose','.host-diagnosed')) {
                $ownedEvents+=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,($StopName+$suffix))
            }
        }
        $ownedEvents
    }catch{foreach($event in $ownedEvents){$event.Dispose()};throw}
}
function Assert-ReadyHostPair($Inventory,[string]$Root,[string]$Build,[string]$Scenario,[bool]$CoreInitControl) {
    if($Scenario -notin @('native-bootstrap','ghost-calibration','lifecycle') -and !$CoreInitControl){return}
    foreach($pair in @(@('nr-qa-host.exe','bin/64bit/nr-qa-host.exe'),@('obs-nr-filter.dll','obs-plugins/64bit/obs-nr-filter.dll'))) {
        $actual=@($Inventory|Where-Object {$_.path -eq (Join-Path $Root $pair[1])})
        if($actual.Count -ne 1 -or $actual[0].sha256 -ne (Get-FileHash (Join-Path $Build $pair[0])).Hash){throw 'NR_QA_BOOTSTRAP_FRESH_BINARY_BINDING'}
    }
}
function Read-CoreControlContext([string]$Output) {
    foreach($line in ($Output -split [char]10)) {
        if(!$line.StartsWith('NR_CORE_CONTROL_CONTEXT ')){continue}
        $c=$line.Substring(24)|ConvertFrom-Json
        if(!$c.instance -or !$c.device -or !$c.queue -or $c.vendor_id -ne 4318 -or $c.software -isnot [bool] -or $c.software -or
            !$c.device_id -or @($c.adapter_description_utf16).Count -lt 1 -or @($c.adapter_description_utf16).Count -gt 128){throw 'NR_QA_CORE_CONTEXT'}
        foreach($part in @('high','low')) {
            if($c.('renderer_luid_'+$part) -ne $c.('context_luid_'+$part) -or $c.('renderer_luid_'+$part) -ne $c.('worker_luid_'+$part)){throw 'NR_QA_CORE_LUID'}
        }
        $c
    }
}
function Read-CoreControlRows([string]$Output) {
    foreach($line in ($Output -split [char]10)) {
        if(!$line.StartsWith('NR_CORE_CONTROL ')){continue}
        $row=$line.Substring(16)|ConvertFrom-Json
        if($row.stage -notin @('after-first-capture','post-close','normal-close','retained-request','retained-normal-close') -or
            $row.request -isnot [long] -and $row.request -isnot [int] -or $row.request -lt 0 -or
            $row.completed_steps -lt 0 -or $row.completed_steps -gt 6){throw 'NR_QA_CORE_SCHEMA'}
        foreach($field in @('pending','normal_closed','callback_attached','exception','model_coherent')) {
            if($row.$field -isnot [bool]){throw 'NR_QA_CORE_SCHEMA'}
        }
        $v=$row.vendor_trace
        if(!$v.owner -or !$v.instance -or !$v.generation -or !$v.device -or !$v.coherent -or
            $v.nested_core_observed -isnot [bool] -or $v.nested_core_observed -or @($v.calls).Count -ne 7){throw 'NR_QA_CORE_IDENTITY'}
        for($i=0;$i -lt 7;$i++) {
            $c=$v.calls[$i]
            if($c.stage -ne $i -or $c.count -lt 0){throw 'NR_QA_CORE_CALL_SCHEMA'}
            foreach($receipt in @($c.first,$c.latest)) {
                if($receipt.result_known -isnot [bool] -or $receipt.remaining_known -isnot [bool] -or
                    ($receipt.result_known -and (!$receipt.enter -or !$receipt.returned -or !$receipt.module -or !$receipt.procedure -or !$receipt.thread))){throw 'NR_QA_CORE_CALL_SCHEMA'}
            }
        }
        if($v.calls[0].count -ne 1 -or !$v.calls[0].first.result_known -or $v.calls[0].first.result -ne 1){throw 'NR_QA_CORE_INIT'}
        foreach($i in @(1,2,3,4)){if($v.calls[$i].count -ne 0){throw 'NR_QA_CORE_NR_ENTERED'}}
        $row
    }
}
function Get-CoreControlEnvelope([string]$Output,[UInt64]$Request) {
    $rows=@();$current=@();$context=@();$failure=$null
    try {
        $rows=@(Read-CoreControlRows $Output)
        $context=@(Read-CoreControlContext $Output)
        if($rows.Count -and $context.Count -ne 1){throw 'NR_QA_CORE_CONTEXT_SET'}
        $current=@($rows|Where-Object {$_.stage -eq 'retained-request' -and $_.request -eq $Request})
        if($Output.Contains('NR_CORE_CONTROL ') -and $current.Count -ne 1){throw 'NR_QA_CORE_CURRENT_REPLY'}
    }catch{$failure=$_.Exception.Message}
    @{rows=$rows;current=$current;context=$context;failure=$failure}
}
function Assert-CoreControl($Rows,$Capture,$Context) {
    $first=@($Rows|Where-Object {$_.stage -eq 'after-first-capture'})
    $closed=@($Rows|Where-Object {$_.stage -eq 'normal-close'})
    if($first.Count -ne 1 -or $closed.Count -ne 1 -or @($Capture).Count -ne 1 -or @($Context).Count -ne 1){throw 'NR_QA_CORE_SET'}
    if($Context[0].device -ne $first[0].vendor_trace.device -or $Context[0].instance -ne $first[0].vendor_trace.instance){throw 'NR_QA_CORE_CONTEXT_OWNER'}
    $c=$Capture[0]
    if($c.captures -ne 1 -or $c.fault_code -ne 57369 -or !$c.dispatch_returned -or !$c.observed -or
        !$c.raw_hash -or $c.raw_hash -ne $c.analysis_hash){throw 'NR_QA_CORE_CAPTURE'}
    foreach($field in @('created','evaluated','composed','submitted','eligible','resources','runtime_module','applied')) {
        if($c.$field -isnot [bool] -or $c.$field){throw 'NR_QA_CORE_CAPTURE'}
    }
    $s=$closed[0];$v=$s.vendor_trace;$shutdown=$v.calls[5]
    foreach($field in @('owner','instance','generation','device')) {
        if($v.$field -ne $first[0].vendor_trace.$field){throw 'NR_QA_CORE_OWNER_CHANGED'}
    }
    if(!$s.normal_closed -or $s.pending -or $s.callback_attached -or $s.exception -or !$s.model_coherent -or
        $s.completed_steps -ne 6 -or $s.module_error -or $shutdown.count -lt 1 -or
        !$shutdown.first.result_known -or !$shutdown.first.remaining_known -or $shutdown.first.result -ne 1 -or
        $shutdown.first.remaining -ne 0 -or !$shutdown.latest.result_known -or $shutdown.latest.result -ne 1 -or
        $shutdown.latest.remaining -ne 0){throw 'NR_QA_CORE_CLOSE_REFUSED'}
}
function Write-RetainedReceipt([string]$Text,[string]$Path) {
    try{[IO.File]::WriteAllText($Path,$Text,[Text.UTF8Encoding]::new($false))}
    catch{[Console]::Error.WriteLine('NR_QA_RETAINED_RECEIPT_WRITE_FAILURE: '+$_.Exception.Message)}
}
function Wait-RetainedHost($Process,$ExpectedIdentity,$Events,$ExitSignal,$Output,[string]$RequestName,[string]$ReceiptPath) {
    $request=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::AutoReset,$RequestName)
    $response=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,($RequestName+'.reply'))
    $controller=[Diagnostics.Process]::GetCurrentProcess()
    $sequence=0
    try {
        $supervisor=Identity $controller
        $registered=@{state='supervised-retained';result='FAIL';host=$ExpectedIdentity;supervisor=$supervisor;
            request_event=$RequestName;reply_event=($RequestName+'.reply');raw_log_retained=$true;
            handles_owned=$true;automatic_retry=$false;output_sink_failure=$Output.SinkFailure}|ConvertTo-Json -Depth 8
        Write-RetainedReceipt $registered $ReceiptPath
        $Events[0].Set()|Out-Null
        for(;;) {
            $wake=[Threading.WaitHandle]::WaitAny([Threading.WaitHandle[]]@($ExitSignal.Exited,$request))
            if($wake -eq 0){break}
            $response.Reset()|Out-Null
            $sameIdentity=$false
            try{$sameIdentity=(Identity $Process|ConvertTo-Json -Compress) -eq ($ExpectedIdentity|ConvertTo-Json -Compress)}catch{}
            if(!$sameIdentity){
                $refused=@{state='supervised-retained';result='FAIL';failure='NR_QA_RETAINED_IDENTITY_REFUSED';
                    host=$ExpectedIdentity;supervisor=$supervisor;handles_owned=$true;
                    request_event=$RequestName;reply_event=($RequestName+'.reply')}|ConvertTo-Json -Depth 8
                Write-RetainedReceipt $refused $ReceiptPath
                $response.Set()|Out-Null
                continue
            }
            $sequence++
            $Events[2].Reset()|Out-Null;$Output.Reply.Reset()|Out-Null
            $Events[1].Set()|Out-Null
            $reply=[Threading.WaitHandle]::WaitAny([Threading.WaitHandle[]]@($ExitSignal.Exited,$Output.Reply),15000)
            $fresh=$reply -eq 1 -and $Output.ReplySequence -eq $sequence
            $closureFailure=$null;$closures=@()
            try{$closures=@(Read-LifecycleClosure $Output.Text)}catch{$closureFailure=$_.Exception.Message}
            $core=Get-CoreControlEnvelope $Output.Text $sequence
            $diagnostic=@{state='supervised-retained';result='FAIL';host=$ExpectedIdentity;supervisor=$supervisor;
                request_event=$RequestName;reply_event=($RequestName+'.reply');request=$sequence;
                fresh_reply=$fresh;closures=$closures;current_closure=@($closures|Where-Object {$_.stage -eq 'retained-request' -and $_.request -eq $sequence});
                closure_parse_failure=$closureFailure;output_sink_failure=$Output.SinkFailure;raw_diagnostics=$Output.Text;
                core_control=$core.rows;current_core_control=$core.current;core_control_context=$core.context;core_control_parse_failure=$core.failure;
                log_sink_failures=$Output.LogFailures;console_sink_failures=$Output.ConsoleFailures;
                handles_owned=$true;host_exited=$Process.HasExited}|ConvertTo-Json -Depth 14
            Write-RetainedReceipt $diagnostic ($ReceiptPath+'.request-'+$sequence+'.json')
            Write-RetainedReceipt $diagnostic $ReceiptPath
            $response.Set()|Out-Null
        }
        $Process.WaitForExit()
        $finalClosures=@()
        try{$finalClosures=@(Read-LifecycleClosure $Output.Text)}catch{}
        $finalCore=Get-CoreControlEnvelope $Output.Text $sequence
        $exited=@{state='supervised-exited';result='FAIL';host=$ExpectedIdentity;supervisor=$supervisor;
            exit=$Process.ExitCode;request=$sequence;closures=$finalClosures;
            core_control=$finalCore.rows;current_core_control=$finalCore.current;core_control_context=$finalCore.context;core_control_parse_failure=$finalCore.failure;
            handles_owned=$true;output_sink_failure=$Output.SinkFailure;raw_diagnostics=$Output.Text}|ConvertTo-Json -Depth 14
        Write-RetainedReceipt $exited $ReceiptPath
    } finally {
        $request.Dispose();$response.Dispose();$controller.Dispose()
    }
}
function Request-RetainedHost([string]$ReceiptPath) {
    $receipt=Get-Content -Raw -LiteralPath $ReceiptPath|ConvertFrom-Json
    if($receipt.state -ne 'supervised-retained'){throw 'NR_QA_NO_RETAINED_SUPERVISOR'}
    foreach($expected in @($receipt.host,$receipt.supervisor)) {
        $p=[Diagnostics.Process]::GetProcessById($expected.pid)
        try {
            if((Identity $p|ConvertTo-Json -Compress) -ne ($expected|ConvertTo-Json -Compress)){throw 'NR_QA_RETAINED_IDENTITY_REFUSED'}
        } finally {$p.Dispose()}
    }
    $request=[Threading.EventWaitHandle]::OpenExisting($receipt.request_event)
    $response=[Threading.EventWaitHandle]::OpenExisting($receipt.reply_event)
    try {
        $response.Reset()|Out-Null;$request.Set()|Out-Null
        if(!$response.WaitOne(15000)){throw 'NR_QA_RETAINED_RESPONSE_TIMEOUT'}
        Get-Content -Raw -LiteralPath $ReceiptPath|ConvertFrom-Json
    } finally {$request.Dispose();$response.Dispose()}
}
function FileIdentity([string]$Path) {
    $fileId = (& "$env:SystemRoot/System32/fsutil.exe" file queryfileid $Path | Out-String).Trim()
    if ($LASTEXITCODE) { throw 'NR_QA_FILEID_FAILED' }
    $item = Get-Item -LiteralPath $Path
    @{path=$Path;sha256=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash;bytes=$item.Length;file_id=$fileId;attributes=$item.Attributes.ToString()}
}
function Resolve-NrDriver([string]$Inspector) {
    if((Get-Variable -Name NrQaGhostInspectorOwner -Scope Global -ErrorAction SilentlyContinue) -and -not $global:NrQaGhostInspectorOwner.closed){throw 'NR_QA_DRIVER_INSPECT_OWNER_EXISTS'}
    $p=[Diagnostics.Process]::new()
    $p.StartInfo.FileName=$Inspector;$p.StartInfo.WorkingDirectory=Split-Path -Parent $Inspector
    $p.StartInfo.UseShellExecute=$false;$p.StartInfo.CreateNoWindow=$true
    $p.StartInfo.RedirectStandardOutput=$true;$p.StartInfo.RedirectStandardError=$true
    $p.EnableRaisingEvents=$true;$signal=[NrProcessExit]::new($p)
    # N1B-CUSTODY-EOF: retain the exact process, signal and reader tasks until original reap and both successful EOF observations.
    # Exit status alone is not a closure barrier; incomplete proof stays addressable in the original owner.
    $global:NrQaGhostInspectorOwner=@{process=$p;signal=$signal;identity=$null;started=$false;closed=$false;state='created';inspector=$Inspector;
        stdout=$null;stderr=$null;reaped=$false;stdoutObserved=$false;stderrObserved=$false}
    $owner=$global:NrQaGhostInspectorOwner
    try {
        if(!$p.Start()){throw 'NR_QA_DRIVER_INSPECT_START'}
        $owner.started=$true;$owner.state='started'
        $owner.stdout=$p.StandardOutput.ReadToEndAsync();$owner.stderr=$p.StandardError.ReadToEndAsync()
        $identity=Identity $p
        $owner.identity=$identity
        if(!$signal.Exited.WaitOne(15000)){$owner.state='unresolved-retained';throw 'NR_QA_DRIVER_INSPECT_UNRESOLVED'}
        if(!$p.WaitForExit(5000)){$owner.state='reap-unresolved-retained';throw 'NR_QA_DRIVER_INSPECT_REAP'}
        $owner.reaped=$true

        $text=$owner.stdout.GetAwaiter().GetResult();$owner.stdoutObserved=$true
        $errorText=$owner.stderr.GetAwaiter().GetResult();$owner.stderrObserved=$true
        if($p.ExitCode){throw ('NR_QA_DRIVER_BINDING_REFUSED: '+$errorText)}
        $binding=$text|ConvertFrom-Json
        if($binding.schema -ne 1 -or $binding.kind -ne 'active-nvidia-driver-core' -or
            $binding.loaded -isnot [bool] -or $binding.loaded -or $binding.gpu_calls -ne 0 -or
            $binding.signature_verified -isnot [bool] -or !$binding.signature_verified -or
            $binding.required_exports -ne 7 -or $binding.sha256 -cnotmatch '^[0-9a-f]{64}$' -or
            ![IO.Path]::IsPathRooted($binding.path) -or [IO.Path]::GetFileName($binding.path) -ne '_nvngx.dll' -or
            !$binding.device_instance -or !$binding.published_inf -or !$binding.active_package -or
            !$binding.driver_version -or !$binding.inf_section -or
            ![IO.Path]::IsPathRooted($binding.driver_store_inf)) {throw 'NR_QA_DRIVER_INSPECT_SCHEMA'}
        $actual=FileIdentity $binding.path
        if($actual.sha256.ToLowerInvariant() -ne $binding.sha256){throw 'NR_QA_DRIVER_CHANGED'}
        @{binding=$binding;file=$actual;inspector=(FileIdentity $Inspector);identity=$identity;
            exit=$p.ExitCode;subscription_before_start=$true;module_loaded=$false;gpu_calls=0}
    } finally {
        if(!$owner.started){$signal.Dispose();$p.Dispose();$owner.closed=$true;$owner.state='never-started-closed'}
        elseif($p.HasExited){
            try{
                if(!$owner.reaped){$owner.reaped=$p.WaitForExit(0)}
                if($owner.reaped){
                    if(!$owner.stdoutObserved -and $null -ne $owner.stdout -and $owner.stdout.IsCompleted){$null=$owner.stdout.GetAwaiter().GetResult();$owner.stdoutObserved=$true}
                    if(!$owner.stderrObserved -and $null -ne $owner.stderr -and $owner.stderr.IsCompleted){$null=$owner.stderr.GetAwaiter().GetResult();$owner.stderrObserved=$true}
                }
            }catch{$owner.state='drain-unresolved-retained'}
            if($owner.reaped -and $owner.stdoutObserved -and $owner.stderrObserved){$signal.Dispose();$p.Dispose();$owner.closed=$true;$owner.state='closed-after-exit'}
            else{$owner.state='drain-unresolved-retained'}
        }
    }
}
function Assert-NoUnrelatedProcess {
    $active = @(Get-Process | Where-Object { $_.ProcessName -match '^(obs64|nr-qa-host|CrimsonDesert|GTA5|GTA5_Enhanced|Cyberpunk2077|eldenring|BlackMythWukong|cs2|dota2|FortniteClient.*)$' })
    if ($active.Count) { throw ('NR_QA_UNRELATED_PROCESS: ' + ($active.Id -join ',')) }
}
function Assert-BootstrapRows($Frames,$Lanes,$Decoded,$Cleanup,$Slots,$SlotCleanup) {
    if($Lanes.Count -ne 4 -or $Cleanup.Count -ne 4 -or $Slots.Count -ne 1 -or $SlotCleanup.Count -ne 1) { throw 'NR_QA_BOOTSTRAP_SET' }
    $slot=$Slots[0]
    if($slot.targeted_slot -ne 2 -or $slot.lower_reusable_slot -ne 0 -or $slot.old_api_slot -ne 0 -or
        $slot.refusal_ownership_unchanged -isnot [bool] -or !$slot.refusal_ownership_unchanged -or
        $slot.serial_not_consumed -isnot [bool] -or !$slot.serial_not_consumed -or $slot.producer_cursor -ne 6) { throw 'NR_QA_BOOTSTRAP_SLOT' }
    foreach($close in @($Cleanup)+@($SlotCleanup)) {
        if($close.closed -isnot [bool] -or !$close.closed -or $close.worker_exit -isnot [bool] -or
            !$close.worker_exit -or $close.retained -isnot [bool] -or $close.retained) { throw 'NR_QA_BOOTSTRAP_CLEANUP' }
    }
    for($lane=0;$lane -lt 4;$lane++) {
        $hdr=$lane -ge 2;$deterministic=($lane%2)-eq 0
        $summary=$Lanes[$lane];$close=$Cleanup[$lane]
        $group=@($Frames | Where-Object {$_.lane -eq $lane})
        $recorded=@($Decoded | Where-Object {$_.lane -eq $lane})
        if($summary.lane -ne $lane -or $close.lane -ne $lane -or $summary.hdr -isnot [bool] -or
            $summary.deterministic -isnot [bool] -or $summary.hdr -ne $hdr -or $summary.deterministic -ne $deterministic -or
            $summary.packets -lt 8 -or $summary.packets -gt 16 -or $recorded.Count -ne $summary.packets -or
            $summary.decoded_frames -ne $recorded.Count -or $group.Count -lt 3 -or $group.Count -gt 16) { throw 'NR_QA_BOOTSTRAP_SET' }
        $applied=0;$previous=$null
        foreach($row in $group) {
            foreach($field in @('hdr','deterministic','genuine_nr','same_latch_views')) {
                if($row.$field -isnot [bool]) { throw 'NR_QA_BOOTSTRAP_BOOLEAN' }
            }
            if($row.hdr -ne $hdr -or $row.deterministic -ne $deterministic -or !$row.same_latch_views -or
                $row.width -ne 1920 -or $row.height -ne 1080 -or $row.instance -le 0 -or $row.content_epoch -le 0 -or
                $row.interval -ne 1000000000 -or $row.tick -ne ([Math]::Floor($row.clock/$row.interval)+1) -or
                $row.ingress -le 0 -or $row.alpha_errors -ne 0 -or $row.source_id -ne $row.selected -or
                $row.filter_id -ne $row.source_id -or $row.program_id -ne $row.source_id -or
                $row.kind -notin @(1,2,3) -or $row.selected -ne $(if($row.kind -eq 1){$row.ingress}else{$row.ingress-1})) {
                throw 'NR_QA_BOOTSTRAP_LINEAGE'
            }
            if($previous -and ($row.tick -ne $previous.tick+1 -or $row.ingress -ne $previous.ingress+1 -or
                $row.unrelated_hash -ne $previous.unrelated_hash)) { throw 'NR_QA_BOOTSTRAP_LINEAGE' }
            foreach($field in @('raw_hash','filter_hash','program_hash','unrelated_hash')) {
                if(!$row.$field -or [UInt64]$row.$field -eq 0) { throw 'NR_QA_BOOTSTRAP_IMAGE_HASH' }
            }
            if($deterministic) {
                if($row.genuine_nr -or $row.raw_hash -ne $row.filter_hash -or $row.same_key_nr_rms -ne 0) { throw 'NR_QA_BOOTSTRAP_DETERMINISTIC' }
            }
            if($row.kind -eq 3) {
                $applied++
                if($row.submission_serial -le 0 -or $row.history_epoch -le 0 -or $row.resource_generation -ne 1) { throw 'NR_QA_BOOTSTRAP_KEY' }
                if(!$deterministic -and (!$row.genuine_nr -or $row.module_base -le 0 -or
                    $row.same_key_nr_rms -is [string] -or [double]::IsNaN($row.same_key_nr_rms) -or
                    [double]::IsInfinity($row.same_key_nr_rms) -or $row.same_key_nr_rms -le 0)) { throw 'NR_QA_BOOTSTRAP_NR_NOOP' }
            }
            $previous=$row
        }
        if($applied -lt 3) { throw 'NR_QA_BOOTSTRAP_PREVIEW_ONLY' }
        $previousId=0;$positive=0
        foreach($row in $recorded) {
            if($row.width -ne 1920 -or $row.height -ne 1080 -or $row.bit_depth -ne $(if($hdr){10}else{8}) -or
                ($hdr -and $row.transfer -ne 2) -or (!$hdr -and $row.transfer -eq 2) -or
                $row.full_range -isnot [bool] -or $row.full_range -or
                $row.primaries_matrix_preserved -isnot [bool] -or !$row.primaries_matrix_preserved -or
                $row.codes_compared -ne 3110400 -or $row.code_errors -ne 0 -or
                ($hdr -and $row.decoded_peak_linear_nits -le 200)) { throw 'NR_QA_BOOTSTRAP_CODEC_COLOR' }
            if($previousId -and $row.source_id -ne $previousId+1) { throw 'NR_QA_BOOTSTRAP_DECODED_LINEAGE' }
            if(!@($group | Where-Object {$_.program_id -eq $row.source_id -and $_.kind -eq 3}).Count) { throw 'NR_QA_BOOTSTRAP_PREVIEW_ONLY' }
            if(!$deterministic -and $row.same_id_raw_decoded_nr_rms_codes -gt 0) {$positive++}
            $previousId=$row.source_id
        }
        if(!$deterministic -and $positive -lt 3) { throw 'NR_QA_BOOTSTRAP_DECODED_NR_NOOP' }
    }
}
function Assert-LifecycleRows($Rows,$Cleanup,[switch]$Native,$Witness=@()) {
    if($Rows.Count -ne 15 -or $Cleanup.Count -ne 1){throw 'NR_QA_LIFECYCLE_SET'}
    $cases=@('initial','identical-valid','explicit-cut','invalid','reconnect','unknown','known-reconnect',
        'HDR','SDR','resize','settings','disable','enable','remove-readd','new-instance')
    for($i=0;$i -lt $Rows.Count;$i++) {
        $row=$Rows[$i]
        if($row.step -ne $i -or $row.case -ne $cases[$i] -or $row.instance -le 0 -or
            $row.source_token -le 0 -or $row.device_token -le 0 -or $row.resource_generation -le 0 -or
            $row.content_epoch -le 0 -or $row.history_epoch -le 0 -or
            $row.same_latch_views -isnot [bool] -or !$row.same_latch_views -or
            $row.physical_signal_known -isnot [bool] -or $row.physical_signal_known -ne ($i -ne 5) -or
            $row.validity_metadata_present -isnot [bool] -or !$row.validity_metadata_present -or
            $row.source_validity -ne $(if($i -eq 5){0}elseif($i -eq 3){2}else{1}) -or
            $row.placeholder -isnot [bool] -or !$row.image_hash -or [UInt64]$row.image_hash -eq 0) {
            throw 'NR_QA_LIFECYCLE_IDENTITY'
        }
        if($row.selected -gt $row.ingress -or ($row.placeholder -and $row.selected -ne 0) -or
            (!$row.placeholder -and $row.selected -le 0) -or
            ($i -in @(3,5,11) -and !$row.placeholder)){throw 'NR_QA_LIFECYCLE_STALE'}
        if($i -eq 1 -and $row.content_epoch -ne $Rows[0].content_epoch){throw 'NR_QA_LIFECYCLE_FALSE_LOSS'}
        if($i -eq 14 -and $row.instance -eq $Rows[13].instance){throw 'NR_QA_LIFECYCLE_INSTANCE'}
        if($i -gt 0 -and $i -lt 14) {
            $old=$Rows[$i-1]
            if($row.instance -ne $old.instance -or $row.history_epoch -lt $old.history_epoch -or
                ($i -ne 1 -and ($row.content_epoch -le $old.content_epoch -or $row.history_epoch -le $old.history_epoch)) -or
                ($i -in @(7,8,9,10) -and $row.resource_generation -le $old.resource_generation) -or
                ($i -notin @(7,8,9,10) -and $row.resource_generation -ne $old.resource_generation)) {
                throw 'NR_QA_LIFECYCLE_EPOCH_TRANSITION'
            }
        }
        if($row.deterministic -isnot [bool] -or $row.deterministic -eq $Native.IsPresent -or
            $row.genuine_nr -isnot [bool] -or $row.ordinary_ofa -isnot [bool]){throw 'NR_QA_LIFECYCLE_PROCESSOR'}
        if($row.view_content_id -le 0 -or $row.current_content_id -le 0 -or
            ($row.placeholder -and $row.view_content_id -ne $row.current_content_id)) {throw 'NR_QA_LIFECYCLE_IMAGE_PHASE'}
        if(!$row.placeholder) {
            foreach($field in @('raw_hash','current_raw_hash','output_hash')) {
                if(!$row.$field -or [UInt64]$row.$field -eq 0){throw 'NR_QA_LIFECYCLE_IMAGE_HASH'}
            }
            if($row.view_content_id -ne $row.raw_content_id -or
                ($row.kind -eq 1 -and ($row.selected -ne $row.ingress -or $row.raw_hash -ne $row.current_raw_hash)) -or
                ($row.kind -in @(2,3) -and $row.selected+1 -ne $row.ingress) -or $row.kind -notin @(1,2,3) -or
                ((!$Native -or $row.kind -ne 3) -and $row.raw_hash -ne $row.output_hash)) {
                throw 'NR_QA_LIFECYCLE_IMAGE_PHASE'
            }
            if($Native -and (!$row.genuine_nr -or $row.kind -ne 3 -or $row.output_hash -eq $row.raw_hash)) {
                throw 'NR_QA_LIFECYCLE_NR_NOOP'
            }
            if($Native) {
                $match=@($Witness|Where-Object {$_.step -eq $i -and $_.instance -eq $row.instance -and
                    $_.ingress -eq $row.selected -and $_.content_epoch -eq $row.content_epoch -and
                    $_.history_epoch -eq $row.selected_history_epoch -and $_.submission_serial -eq $row.submission_serial -and
                    $_.resource_generation -eq $row.resource_generation -and $_.ordinary_ofa -and
                    !$_.flow_reset -and $_.reference_valid -and $_.raw_hash -eq $row.raw_hash -and
                    $_.output_hash -eq $row.output_hash})
                if($match.Count -ne 1){throw 'NR_QA_LIFECYCLE_EDITED_TRANSITION'}
            }
        }
        if(!$Native -and ($row.genuine_nr -or $row.ordinary_ofa)){throw 'NR_QA_LIFECYCLE_PROCESSOR'}
    }
    $close=$Cleanup[0]
    if($close.closed -isnot [bool] -or !$close.closed -or $close.worker_exit -isnot [bool] -or
        !$close.worker_exit -or $close.retained -isnot [bool] -or $close.retained){throw 'NR_QA_LIFECYCLE_CLEANUP'}
    if($Native -and ($close.genuine_nr_count -lt 12 -or $close.ordinary_ofa_count -le 0 -or
        !@($Rows|Where-Object {$_.ordinary_ofa}).Count)){throw 'NR_QA_LIFECYCLE_NR_NOOP'}
    if($Native) {
        if($Witness.Count -lt 12 -or $Witness.Count -gt 240){throw 'NR_QA_LIFECYCLE_WITNESS_SET'}
        foreach($w in $Witness) {
            foreach($field in @('evaluated','composed','submitted','eligible','reference_valid','flow_reset','ordinary_ofa')) {
                if($w.$field -isnot [bool]){throw 'NR_QA_LIFECYCLE_WITNESS'}
            }
            if(!$w.evaluated -or !$w.composed -or !$w.submitted -or !$w.eligible -or
                $w.module_base -le 0 -or !$w.module_path -or $w.invalid -ne 0 -or
                $w.submission_epoch -le $w.creation_epoch -or $w.completion_value -le 0 -or
                $w.completion_done -lt $w.completion_value -or $w.completion_done -eq [UInt64]::MaxValue -or
                $w.producer -le 0 -or $w.producer_done -lt $w.producer -or
                $w.worker -le 0 -or $w.worker_done -lt $w.worker -or $w.retired_generations -gt 2 -or
                $w.renderer_luid_high -ne $w.worker_luid_high -or $w.renderer_luid_low -ne $w.worker_luid_low -or
                !$w.raw_hash -or !$w.output_hash -or [UInt64]$w.raw_hash -eq 0 -or [UInt64]$w.output_hash -eq 0) {
                throw 'NR_QA_LIFECYCLE_WITNESS'
            }
            if($w.flow_reset) {
                if($w.reference_valid -or $w.ordinary_ofa -or $w.raw_hash -ne $w.output_hash){
                    throw 'NR_QA_LIFECYCLE_RESET_IMAGE'
                }
            }elseif(!$w.reference_valid -or !$w.ordinary_ofa -or $w.raw_hash -eq $w.output_hash){
                throw 'NR_QA_LIFECYCLE_NR_NOOP'
            }
        }
        if($close.actual_evaluations -ne $Witness.Count -or
            $close.actual_reset_samples -ne @($Witness|Where-Object {$_.flow_reset}).Count){
            throw 'NR_QA_LIFECYCLE_WITNESS_SET'
        }
    }
}
function Assert-ModelRows($Rows,$Contexts,$Cleanup,$Modules) {
    if ($Rows.Count -ne 14 -or $Contexts.Count -ne 5 -or $Cleanup.Count -ne 5 -or !$Modules.Count) { throw 'NR_QA_MODEL_ROW_SET' }
    for ($lane=0;$lane -lt 5;$lane++) {
        $context=$Contexts[$lane];$close=$Cleanup[$lane]
        if ($context.lane -ne $lane -or $context.software -isnot [bool] -or $context.software -or
            $context.private_direct_queue -isnot [bool] -or !$context.private_direct_queue -or
            $context.adapter -ne 'RTX 4090' -or [UInt64]$context.device -eq 0 -or [UInt64]$context.queue -eq 0 -or
            $context.renderer_luid_high -ne $context.worker_luid_high -or $context.renderer_luid_low -ne $context.worker_luid_low) { throw 'NR_QA_MODEL_CONTEXT' }
        if ($close.lane -ne $lane -or $close.worker_exit -isnot [bool] -or !$close.worker_exit -or
            $close.closed -isnot [bool] -or !$close.closed -or $close.retained -isnot [bool] -or $close.retained -or
            $close.applied -ne $(if($lane -eq 0){3}else{0})) { throw 'NR_QA_MODEL_CLEANUP' }
        if ($lane -eq 0 -and ($close.nr_selections -ne 3 -or $close.raw_selections -ne 0)) { throw 'NR_QA_MODEL_PUBLICATION' }
        $group=@($Rows | Where-Object {$_.lane -eq $lane})
        $count=if($lane -eq 0){6}elseif($lane -lt 3){1}else{3}
        if ($group.Count -ne $count) { throw 'NR_QA_MODEL_CASE_SET' }
        for ($step=0;$step -lt $count;$step++) {
            $row=$group[$step]
            foreach ($field in @('created','evaluated','composed','submitted','gpu_composed','reset','driver_handle','capture_pin_open')) {
                if ($row.$field -isnot [bool]) { throw 'NR_QA_MODEL_BOOLEAN' }
            }
            if (!$row.capture_pin_open -or $row.capture_pin -le 0 -or $row.consumer_done -ge $row.capture_pin) {
                throw 'NR_QA_MODEL_PIN_ORDER'
            }
            if ($row.step -ne $step -or $row.instance_id -ne (111+$lane) -or $row.ingress_sequence -ne ($step+1) -or
                $row.submission_serial -ne ($step+1) -or $row.settings_revision -ne 1 -or $row.resource_generation -ne 1 -or
                $row.tick -ne ([Math]::Floor($row.clock/$row.interval)+1) -or $row.deadline -ne ($row.tick+1) -or
                $row.slot -notin @(0,1,2) -or $row.feature -ne 18 -or $row.width -ne 1920 -or $row.height -ne 1080) { throw 'NR_QA_MODEL_KEY' }
            if (!$row.raw_hash -or [UInt64]$row.raw_hash -eq 0 -or $row.raw_hash -ne $row.analysis_hash -or
                [UInt64]$row.raw_resource -eq 0 -or [UInt64]$row.analysis_resource -eq 0 -or
                $row.raw_resource -eq $row.analysis_resource -or $row.source_format -ne 87 -or
                $row.producer -le 0 -or $row.producer_done -lt $row.producer -or $row.consumer_done -lt $row.consumer) { throw 'NR_QA_MODEL_INPUT_OR_FENCE' }
            if ($row.gpu_composed) {
                if (!$row.evaluated -or !$row.composed -or !$row.submitted -or $row.code -ne 1 -or
                    $row.invalid_status -ne 0 -or $row.changed_pixels -le 0 -or $row.physical_done -ne 1 -or
                    $row.epoch -le $row.creation_epoch -or [UInt64]$row.module_base -eq 0 -or $row.driver_handle) { throw 'NR_QA_MODEL_INFERENCE' }
                foreach ($field in @('input_hash','model_hash','motion_hash','depth_hash','output_hash')) {
                    if (!$row.$field -or [UInt64]$row.$field -eq 0) { throw 'NR_QA_MODEL_HASH' }
                }
                if ($row.input_hash -eq $row.model_hash) { throw 'NR_QA_MODEL_NOOP' }
                if ($row.color_resources.Count -ne 5 -or @($row.color_resources | Select-Object -Unique).Count -ne 5 -or
                    @($row.color_resources | Where-Object {[UInt64]$_ -eq 0}).Count -or
                    $row.descriptor_heaps.Count -ne 3 -or @($row.descriptor_heaps | Select-Object -Unique).Count -ne 3 -or
                    @($row.descriptor_heaps | Where-Object {[UInt64]$_ -eq 0}).Count -or [UInt64]$row.status_resource -eq 0 -or
                    [UInt64]$row.output_resource -eq 0 -or [UInt64]$row.motion_resource -eq 0 -or [UInt64]$row.depth_resource -eq 0) { throw 'NR_QA_MODEL_RESOURCE_ALIAS' }
                if ($row.mv_scale_x -ne 1 -or $row.mv_scale_y -ne 1 -or $row.pre_exposure -ne 1 -or $row.exposure -ne 1) { throw 'NR_QA_MODEL_GUIDE_OR_EXPOSURE' }
                if ($row.chronology.Count -ne 8) { throw 'NR_QA_MODEL_CHRONOLOGY' }
                for ($i=1;$i -lt 7;$i++) {
                    if ($row.chronology[$i] -le 0 -or $row.chronology[$i] -gt $row.chronology[$i+1]) { throw 'NR_QA_MODEL_CHRONOLOGY' }
                }
            }
            if ($lane -eq 0) {
                if (($step -eq 0 -and (!$row.created -or $row.gpu_composed)) -or
                    ($step -gt 0 -and !$row.gpu_composed)) { throw 'NR_QA_MODEL_WARM_SET' }
                if ($step -in @(1,4) -and !$row.reset) { throw 'NR_QA_MODEL_RESET' }
                if ($step -in @(2,3,5) -and ($row.reset -or $row.selected -ne 3 -or
                    $row.selected_sequence -ne $step -or $row.target_changed_pixels -le 0 -or [UInt64]$row.target_hash -eq 0)) { throw 'NR_QA_MODEL_PUBLICATION' }
                if ($step -eq 1 -and $row.selected -ne 1) { throw 'NR_QA_MODEL_RAW_PHASE' }
            } elseif (($lane -lt 3 -or $step -ge 1) -and ($row.gpu_composed -or $row.submitted -or $row.code -eq 1)) {
                throw 'NR_QA_MODEL_FAILURE_CONTROL'
            }
            if ($lane -ge 3 -and $step -eq 2 -and ($row.selected -ne 2 -or $row.target_hash -ne $group[1].raw_hash)) { throw 'NR_QA_MODEL_RAW_PHASE' }
        }
    }
}
function Assert-ColorRows($Rows, $Dispatches, $Contexts, $Controls) {
    if ($Rows.Count -ne 15 -or $Dispatches.Count -ne 50 -or $Contexts.Count -ne 1 -or $Controls.Count -ne 8) { throw 'NR_QA_COLOR_ROW_SET' }
    $context=$Contexts[0]
    if ($context.software -isnot [bool] -or $context.private_direct_queue -isnot [bool]) { throw 'NR_QA_COLOR_CONTEXT' }
    if ($context.software -or !$context.private_direct_queue -or $context.vendor_id -ne 4318 -or $context.adapter -ne 'RTX 4090' -or !$context.device -or !$context.queue -or [UInt64]$context.device -eq 0 -or [UInt64]$context.queue -eq 0 -or $context.renderer_luid_high -ne $context.worker_luid_high -or $context.renderer_luid_low -ne $context.worker_luid_low) { throw 'NR_QA_COLOR_CONTEXT' }
    for ($i=0; $i -lt 50; $i++) {
        $d=$Dispatches[$i]
        foreach ($field in @('restore','status_cleared','subscribed_before_trigger','common_handoff','cleanup')) {
            if ($d.$field -isnot [bool]) { throw 'NR_QA_COLOR_FENCE' }
        }
        if ($d.serial -ne ($i+1) -or $d.width -ne 16 -or $d.height -ne 8 -or !$d.status_cleared -or !$d.subscribed_before_trigger -or $d.fence_signaled -ne 1 -or $d.fence_completed -ne 1 -or !$d.common_handoff -or !$d.cleanup) { throw 'NR_QA_COLOR_FENCE' }
        $ids=@($d.source_resource,$d.canonical_resource,$d.output_resource,$d.status_resource)
        if (@($ids | Select-Object -Unique).Count -ne 4 -or @($ids | Where-Object { !$_ -or [UInt64]$_ -eq 0 }).Count -or !$d.heap -or [UInt64]$d.heap -eq 0) { throw 'NR_QA_COLOR_ALIAS' }
        if (!$d.raw_expected_hash -or !$d.raw_readback_hash -or $d.raw_expected_hash -ne $d.raw_readback_hash -or !$d.output_hash -or [UInt64]$d.raw_expected_hash -eq 0 -or [UInt64]$d.output_hash -eq 0) { throw 'NR_QA_COLOR_SOURCE_IDENTITY' }
    }
    for ($i=0; $i -lt 15; $i++) {
        $row=$Rows[$i]
        $color=if($i -lt 6){1}elseif($i -lt 9){2}elseif($i -lt 12){3}else{4}
        $format=if($i -lt 3){2}elseif($i -lt 6){1}elseif($i -lt 12){3}else{4}
        $white=if($color -eq 1){1}else{3.80625}
        $scale=if($color -eq 1){1}elseif($color -eq 2){1.5}elseif($color -eq 3){3.80625}else{0.01875}
        foreach ($field in @('source_load','canonical_load','canonical_store','output_store','basic_double')) {
            if ($row.$field -isnot [bool]) { throw 'NR_QA_COLOR_METADATA_OR_CAPS' }
        }
        if ($row.normalized_codes -is [string] -or [double]::IsNaN($row.normalized_codes) -or [double]::IsInfinity($row.normalized_codes) -or $row.half_ulps -is [string] -or [double]::IsNaN($row.half_ulps) -or [double]::IsInfinity($row.half_ulps)) { throw 'NR_QA_COLOR_ORACLE' }
        if ($row.case -ne $i -or $row.color -ne $color -or $row.format -ne $format -or $row.alpha -ne (($i%3)+1) -or $row.width -ne 16 -or $row.height -ne 8 -or $row.source_white_nits -ne 203 -or $row.pre_exposure -ne 2 -or $row.exposure -ne 3 -or $row.white_point -ne $white -or $row.linear_scale -ne $scale -or !$row.source_load -or !$row.canonical_load -or !$row.canonical_store -or !$row.output_store -or !$row.basic_double) { throw 'NR_QA_COLOR_METADATA_OR_CAPS' }
        if ($row.compared_pixels -ne 256 -or $row.identity_pixels -ne 128 -or $row.alpha_errors -ne 0 -or $row.normalized_codes -lt 0 -or $row.normalized_codes -gt 1 -or $row.half_ulps -lt 0 -or $row.half_ulps -gt 2 -or $row.negative_checks -lt 0 -or ($color -in @(2,3) -and $row.negative_checks -eq 0)) { throw 'NR_QA_COLOR_ORACLE' }
        foreach ($field in @('raw_hash','raw_output_hash','identity_hash','prepared_hash','oracle_prepared_hash','restored_hash','oracle_restored_hash')) {
            if (!$row.$field -or [UInt64]$row.$field -eq 0) { throw 'NR_QA_COLOR_HASH' }
        }
        if ($row.identity_hash -ne $row.raw_output_hash) { throw 'NR_QA_COLOR_IDENTITY' }
        $dxgi=if($format -eq 1){87}elseif($format -eq 2){28}elseif($format -eq 3){10}else{24}
        for ($phase=0; $phase -lt 3; $phase++) {
            $d=$Dispatches[3*$i+$phase]
            $expectedOutput=if($phase -eq 0){10}elseif($format -eq 1){28}else{$dxgi}
            $serial=@($row.prepare_serial,$row.identity_serial,$row.restore_serial)[$phase]
            if ($serial -ne $d.serial -or $d.restore -ne ($phase -ne 0) -or $d.invalid_status -ne 0 -or $d.source_format -ne $dxgi -or $d.output_format -ne $expectedOutput -or $d.raw_readback_hash -ne $row.raw_hash) { throw 'NR_QA_COLOR_PHASE' }
            $hash=@($row.prepared_hash,$row.identity_hash,$row.restored_hash)[$phase]
            if ($d.output_hash -ne $hash) { throw 'NR_QA_COLOR_PHASE' }
        }
    }
    $names=@('nonfinite-input','nonfinite-resolve','canonical-overflow','restore-overflow','malformed-premultiplication')
    $status=@(1,8,4,16,2)
    for ($i=0; $i -lt 5; $i++) {
        $c=$Controls[$i];$d=$Dispatches[45+$i]
        if ($c.rejected -isnot [bool] -or $c.published -isnot [bool]) { throw 'NR_QA_COLOR_INVALID_STATUS' }
        if ($c.name -ne $names[$i] -or $c.stage -ne 'completed_gpu' -or !$c.rejected -or $c.published -or $c.serial -ne $d.serial -or $c.invalid_status -ne $status[$i] -or $d.invalid_status -ne $status[$i]) { throw 'NR_QA_COLOR_INVALID_STATUS' }
    }
    for ($i=5; $i -lt 8; $i++) {
        $c=$Controls[$i];$name=@('unknown-primaries','unavailable-format-capability','unavailable-output-format')[$i-5]
        if ($c.rejected -isnot [bool] -or $c.submitted -isnot [bool]) { throw 'NR_QA_COLOR_ADMISSION_CONTROL' }
        $stage=if($i -eq 7){'record'}else{'plan'}
        if ($c.name -ne $name -or $c.stage -ne $stage -or !$c.rejected -or $c.submitted) { throw 'NR_QA_COLOR_ADMISSION_CONTROL' }
    }
}
# Emission order of nr_flow_scenario.cpp: 1080p, 1440p, then 1440p at the owner 60 fps interval (FAST policy).
function Get-FlowPassSpecs { @(@{instance=81;width=1920;height=1080;interval=0},@{instance=82;width=2560;height=1440;interval=0},@{instance=83;width=2560;height=1440;interval=16666667}) }
function Assert-FlowRows($Rows, $Caps) {
    $specs=Get-FlowPassSpecs
    if ($Rows.Count -ne 48*$specs.Count -or $Caps.Count -ne $specs.Count) { throw 'NR_QA_FLOW_ROWS' }
    for ($c=0; $c -lt $specs.Count; $c++) {
        $cap=$Caps[$c]; $spec=$specs[$c]
        $grids = @($cap.supported_grids)
        # Ordinary passes take the finest grid; the owner 1440p@60 interval pass must submit FAST (20) on a 4-pixel or coarser grid.
        $gridOk = if ($spec.interval) { $cap.grid -ge 4 -and $cap.grid -in $grids -and $cap.submitted_perf_level -eq 20 } else { $cap.grid -eq ($grids | Measure-Object -Minimum).Minimum }
        if (!$grids.Count -or @($grids | Where-Object { $_ -notin @(1,2,4) }).Count -or !$gridOk -or $cap.frame_interval_ns -ne $spec.interval -or !$cap.signed_nvidia_system32 -or $cap.api_max -lt 80 -or $cap.cost_is_probability -or $cap.cost_dxgi_format -notin @(62,42) -or 87 -notin $cap.input_formats -or 38 -notin $cap.output_formats -or $cap.cost_dxgi_format -notin $cap.cost_formats -or $cap.module_signer -ne 'NVIDIA Corporation') { throw 'NR_QA_FLOW_CAPABILITY' }
    }
    for ($k=0; $k -lt $specs.Count; $k++) {
        $spec=$specs[$k]; $width=$spec.width
        $height = $spec.height
        $group = @($Rows[(48*$k)..(48*$k+47)])
        if ($group.Count -ne 48) { throw 'NR_QA_FLOW_EXTENT_SET' }
        for ($i=0; $i -lt $group.Count; $i+=2) {
            $cold=$group[$i]; $flow=$group[$i+1]
            if (!$cold.reset -or $flow.reset -or $cold.sequence -ne ($i+1) -or $flow.sequence -ne ($i+2) -or $cold.history_epoch -ne ($i/2+1) -or $flow.history_epoch -ne $cold.history_epoch) { throw 'NR_QA_FLOW_CHRONOLOGY' }
            $sub = $i -ge 24
            foreach ($row in @($cold,$flow)) {
                $instance=$spec.instance
                if ($row.instance_id -ne $instance -or $row.content_epoch -ne $row.history_epoch -or $row.resource_generation -ne 1 -or $row.submission_serial -ne $row.sequence) { throw 'NR_QA_FLOW_CHRONOLOGY' }
                $x=if($sub){16}else{0}; $y=if($sub){24}else{0}
                $w=if($sub){$width-32}else{$width}; $h=if($sub){$height-48}else{$height}
                if ($row.width -ne $width -or $row.height -ne $height -or $row.active_x -ne $x -or $row.active_y -ne $y -or $row.active_width -ne $w -or $row.active_height -ne $h -or $row.errors -ne 0 -or $row.fixed_vector_bound_pixels -ne 1 -or $row.max_error_pixels -lt 0 -or $row.max_error_pixels -gt 1 -or !$row.motion_hash) { throw 'NR_QA_FLOW_PIXELS' }
            }
            $local = [int](($i % 24)/2)
            $amount = @(1,8,32)[[int][Math]::Floor($local/4)]
            $sign = if (($local % 4) -lt 2) { -1 } else { 1 }
            $dx = if (($local % 2) -eq 0) { $amount*$sign } else { 0 }
            $dy = if (($local % 2) -eq 1) { $amount*$sign } else { 0 }
            if ($cold.dx -ne 0 -or $cold.dy -ne 0 -or $cold.scored_pixels -ne 0 -or $flow.dx -ne $dx -or $flow.dy -ne $dy -or $flow.expected_mv_x -ne (-$dx) -or $flow.expected_mv_y -ne (-$dy) -or $flow.scored_pixels -ne (($flow.active_width-128)*($flow.active_height-128))) { throw 'NR_QA_FLOW_DIRECTION_OR_UNITS' }
        }
    }
}
function Assert-FlowPasses($Passes, $Inputs, $Rows, $Caps) {
    $specs=Get-FlowPassSpecs
    $ordinary=@($Rows | Where-Object { !$_.reset })
    if ($Passes.Count -ne 24*$specs.Count -or $Inputs.Count -ne 24*$specs.Count -or $ordinary.Count -ne 24*$specs.Count) { throw 'NR_QA_FLOW_PASS_SET' }
    for($i=0;$i -lt $Passes.Count;$i++) {
        $p=$Passes[$i]; $input=$Inputs[$i]; $row=$ordinary[$i]
        foreach($field in @('instance_id','history_epoch','content_epoch','resource_generation','submission_serial')) {
            if($p.$field -ne $row.$field -or $input.$field -ne $row.$field) { throw 'NR_QA_FLOW_PASS_KEY' }
        }
        if($p.sequence -ne $row.sequence -or $input.current_sequence -ne $row.sequence -or $input.previous_sequence -ne ($row.sequence-1)) { throw 'NR_QA_FLOW_PASS_KEY' }
        $cap=$Caps[[int][Math]::Floor($i/24)]
        $hintGrids=@($cap.hint_grids)
        $admitted=@($hintGrids | Where-Object { $_ -ge $cap.grid })
        if(($cap.hint_support -join ',') -ne '1' -or !$admitted.Count -or @($hintGrids | Where-Object { $_ -notin @(1,2,4,8) }).Count -or 38 -notin $cap.hint_formats -or 38 -notin $cap.global_formats -or $p.hint_grid -ne ($admitted | Measure-Object -Minimum).Minimum -or $p.output_grid -ne $cap.grid) { throw 'NR_QA_FLOW_HINT_CAPABILITY' }
        if($p.native_passes -ne 2 -or $p.predictor_source -ne 'native_global' -or $p.hint_published_as_final_estimate -or $p.hint_errors -ne 0 -or $p.global_width -ne 1 -or $p.global_height -ne 1 -or !$p.bootstrap_hash -or !$p.hint_hash -or !$row.hardware_hash -or $row.hardware_hash -eq '0') { throw 'NR_QA_FLOW_ESTIMATE_IDENTITY' }
        $ids=@('current_resource','reference_resource','bootstrap_resource','global_resource','hint_resource','local_resource','bootstrap_current_resource','bootstrap_reference_resource') | ForEach-Object { $p.$_ }
        if(@($ids | Select-Object -Unique).Count -ne 8 -or @($ids | Where-Object { !$_ -or [UInt64]$_ -eq 0 }).Count -or $p.registered_handles.Count -ne 10 -or @($p.registered_handles | Select-Object -Unique).Count -ne 10 -or @($p.registered_handles | Where-Object { !$_ -or [UInt64]$_ -eq 0 }).Count) { throw 'NR_QA_FLOW_SESSION_ALIAS' }
        if($input.width -ne $row.width -or $input.height -ne $row.height -or $input.format -ne 87 -or $input.compared_pixels -ne (4*$row.width*$row.height) -or $input.gpu_analysis_errors -ne 0 -or $input.input_hashes.Count -ne 4 -or $input.input_hashes[0] -ne $input.input_hashes[2] -or $input.input_hashes[1] -ne $input.input_hashes[3]) { throw 'NR_QA_FLOW_INPUT_IDENTITY' }
        if($p.input_admitted -le 0 -or $p.predictor_worker -le $p.input_admitted -or $p.conversion_worker -le $p.predictor_worker -or $p.bootstrap_ofa -le 0 -or $p.refinement_ofa -le $p.bootstrap_ofa -or $p.ofa_completed -ne $p.refinement_ofa -or $p.worker_completed -ne $p.conversion_worker -or $p.consumer_completed -ne $p.consumer -or $p.current_producer -lt $p.previous_producer -or $p.previous_producer -le 0 -or $p.consumer -le $p.current_producer -or $p.ofa_completed -eq [UInt64]::MaxValue -or $p.worker_completed -eq [UInt64]::MaxValue -or $p.consumer_completed -eq [UInt64]::MaxValue) { throw 'NR_QA_FLOW_PASS_FENCE' }
        $fences=@($p.producer_fence,$p.ofa_fence,$p.worker_fence,$p.consumer_fence)
        if(@($fences | Where-Object { !$_ -or [UInt64]$_ -eq 0 }).Count -or $p.producer_fence -ne $p.consumer_fence -or @($fences | Select-Object -Unique).Count -ne 3) { throw 'NR_QA_FLOW_PASS_FENCE' }
    }
}
function Stage-Host {
    foreach ($dir in @('bin/64bit','profile','obs-plugins/64bit','data/libobs','data/obs-plugins/obs-nr-filter','data/obs-plugins/obs-x264')) {
        New-Item -ItemType Directory -Force (Join-Path $root $dir) | Out-Null
    }
    $files = @('obs.dll','libobs-d3d11.dll','avcodec-62.dll','avformat-62.dll','avutil-60.dll','swscale-9.dll','swresample-6.dll','zlib.dll','w32-pthreads.dll','libx264-164.dll','librist.dll','srt.dll') | ForEach-Object { 'bin/64bit/' + $_ }
    $files += 'obs-plugins/64bit/obs-x264.dll'
    if($Scenario -in @('native-bootstrap','ghost-calibration')) {
        $files+=@('bin/64bit/avdevice-62.dll','bin/64bit/avfilter-11.dll','bin/64bit/obs-nvenc-test.exe','obs-plugins/64bit/obs-nvenc.dll','obs-plugins/64bit/obs-ffmpeg.dll')
        foreach($module in @('obs-nvenc','obs-ffmpeg')) {
            $files+=@(Get-ChildItem (Join-Path $installed ('data/obs-plugins/'+$module+'/locale')) -File |
                ForEach-Object {'data/obs-plugins/'+$module+'/locale/'+$_.Name})
        }
    }
    if ($Scenario -eq 'sampler') { $files += 'bin/64bit/libobs-opengl.dll' }
    $files += @(Get-ChildItem (Join-Path $installed 'data/libobs') -File | ForEach-Object { 'data/libobs/' + $_.Name })
    $files += @(Get-ChildItem (Join-Path $installed 'data/obs-plugins/obs-x264/locale') -File | ForEach-Object { 'data/obs-plugins/obs-x264/locale/' + $_.Name })
    $pins = @{
        'bin/64bit/obs.dll'='9B9A76F64A1D27262209F1CFBEAF6D57A175CC4E823C71882BB7D5B5AB99BDCF'
        'bin/64bit/libobs-d3d11.dll'='A2A065786F146EEE219031BC9A28845CFF78ABF570296E88DD5AD051ABB8188F'
        'obs-plugins/64bit/obs-x264.dll'='0610DAE11937263E4520FC2562845D50EF4B27A7EF3ABDB6997DFDE0CEA80EFC'
        'obs-plugins/64bit/obs-nvenc.dll'='6187771511C43960016F79C760249C08E43A9835CAA0F58C9E41B98CC92032CC'
        'obs-plugins/64bit/obs-ffmpeg.dll'='A47BE5E81294BF594905696DD3D1372F7E79F2BB9DABA9900DD166FDF62BFA12'
        'bin/64bit/avdevice-62.dll'='824ED5CDCD7512F3EDAB94D28F7FA072353B4E0AFEBB14E4E08A6793686890AC'
        'bin/64bit/avfilter-11.dll'='E9C3951157A47AC2FC692D8B4B346AD1A460D05ECC379FE35F615079A6EDB35E'
        'bin/64bit/obs-nvenc-test.exe'='8AEB65065DA88D9C3919189871EE5508D8252AEBE01CEC8BCD82518D292FD3D4'
    }
    foreach ($file in $files) {
        $source = Join-Path $installed $file
        $dest = Join-Path $root $file
        $before = FileIdentity $source
        if ($pins.ContainsKey($file) -and $before.sha256 -ne $pins[$file]) { throw 'NR_QA_INSTALLED_IDENTITY_CHANGED' }
        if ($file.EndsWith('.dll') -or $file -eq 'bin/64bit/obs-nvenc-test.exe') {
            $signature = Get-AuthenticodeSignature -LiteralPath $source
            if ($signature.Status -ne 'Valid' -or !$signature.SignerCertificate.Subject.Contains('OBS Project, LLC')) { throw 'NR_QA_OFFICIAL_SIGNATURE_REFUSED' }
            $before.signature_status = $signature.Status.ToString()
            $before.signer = $signature.SignerCertificate.Subject
        }
        New-Item -ItemType Directory -Force (Split-Path $dest) | Out-Null
        $sources.Add($before)
        # Minimal private runtime copies: linking Program Files is denied by NTFS.
        Copy-Item -LiteralPath $source -Destination $dest
        $copy = FileIdentity $dest
        if ($copy.sha256 -ne $before.sha256 -or $copy.bytes -ne $before.bytes) { throw 'NR_QA_COPY_IDENTITY_CHANGED' }
    }
    Copy-Item (Join-Path $build 'nr-qa-host.exe') (Join-Path $root 'bin/64bit/nr-qa-host.exe')
    Copy-Item (Join-Path $build 'obs-nr-filter.dll') (Join-Path $root 'obs-plugins/64bit/obs-nr-filter.dll')
    if ($Scenario -eq 'texture-roundtrip') {
        foreach ($shader in @('nr_interop_vertex.cso','nr_interop_pixel.cso')) {
            Copy-Item (Join-Path (Split-Path $build) $shader) (Join-Path $root ('bin/64bit/' + $shader))
        }
    }
    Copy-Item (Join-Path $work 'data/locale') (Join-Path $root 'data/obs-plugins/obs-nr-filter/locale') -Recurse
    if ($Scenario -eq 'flow-translation') {
        Copy-Item (Join-Path (Split-Path $build) 'nr_guides.cso') (Join-Path $root 'bin/64bit/nr_guides.cso')
    }
    if ($Scenario -eq 'color-alpha') {
        $shaderSource=Join-Path (Split-Path $build) 'nr_color.dxil'
        $shaderDest=Join-Path $root 'bin/64bit/nr_color.dxil'
        $before=FileIdentity $shaderSource
        $sources.Add($before)
        Copy-Item $shaderSource $shaderDest
        $staged=FileIdentity $shaderDest
        if ($staged.sha256 -ne $before.sha256 -or $staged.bytes -ne $before.bytes) { throw 'NR_QA_COLOR_SHADER_IDENTITY' }
        $script:binding += $staged
    }
    if ($Scenario -in @('native-model','native-bootstrap','ghost-calibration','lifecycle')) {
        New-Item -ItemType Directory -Force (Join-Path $root 'profile/ngx-data') | Out-Null
        $shaders=@('nr_codec.dxil','nr_color.dxil','nr_guides.cso','nr_interop_vertex.cso','nr_interop_pixel.cso')
        if($Scenario -in @('native-bootstrap','ghost-calibration','lifecycle')){$shaders+='nr_obs_analysis.cso'}
        if($Scenario -eq 'ghost-calibration'){$shaders+='nr_ghost_chunk.dxil'}
        foreach ($shader in $shaders) {
            $source=Join-Path (Split-Path $build) $shader
            $dest=Join-Path $root ('bin/64bit/' + $shader)
            $before=FileIdentity $source
            $sources.Add($before)
            Copy-Item -LiteralPath $source -Destination $dest
            $staged=FileIdentity $dest
            if ($staged.sha256 -ne $before.sha256 -or $staged.bytes -ne $before.bytes) { throw 'NR_QA_MODEL_SHADER_IDENTITY' }
            $script:binding += $staged
        }
    }
}
function Assert-CleanInventory($Inventory, [string]$Graphics = 'libobs-d3d11.dll') {
    foreach ($module in $Inventory) {
        $name = [IO.Path]::GetFileName($module.path)
        if ($name -match '^(nvngx.*|.*feeder.*|.*dlss5.*|.*reshade.*|.*optiscaler.*)$' -or $name -match '\.(asi|addon64)$') { throw 'NR_QA_INJECTION_LOADED' }
        if ($name -in @('winmm.dll','dxgi.dll') -and !$module.path.StartsWith($env:SystemRoot + '\System32\',[StringComparison]::OrdinalIgnoreCase)) { throw 'NR_QA_PROXY_LOADED' }
        if (!$module.path.StartsWith($root + '\',[StringComparison]::OrdinalIgnoreCase) -and !$module.path.StartsWith($env:SystemRoot + '\',[StringComparison]::OrdinalIgnoreCase)) { throw ('NR_QA_EXTERNAL_MODULE: ' + $module.path) }
    }
    foreach ($required in @('obs.dll',$Graphics,'obs-nr-filter.dll','obs-x264.dll')) {
        if (!($Inventory | Where-Object { [IO.Path]::GetFileName($_.path) -eq $required })) { throw ('NR_QA_MODULE_MISSING: ' + $required) }
    }
}
