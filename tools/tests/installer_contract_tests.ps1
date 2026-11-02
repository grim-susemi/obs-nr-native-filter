param([Parameter(Mandatory)][string]$Work,[Parameter(Mandatory)][string]$Evidence,[switch]$RedOnly,[switch]$RecoveryOnly)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$checks=0
function Assert($value,$message){$script:checks++;if(!$value){throw ('INSTALLER_ASSERT '+$message)}}
function FileProof($p){@{path=$p;sha256=(Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash;file_id=((& "$env:SystemRoot/System32/fsutil.exe" file queryfileid $p)|Out-String).Trim()}}
function Sandbox($name){
 $root=Join-Path $Evidence ('sandboxes/'+$name+'-'+[Guid]::NewGuid().ToString('N'))
 New-Item -ItemType Directory -Path $root|Out-Null
 @{schema=1;owner='obs-nr-native-filter-task20';qa_only=$true;root=[IO.Path]::GetFullPath($root)}|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $root '.nr-installer-sandbox.json') -Encoding UTF8
 $root
}
if($RecoveryOnly){
 . (Join-Path $Work 'tools/NrInstallerSupport.ps1')
 $productionCopy=${function:Copy-NrInstallerPayload}
 $cases=@()
 foreach($fault in @('after-three','partial-fourth','foreign-fourth')){
  $recoveryRoot=Sandbox $fault;$sentinel=Join-Path $recoveryRoot 'foreign-owner.txt'
  [IO.File]::WriteAllText($sentinel,'FOREIGN_RECOVERY_KEEP');$before=FileProof $sentinel
  $script:copyCount=0;$script:foreign=$null
  function Open-NrInstallerOwnedFile([string]$Destination){
   if($fault -eq 'foreign-fourth' -and $script:copyCount -eq 3){
    [IO.File]::WriteAllText($Destination,'FOREIGN_ARRIVED_AFTER_PREFLIGHT');$script:foreign=FileProof $Destination
   }
   [IO.File]::Open($Destination,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
  }
  function Copy-NrInstallerPayload([string]$Source,[string]$Destination,$OwnedStream){
   $script:copyCount++
   if($script:copyCount -eq 4){
    if($fault -eq 'partial-fourth'){
     if($OwnedStream){$bytes=[byte[]](1,2,3,4,5);$OwnedStream.Write($bytes,0,$bytes.Length);$OwnedStream.Flush($true)}
     else{$stream=[IO.File]::Open($Destination,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None);try{$bytes=[byte[]](1,2,3,4,5);$stream.Write($bytes,0,$bytes.Length)}finally{$stream.Dispose()}}
    }
    if($fault -eq 'foreign-fourth'){
     [IO.File]::WriteAllText($Destination,'FOREIGN_ARRIVED_AFTER_PREFLIGHT');$script:foreign=FileProof $Destination
     & $productionCopy $Source $Destination
    }
    throw ('CONTROLLED_CPU_IO_FAILURE '+$fault)
   }
   & $productionCopy $Source $Destination $OwnedStream
  }
  $failure=$null;try{Invoke-NrInstaller Install $recoveryRoot|Out-Null}catch{$failure=$_.Exception.Message}
  Assert ($failure -ne $null) 'controlled operation failed'
  $actualFiles=@(Get-ChildItem -LiteralPath $recoveryRoot -File -Recurse|Where-Object {$_.Name -notlike '.nr-*' -and $_.Name -ne 'foreign-owner.txt'}|ForEach-Object {FileProof $_.FullName})
  Write-Output ('ACTUAL_IO_FAULT '+$fault+' copies='+$script:copyCount+' files='+$actualFiles.Count+' failure='+$failure)
  @{fault=$fault;failure=$failure;files=$actualFiles;foreign=$script:foreign}|ConvertTo-Json -Depth 12|Set-Content (Join-Path $Evidence ($fault+'-before-recovery.json'))
  Assert (Test-Path (Join-Path $recoveryRoot '.nr-plugin-install.json')) 'partial failure must retain reachable ownership receipt'
  if($fault -eq 'foreign-fourth'){
   $proof=$actualFiles|ConvertTo-Json -Depth 8 -Compress
   $refusal=$null;try{& (Join-Path $Work 'tools/Remove-Nr.ps1') -DestRoot $recoveryRoot|Out-Null}catch{$refusal=$_.Exception.Message}
   Assert ($refusal -and $refusal.Contains('UNKNOWN_DESTINATION')) 'unowned fourth refuses entire remove'
   foreach($f in $actualFiles){Assert ((FileProof $f.path|ConvertTo-Json -Compress) -ceq ($f|ConvertTo-Json -Compress)) 'all bytes preserved on unknown refusal'}
  }else{
   $diagnosis=& (Join-Path $Work 'tools/Diagnose-Nr.ps1') -DestRoot $recoveryRoot|ConvertFrom-Json
   Assert (!$diagnosis.payload_verified -and $diagnosis.recovery_verified -and !$diagnosis.usable) 'pending diagnostic truthful'
   if($fault -eq 'partial-fourth'){
    $partial=@($diagnosis.files|Where-Object {!$_.complete})
    Assert ($partial.Count -eq 1 -and $partial[0].length -eq 5 -and $partial[0].sha256 -cne $partial[0].source_sha256) 'partial five bytes never claimed full source hash'
    $rp=Join-Path $recoveryRoot '.nr-plugin-install.json';$original=Get-Content -Raw $rp
    $r=$original|ConvertFrom-Json;$r.files[0].file_id='FOREIGN_UNKNOWN_ID';$r|ConvertTo-Json -Depth 14|Set-Content $rp
    $rejected=$null;try{& (Join-Path $Work 'tools/Remove-Nr.ps1') -DestRoot $recoveryRoot|Out-Null}catch{$rejected=$_.Exception.Message}
    Assert ($rejected -and $rejected.Contains('OWNED_BYTES_CHANGED')) 'pending wrong FileID removal refused'
    foreach($f in $actualFiles){Assert ((FileProof $f.path|ConvertTo-Json -Compress) -ceq ($f|ConvertTo-Json -Compress)) 'wrong identity preserves all partial files'}
    [IO.File]::WriteAllText($rp,$original)
   }
   $remove=& (Join-Path $Work 'tools/Remove-Nr.ps1') -DestRoot $recoveryRoot|ConvertFrom-Json
   foreach($f in $actualFiles){Assert (!(Test-Path $f.path)) 'verified partial owned file removed'}
  }
  Assert ((FileProof $sentinel|ConvertTo-Json -Compress) -ceq ($before|ConvertTo-Json -Compress)) 'recovery foreign sentinel same bytes and FileID'
  $cases+=@{fault=$fault;root=$recoveryRoot;failure=$failure;copies=$script:copyCount;before=$actualFiles;sentinelBefore=$before;sentinelAfter=(FileProof $sentinel)}
 }
 $cases|ConvertTo-Json -Depth 14|Set-Content (Join-Path $Evidence 'recovery-cases.json')
 Write-Output ('PASS recovery actual production operations checks='+$checks+' controlledIO=true GPU0 OBS0')
 exit 0
}
$root=Sandbox 'roundtrip'
$sentinel=Join-Path $root 'foreign-owner.txt';'FOREIGN_OWNER_KEEP_BYTES'|Set-Content -LiteralPath $sentinel -Encoding UTF8
$before=FileProof $sentinel
Assert (Test-Path (Join-Path $Work 'tools/Install-Nr.ps1')) 'NEW install feature absent; nine payload files must install at explicit owned sandbox'
$install=& (Join-Path $Work 'tools/Install-Nr.ps1') -DestRoot $root|ConvertFrom-Json
Assert ($install.state -eq 'QA_BYTES_INSTALLED' -and $install.files.Count -eq 9) 'actual nine payload installed'
$diagnose=& (Join-Path $Work 'tools/Diagnose-Nr.ps1') -DestRoot $root|ConvertFrom-Json
Assert ($diagnose.state -eq 'QA_ONLY_NATIVE_SETUP_PENDING_TASK15' -and !$diagnose.usable -and $diagnose.payload_verified) 'truthful diagnose bytes versus usability'
function Refuse($body,$reason){
 $caught=$null;try{& $body|Out-Null}catch{$caught=$_.Exception.Message}
 Assert ($caught -and $caught.Contains($reason)) ('refusal '+$reason+' actual='+$caught)
 $script:refusals+=@{expected=$reason;actual=$caught}
}
$refusals=@()
$receipt=Join-Path $root '.nr-plugin-install.json'
$originalReceipt=Get-Content -Raw -LiteralPath $receipt
function PayloadProof { @($install.files|ForEach-Object {FileProof (Join-Path $root $_.relative)})|ConvertTo-Json -Depth 6 -Compress }
$intact=PayloadProof
foreach($mutation in @('traversal','duplicate','wrongroot','absolute')){
 $r=$originalReceipt|ConvertFrom-Json
 switch($mutation){
  traversal {$r.files[0].relative='../foreign-owner.txt'}
  duplicate {$r.files[0].relative=$r.files[1].relative}
  wrongroot {$r.root=$root+'-wrong'}
  absolute {$r.files[0].relative=$sentinel}
 }
 $r|ConvertTo-Json -Depth 14|Set-Content -LiteralPath $receipt -Encoding UTF8
 Refuse {& (Join-Path $Work 'tools/Remove-Nr.ps1') -DestRoot $root} 'NR_INSTALL_RECEIPT_'
 Assert ((PayloadProof) -ceq $intact) ('all nine untouched after '+$mutation)
}
[IO.File]::WriteAllText($receipt,$originalReceipt)
$last=Join-Path $root $install.files[8].relative
$originalBytes=[IO.File]::ReadAllBytes($last)
[IO.File]::WriteAllText($last,'TAMPERED_OWNED_BYTES')
$tampered=PayloadProof
Refuse {& (Join-Path $Work 'tools/Remove-Nr.ps1') -DestRoot $root} 'NR_INSTALL_OWNED_BYTES_CHANGED'
Assert ((PayloadProof) -ceq $tampered) 'last owned byte tamper refuses before any deletion'
[IO.File]::WriteAllBytes($last,$originalBytes)
Assert ((PayloadProof) -ceq $intact) 'same file IDs after restored test bytes'
$extra=Join-Path $root 'data/obs-plugins/obs-nr-filter/foreign-extra.txt'
[IO.File]::WriteAllText($extra,'FOREIGN_EXTRA_KEEP');$extraBefore=FileProof $extra
$remove=& (Join-Path $Work 'tools/Remove-Nr.ps1') -DestRoot $root|ConvertFrom-Json
Assert ($remove.state -eq 'OWNED_BYTES_REMOVED') 'actual remove receipt'
foreach($r in $install.files){Assert (!(Test-Path (Join-Path $root $r.relative))) ('owned file removed '+$r.relative)}
Assert ((FileProof $sentinel|ConvertTo-Json -Compress) -ceq ($before|ConvertTo-Json -Compress)) 'foreign sentinel hash/FileID unchanged'
Assert ((FileProof $extra|ConvertTo-Json -Compress) -ceq ($extraBefore|ConvertTo-Json -Compress)) 'foreign module-data extra preserved'
$collision=Sandbox 'collision';$collisionFile=Join-Path $collision 'obs-plugins/64bit/obs-nr-filter.dll'
New-Item -ItemType Directory -Path (Split-Path $collisionFile)|Out-Null
[IO.File]::WriteAllText($collisionFile,'FOREIGN_SAME_NAME');$collisionBefore=FileProof $collisionFile
Refuse {& (Join-Path $Work 'tools/Install-Nr.ps1') -DestRoot $collision} 'NR_INSTALL_COLLISION'
Assert ((FileProof $collisionFile|ConvertTo-Json -Compress) -ceq ($collisionBefore|ConvertTo-Json -Compress)) 'foreign collision diff/refusal preserves FileID'
Assert (!(Test-Path (Join-Path $collision 'data'))) 'collision preflight writes no payload'
Refuse {& (Join-Path $Work 'tools/Install-Nr.ps1') -DestRoot 'relative'} 'NR_INSTALL_ABSOLUTE_ROOT_REQUIRED'
Refuse {& (Join-Path $Work 'tools/Install-Nr.ps1') -DestRoot 'C:/Program Files/obs-studio'} 'NR_INSTALL_QA_ROOT_REFUSED'
$wrong=Sandbox 'wrong-marker';$marker=Join-Path $wrong '.nr-installer-sandbox.json'
$m=Get-Content -Raw $marker|ConvertFrom-Json;$m.root=$root;$m|ConvertTo-Json|Set-Content $marker
Refuse {& (Join-Path $Work 'tools/Install-Nr.ps1') -DestRoot $wrong} 'NR_INSTALL_WRONG_ROOT'
# Controlled external inventory at the actual script enumeration seam; no OBS is launched.
$running=Sandbox 'controlled-owned-obs'
function Get-CimInstance {param($ClassName,$Filter) [pscustomobject]@{ExecutablePath=(Join-Path $running 'bin/64bit/obs64.exe');CreationDate=[datetime]::UtcNow}}
try{Refuse {& (Join-Path $Work 'tools/Install-Nr.ps1') -DestRoot $running} 'NR_INSTALL_OWNED_OBS_RUNNING'}finally{Remove-Item Function:Get-CimInstance}
Assert (!(Test-Path (Join-Path $running 'obs-plugins'))) 'controlled running inventory refusal before mutation'
@{refusals=$refusals;controlled_running_obs_not_launched=$true;collisionBefore=$collisionBefore;collisionAfter=(FileProof $collisionFile);all_owned_preflight_before_delete=$true}|ConvertTo-Json -Depth 14|Set-Content (Join-Path $Evidence 'adversarial.json') -Encoding UTF8
@{scenario='actual install-diagnose-remove';root=$root;install=$install;diagnose=$diagnose;remove=$remove;foreignBefore=$before;foreignAfter=(FileProof $sentinel)}|ConvertTo-Json -Depth 14|Set-Content -LiteralPath (Join-Path $Evidence 'install.json') -Encoding UTF8
Write-Output ('PASS installer actual roundtrip checks='+$checks+' QA_ONLY=true nativeLoad0 GPU0 OBS0')
