param([Parameter(Mandatory)][ValidateSet('baseline-configure','baseline-build','baseline','configure','build','tests')][string]$Step)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$build=Join-Path $root 'out/task-13-cpu'
$bin='C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin'
$files=@('src/frame_pipeline.h','src/frame_pipeline.cpp','src/nr_contract.h','src/interop.h',
    'src/nr_lifecycle.h','src/nr_lifecycle.cpp','tools/tests/lifecycle_contract_tests.cpp',
    'cmake/nr/Lifecycle.cmake','tools/task13-cpu/CMakeLists.txt','tools/task13-cpu/Run-Cpu.ps1')
function Binding {
    @($files | ForEach-Object {
        $path=Join-Path $root $_
        if(Test-Path -LiteralPath $path) {
            [ordered]@{path=$_;sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash;bytes=(Get-Item -LiteralPath $path).Length}
        } else {[ordered]@{path=$_;absent=$true}}
    })
}
$exe=Join-Path $bin 'cmake.exe'
$arguments=@()
switch($Step) {
    baseline-configure {$arguments=@('-S',(Join-Path $root 'tools/task13-cpu'),'-B',(Join-Path $build 'baseline'),'-G','Visual Studio 17 2022','-A','x64')}
    baseline-build {$arguments=@('--build',(Join-Path $build 'baseline'),'--config','Release','--parallel','4','--','/nodeReuse:false')}
    baseline {$exe=Join-Path $build 'baseline/Release/lifecycle_contract_tests.exe';$arguments=@('--without-observer')}
    configure {$arguments=@('-S',(Join-Path $root 'tools/task13-cpu'),'-B',(Join-Path $build 'current'),'-G','Visual Studio 17 2022','-A','x64')}
    build {$arguments=@('--build',(Join-Path $build 'current'),'--config','Release','--parallel','4','--','/nodeReuse:false')}
    tests {$exe=Join-Path $bin 'ctest.exe';$arguments=@('--test-dir',(Join-Path $build 'current'),'-C','Release','-R','^lifecycle_contract$','--output-on-failure','-V')}
}
$before=Binding
$p=[Diagnostics.Process]::new()
$p.StartInfo.FileName=$exe
$p.StartInfo.Arguments=($arguments | ForEach-Object {'"'+$_+'"'}) -join ' '
$p.StartInfo.WorkingDirectory=$root
$p.StartInfo.UseShellExecute=$false
$p.StartInfo.RedirectStandardOutput=$true
$p.StartInfo.RedirectStandardError=$true
$p.StartInfo.EnvironmentVariables['MSBUILDDISABLENODEREUSE']='1'
$p.EnableRaisingEvents=$true
$event='nr13-'+[Guid]::NewGuid().ToString('N')
Register-ObjectEvent -InputObject $p -EventName Exited -SourceIdentifier $event | Out-Null
$registered=[DateTime]::UtcNow.ToString('o')
$record=[ordered]@{step=$Step;registeredUtc=$registered;subscriptionBeforeStart=$true;event=$event;
    command=$exe;arguments=$arguments;workingDirectory=$root;sources=$before;CPU_only=$true}
Write-Output ('NR13_REGISTERED '+($record | ConvertTo-Json -Depth 8 -Compress))
try {
    if(-not $p.Start()) {throw 'CPU child did not start'}
    $owner=[ordered]@{pid=$p.Id;creationUtc=$p.StartTime.ToUniversalTime().ToString('o');image=$p.MainModule.FileName}
    $out=$p.StandardOutput.ReadToEndAsync()
    $err=$p.StandardError.ReadToEndAsync()
    Write-Output ('NR13_START '+([ordered]@{step=$Step;identity=$owner;startedUtc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json -Compress))
    if(-not (Wait-Event -SourceIdentifier $event -Timeout 1200)) {throw 'Exact CPU exit event exceeded 1200s; ownership retained, not killed'}
    $stdout=$out.GetAwaiter().GetResult()
    $stderr=$err.GetAwaiter().GetResult()
    $after=Binding
    $same=($before | ConvertTo-Json -Compress) -ceq ($after | ConvertTo-Json -Compress)
    $code=$p.ExitCode
    Write-Output ('NR13_RESULT '+([ordered]@{step=$Step;identity=$owner;exit=$code;exitedUtc=[DateTime]::UtcNow.ToString('o');
        sourcesUnchanged=$same;sources=$after;stdout=$stdout;stderr=$stderr;CPU_only=$true} | ConvertTo-Json -Depth 8 -Compress))
    if(-not $same) {throw 'CPU source drift during invocation'}
} finally {
    Unregister-Event -SourceIdentifier $event
    Get-Event -SourceIdentifier $event -ErrorAction SilentlyContinue | Remove-Event
    $p.Dispose()
    Write-Output ('NR13_CLEANUP '+([ordered]@{step=$Step;event=$event;subscriptionRemoved=$true;processObjectDisposed=$true} | ConvertTo-Json -Compress))
}
exit $code
