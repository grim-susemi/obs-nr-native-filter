# SPDX-License-Identifier: GPL-2.0-or-later
param([Parameter(Mandatory)][string]$Work,[Parameter(Mandatory)][string]$Evidence,[switch]$XeFgOnly)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$checks=0;$results=@()
$e=[IO.Path]::GetFullPath($Evidence).TrimEnd('\','/')
if(-not $XeFgOnly){
 $allowed=[IO.Path]::GetFullPath((Join-Path $Work '.omo/evidence/obs-nr-native-filter-20260930/task-20/cpu-reparse-r1')).TrimEnd('\','/')
 if($e -cne $allowed){throw 'REPARSE_TEST_EXACT_EVIDENCE_REQUIRED'}
}
function Assert($value,$message){$script:checks++;if(!$value){throw ('REPARSE_ASSERT '+$message)}}
function Hash($p){(Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLowerInvariant()}
function Proof($p){
 $id=(& "$env:SystemRoot/System32/fsutil.exe" file queryfileid $p|Out-String).Trim()
 if($LASTEXITCODE){throw 'REPARSE_TEST_FILE_ID_FAILED'}
 @{path=$p;sha256=(Hash $p);file_id=$id}
}
function OwnDirectory($p){
 $full=[IO.Path]::GetFullPath($p)
 if(!$full.StartsWith(($e+'\'),[StringComparison]::OrdinalIgnoreCase)){throw 'REPARSE_TEST_OUTSIDE_OWN_ROOT'}
 New-Item -ItemType Directory -Path $full|Out-Null
 $full
}
function Marker($directory,$reportedRoot){
 @{schema=1;owner='obs-nr-native-filter-task20';qa_only=$true;root=$reportedRoot}|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $directory '.nr-installer-sandbox.json') -Encoding UTF8
}
function Junction($path,$target){
 foreach($p in @($path,$target)){if(![IO.Path]::GetFullPath($p).StartsWith(($e+'\'),[StringComparison]::OrdinalIgnoreCase)){throw 'REPARSE_TEST_JUNCTION_OUTSIDE_OWN_ROOT'}}
 New-Item -ItemType Junction -Path $path -Target $target|Out-Null
 $item=Get-Item -LiteralPath $path -Force
 Assert (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) 'actual NTFS ReparsePoint attribute'
 Assert ($item.LinkType -eq 'Junction') 'actual Junction type'
 Assert ([IO.Path]::GetFullPath([string]$item.Target[0]) -ceq [IO.Path]::GetFullPath($target)) 'actual target equals owned target'
 $query=(& "$env:SystemRoot/System32/fsutil.exe" reparsepoint query $path|Out-String).Trim()
 if($LASTEXITCODE){throw 'REPARSE_TEST_QUERY_FAILED'}
 @{path=$path;target=$target;attributes=$item.Attributes.ToString();link_type=$item.LinkType;actual_target=@($item.Target);native_query=$query;creation_command=@('New-Item','-ItemType','Junction','-Path',$path,'-Target',$target)}
}
function Refused($root){
 $message=$null;try{& (Join-Path $Work 'tools/Install-Nr.ps1') -DestRoot $root|Out-Null}catch{$message=$_.Exception.Message}
 Assert ($message -and $message.Contains('NR_INSTALL_REPARSE_REFUSED')) ('unchanged actual entrypoint refusal '+$message)
 $message
}
function Invoke-XeFgReparse {
 $installer=Join-Path $Work 'packaging/windows/install.ps1'
 $tokens=$null;$errors=$null
 $ast=[Management.Automation.Language.Parser]::ParseFile((Resolve-Path $installer),[ref]$tokens,[ref]$errors)
 if($errors.Count){throw $errors[0]}
 foreach($f in $ast.FindAll({param($n)$n -is [Management.Automation.Language.FunctionDefinitionAst]},$true)){. ([scriptblock]::Create($f.Extent.Text))}
 $script:StateDir=Join-Path $e 'state'
 $script:Payload=Join-Path $e 'payload'
 $script:L='en';$Version='test';$RuntimeRel='bin\64bit\nvngx_dlssnr.dll';$SrRel='bin\64bit\dlss\nvngx_dlss.dll'
 $VfgDirRel='bin\64bit\nvidia-vfg';$VfgDlls=@()
 $Yes=$true;$SkipOptional=$true;$RuntimeDecided=$false;$InstallXeFg=$false;$XeSsRuntimeDir='';$XeSsSdkZip=''
 $DownloadOptional=$false;$DownloadPushFlow=$false;$SkipPushFlow=$true
 $PushFlowRuntimeDir='';$PushFlowModelsDir='';$PushFlowModelZipUrl='';$PushFlowModelZipSha=''
 $StageCleanupDir='';$ObsDir='';$Lang='en';$SrRuntime='';$VfgRuntimeDir='';$Runtime=''
 function Say{}
 function Show-Screen{}
 New-Item -ItemType Directory -Force -Path $e|Out-Null
 $root=OwnDirectory (Join-Path $e ('xefg-'+[Guid]::NewGuid().ToString('N')))
 $target=OwnDirectory (Join-Path $root 'foreign-target')
 $sentinel=Join-Path $target 'sentinel.txt';[IO.File]::WriteAllText($sentinel,'XESS_TARGET_KEEP')
 $before=Proof $sentinel
 $junction=Join-Path $root 'bin\64bit\xess-fg'
 New-Item -ItemType Directory -Force -Path (Split-Path -Parent $junction)|Out-Null
 $link=Junction $junction $target
 $assertRefusal=$null
 try{Assert-XeFgPath $root 'bin\64bit\xess-fg\libxess_fg.dll'}catch{$assertRefusal=$_.Exception.Message}
 Assert ($assertRefusal -and $assertRefusal.Contains('XeSS runtime path must not contain a reparse point')) 'Assert-XeFgPath must refuse the junction'
 $source=OwnDirectory (Join-Path $root 'source')
 foreach($n in 'libxess_fg.dll','libxell.dll','LICENSE.txt'){[IO.File]::WriteAllText((Join-Path $source $n),'fixture')}
 $manifest=[ordered]@{}
 $manifest['obs-plugins\64bit\obs-nr-filter.dll']='unused'
 $installRefusal=$null
 try{Install-Files $root '' '' '' $manifest @{runtime='';models=''} $source}catch{$installRefusal=$_.Exception.Message}
 Assert ($installRefusal -and $installRefusal.Contains('XeSS runtime path must not contain a reparse point')) 'Install-Files must refuse the junction before hashing or copying'
 Assert (!(Test-Path -LiteralPath (Join-Path $root 'obs-plugins\64bit\obs-nr-filter.dll'))) 'junction refusal must write no payload'
 $state=[pscustomobject]@{obsDir=$root;ownedFiles=@([pscustomobject]@{relative='bin\64bit\xess-fg\libxess_fg.dll';hash='unused';backup=''})}
 $removeRefusal=$null
 try{Remove-Files $root $manifest $state}catch{$removeRefusal=$_.Exception.Message}
 Assert ($removeRefusal -and $removeRefusal.Contains('XeSS runtime path must not contain a reparse point')) 'Remove-Files must refuse the junction'
 $after=Proof $sentinel
 Assert ($before.sha256 -ceq $after.sha256 -and $before.file_id -ceq $after.file_id) 'junction target sentinel bytes and FileID preserved'
 @{schema=1;utc=[datetime]::UtcNow.ToString('o');checks=$checks;link=$link;assert_refusal=$assertRefusal;install_refusal=$installRefusal;remove_refusal=$removeRefusal;sentinel_before=$before;sentinel_after=$after;payload_absent=$true;GPU=$false;OBSLaunch=$false}|ConvertTo-Json -Depth 14|Set-Content -LiteralPath (Join-Path $e 'actual-xefg-reparse.json') -Encoding UTF8
 Write-Output ('PASS actual XeSS junction checks='+$checks+' payload0 sentinelSameSHAandFileID GPU0 OBS0')
}
if($XeFgOnly){Invoke-XeFgReparse;exit 0}
$sandbox=OwnDirectory (Join-Path $e ('sandboxes/run-'+[Guid]::NewGuid().ToString('N')))
$target=OwnDirectory (Join-Path $sandbox 'root-target')
$rootLink=Join-Path $sandbox 'root-junction'
Marker $target $rootLink
$sentinel=Join-Path $target 'foreign-root.txt';[IO.File]::WriteAllText($sentinel,'FOREIGN_ROOT_JUNCTION_KEEP')
$before=Proof $sentinel;$link=Junction $rootLink $target;$refusal=Refused $rootLink
Assert (!(Test-Path -LiteralPath (Join-Path $target '.nr-plugin-install.json'))) 'root junction no journal'
Assert (!(Test-Path -LiteralPath (Join-Path $target 'obs-plugins'))) 'root junction no payload'
$after=Proof $sentinel
Assert ($before.sha256 -ceq $after.sha256 -and $before.file_id -ceq $after.file_id) 'root target sentinel bytes and FileID preserved'
$results+=@{scenario='DestRoot actual junction';root=$rootLink;link=$link;refusal=$refusal;sentinel_before=$before;sentinel_after=$after;journal_absent=$true;payload_absent=$true}
$root=OwnDirectory (Join-Path $sandbox 'ordinary-root');Marker $root $root
$foreign=OwnDirectory (Join-Path $sandbox 'foreign-data-target')
$sentinel=Join-Path $foreign 'foreign-data.txt';[IO.File]::WriteAllText($sentinel,'FOREIGN_DATA_JUNCTION_KEEP')
$before=Proof $sentinel;$link=Junction (Join-Path $root 'data') $foreign;$refusal=Refused $root
Assert (!(Test-Path -LiteralPath (Join-Path $root '.nr-plugin-install.json'))) 'data ancestor no journal'
Assert (!(Test-Path -LiteralPath (Join-Path $root 'obs-plugins'))) 'data ancestor no DLL payload'
Assert (!(Test-Path -LiteralPath (Join-Path $foreign 'obs-plugins'))) 'foreign data target no module data'
$after=Proof $sentinel
Assert ($before.sha256 -ceq $after.sha256 -and $before.file_id -ceq $after.file_id) 'data target sentinel bytes and FileID preserved'
$results+=@{scenario='ordinary root actual data ancestor junction';root=$root;link=$link;refusal=$refusal;sentinel_before=$before;sentinel_after=$after;journal_absent=$true;payload_absent=$true}
@{schema=1;utc=[datetime]::UtcNow.ToString('o');actual_checks=$checks;cases=$results;cleanup='No deletion or recursive traversal; owned junctions and targets preserved';GPU=$false;OBSLaunch=$false;nativeLoad=$false;processControl=$false;Task20Accepted=$false}|ConvertTo-Json -Depth 14|Set-Content -LiteralPath (Join-Path $e 'actual-reparse.json') -Encoding UTF8
Write-Output ('PASS actual NTFS junction checks='+$checks+' cases2 journal0 payload0 sentinelSameSHAandFileID GPU0 OBS0 nativeLoad0 Task20Acceptedfalse')
