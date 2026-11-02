param([Parameter(Mandatory=$true)][ValidateSet('configure','build','lineage','oracle','manifest')][string]$Step)
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$build=Join-Path $root 'out/task-26/cpu'
$bin='C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin'
$files=@('src/ghost_fixture.h','src/ghost_fixture.cpp','src/ghost_oracle.h','src/ghost_oracle.cpp','src/nr_contract.h',
    'tools/tests/ghost_lineage_tests.cpp','tools/tests/ghost_scheduler_cpu.h','tools/tests/ghost_oracle_tests.cpp',
    'src/frame_pipeline.h','src/frame_pipeline.cpp','src/interop.h','src/interop.cpp','cmake/nr/Ghost.cmake',
    'tools/task26-cpu/CMakeLists.txt','tools/task26-cpu/Run-Cpu.ps1',
    '.omo/evidence/obs-nr-native-filter-20260930/task-26/manifests/cpu-fixtures.json')
function Binding {
    @($files | ForEach-Object {
        $p=Join-Path $root $_
        [ordered]@{path=$_;sha256=(Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash;bytes=(Get-Item -LiteralPath $p).Length}
    })
}
$exe=Join-Path $bin 'cmake.exe'
$arguments=@()
switch($Step) {
    configure {$arguments=@('-S',(Join-Path $root 'tools/task26-cpu'),'-B',$build,'-G','Visual Studio 17 2022','-A','x64')}
    build {$arguments=@('--build',$build,'--config','Release','--parallel','4','--','/nodeReuse:false')}
    lineage {$exe=Join-Path $bin 'ctest.exe';$arguments=@('--test-dir',$build,'-C','Release','-R','^ghost_lineage$','--output-on-failure','-V')}
    oracle {$exe=Join-Path $bin 'ctest.exe';$arguments=@('--test-dir',$build,'-C','Release','-R','^ghost_oracle$','--output-on-failure','-V')}
    manifest {$exe=Join-Path $build 'Release/ghost_lineage_tests.exe';$arguments=@('--manifest')}
}
$before=Binding
$info=New-Object Diagnostics.ProcessStartInfo
$info.FileName=$exe
$info.Arguments=($arguments | ForEach-Object {'"'+$_+'"'}) -join ' '
$info.WorkingDirectory=$root
$info.UseShellExecute=$false
$info.RedirectStandardOutput=$true
$info.RedirectStandardError=$true
$p=New-Object Diagnostics.Process
$p.StartInfo=$info
$p.EnableRaisingEvents=$true
$event='nr26-'+[Guid]::NewGuid().ToString('N')
# The bounded exact exit subscription exists before Start, including very short processes.
Register-ObjectEvent -InputObject $p -EventName Exited -SourceIdentifier $event
try {
    $registered=[DateTime]::UtcNow.ToString('o')
    if(-not $p.Start()) {throw 'CPU process did not start'}
    $identity=[ordered]@{pid=$p.Id;creationUtc=$p.StartTime.ToUniversalTime().ToString('o');image=$exe}
    $out=$p.StandardOutput.ReadToEndAsync()
    $err=$p.StandardError.ReadToEndAsync()
    Write-Output ('NR26_BEGIN '+([ordered]@{step=$Step;registeredUtc=$registered;command=$exe;args=$arguments;identity=$identity;sources=$before}|ConvertTo-Json -Depth 8 -Compress))
    if(-not (Wait-Event -SourceIdentifier $event -Timeout 1200)) {throw 'Exact CPU exit event exceeded 1200s; process was not killed'}
    $stdout=$out.GetAwaiter().GetResult()
    $stderr=$err.GetAwaiter().GetResult()
    $after=Binding
    $same=($before|ConvertTo-Json -Compress) -ceq ($after|ConvertTo-Json -Compress)
    $manifest=$null
    if($Step -eq 'manifest' -and $p.ExitCode -eq 0) {
        $parsed=$stdout|ConvertFrom-Json
        $retained=Get-Content -LiteralPath (Join-Path $root '.omo/evidence/obs-nr-native-filter-20260930/task-26/manifests/cpu-fixtures.json') -Raw|ConvertFrom-Json
        $equivalent=($parsed|ConvertTo-Json -Depth 100 -Compress) -ceq ($retained|ConvertTo-Json -Depth 100 -Compress)
        if(-not $equivalent) {throw 'Retained fixture manifest differs from current source generator'}
        $hash=[Security.Cryptography.SHA256]::Create()
        try {$sha=([BitConverter]::ToString($hash.ComputeHash([Text.Encoding]::UTF8.GetBytes($stdout)))).Replace('-','')}
        finally {$hash.Dispose()}
        $manifest=[ordered]@{sha256=$sha;bytes=[Text.Encoding]::UTF8.GetByteCount($stdout);episodes=$parsed.episodes.Count;
            ticks=@($parsed.episodes|ForEach-Object {$_.ticks.Count});schema=$parsed.schema;seeds=$parsed.seeds;
            retainedManifestEquivalent=$equivalent}
        $stdout='Manifest parsed and source-bound; regenerate with this same command. No physical thresholds.'
    }
    Write-Output ('NR26_RESULT '+([ordered]@{step=$Step;command=$exe;args=$arguments;identity=$identity;exit=$p.ExitCode;
        sourcesUnchanged=$same;sources=$after;stdout=$stdout;stderr=$stderr;manifest=$manifest;CPU_only=$true}|ConvertTo-Json -Depth 8 -Compress))
    if(-not $same) {throw 'CPU sources changed during invocation'}
    exit $p.ExitCode
} finally {
    Unregister-Event -SourceIdentifier $event
    Get-Event -SourceIdentifier $event -ErrorAction SilentlyContinue|Remove-Event
    $p.Dispose()
}
