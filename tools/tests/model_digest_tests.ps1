$digestRoot=Join-Path ([IO.Path]::GetTempPath()) ('nr-model-digest-'+[guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($digestRoot) | Out-Null
try {
    $compare=Join-Path $Work 'tools/host/CompareModelDigest.ps1'
    $baseline=Join-Path $digestRoot 'baseline.json'
    $actual=Join-Path $digestRoot 'actual.json'
    $tolerance=Join-Path $digestRoot 'tolerance.json'
    $left=Join-Path $digestRoot 'left.rgba'
    $right=Join-Path $digestRoot 'right.rgba'
    $pixels=[byte[]](0,1,2,3,252,253,254,255)
    [IO.File]::WriteAllBytes($left,$pixels)
    [IO.File]::WriteAllBytes($right,$pixels)
    $hash=(Get-FileHash -LiteralPath $left -Algorithm SHA256).Hash.ToLowerInvariant()
    @{width=2;height=1;format='DXGI_FORMAT_R8G8B8A8_UNORM';sha256=$hash;raw_path='left.rgba'} |
        ConvertTo-Json | Set-Content -LiteralPath $baseline -Encoding UTF8
    @{width=2;height=1;format='DXGI_FORMAT_R8G8B8A8_UNORM';sha256=$hash;raw_path='right.rgba'} |
        ConvertTo-Json | Set-Content -LiteralPath $actual -Encoding UTF8
    @{width=2;height=1;format='DXGI_FORMAT_R8G8B8A8_UNORM';max_absolute_channel_byte=0} |
        ConvertTo-Json | Set-Content -LiteralPath $tolerance -Encoding UTF8
    $args=@($compare,$baseline,$actual,$tolerance) | ForEach-Object {$_.Replace("'","''")}
    $command="& '$($args[0])' -Baseline '$($args[1])' -Actual '$($args[2])' -Tolerance '$($args[3])'"
    $result=Child $command
    if($result.exit){throw ('Equal digest comparison failed '+$result.stderr)}
    $report=$result.stdout | ConvertFrom-Json
    if(!$report.pass -or $report.different_bytes -ne 0 -or $report.max_absolute_channel_byte -ne 0){
        throw 'Equal digest comparison values'
    }
    $pixels[2]++
    [IO.File]::WriteAllBytes($right,$pixels)
    $row=Get-Content -LiteralPath $actual -Raw | ConvertFrom-Json
    $row.sha256=(Get-FileHash -LiteralPath $right -Algorithm SHA256).Hash.ToLowerInvariant()
    $row | ConvertTo-Json | Set-Content -LiteralPath $actual -Encoding UTF8
    $result=Child $command
    $report=$result.stdout | ConvertFrom-Json
    if($result.exit -ne 1 -or $report.pass -or $report.different_bytes -ne 1 -or $report.max_absolute_channel_byte -ne 1){
        throw 'Changed digest was not rejected with exact byte values'
    }
    $row.sha256='0'*64
    $row | ConvertTo-Json | Set-Content -LiteralPath $actual -Encoding UTF8
    if(!(Child $command).exit){throw 'Invalid digest hash accepted'}
    Write-Output 'PASS golden digest equality, mutation and hash refusal CPU_only=true'
} finally {
    Remove-Item -LiteralPath $digestRoot -Recurse -Force
}
