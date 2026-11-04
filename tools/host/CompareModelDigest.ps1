# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Baseline,
    [Parameter(Mandatory)][string]$Actual,
    [Parameter(Mandatory)][string]$Tolerance
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
Add-Type -TypeDefinition @'
using System;
using System.IO;
public static class NrByteComparison {
    public static long[] Compare(string a, string b) {
        long count=0, max=0;
        using(var left=File.OpenRead(a)) using(var right=File.OpenRead(b)) {
            var x=new byte[65536]; var y=new byte[65536];
            while(true) {
                int nx=left.Read(x,0,x.Length);
                if(nx==0) {
                    if(right.ReadByte()!=-1) throw new InvalidDataException("NR_GOLDEN_LENGTH_MISMATCH");
                    break;
                }
                int ny=0;
                while(ny<nx) {
                    int n=right.Read(y,ny,nx-ny);
                    if(n==0) throw new InvalidDataException("NR_GOLDEN_LENGTH_MISMATCH");
                    ny+=n;
                }
                for(int i=0;i<nx;++i) {
                    int diff=Math.Abs((int)x[i]-y[i]);
                    if(diff!=0) ++count;
                    if(diff>max) max=diff;
                }
            }
        }
        return new long[]{count,max};
    }
}
'@
function Read-Digest([string]$Path) {
    $full=[IO.Path]::GetFullPath($Path)
    $value=Get-Content -LiteralPath $full -Raw -Encoding UTF8 | ConvertFrom-Json
    if($value.width -le 0 -or $value.height -le 0 -or
        $value.format -cne 'DXGI_FORMAT_R8G8B8A8_UNORM' -or
        $value.sha256 -cnotmatch '^[0-9a-f]{64}$' -or
        !$value.raw_path -or [IO.Path]::GetFileName($value.raw_path) -cne $value.raw_path){
        throw 'NR_GOLDEN_DIGEST_SCHEMA'
    }
    $raw=Join-Path (Split-Path -Parent $full) $value.raw_path
    $file=Get-Item -LiteralPath $raw
    if($file.Length -ne ([long]$value.width*[long]$value.height*4)){throw 'NR_GOLDEN_LENGTH'}
    $hash=(Get-FileHash -LiteralPath $raw -Algorithm SHA256).Hash.ToLowerInvariant()
    if($hash -cne $value.sha256){throw 'NR_GOLDEN_DIGEST_MISMATCH'}
    @{metadata=$value;path=$full;raw=$file.FullName;sha256=$hash}
}
$limit=Get-Content -LiteralPath $Tolerance -Raw -Encoding UTF8 | ConvertFrom-Json
if($limit.max_absolute_channel_byte -ne 0){throw 'NR_GOLDEN_TOLERANCE_NOT_ZERO'}
$a=Read-Digest $Baseline;$b=Read-Digest $Actual
if($a.metadata.width -ne $b.metadata.width -or $a.metadata.height -ne $b.metadata.height -or
    $a.metadata.format -cne $b.metadata.format -or $a.metadata.width -ne $limit.width -or
    $a.metadata.height -ne $limit.height -or $a.metadata.format -cne $limit.format){
    throw 'NR_GOLDEN_SHAPE_MISMATCH'
}
$comparison=[NrByteComparison]::Compare($a.raw,$b.raw)
$passed=$comparison[0] -eq 0 -and $comparison[1] -eq 0
@{baseline=$a.path;actual=$b.path;baseline_raw=$a.raw;actual_raw=$b.raw;
  baseline_sha256=$a.sha256;actual_sha256=$b.sha256;tolerance=0;
  different_bytes=$comparison[0];max_absolute_channel_byte=$comparison[1];pass=$passed} |
    ConvertTo-Json -Depth 4
if(!$passed){exit 1}
