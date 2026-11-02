# SPDX-License-Identifier: GPL-2.0-or-later
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$NrInstallerWork=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$NrInstallerCarrier=Join-Path $NrInstallerWork '.omo/evidence/obs-nr-native-filter-20260930/task-13/codex-continuation-20261004/core-control-r6b/candidate-artifacts-final-v2.json'
$NrInstallerCarrierHash='cfeeaa22495f04f76f19a7df971d77b0f01047d26e0ded0732b8dbfb39937b4e'
function Get-NrInstallerHash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
function Get-NrInstallerFileId([string]$Path){
    $id=(& "$env:SystemRoot/System32/fsutil.exe" file queryfileid $Path|Out-String).Trim()
    if($LASTEXITCODE){throw 'NR_INSTALL_FILE_ID_FAILED'}
    $id
}
function Assert-NrInstallerNoReparse([string]$Path){
    $p=[IO.Path]::GetFullPath($Path)
    while($p){
        if(Test-Path -LiteralPath $p){if((Get-Item -LiteralPath $p -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'NR_INSTALL_REPARSE_REFUSED'}}
        $parent=[IO.Path]::GetDirectoryName($p.TrimEnd('\','/'));if($parent -eq $p){break};$p=$parent
    }
}
function Resolve-NrInstallerRoot([string]$DestRoot){
    if($DestRoot -notmatch '^[A-Za-z]:[\\/]'){throw 'NR_INSTALL_ABSOLUTE_ROOT_REQUIRED'}
    $root=[IO.Path]::GetFullPath($DestRoot).TrimEnd('\','/')
    $allowed=@((Join-Path $NrInstallerWork '.omo/evidence/obs-nr-native-filter-20260930/task-20'),(Join-Path $NrInstallerWork 'out/task-20'))
    if(!@($allowed|Where-Object {$root.StartsWith(($_.TrimEnd('\','/')+'\'),[StringComparison]::OrdinalIgnoreCase)}).Count){throw 'NR_INSTALL_QA_ROOT_REFUSED'}
    Assert-NrInstallerNoReparse $root
    $marker=Join-Path $root '.nr-installer-sandbox.json'
    if(!(Test-Path -LiteralPath $marker -PathType Leaf)){throw 'NR_INSTALL_OWNER_MARKER_REQUIRED'}
    Assert-NrInstallerNoReparse $marker
    $m=Get-Content -Raw -LiteralPath $marker|ConvertFrom-Json
    if($m.schema -ne 1 -or $m.owner -cne 'obs-nr-native-filter-task20' -or $m.qa_only -isnot [bool] -or !$m.qa_only -or
        [IO.Path]::GetFullPath($m.root).TrimEnd('\','/') -cne $root){throw 'NR_INSTALL_WRONG_ROOT'}
    $root
}
function Assert-NrInstallerStopped([string]$Root,$Processes){
    foreach($p in $Processes){
        if(!$p.ExecutablePath -or !$p.CreationDate){throw 'NR_INSTALL_OBS_IDENTITY_UNKNOWN'}
        if([IO.Path]::GetFullPath($p.ExecutablePath).StartsWith(($Root+'\'),[StringComparison]::OrdinalIgnoreCase)){throw 'NR_INSTALL_OWNED_OBS_RUNNING'}
    }
}
function Get-NrInstallerPayload {
    if((Get-NrInstallerHash $NrInstallerCarrier) -cne $NrInstallerCarrierHash){throw 'NR_INSTALL_CARRIER_CHANGED'}
    $carrier=Get-Content -Raw -LiteralPath $NrInstallerCarrier|ConvertFrom-Json
    if($carrier.artifacts.Count -ne 12){throw 'NR_INSTALL_CARRIER_SCHEMA'}
    $build=Join-Path $NrInstallerWork 'out/task-13-core-control-r6b/native'
    $mapping=,@('Debug/obs-nr-filter.dll','obs-plugins/64bit/obs-nr-filter.dll')
    foreach($name in @('nr_codec.dxil','nr_color.dxil','nr_guides.cso','nr_interop_vertex.cso','nr_interop_pixel.cso','nr_obs_analysis.cso')){$mapping+=,@($name,('data/obs-plugins/obs-nr-filter/shaders/'+$name))}
    foreach($pair in $mapping){
        $source=[IO.Path]::GetFullPath((Join-Path $build $pair[0]))
        $row=@($carrier.artifacts|Where-Object {[IO.Path]::GetFullPath($_.path) -ceq $source})
        if($row.Count -ne 1 -or (Get-NrInstallerHash $source) -cne $row[0].sha256){throw 'NR_INSTALL_SOURCE_CHANGED'}
        @{relative=$pair[1];source=$source;sha256=$row[0].sha256}
    }
    foreach($locale in @('en-US.ini','ko-KR.ini')){
        $source=Join-Path $NrInstallerWork ('data/locale/'+$locale);$pin='64daafeca1278f96e5e3fdb2ed64b04fa11f5a5a833573c9e9b1f59a2cbfdfeb'
        if((Get-NrInstallerHash $source) -cne $pin){throw 'NR_INSTALL_LOCALE_CHANGED'}
        @{relative=('data/obs-plugins/obs-nr-filter/locale/'+$locale);source=$source;sha256=$pin}
    }
}
function Get-NrInstallerRuntime {
    $path='C:/Program Files/obs-studio/bin/64bit/nvngx_dlssnr.dll';$pin='e67dee209320cdafe0e93e45675d7aa34323a53acc57a72b2e40a181581c989a'
    $hash=Get-NrInstallerHash $path;$signature=Get-AuthenticodeSignature -LiteralPath $path
    @{path=$path;sha256=$hash;selected_pin=$pin;pin_matches=($hash -ceq $pin);signature_status=$signature.Status.ToString();loaded=$false;copied=$false;compatibility='UNVERIFIED';policy='selected-owner-pin read-only; signature failure not normalized'}
}
function Open-NrInstallerOwnedFile([string]$Destination){
    [IO.File]::Open($Destination,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
}
function Copy-NrInstallerPayload([string]$Source,[string]$Destination,$OwnedStream){
    if(!$OwnedStream){[IO.File]::Copy($Source,$Destination,$false);return}
    $input=[IO.File]::OpenRead($Source)
    try{$input.CopyTo($OwnedStream);$OwnedStream.Flush($true)}finally{$input.Dispose()}
}
function Save-NrInstallerJournal([string]$Path,$Record,[switch]$Initial){
    # A failed journal write never licenses deletion of unrecorded bytes.
    $target=$(if($Initial){$Path}else{$Path+'.pending'})
    Assert-NrInstallerNoReparse $target
    $s=[IO.File]::Open($target,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
    try{$b=[Text.UTF8Encoding]::new($false).GetBytes(($Record|ConvertTo-Json -Depth 14));$s.Write($b,0,$b.Length);$s.Flush($true)}finally{$s.Dispose()}
    if(!$Initial){[IO.File]::Replace($target,$Path,[System.Management.Automation.Language.NullString]::Value)}
}
function Get-NrInstallerReceipt([string]$Root,$Payload){
    $path=Join-Path $Root '.nr-plugin-install.json';Assert-NrInstallerNoReparse $path
    if(Test-Path -LiteralPath ($path+'.pending')){throw 'NR_INSTALL_JOURNAL_UNCERTAIN'}
    $r=Get-Content -Raw -LiteralPath $path|ConvertFrom-Json
    if($r.schema -ne 2 -or $r.state -cnotin @('QA_BYTES_INSTALLED','QA_INSTALL_PENDING') -or $r.qa_only -isnot [bool] -or !$r.qa_only -or
        $r.root -cne $Root -or $r.source_carrier_sha256 -cne $NrInstallerCarrierHash -or @($r.planned).Count -ne 9 -or @($r.files).Count -gt 9){throw 'NR_INSTALL_RECEIPT_REFUSED'}
    if(@($r.planned.relative|Sort-Object -Unique).Count -ne 9 -or @($r.files|ForEach-Object {$_.relative}|Sort-Object -Unique).Count -ne @($r.files).Count){throw 'NR_INSTALL_RECEIPT_DUPLICATE'}
    foreach($planned in $r.planned){
        $expected=@($Payload|Where-Object {$_.relative -ceq $planned.relative})
        if($expected.Count -ne 1 -or $planned.sha256 -cne $expected[0].sha256){throw 'NR_INSTALL_RECEIPT_PATH_REFUSED'}
        $destination=Join-Path $Root $planned.relative;Assert-NrInstallerNoReparse $destination
    }
    if($r.state -ceq 'QA_BYTES_INSTALLED' -and @($r.files).Count -ne 9){throw 'NR_INSTALL_RECEIPT_REFUSED'}
    foreach($file in $r.files){
        $expected=@($Payload|Where-Object {$_.relative -ceq $file.relative})
        if($expected.Count -ne 1 -or $file.source_sha256 -cne $expected[0].sha256 -or $file.creation -cne 'CreateNew-owned-handle' -or
           $file.sha256 -notmatch '^[a-f0-9]{64}$' -or $file.complete -isnot [bool] -or
           (($r.state -ceq 'QA_BYTES_INSTALLED' -or $file.complete) -and $file.sha256 -cne $expected[0].sha256)){throw 'NR_INSTALL_RECEIPT_PATH_REFUSED'}
        $destination=Join-Path $Root $file.relative;Assert-NrInstallerNoReparse $destination
        if(!(Test-Path -LiteralPath $destination -PathType Leaf) -or (Get-NrInstallerHash $destination) -cne $file.sha256 -or
            (Get-NrInstallerFileId $destination) -cne $file.file_id -or (Get-Item -LiteralPath $destination).Length -ne $file.length){throw ('NR_INSTALL_OWNED_BYTES_CHANGED '+$file.relative)}
    }
    foreach($planned in $r.planned){
        if(!@($r.files|Where-Object {$_.relative -ceq $planned.relative}).Count -and (Test-Path -LiteralPath (Join-Path $Root $planned.relative))){throw 'NR_INSTALL_UNKNOWN_DESTINATION'}
    }
    $r
}
function Invoke-NrInstaller([ValidateSet('Install','Diagnose','Remove')][string]$Action,[string]$DestRoot){
    $root=Resolve-NrInstallerRoot $DestRoot
    Assert-NrInstallerStopped $root @(Get-CimInstance -ClassName Win32_Process -Filter "Name='obs64.exe'")
    $payload=@(Get-NrInstallerPayload);$receipt=Join-Path $root '.nr-plugin-install.json'
    if($Action -eq 'Install'){
        $collisions=@(foreach($f in $payload){$p=Join-Path $root $f.relative;Assert-NrInstallerNoReparse $p;if(Test-Path -LiteralPath $p){@{path=$p;expected_sha256=$f.sha256;actual_sha256=$(if(Test-Path -LiteralPath $p -PathType Leaf){Get-NrInstallerHash $p}else{$null});file_id=(Get-NrInstallerFileId $p);action='REFUSE_PRESERVE_FOREIGN_NO_OVERWRITE'}}})
        if((Test-Path -LiteralPath $receipt) -or (Test-Path -LiteralPath ($receipt+'.pending')) -or $collisions.Count){throw ('NR_INSTALL_COLLISION '+($collisions|ConvertTo-Json -Depth 6 -Compress))}
        $moduleData=Join-Path $root 'data/obs-plugins/obs-nr-filter'
        if(Test-Path -LiteralPath $moduleData){throw 'NR_INSTALL_FOREIGN_MODULE_DATA_DIRECTORY'}
        $runtime=Get-NrInstallerRuntime;if(!$runtime.pin_matches){throw 'NR_INSTALL_SELECTED_RUNTIME_CHANGED'}
        $result=@{schema=2;state='QA_INSTALL_PENDING';qa_only=$true;root=$root;source_carrier_sha256=$NrInstallerCarrierHash;planned=@($payload|ForEach-Object {@{relative=$_.relative;sha256=$_.sha256}});files=@();created_directories=@();runtime=$runtime;usable=$false;dependency='QA_ONLY_NATIVE_SETUP_PENDING_TASK15';classification='Debug candidate bytes; TEST-NOT-RELEASE; Tasks15/17 open; no plugin/runtime load'}
        Save-NrInstallerJournal $receipt $result -Initial
        foreach($f in $payload){
            $p=Join-Path $root $f.relative;$directory=Split-Path $p
            $missing=@();$parent=$directory
            while($parent -ne $root -and !(Test-Path -LiteralPath $parent)){$missing+=$parent;$parent=Split-Path $parent}
            foreach($dir in @($missing|Sort-Object Length)){Assert-NrInstallerNoReparse $dir;[IO.Directory]::CreateDirectory($dir)|Out-Null;$result.created_directories+=$dir}
            Assert-NrInstallerNoReparse $p
            $owned=$null;$row=$null
            try{
                $owned=Open-NrInstallerOwnedFile $p
                if($owned.Length -ne 0){throw 'NR_INSTALL_NEW_FILE_NOT_EMPTY'}
                $row=@{relative=$f.relative;source_sha256=$f.sha256;sha256='e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855';length=0;file_id=(Get-NrInstallerFileId $p);source=$f.source;creation='CreateNew-owned-handle';complete=$false}
                $result.files+=,$row
                Save-NrInstallerJournal $receipt $result
                Copy-NrInstallerPayload $f.source $p $owned
                $owned.Dispose();$owned=$null
                if((Get-NrInstallerFileId $p) -cne $row.file_id){throw 'NR_INSTALL_WRITE_IDENTITY_CHANGED'}
                $row.sha256=Get-NrInstallerHash $p;$row.length=(Get-Item -LiteralPath $p).Length
                if($row.sha256 -cne $f.sha256){throw 'NR_INSTALL_COPY_HASH_MISMATCH'}
                $row.complete=$true;Save-NrInstallerJournal $receipt $result
            }catch{
                $failure=$_.Exception.Message
                if($owned){$owned.Dispose();$owned=$null}
                if($row -and !(Test-Path -LiteralPath ($receipt+'.pending'))){
                    if((Get-NrInstallerFileId $p) -cne $row.file_id){throw ('NR_INSTALL_UNCERTAIN_OWNER '+$failure)}
                    $row.sha256=Get-NrInstallerHash $p;$row.length=(Get-Item -LiteralPath $p).Length
                    $result.failure=$failure;Save-NrInstallerJournal $receipt $result
                }
                throw ('NR_INSTALL_PENDING '+$failure)
            }finally{if($owned){$owned.Dispose()}}
        }
        $result.state='QA_BYTES_INSTALLED';Save-NrInstallerJournal $receipt $result
        return $result
    }
    $record=Get-NrInstallerReceipt $root $payload
    if($Action -eq 'Diagnose'){
        return @{schema=2;state='QA_ONLY_NATIVE_SETUP_PENDING_TASK15';installation_state=$record.state;qa_only=$true;payload_verified=($record.state -ceq 'QA_BYTES_INSTALLED');recovery_verified=$true;usable=$false;root=$root;files=$record.files;runtime=(Get-NrInstallerRuntime);native_load=$false;locales='Unmodified foundation wording; Task15 public integration pending';Task20Accepted=$false;package_release=$false}
    }
    # Every allowed file was checked before any deletion. Never recurse or remove DestRoot.
    foreach($f in $record.files){$p=Join-Path $root $f.relative;Assert-NrInstallerNoReparse $p;Remove-Item -LiteralPath $p}
    Remove-Item -LiteralPath $receipt
    return @{schema=1;state='OWNED_BYTES_REMOVED';qa_only=$true;root=$root;removed=@($record.files.relative);directories_preserved=$true;foreign_bytes_preserved=$true;runtime_removed=$false}
}
