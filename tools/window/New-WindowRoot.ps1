[CmdletBinding()]
param([Parameter(Mandatory)][string]$Params,[Parameter(Mandatory)][string]$Output,[switch]$DryRun)
$ErrorActionPreference='Stop';Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'WindowRoot.ps1')
$p=Get-Content -LiteralPath $Params -Raw -Encoding UTF8|ConvertFrom-Json
Assert-WindowCpuAvailable $p.work
$r=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'R32-REFERENCES.json') -Raw -Encoding UTF8|ConvertFrom-Json
. Import-WindowR32 $r
$rendered=Get-WindowRendered $p $PSScriptRoot -DryRun:$DryRun
$out=[IO.Path]::GetFullPath($Output)
if(!$DryRun -and $out -ine [IO.Path]::GetFullPath($p.instanceRoot)){throw 'WINDOW_WRONG_ROOT'}
Assert-WindowOrdinaryPath $out
if(Test-Path -LiteralPath $out){throw 'WINDOW_FRESH_ROOT_REQUIRED'}
[void][IO.Directory]::CreateDirectory($out)
foreach($name in $rendered.Keys){Write-WindowNew (Join-Path $out $name) $rendered[$name]}
$files=@(foreach($name in $rendered.Keys){Get-WindowPin (Join-Path $out $name)})
Write-WindowNew (Join-Path $out 'GENERATION.json') (ConvertTo-WindowText ([ordered]@{
    schema='nr-window-generation-v1';params=(Get-WindowPin $Params);files=$files;dryRun=[bool]$DryRun;
    instanceEqualsGeneratorParams=$true;nativeExecuted=$false;gpuExecuted=$false;admissionGranted=$false}))
Write-Output ('WINDOW_GENERATED dryRun='+[bool]$DryRun+' files='+$files.Count)
