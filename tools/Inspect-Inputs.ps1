[CmdletBinding()]
param(
 [Parameter(Mandatory=$true)][string]$Report,
 [string]$Root="C:/Users/USER/orca/projects/obs-nr-native-filter-wt/w1",
 [string]$Runtime="C:/Program Files/obs-studio/bin/64bit/nvngx_dlssnr.dll",
 [string]$InputSpec="provenance/inputs.spec.json"
)
$ErrorActionPreference="Stop"
$owned="C:/Users/USER/orca/projects/obs-nr-native-filter-wt/w1"
$taskEvidence="$owned/.omo/evidence/obs-nr-native-filter-20260930/task-1"
$evidence="$owned/.omo/evidence/obs-nr-native-filter-20260930/task-1/inspector"
$reasons=New-Object System.Collections.Generic.List[object]
$records=New-Object System.Collections.Generic.List[object]
function Block([string]$code,[string]$detail) { $reasons.Add([ordered]@{code=$code;detail=$detail}) }
function Full([string]$path) { [IO.Path]::GetFullPath($path).TrimEnd([char[]]"\/") }
function Under([string]$path,[string]$base) { (Full $path).StartsWith((Full $base)+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) }
function LocalPath([string]$path) {
 if ([IO.Path]::IsPathRooted($path)) { throw "Expected relative owned path: $path" }
 $full=Full (Join-Path $owned $path)
 if (!(Under $full $owned)) { throw "Path escapes owned root: $path" }
 return $full
}
function ReadJson([string]$path) { Get-Content -LiteralPath $path -Raw | ConvertFrom-Json }
function Identity([string]$path) {
 if (!(Test-Path -LiteralPath $path -PathType Leaf)) { return [ordered]@{path=$path;present=$false} }
 $item=Get-Item -LiteralPath $path
 return [ordered]@{path=$path;present=$true;bytes=$item.Length;sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()}
}
function CheckFile([string]$path,$entry,[string]$category) {
 $id=Identity $path
 $records.Add([ordered]@{category=$category;identity=$id;expected=$entry})
 if (!$id.present) { Block "missing_file" $path; return }
 if ($entry.sha256 -notmatch "^[a-fA-F0-9]{64}$" -or $null -eq $entry.bytes -or $id.sha256 -ne $entry.sha256 -or $id.bytes -ne $entry.bytes) { Block "hash_drift" $path }
}
# Static ranged reads only: no DLL loading or full-file byte buffer.
function InspectPe([string]$path) {
 $stream=[IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
 try {
  $reader=New-Object IO.BinaryReader($stream)
  function Bytes([long]$offset,[int]$count) {
   if ($offset -lt 0 -or $count -lt 0 -or $offset -gt $stream.Length -or $count -gt $stream.Length-$offset) { throw "PE file range invalid: $offset + $count" }
   $null=$stream.Seek($offset,[IO.SeekOrigin]::Begin); $data=$reader.ReadBytes($count)
   if ($data.Length -ne $count) { throw "PE truncated during read" }
   return ,$data
  }
  function U16([long]$offset) { [BitConverter]::ToUInt16((Bytes $offset 2),0) }
  function U32([long]$offset) { [BitConverter]::ToUInt32((Bytes $offset 4),0) }
  if ((U16 0) -ne 0x5a4d) { throw "Missing MZ signature" }
  $nt=[long](U32 60)
  if ((U32 $nt) -ne 0x4550 -or (U16 ($nt+4)) -ne 0x8664) { throw "Not AMD64 PE" }
  $count=U16 ($nt+6); $optional=U16 ($nt+20); $opt=$nt+24
  if ($count -lt 1 -or $count -gt 96 -or $optional -lt 120 -or (U16 $opt) -ne 0x20b) { throw "Invalid PE64 headers" }
  $null=Bytes $opt $optional
  if ((U32 ($opt+108)) -lt 1) { throw "Missing export directory" }
  $headers=U32 ($opt+60); $exportRva=U32 ($opt+112); $exportSize=U32 ($opt+116)
  $sections=@()
  for ($i=0;$i -lt $count;$i++) {
   $s=$opt+$optional+$i*40; $null=Bytes $s 40
   $sections+=@{rva=[long](U32 ($s+12));raw=[long](U32 ($s+20));size=[long](U32 ($s+16));virtual=[long](U32 ($s+8))}
  }
  function Rva([long]$rva,[long]$size) {
   if ($rva -lt 0 -or $size -lt 0 -or $rva+$size -gt 0x100000000L) { throw "PE RVA overflow" }
   $matches=@($sections | Where-Object { $rva -ge $_.rva -and $rva -lt $_.rva+[Math]::Max($_.virtual,$_.size) })
   if ($rva -lt $headers -and $rva+$size -le $headers -and $matches.Count -eq 0) { $offset=$rva }
   elseif ($matches.Count -eq 1 -and $rva-$matches[0].rva+$size -le $matches[0].size) { $offset=$matches[0].raw+$rva-$matches[0].rva }
   else { throw "Unbacked or ambiguous PE RVA: $rva" }
   if ($offset -gt $stream.Length -or $size -gt $stream.Length-$offset) { throw "PE RVA outside file" }
   return $offset
  }
  function CString([long]$rva) {
   $text=New-Object Text.StringBuilder
   for ($n=0;$n -lt 4096;$n++) {
    $b=(Bytes (Rva ($rva+$n) 1) 1)[0]
    if ($b -eq 0) { return $text.ToString() }
    if ($b -lt 32 -or $b -gt 126) { throw "Non-ASCII export name" }
    $null=$text.Append([char]$b)
   }
   throw "Unterminated PE export string"
  }
  if ($exportRva -eq 0 -or $exportSize -lt 40) { throw "Missing PE exports" }
  $directory=Rva $exportRva $exportSize
  $functions=[long](U32 ($directory+20)); $names=[long](U32 ($directory+24))
  if ($functions -gt 65536 -or $names -gt 65536 -or $names -gt $functions) { throw "Invalid export counts" }
  $functionTable=Rva (U32 ($directory+28)) ($functions*4)
  $nameTable=Rva (U32 ($directory+32)) ($names*4)
  $ordinalTable=Rva (U32 ($directory+36)) ($names*2)
  $exports=@(); $forwarded=@()
  for ($i=0;$i -lt $names;$i++) {
   $name=CString (U32 ($nameTable+$i*4)); $ordinal=U16 ($ordinalTable+$i*2)
   if ($ordinal -ge $functions) { throw "Export ordinal out of bounds" }
   $target=[long](U32 ($functionTable+$ordinal*4))
   if ($target -eq 0) { throw "Named export has null target" }
   if ($target -ge $exportRva -and $target -lt $exportRva+$exportSize) { $null=CString $target; $forwarded+=$name }
   else { $null=Rva $target 1 }
   $exports+=$name
  }
  return [ordered]@{machine="AMD64";pe="PE32+";exports=$exports;forwarded_exports=$forwarded;loaded=$false;evaluated=$false}
 } finally { $stream.Dispose() }
}
# Refuse unsafe output before creating anything. Reparse points are not owned writes.
try {
 if ($Report.Replace([char]92,[char]47).Split([char]47) -contains "..") { throw "Report traversal is not admitted" }
 $Report=Full $Report
 if (!(Under $Report $taskEvidence) -or [IO.Path]::GetExtension($Report) -ne ".json") { throw "Report must be owned task1 JSON: $taskEvidence" }
 $parent=[IO.Path]::GetDirectoryName($Report)
 for ($p=$Report;$p -and ((Under $p $owned) -or (Full $p) -eq (Full $owned));$p=[IO.Path]::GetDirectoryName($p)) {
  if ((Test-Path -LiteralPath $p) -and ((Get-Item -LiteralPath $p -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Report path has reparse point: $p" }
 }
 if ((Test-Path -LiteralPath $Report) -and (Full $Report) -eq (Full $Runtime)) { throw "Report collides with runtime" }
} catch { Write-Error -ErrorAction Continue $_; exit 2 }
$before=@(); $after=@(); $selectedAfter=@(); $runtimeRecord=$null; $gitRecord=$null; $spec=$null
$protected=@("$owned/provenance/source-lock.json","$owned/provenance/sdk-lock.json","$owned/provenance/inputs.spec.json","C:/Program Files/obs-studio/bin/64bit/nvngx_dlssnr.dll","C:/Program Files/obs-studio/bin/64bit/winmm.dll","C:/Program Files/obs-studio/bin/64bit/dxgi.dll","C:/Program Files/obs-studio/bin/64bit/dlss5-feed.addon64","C:/Program Files/obs-studio/bin/64bit/OptiScaler.ini","C:/Program Files/obs-studio/bin/64bit/ReShade.ini")
try {
 if ((Full $Root) -ne (Full $owned)) { Block "wrong_root" $Root; throw "Only explicit owned W1 is admitted" }
 $specPath=LocalPath $InputSpec
 if ($specPath -ne (Full "$owned/provenance/inputs.spec.json") -and !(Under $specPath $evidence)) { throw "Alternate spec must be owned Inspector fixture" }
 $protected+=@($specPath,$Runtime)
 $before=@($protected | Select-Object -Unique | ForEach-Object { Identity $_ })
 $spec=ReadJson $specPath
 if ($spec.schema_version -ne 1 -or $spec.kind -ne "nr-input-spec" -or (Full $spec.owned_root) -ne (Full $owned)) { throw "Invalid input spec" }
 if ($spec.template_commit -ne "3e7d7ac3b5342cd7d9b88890b9c70b472d1520fc" -or $spec.obs.version -ne "32.2.2" -or $spec.obs.commit -ne "ba2f32bdf791005443988a4955e963663e16b1ed" -or $spec.nvofa_sdk_version -ne "5.0") { Block "wrong_pin" "Spec pin mismatch" }
 try {
  $top=& git -C $owned rev-parse --show-toplevel 2>&1
  if ($LASTEXITCODE -ne 0 -or (Full "$top") -ne (Full $owned)) { throw "Not owned Git worktree" }
  $origin=& git -C $owned remote get-url origin
  if ($LASTEXITCODE -ne 0 -or $origin -ne "https://github.com/obsproject/obs-plugintemplate.git") { throw "Wrong official template origin" }
  $common=& git -C $owned rev-parse --path-format=absolute --git-common-dir
  if ($LASTEXITCODE -ne 0 -or (Full "$common") -ne (Full "C:/Users/USER/orca/projects/obs-nr-native-filter/.git") -or (Get-Item -LiteralPath "$owned/.git" -Force).PSIsContainer) { throw "Not the bootstrap shared-history worktree" }
  & git -C $owned merge-base --is-ancestor "3e7d7ac3b5342cd7d9b88890b9c70b472d1520fc" HEAD
  if ($LASTEXITCODE -ne 0) { throw "Template history pin absent" }
  $branch=& git -C $owned branch --show-current
  if ($LASTEXITCODE -ne 0 -or $branch -ne "native-nr-foundation") { throw "Wrong foundation branch" }
  $head=& git -C $owned rev-parse HEAD
  $dirty=@(& git -C $owned status --porcelain)
  $gitRecord=[ordered]@{root=$top;branch=$branch;head=$head;template_commit=$spec.template_commit;dirty=$dirty;dirty_policy="Recorded without stash/reset or rejection of unrelated work"}
 } catch { Block "repository" $_.Exception.Message }
 try {
  $sourcePath=LocalPath $spec.source_lock
  $source=ReadJson $sourcePath
  $records.Add([ordered]@{category="source_lock";identity=(Identity $sourcePath);notices=$source.notices;limits=$source.limits})
  if ($source.schema_version -ne 1 -or $source.kind -ne "nr-source-lock" -or @($source.files).Count -eq 0 -or !$source.notices -or !$source.limits) { throw "Invalid source lock schema or license boundary" }
  $roots=@{S="C:/omo-research/obs-mfg-v020-candidate/source";D="C:/omo-research/obs-mfg-v020-candidate/feeder";Q="C:/omo-research/obs-mfg-v020-candidate/release-repo"}
  foreach ($key in $roots.Keys) {
   if ((Full $source.roots.$key) -ne (Full $roots[$key]) -or (Full $spec.roots.$key) -ne (Full $roots[$key])) { throw "Wrong snapshot root: $key" }
  }
  foreach ($file in $source.files) {
   if (!$roots.ContainsKey([string]$file.root_id) -or [IO.Path]::IsPathRooted($file.relative_path) -or @($file.license_ids).Count -eq 0) { throw "Invalid source file/license entry" }
   $path=Full (Join-Path $roots[$file.root_id] $file.relative_path)
   if (!(Under $path $roots[$file.root_id])) { throw "Source path escape" }
   CheckFile $path $file "source"
  }
  foreach ($file in $source.attribution) {
   if (!$roots.ContainsKey([string]$file.root_id) -or [IO.Path]::IsPathRooted($file.relative_path)) { throw "Invalid attribution path" }
   $path=Full (Join-Path $roots[$file.root_id] $file.relative_path)
   if (!(Under $path $roots[$file.root_id])) { throw "Attribution path escape" }
   CheckFile $path $file "license_source"
  }
  foreach ($notice in $source.notices.PSObject.Properties) {
   if ($notice.Value.notice_file) {
    $path=LocalPath $notice.Value.notice_file; $id=Identity $path
    $records.Add([ordered]@{category="license_notice";identity=$id})
    if (!$id.present) { Block "missing_license" $path }
   }
  }
 } catch { Block "source_lock" $_.Exception.Message }
 try {
  $sdkPath=LocalPath $spec.sdk_lock; $sdk=ReadJson $sdkPath
  $records.Add([ordered]@{category="sdk_lock";identity=(Identity $sdkPath);notices=$sdk.notices;limits=$sdk.limits;obs=$sdk.obs; nvofa_source=$sdk.nvofa.source;ngx_source=$sdk.ngx.source})
  if ($sdk.schema_version -ne 1 -or $sdk.kind -ne "nr-sdk-lock" -or !$sdk.notices -or !$sdk.limits) { throw "Invalid SDK lock schema/license boundary" }
  if ($sdk.obs.version -ne "32.2.2" -or $sdk.obs.commit -ne "ba2f32bdf791005443988a4955e963663e16b1ed") { Block "wrong_pin" "OBS SDK pin mismatch" }
  if ($sdk.nvofa.sdk_version -ne "5.0") { Block "sdk_package_version" ("NVOFA package version mismatch: "+$sdk.nvofa.sdk_version) }
  if ($sdk.nvofa.status -eq "MISSING" -or $sdk.nvofa.source.url -notmatch "^https://(developer\.nvidia\.com|github\.com/NVIDIA)/" -or (!$sdk.nvofa.source.revision -and $sdk.nvofa.source.archive_sha256 -notmatch "^[a-fA-F0-9]{64}$")) { Block "wrong_sdk" "NVOFA SDK 5.0 official package provenance required" }
  if (!$sdk.ngx.source) { throw "NGX local-drop provenance missing" }
  foreach ($family in @("obs","nvofa","ngx")) {
   if (@($sdk.$family.files).Count -eq 0) { Block "missing_sdk" "Empty SDK family: $family"; continue }
   foreach ($file in $sdk.$family.files) { CheckFile (LocalPath $file.path) $file "sdk_$family" }
  }
  $ofaNotice=@($sdk.notices | Where-Object { $_.family -eq "nvofa" })
  if ($ofaNotice.Count -ne 1 -or !$ofaNotice[0].file -or $ofaNotice[0].license -match "MISSING|UNKNOWN") { Block "missing_license" "NVOFA official package license absent" }
  else {
   $licensePath=LocalPath $ofaNotice[0].file; $id=Identity $licensePath
   $records.Add([ordered]@{category="sdk_license";identity=$id})
   if (!$id.present -or @($sdk.nvofa.files | Where-Object { (LocalPath $_.path) -eq $licensePath }).Count -ne 1) { Block "missing_license" "NVOFA license must be hash-bound in SDK files" }
  }
  $dx12=@($sdk.nvofa.files | Where-Object { [IO.Path]::GetFileName($_.path) -eq "nvOpticalFlowD3D12.h" })
  if ($dx12.Count -ne 1) { Block "nvofa_d3d12" "DirectX12 header absent (Common API2-only tree is not admitted)" }
  else {
   $header=Get-Content -LiteralPath (LocalPath $dx12[0].path) -Raw
   if ($header -notmatch "NvOFAPICreateInstanceD3D12" -or $header -notmatch "NV_OF_D3D12_API_FUNCTION_LIST") { Block "nvofa_d3d12" "DirectX12 API declarations absent" }
  }
 } catch { Block "sdk_lock" $_.Exception.Message }
 try {
  $id=Identity $Runtime
  if (!$id.present) { throw "Runtime missing: $Runtime" }
  $version=[Diagnostics.FileVersionInfo]::GetVersionInfo((Full $Runtime))
  $actualVersion="{0}.{1}.{2}.{3}" -f $version.FileMajorPart,$version.FileMinorPart,$version.FileBuildPart,$version.FilePrivatePart
  $ownerIdentityMatches=$spec.runtime.owner_selected -eq $true -and (Full $Runtime) -eq (Full $spec.runtime.path) -and (Full $spec.runtime.path) -eq (Full "C:/Program Files/obs-studio/bin/64bit/nvngx_dlssnr.dll") -and $id.sha256 -eq "e67dee209320cdafe0e93e45675d7aa34323a53acc57a72b2e40a181581c989a" -and $spec.runtime.sha256 -eq $id.sha256 -and $actualVersion -eq "310.8.0.0" -and $spec.runtime.version -eq $actualVersion
  $runtimeRecord=[ordered]@{identity=$id;version=$actualVersion;static_pe=$null;owner_selected=$ownerIdentityMatches;loaded=$false;evaluated=$false}
  if (!$ownerIdentityMatches) { Block "runtime_identity" "Declared owner runtime path/hash/version mismatch" }
  $pe=InspectPe $Runtime; $runtimeRecord.static_pe=$pe
  $exportSource="C:/omo-research/obs-mfg-v020-candidate/source/OptiScaler/dlssnr/DlssNr_CompatibilityRuntime.cpp"
  $text=Get-Content -LiteralPath $exportSource -Raw
  $pattern="symbol\("+[char]34+"([^"+[char]34+"]+)"+[char]34+"\)"
  $required=@([regex]::Matches($text,$pattern) | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
  $records.Add([ordered]@{category="runtime_export_contract";identity=(Identity $exportSource);required_exports=$required})
  if ($required.Count -ne 5 -or @(Compare-Object $required @($spec.runtime.required_exports)).Count -ne 0) { Block "runtime_export_contract" "Spec disagrees with actual compatibility source" }
  foreach ($name in $required) {
   if ($pe.exports -notcontains $name -or $pe.forwarded_exports -contains $name) { Block "runtime_export" "Required direct export absent: $name" }
  }
 } catch { Block "runtime_pe" $_.Exception.Message }
 try {
  $vswhere="C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
  $vs=& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if ($LASTEXITCODE -ne 0 -or !$vs) { throw "VS C++ tools not found" }
  $records.Add([ordered]@{category="toolchain";identity=(Identity $vswhere);installation=$vs})
  $tools=@("Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe","Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe","Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe")
  foreach ($relative in $tools) {
   $path=Join-Path $vs $relative; $id=Identity $path
   if (!$id.present) { throw "Missing bundled tool: $path" }
   $version=@(& $path --version 2>&1)
   if ($LASTEXITCODE -ne 0) { throw "Version discovery failed: $path" }
   $records.Add([ordered]@{category="toolchain";identity=$id;version=$version;discovery="Local VS path; no PATH modification"})
  }
  $msvc=Get-ChildItem -LiteralPath "$vs/VC/Tools/MSVC" -Directory | Sort-Object Name -Descending | Select-Object -First 1
  foreach ($name in @("cl.exe","link.exe")) {
   $path=Join-Path $msvc.FullName "bin/Hostx64/x64/$name"; $id=Identity $path
   if (!$id.present) { throw "Missing MSVC: $path" }
   $records.Add([ordered]@{category="toolchain";identity=$id;version=(Get-Item -LiteralPath $path).VersionInfo.FileVersion})
  }
  $actualTools=@($records | Where-Object { $_.category -eq "toolchain" })
  if (@($spec.toolchain).Count -ne $actualTools.Count) { throw "Toolchain hash inventory mismatch" }
  foreach ($tool in $actualTools) {
   $pin=@($spec.toolchain | Where-Object { (Full $_.path) -eq (Full $tool.identity.path) })
   if ($pin.Count -ne 1 -or $pin[0].sha256 -ne $tool.identity.sha256 -or $pin[0].bytes -ne $tool.identity.bytes) { Block "toolchain_hash" $tool.identity.path }
  }
 } catch { Block "toolchain" $_.Exception.Message }
} catch { Block "invalid_input" $_.Exception.Message }
try {
 $after=@($before | ForEach-Object { Identity $_.path })
 for ($i=0;$i -lt $before.Count;$i++) {
  if (($before[$i] | ConvertTo-Json -Compress) -ne ($after[$i] | ConvertTo-Json -Compress)) { Block "protected_changed" ("Owner/concurrent change observed; not undone: "+$before[$i].path) }
 }
 foreach ($record in $records) {
  if ($record.identity.present) {
   $current=Identity $record.identity.path
   $selectedAfter+=$current
   if ($current.sha256 -ne $record.identity.sha256 -or $current.bytes -ne $record.identity.bytes) { Block "input_changed" $record.identity.path }
  }
 }
} catch { Block "protection" $_.Exception.Message }
foreach ($path in @($protected)+@($records | ForEach-Object { $_.identity.path })) {
 if ($path -and ((Full $Report) -eq (Full $path) -or (Full ($Report+".partial")) -eq (Full $path))) { Write-Error -ErrorAction Continue "Report collides with input: $path"; exit 2 }
}
$exitCode=1; $status="BLOCKED"
if ($reasons.Count -eq 0) { $exitCode=0; $status="ADMITTED" }
$command="powershell.exe -NoProfile -ExecutionPolicy Bypass -File "+[char]34+$PSCommandPath+[char]34+" -Report "+[char]34+$Report+[char]34+" -Runtime "+[char]34+$Runtime+[char]34+" -Root "+[char]34+$Root+[char]34+" -InputSpec "+[char]34+$InputSpec+[char]34
$result=[ordered]@{schema_version=1;kind="nr-input-inspection";complete=$true;status=$status;exit=$exitCode;command=$command;utc=[DateTime]::UtcNow.ToString("o");repository=$gitRecord;runtime=$runtimeRecord;records=@($records.ToArray());reasons=@($reasons.ToArray());protection=@{before=$before;after=$after;selected_before=@($records | ForEach-Object { $_.identity });selected_after=$selectedAfter;changes_undone=$false};limits=@("Admission is static, not mapped/evaluated Feature 18 proof.","Missing locks are blockers; no placeholder hashes admitted.")}
try {
 $null=[IO.Directory]::CreateDirectory($parent)
 $temporary=$Report+".partial"
 if ((Test-Path -LiteralPath $temporary) -and ((Get-Item -LiteralPath $temporary -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Partial report is a reparse point" }
 [IO.File]::WriteAllText($temporary,($result | ConvertTo-Json -Depth 24),(New-Object Text.UTF8Encoding($false)))
 if (Test-Path -LiteralPath $Report) { [IO.File]::Replace($temporary,$Report,[NullString]::Value) } else { [IO.File]::Move($temporary,$Report) }
} catch { Write-Error -ErrorAction Continue $_; exit 2 }
Write-Output ($result | ConvertTo-Json -Depth 24 -Compress)
exit $exitCode
