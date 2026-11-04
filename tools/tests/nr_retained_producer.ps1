param(
    [Parameter(Mandatory)][string]$Ready,
    [Parameter(Mandatory)][string]$Start,
    [Parameter(Mandatory)][string]$Stop,
    [Parameter(Mandatory)][ValidateSet('retained','ordinary')][string]$Mode
)
$ErrorActionPreference='Stop'
$readyEvent=[Threading.EventWaitHandle]::OpenExisting($Ready)
$startEvent=[Threading.EventWaitHandle]::OpenExisting($Start)
$stopEvent=[Threading.EventWaitHandle]::OpenExisting($Stop)
try {
    [void]$readyEvent.Set()
    if(!$startEvent.WaitOne(15000)){throw 'CPU_PRODUCER_START_TIMEOUT'}
    if($Mode -eq 'retained') {
        [Console]::Out.WriteLine('NR_QA_RETAINED {"state":"retained"}')
    } else {
        [Console]::Out.WriteLine('NR_QA_RETAINEDNESS {"state":"ordinary"}')
    }
    [Console]::Out.WriteLine('NR_QA_RETAINED_REPLY {"request":73,"state":"done"}')
    [Console]::Out.Flush()
    if(!$stopEvent.WaitOne(60000)){throw 'CPU_PRODUCER_STOP_TIMEOUT'}
} finally {
    $readyEvent.Dispose();$startEvent.Dispose();$stopEvent.Dispose()
}
