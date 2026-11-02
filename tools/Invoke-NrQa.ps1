[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('host-foundation','host-isolation-refusal','sampler','texture-roundtrip','flow-translation','color-alpha','native-model','native-bootstrap','ghost-calibration','ghost-baseline','lifecycle','two-instances','property-roundtrip','recording-output','handled-faults','shutdown','qualification','gui-av')][string]$Scenario,
    [Parameter(Mandatory)][string]$Evidence,
    [Parameter(Mandatory)][string]$HostRoot,
    [Parameter(Mandatory)][string]$Runtime,
    [switch]$LifecycleNative,
    [switch]$LifecycleDiagnosticR4,
    [switch]$CoreInitControl,
    [string]$Task14CandidateManifest,
    [string]$Task14CandidateSha256,
    [string]$Task14CandidateBuild,
    [ValidatePattern('^native[0-9]{2,4}$')][string]$Task14Attempt,
    [string]$Task15CandidateManifest,
    [string]$Task15CandidateSha256,
    [string]$Task15CandidateBuild,
    [ValidatePattern('^native[0-9]{2,4}$')][string]$Task15Attempt,
    [string]$Task16CandidateManifest,
    [string]$Task16CandidateSha256,
    [string]$Task17CandidateManifest,
    [string]$Task17CandidateSha256,
    [string]$Task27CandidateManifest,
    [string]$Task27CandidateSha256,
    [string]$GhostAcceptedCalibrationSha256,
    [string]$GhostAcceptedControlsSha256,
    [string]$ForeignRoot,
    [int]$ForeignPid = 0
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if($Scenario -eq 'ghost-baseline') {
    if($Task17CandidateManifest -or $Task17CandidateSha256){throw 'T17_PUBLIC_CANDIDATE_SCOPE'}
    $Task17CandidateManifest=$Task27CandidateManifest
    $Task17CandidateSha256=$Task27CandidateSha256
} elseif($Task27CandidateManifest -or $Task27CandidateSha256){throw 'T27_PUBLIC_CANDIDATE_SCOPE'}
if($Scenario -in @('handled-faults','shutdown','ghost-baseline')) {
    # Route derived from the window root's identity (ARCHITECT-DIRECTION Q2), not a literal: the evidence
    # root is exactly one level below a window root named <...>window-r<round>-<tag>-<yyyymmdd> that sits
    # directly under <evidence tree>/task-<n>. The window stage owns candidate/admission validation.
    $t17Tree = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../.omo/evidence/obs-nr-native-filter-20260930')).TrimEnd('\','/')
    $t17Evidence = [IO.Path]::GetFullPath($Evidence).TrimEnd('\','/')
    $t17Window = Split-Path -Parent $t17Evidence
    $t17Task = if($t17Window) { Split-Path -Parent $t17Window }
    if(!$t17Task -or [IO.Path]::GetFileName($t17Window) -cnotmatch '^[a-z0-9-]*window-r[0-9]+-[a-z0-9-]+-[0-9]{8}$' -or
        [IO.Path]::GetFileName($t17Task) -cnotmatch '^task-[0-9]+$' -or
        (Split-Path -Parent $t17Task) -ine $t17Tree) { throw 'NR_FAULT_PARENT_ADMISSION_REQUIRED' }
    $t17Stage = Join-Path $t17Window $(if($Scenario -eq 'ghost-baseline'){'Invoke-Task27Window.ps1'}else{'Invoke-Task17Window.ps1'})
    if(!('NrTask17RoutePaths' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public sealed class NrTask17RoutePaths : IDisposable {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    private static extern SafeFileHandle CreateFileW(string path, uint access, uint share, IntPtr security, uint creation, uint flags, IntPtr template);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    private static extern uint GetFinalPathNameByHandleW(SafeFileHandle handle, System.Text.StringBuilder path, uint size, uint flags);
    [StructLayout(LayoutKind.Sequential)]
    private struct Info {
        public uint attributes;
        public System.Runtime.InteropServices.ComTypes.FILETIME creation, access, write;
        public uint volume, sizeHigh, sizeLow, links, indexHigh, indexLow;
    }
    [DllImport("kernel32.dll", SetLastError=true)]
    private static extern bool GetFileInformationByHandle(SafeFileHandle handle, out Info info);
    private readonly string root;
    private readonly Dictionary<string, SafeFileHandle> held = new Dictionary<string, SafeFileHandle>(StringComparer.OrdinalIgnoreCase);
    public NrTask17RoutePaths(string ownedRoot) {
        root=Path.GetFullPath(ownedRoot).TrimEnd('\\','/');
    }
    public string Hold(string path) {
        string full=Path.GetFullPath(path).TrimEnd('\\','/');
        if(!String.Equals(full,root,StringComparison.OrdinalIgnoreCase) &&
            !full.StartsWith(root+Path.DirectorySeparatorChar,StringComparison.OrdinalIgnoreCase))
            throw new IOException("NR_QA_T17_OUTSIDE_OWNED_ROOT");
        var ancestors=new List<string>();
        for(string current=full;;current=Path.GetDirectoryName(current)) {
            ancestors.Add(current);
            if(String.Equals(current,root,StringComparison.OrdinalIgnoreCase))break;
        }
        ancestors.Reverse();
        foreach(string current in ancestors) {
            if(held.ContainsKey(current))continue;
            // OPEN_REPARSE_POINT inspects the link itself; no share-write/delete keeps the
            // inspected object and every ancestor fixed through all downstream dot-sources.
            var handle=CreateFileW(current,0x80000000,1,IntPtr.Zero,3,0x02200000,IntPtr.Zero);
            if(handle.IsInvalid) {handle.Dispose();throw new IOException("NR_QA_T17_PATH_OPEN");}
            try {
                Info info;
                if(!GetFileInformationByHandle(handle,out info))throw new IOException("NR_QA_T17_PATH_INFO");
                if((info.attributes&0x400)!=0)throw new IOException("NR_QA_T17_REPARSE_PATH");
                var canonical=new System.Text.StringBuilder(32768);
                uint count=GetFinalPathNameByHandleW(handle,canonical,(uint)canonical.Capacity,0);
                if(count==0 || count>=canonical.Capacity)throw new IOException("NR_QA_T17_CANONICAL_PATH");
                string resolved=canonical.ToString();
                if(resolved.StartsWith(@"\\?\",StringComparison.Ordinal))resolved=resolved.Substring(4);
                if(!String.Equals(resolved,current,StringComparison.OrdinalIgnoreCase))
                    throw new IOException("NR_QA_T17_CANONICAL_PATH");
                held.Add(current,handle);
            } catch {handle.Dispose();throw;}
        }
        return full;
    }
    public void Dispose() {foreach(var handle in held.Values)handle.Dispose();held.Clear();}
}
'@
    }
    $t17Paths=[NrTask17RoutePaths]::new((Join-Path $PSScriptRoot '..'))
    try {
        $t17Window=$t17Paths.Hold($t17Window)
        if(Test-Path -LiteralPath $t17Evidence){$null=$t17Paths.Hold($t17Evidence)}
        if(!(Test-Path -LiteralPath $t17Stage -PathType Leaf)) { throw 'NR_QA_T17_WINDOW_STAGE_MISSING' }
        $t17Stage=$t17Paths.Hold($t17Stage)
        $t17Inputs=$t17Paths.Hold((Join-Path $t17Window 'INPUTS.json'))
        if([IO.Path]::GetFullPath($Task17CandidateManifest) -ine $t17Inputs -or
            $Task17CandidateSha256 -cnotmatch '^[0-9a-f]{64}$' -or
            (Get-FileHash -LiteralPath $t17Inputs -Algorithm SHA256).Hash.ToLowerInvariant() -cne $Task17CandidateSha256){
            throw 'WINDOW_INPUTS_BINDING'
        }
        $t17Contract=Get-Content -LiteralPath $t17Inputs -Raw -Encoding UTF8|ConvertFrom-Json
        if(!(Get-Variable WindowSession -Scope Script -ErrorAction SilentlyContinue) -or
            !$script:WindowSession -or $script:WindowSession.inputsPin.sha256 -cne $Task17CandidateSha256){
            throw 'WINDOW_ORIGINAL_SESSION_REQUIRED'
        }
        $t17Scenarios=@($t17Contract.scenarios|Where-Object {$_.scenario -ceq $Scenario})
        if($t17Scenarios.Count -ne 1 -or !$script:WindowSession.active -or
            $script:WindowSession.active.scenario -cne $Scenario){throw 'WINDOW_ROUTE_IDENTITY_BINDING'}
        $t17Pins=@($t17Contract.generatorSources)+@($t17Scenarios[0].runner,$t17Scenarios[0].validator)
        foreach($t17Pin in $t17Pins) {
            $t17PinnedPath=$t17Paths.Hold($t17Pin.path)
            if($t17Pin.sha256 -cnotmatch '^[0-9a-f]{64}$' -or
                (Get-Item -LiteralPath $t17PinnedPath).Length -ne $t17Pin.bytes -or
                (Get-FileHash -LiteralPath $t17PinnedPath -Algorithm SHA256).Hash.ToLowerInvariant() -cne $t17Pin.sha256){
                throw 'WINDOW_STALE_PIN'
            }
        }
        foreach($t17Kind in @('runner','validator')) {
            $t17Expected=$t17Scenarios[0].$t17Kind
            $t17Active=$script:WindowSession.active.$t17Kind
            if($t17Paths.Hold($t17Active.path) -ine $t17Paths.Hold($t17Expected.path) -or
                $t17Active.sha256 -cne $t17Expected.sha256 -or $t17Active.function -cne $t17Expected.function){
                throw 'WINDOW_ROUTE_IDENTITY_BINDING'
            }
        }
        # D3 v2 emits these exact dispatcher bytes. INPUTS pins generic route, runner and
        # validator above; the retained handles keep every pin valid at its moment of use.
        if((Get-FileHash -LiteralPath $t17Stage -Algorithm SHA256).Hash.ToLowerInvariant() -cne
            'f978695cecde711ca029449608f25f76c72dc3a301e6ec3e846cc901b970eece'){throw 'WINDOW_STALE_PIN'}
        $priorGhostPolicy=$env:NR_GHOST_FREEZE_POLICY
        $priorGhostAccepted=$env:NR_GHOST_ACCEPTED_CALIBRATION
        try {
            if($Scenario -eq 'ghost-baseline') {
                $ghostSupportPath=$t17Paths.Hold((Join-Path $PSScriptRoot 'host/GhostDigestSupport.ps1'))
                if((Get-FileHash -LiteralPath $ghostSupportPath -Algorithm SHA256).Hash.ToLowerInvariant() -cne
                    '3505f58cf083034d2ee852886fb7c0437cdfbab0fbeb05949e51e22abb7597fe'){throw 'WINDOW_STALE_PIN'}
                . $ghostSupportPath
                $ghostArguments=@($t17Scenarios[0].validatorCall.arguments)
                $ghostCalibration=@($ghostArguments|Where-Object {$_.name -ceq 'CalibrationSha256' -and $_.source -ceq 'pinSha256'})
                $ghostControls=@($ghostArguments|Where-Object {$_.name -ceq 'ControlsSha256' -and $_.source -ceq 'pinSha256'})
                if($ghostCalibration.Count -ne 1 -or $ghostControls.Count -ne 1){throw 'NR_QA_GHOST_BASELINE_ACCEPTED_HASH_REQUIRED'}
                foreach($ghostPin in @($ghostCalibration[0].pin,$ghostControls[0].pin)) {
                    $ghostPath=$t17Paths.Hold($ghostPin.path)
                    if((Get-Item -LiteralPath $ghostPath).Length -ne $ghostPin.bytes -or
                        (Get-FileHash -LiteralPath $ghostPath -Algorithm SHA256).Hash.ToLowerInvariant() -cne $ghostPin.sha256){throw 'WINDOW_STALE_PIN'}
                }
                if(($GhostAcceptedCalibrationSha256 -and $GhostAcceptedCalibrationSha256 -cne $ghostCalibration[0].pin.sha256) -or
                    ($GhostAcceptedControlsSha256 -and $GhostAcceptedControlsSha256 -cne $ghostControls[0].pin.sha256)){throw 'NR_QA_GHOST_BASELINE_ACCEPTED_HASH_MISMATCH'}
                $ghostPolicyRoot=Join-Path (Join-Path $PSScriptRoot '..') '.omo/evidence/obs-nr-native-filter-20260930/task-26/foundation-20261014'
                $ghostManifest=Join-Path (Join-Path $PSScriptRoot '..') '.omo/evidence/obs-nr-native-filter-20260930/task-26/manifests/cpu-fixtures.json'
                foreach($ghostPolicyName in @('calibration','holdout','masks','epsilon','thresholds')){$null=$t17Paths.Hold((Join-Path $ghostPolicyRoot ($ghostPolicyName+'.json')))}
                $null=$t17Paths.Hold($ghostManifest)
                $ghostPolicy=Read-GhostFreezePolicy $ghostPolicyRoot $ghostManifest
                $env:NR_GHOST_FREEZE_POLICY=$ghostPolicy|ConvertTo-Json -Compress
                $env:NR_GHOST_ACCEPTED_CALIBRATION=@{calibration_path=$ghostCalibration[0].pin.path;controls_path=$ghostControls[0].pin.path;
                    calibration_sha256=$ghostCalibration[0].pin.sha256;controls_sha256=$ghostControls[0].pin.sha256}|ConvertTo-Json -Compress
            }
        . $t17Stage `
            -Scenario $Scenario -Evidence $Evidence -HostRoot $HostRoot -Runtime $Runtime `
            -CandidateManifest $Task17CandidateManifest -CandidateSha256 $Task17CandidateSha256
        } finally {
            $env:NR_GHOST_FREEZE_POLICY=$priorGhostPolicy
            $env:NR_GHOST_ACCEPTED_CALIBRATION=$priorGhostAccepted
        }
    } finally {$t17Paths.Dispose()}
    return
}
if($Task17CandidateManifest -or $Task17CandidateSha256){throw 'T17_PUBLIC_CANDIDATE_SCOPE'}
if($Scenario -eq 'recording-output') {
    . 'C:/Users/USER/orca/projects/obs-nr-native-filter-wt/w2/.omo/evidence/obs-nr-native-filter-20260930/task-16/recording-window-r27-orca-pin-preserved-20261014/Invoke-RecordingPublic.ps1' `
        -Scenario $Scenario -Evidence $Evidence -HostRoot $HostRoot -Runtime $Runtime `
        -CandidateManifest $Task16CandidateManifest -CandidateSha256 $Task16CandidateSha256
    return
}
if($Task16CandidateManifest -or $Task16CandidateSha256){throw 'NR_RECORDING_CANDIDATE_SCOPE'}
$work = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$task6Evidence = [IO.Path]::GetFullPath((Join-Path $work '.omo/evidence/obs-nr-native-filter-20260930/task-6'))
$task6VerifierEvidence = [IO.Path]::GetFullPath((Join-Path $task6Evidence 'verifier'))
$task7Evidence = [IO.Path]::GetFullPath((Join-Path $work '.omo/evidence/obs-nr-native-filter-20260930/task-7'))
$task7VerifierEvidence = [IO.Path]::GetFullPath((Join-Path $task7Evidence 'verifier'))
$task8Evidence = [IO.Path]::GetFullPath((Join-Path $work '.omo/evidence/obs-nr-native-filter-20260930/task-8'))
$task8VerifierEvidence = [IO.Path]::GetFullPath((Join-Path $task8Evidence 'verifier'))
$task9Evidence = [IO.Path]::GetFullPath((Join-Path $work '.omo/evidence/obs-nr-native-filter-20260930/task-9'))
$task9VerifierEvidence = [IO.Path]::GetFullPath((Join-Path $task9Evidence 'verifier'))
$task11Evidence = [IO.Path]::GetFullPath((Join-Path $work '.omo/evidence/obs-nr-native-filter-20260930/task-11'))
$task11VerifierEvidence = [IO.Path]::GetFullPath((Join-Path $task11Evidence 'verifier'))
$task12Evidence = [IO.Path]::GetFullPath((Join-Path $work '.omo/evidence/obs-nr-native-filter-20260930/task-12'))
$task12VerifierEvidence = [IO.Path]::GetFullPath((Join-Path $task12Evidence 'verifier'))
$task13Evidence = [IO.Path]::GetFullPath((Join-Path $work '.omo/evidence/obs-nr-native-filter-20260930/task-13'))
$task13VerifierEvidence = [IO.Path]::GetFullPath((Join-Path $task13Evidence 'verifier'))
$task14Evidence = [IO.Path]::GetFullPath((Join-Path $work '.omo/evidence/obs-nr-native-filter-20260930/task-14'))
$task14VerifierEvidence = [IO.Path]::GetFullPath((Join-Path $task14Evidence 'verifier'))
$task14CandidateRoot = Join-Path $task14Evidence 'instance-integration-20261011/b'
$task14ReusedNative = Join-Path $work 'out/task-13-diagnostic-r4/native'
$task13DiagnosticR4Evidence = [IO.Path]::GetFullPath((Join-Path $task13Evidence 'codex-continuation-20261004/diagnostic-r4-verified'))
if($CoreInitControl -and ($Scenario -ne 'native-model' -or $LifecycleNative -or $LifecycleDiagnosticR4)){throw 'NR_QA_CORE_CONTROL_SCOPE'}
$coreControlEvidence=[IO.Path]::GetFullPath((Join-Path $task13Evidence 'codex-continuation-20261004/core-control-r6d-physical'))
if($LifecycleDiagnosticR4 -and ($Scenario -ne 'lifecycle' -or !$LifecycleNative)){throw 'NR_QA_DIAGNOSTIC_SCOPE'}
$task26Evidence = [IO.Path]::GetFullPath((Join-Path $work '.omo/evidence/obs-nr-native-filter-20260930/task-26'))
$task26VerifierEvidence = [IO.Path]::GetFullPath((Join-Path $task26Evidence 'verifier'))
$evidencePath = [IO.Path]::GetFullPath($Evidence).TrimEnd('\','/')
$task = if ($Scenario -eq 'sampler' -or ($Scenario -eq 'host-foundation' -and $evidencePath -in @($task6Evidence,$task6VerifierEvidence))) { 'task-6' } elseif ($Scenario -eq 'texture-roundtrip' -or ($Scenario -eq 'host-foundation' -and $evidencePath -in @($task7Evidence,$task7VerifierEvidence))) { 'task-7' } else { 'task-2' }
$owned = [IO.Path]::GetFullPath((Join-Path $work ('out/' + $task + '/host')))
if ($Scenario -eq 'flow-translation' -or ($Scenario -eq 'host-foundation' -and $evidencePath -in @($task8Evidence,$task8VerifierEvidence))) {
    $task = 'task-8'
    $owned = [IO.Path]::GetFullPath((Join-Path $work 'out/task-8/host'))
}
if ($Scenario -eq 'color-alpha' -or ($Scenario -eq 'host-foundation' -and $evidencePath -in @($task9Evidence,$task9VerifierEvidence))) {
    $task = 'task-9'
    $owned = [IO.Path]::GetFullPath((Join-Path $work 'out/task-9/host'))
}
if ($Scenario -eq 'native-model' -or ($Scenario -eq 'host-foundation' -and $evidencePath -in @($task11Evidence,$task11VerifierEvidence))) {
    $task = 'task-11'
    $owned = [IO.Path]::GetFullPath((Join-Path $work 'out/task-11/host'))
}
if ($Scenario -eq 'native-bootstrap' -or ($Scenario -eq 'host-foundation' -and $evidencePath -in @($task12Evidence,$task12VerifierEvidence))) {
    $task = 'task-12'
    $owned = [IO.Path]::GetFullPath((Join-Path $work 'out/task-12/host'))
}
if ($Scenario -eq 'ghost-calibration' -or ($Scenario -eq 'host-foundation' -and $evidencePath -in @($task26Evidence,$task26VerifierEvidence))) {
    $task = 'task-26'
    $owned = [IO.Path]::GetFullPath((Join-Path $work 'out/task-26/host'))
}
if ($Scenario -eq 'ghost-baseline') {
    $task = 'task-27'
    $owned = [IO.Path]::GetFullPath((Join-Path $work 'out/task-27/host'))
}
if ($Scenario -eq 'lifecycle' -or ($Scenario -eq 'host-foundation' -and $evidencePath -in @($task13Evidence,$task13VerifierEvidence))) {
    $task='task-13'
    $owned=[IO.Path]::GetFullPath((Join-Path $work 'out/task-13/host'))
}
$root = [IO.Path]::GetFullPath($HostRoot).TrimEnd('\','/')
if ($Scenario -eq 'two-instances' -or ($Scenario -eq 'host-foundation' -and $evidencePath -in @($task14Evidence,$task14VerifierEvidence))) {
    $task='task-14'
    $owned=[IO.Path]::GetFullPath((Join-Path $work 'out/task-14/host'))
}
$evidenceRoot = [IO.Path]::GetFullPath((Join-Path $work ('.omo/evidence/obs-nr-native-filter-20260930/' + $task)))
if ($task -eq 'task-6' -and $evidencePath -eq $task6VerifierEvidence) {
    $evidenceRoot = $task6VerifierEvidence
    $owned = [IO.Path]::GetFullPath((Join-Path $work 'out/task-6-verifier/host'))
}
if ($task -eq 'task-7' -and $evidencePath -eq $task7VerifierEvidence) {
    $evidenceRoot = $task7VerifierEvidence
    $owned = [IO.Path]::GetFullPath((Join-Path $work 'out/task-7-verifier/host'))
}
if ($task -eq 'task-8' -and $evidencePath -eq $task8VerifierEvidence) {
    $evidenceRoot = $task8VerifierEvidence
    $owned = [IO.Path]::GetFullPath((Join-Path $work 'out/task-8-verifier/host'))
}
if ($task -eq 'task-9' -and $evidencePath -eq $task9VerifierEvidence) {
    $evidenceRoot = $task9VerifierEvidence
    $owned = [IO.Path]::GetFullPath((Join-Path $work 'out/task-9-verifier/host'))
}
if ($task -eq 'task-11' -and $evidencePath -eq $task11VerifierEvidence) {
    $evidenceRoot = $task11VerifierEvidence
    $owned = [IO.Path]::GetFullPath((Join-Path $work 'out/task-11-verifier/host'))
}
if ($task -eq 'task-12' -and $evidencePath -eq $task12VerifierEvidence) {
    $evidenceRoot = $task12VerifierEvidence
    $owned = [IO.Path]::GetFullPath((Join-Path $work 'out/task-12-verifier/host'))
}
if ($task -eq 'task-26' -and $evidencePath -eq $task26VerifierEvidence) {
    $evidenceRoot = $task26VerifierEvidence
    $owned = [IO.Path]::GetFullPath((Join-Path $work 'out/task-26-verifier/host'))
}
if ($task -eq 'task-13' -and $evidencePath -eq $task13VerifierEvidence) {
    $evidenceRoot=$task13VerifierEvidence
    $owned=[IO.Path]::GetFullPath((Join-Path $work 'out/task-13-verifier/host'))
}
if ($task -eq 'task-14' -and $evidencePath -eq $task14VerifierEvidence) {
    $evidenceRoot=$task14VerifierEvidence
    $owned=[IO.Path]::GetFullPath((Join-Path $work 'out/task-14-verifier/host'))
}
if($LifecycleDiagnosticR4 -and $evidencePath -eq $task13DiagnosticR4Evidence) {
    $evidenceRoot=$task13DiagnosticR4Evidence
    $owned=[IO.Path]::GetFullPath((Join-Path $work 'out/task-13-diagnostic-r4/verified-host'))
}
if($CoreInitControl) {
    $task='task-13';$evidenceRoot=$coreControlEvidence
    $owned=[IO.Path]::GetFullPath((Join-Path $work 'out/task-13-core-control-r6d/verified-host'))
}
if($Task14Attempt) {
    if($Scenario -ne 'two-instances'){throw 'NR_QA_INSTANCE_ATTEMPT_SCOPE'}
    $evidenceRoot=[IO.Path]::GetFullPath((Join-Path $task14Evidence ('attempts/'+$Task14Attempt)))
    $owned=[IO.Path]::GetFullPath((Join-Path $work ('out/task-14-'+$Task14Attempt+'/host')))
}
if($Scenario -eq 'property-roundtrip') {
    if(!$Task15Attempt -or !$Task15CandidateManifest -or !$Task15CandidateBuild -or
        $Task15CandidateSha256 -cnotmatch '^[0-9a-f]{64}$'){throw 'NR_PROPERTY_CANDIDATE_REQUIRED'}
    $task='task-15'
    $evidenceRoot=[IO.Path]::GetFullPath((Join-Path $work ('.omo/evidence/obs-nr-native-filter-20260930/task-15/attempts/'+$Task15Attempt)))
    $owned=[IO.Path]::GetFullPath((Join-Path $work ('out/task-15-'+$Task15Attempt+'/host')))
}
elseif($Task15Attempt -or $Task15CandidateManifest -or $Task15CandidateSha256 -or $Task15CandidateBuild) {
    throw 'NR_PROPERTY_CANDIDATE_SCOPE'
}
if ($evidencePath -ne $evidenceRoot) { throw 'NR_QA_EVIDENCE_REFUSED' }
if ($Scenario -notin @('host-foundation','host-isolation-refusal','sampler','texture-roundtrip','flow-translation','color-alpha','native-model','native-bootstrap','ghost-calibration','ghost-baseline','lifecycle','two-instances','property-roundtrip')) { throw 'NR_QA_NOT_IMPLEMENTED' }
if (($Task14CandidateManifest -or $Task14CandidateSha256 -or $Task14CandidateBuild) -and $task -ne 'task-14') { throw 'NR_QA_INSTANCE_CANDIDATE_SCOPE' }
if($LifecycleNative -and $Scenario -ne 'lifecycle'){throw 'NR_QA_LIFECYCLE_NATIVE_SCOPE'}
. (Join-Path $PSScriptRoot 'host/HostSupport.ps1')
if($Scenario -eq 'property-roundtrip'){. (Join-Path $PSScriptRoot 'host/PropertySupport.ps1')}
if($Scenario -in @('ghost-calibration','ghost-baseline')){. (Join-Path $PSScriptRoot 'host/GhostDigestSupport.ps1')}
if($Scenario -eq 'ghost-baseline'){
    . (Join-Path $PSScriptRoot 'host/Scenario-GhostBaseline.ps1')
    $ghostAccepted=Get-GhostBaselineAcceptedBinding $work $GhostAcceptedCalibrationSha256 $GhostAcceptedControlsSha256
}elseif($GhostAcceptedCalibrationSha256 -or $GhostAcceptedControlsSha256){throw 'NR_QA_GHOST_BASELINE_SCOPE'}
if ($Scenario -eq 'host-isolation-refusal') {
    if (!$ForeignRoot) { $ForeignRoot = $work }
    if (!$ForeignPid) { $ForeignPid = $PID }
    $before = Identity (Get-Process -Id $ForeignPid)
    $entriesBefore = @(Get-ChildItem -LiteralPath $ForeignRoot -Force | ForEach-Object { $_.FullName })
    $refused = $false
    try { Assert-OwnedRoot $ForeignRoot } catch {
        if ($_.Exception.Message -ne 'NR_QA_FOREIGN_ROOT') { throw }
        $refused = $true
    }
    if (!$refused) { throw 'NR_QA_EXPECTED_REFUSAL' }
    $after = Identity (Get-Process -Id $ForeignPid)
    $entriesAfter = @(Get-ChildItem -LiteralPath $ForeignRoot -Force | ForEach-Object { $_.FullName })
    if (($before | ConvertTo-Json -Compress) -ne ($after | ConvertTo-Json -Compress) -or (Compare-Object $entriesBefore $entriesAfter)) { throw 'NR_QA_FOREIGN_CHANGED' }
    New-Item -ItemType Directory -Force $Evidence | Out-Null
    @{scenario=$Scenario;result='PASS';refusal='NR_QA_FOREIGN_ROOT';foreign_before=$before;foreign_after=$after;foreign_entries_unchanged=$true;stop_calls=0;foreign_write_calls=0;cleanup=@{process_created=$false;host_created=$false}} |
        ConvertTo-Json -Depth 8 | Set-Content -Encoding UTF8 (Join-Path $Evidence 'host-isolation-refusal.json')
    return
}
Assert-OwnedRoot $root
if($task -eq 'task-15' -and (Test-Path -LiteralPath $Evidence)){throw 'NR_PROPERTY_EXISTING_EVIDENCE'}
if ($ForeignPid -or $ForeignRoot) { throw 'NR_QA_FOREIGN_PROCESS' }
$spec = Get-Content (Join-Path $work 'provenance/inputs.spec.json') -Raw | ConvertFrom-Json
if (!$spec.runtime.owner_selected) { throw 'NR_QA_RUNTIME_OWNER_REFUSED' }
if ([IO.Path]::GetFullPath($Runtime) -ne [IO.Path]::GetFullPath($spec.runtime.path)) { throw 'NR_QA_RUNTIME_REFUSED' }
if ((Get-FileHash -LiteralPath $Runtime -Algorithm SHA256).Hash.ToLowerInvariant() -ne $spec.runtime.sha256) { throw 'NR_QA_RUNTIME_CHANGED' }
Assert-NoUnrelatedProcess
$installed = 'C:/Program Files/obs-studio'
$build = Join-Path $work 'out/build/windows-x64-debug/Debug'
if ($task -eq 'task-8') { $build = Join-Path $work 'out/task-8/native/Debug' }
if ($task -eq 'task-9') {
    $build = Join-Path $work 'out/task-9/native/Debug'
    if ($evidencePath -eq $task9VerifierEvidence) { $build = Join-Path $work 'out/task-9-verifier/native/Debug' }
}
if ($task -eq 'task-11') {
    $build = Join-Path $work 'out/task-11/native/Debug'
    if ($evidencePath -eq $task11VerifierEvidence) { $build = Join-Path $work 'out/task-11-verifier/native/Debug' }
}
if ($task -eq 'task-12') {
    $build = Join-Path $work 'out/task-12/native/Debug'
    if ($evidencePath -eq $task12VerifierEvidence) { $build = Join-Path $work 'out/task-12-verifier/native/Debug' }
}
if ($task -eq 'task-26') {
    $build = Join-Path $work 'out/task-26/native/Debug'
    if ($evidencePath -eq $task26VerifierEvidence) { $build = Join-Path $work 'out/task-26-verifier/native/Debug' }
}
if ($task -eq 'task-27') { $build = Join-Path $work 'out/task-27/native/Debug' }
if ($task -eq 'task-13') {
    $build=Join-Path $work 'out/task-13/native/Debug'
    if($evidencePath -eq $task13VerifierEvidence){$build=Join-Path $work 'out/task-13-verifier/native/Debug'}
    if($LifecycleDiagnosticR4){$build=Join-Path $work 'out/task-13-diagnostic-r4/native/Debug'}
    if($CoreInitControl){$build=Join-Path $work 'out/task-13-core-control-r6d/native/Debug'}
}
if ($task -eq 'task-14') {
    # This path cannot fall back to any stock or Task13 binary.
    $build=if($Task14CandidateBuild){[IO.Path]::GetFullPath($Task14CandidateBuild)}else{$task14CandidateRoot}
    $candidatePrefix=[IO.Path]::GetFullPath($task14CandidateRoot).TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar
    if([IO.Path]::GetFullPath($build) -ne [IO.Path]::GetFullPath($task14CandidateRoot) -and
        ![IO.Path]::GetFullPath($build).StartsWith($candidatePrefix,[StringComparison]::OrdinalIgnoreCase)){throw 'NR_QA_INSTANCE_CANDIDATE_PATH'}
}
if($task -eq 'task-15') {
    $build=[IO.Path]::GetFullPath($Task15CandidateBuild)
    $propertyFreeze=Assert-NrPropertyCandidate $Task15CandidateManifest $Task15CandidateSha256 $build $work
}
foreach ($binary in @('obs-nr-filter.dll','nr-qa-host.exe')) {
    if($task -eq 'task-15'){continue} # Both explicitly pinned paths were checked above.
    if (!(Test-Path (Join-Path $build $binary))) { throw ('NR_QA_BUILD_MISSING: ' + $binary) }
}
if ($Scenario -eq 'color-alpha' -and !(Test-Path (Join-Path (Split-Path $build) 'nr_color.dxil'))) { throw 'NR_QA_BUILD_MISSING: nr_color.dxil' }
if ($Scenario -in @('native-model','native-bootstrap','ghost-calibration','ghost-baseline','lifecycle','two-instances')) {
    $inspectorBuild=if($task -eq 'task-14'){Join-Path $task14ReusedNative 'Debug'}else{$build}
    $shaderBuild=if($task -eq 'task-14'){$task14ReusedNative}else{Split-Path $build}
    if (!(Test-Path (Join-Path $inspectorBuild 'nr-driver-inspect.exe'))) { throw 'NR_QA_BUILD_MISSING: nr-driver-inspect.exe' }
    $shaders=@('nr_codec.dxil','nr_color.dxil','nr_guides.cso','nr_interop_vertex.cso','nr_interop_pixel.cso')
    if($Scenario -in @('native-bootstrap','ghost-calibration','ghost-baseline','lifecycle','two-instances')){$shaders+='nr_obs_analysis.cso'}
    if($Scenario -in @('ghost-calibration','ghost-baseline')){$shaders+='nr_ghost_chunk.dxil'}
    foreach ($shader in $shaders) {
        if (!(Test-Path (Join-Path $shaderBuild $shader))) { throw ('NR_QA_BUILD_MISSING: ' + $shader) }
    }
}
$sources = [Collections.Generic.List[object]]::new()
if($CoreInitControl) {
    $coreManifest=Join-Path $task13Evidence 'codex-continuation-20261004/core-control-r6d-cpu/candidate-artifacts.json'
    $coreManifestSha256='4a66b586cec9c1cd8d387afd632d4f920a41ebdbfde5df26f88bd79da602817c'
    if((Get-FileHash -LiteralPath $coreManifest -Algorithm SHA256).Hash.ToLowerInvariant() -ne $coreManifestSha256){throw 'NR_QA_CORE_MANIFEST_CHANGED'}
    $coreFreeze=Get-Content -LiteralPath $coreManifest -Raw|ConvertFrom-Json
    if($coreFreeze.artifacts.Count -ne 12){throw 'NR_QA_CORE_ARTIFACT_SET'}
    foreach($artifact in $coreFreeze.artifacts){
        if((Get-FileHash -LiteralPath $artifact.path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $artifact.sha256){throw ('NR_QA_CORE_ARTIFACT_CHANGED: '+$artifact.path)}
    }
}
if($LifecycleDiagnosticR4) {
    $diagnosticManifest=Join-Path $task13Evidence 'codex-continuation-20261004/diagnostic-r4/freeze-manifest-r4.json'
    $diagnosticManifestSha256='34516bb8dbde9c8fade067c1e66d5422ebc3754cbf616d4055a520e8964f5405'
    if((Get-FileHash -LiteralPath $diagnosticManifest -Algorithm SHA256).Hash.ToLowerInvariant() -ne $diagnosticManifestSha256){throw 'NR_QA_DIAGNOSTIC_MANIFEST_CHANGED'}
    $diagnosticFreeze=Get-Content -LiteralPath $diagnosticManifest -Raw|ConvertFrom-Json
    if($diagnosticFreeze.artifacts.Count -ne 12){throw 'NR_QA_DIAGNOSTIC_ARTIFACT_SET'}
    foreach($artifact in $diagnosticFreeze.artifacts) {
        if((Get-FileHash -LiteralPath $artifact.path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $artifact.sha256){throw ('NR_QA_DIAGNOSTIC_ARTIFACT_CHANGED: '+$artifact.path)}
    }
}
function Test-NrQaOriginalOwnerClosed($Owner) {
    if($Owner.started -eq $false){return $true}
    if(!$Owner.kernelAttached -or !$Owner.kernelWait.WaitOne(0)){return $false}
    if(!$Owner.process.WaitForExit(0)){return $false}
    if($Owner.outputStarted -ne $true -or $Owner.errorStarted -ne $true){return $false}
    if($Owner.nativeOutput) {
        if(!$Owner.nativeOutput.OutputClosed.WaitOne(0) -or !$Owner.nativeOutput.ErrorClosed.WaitOne(0)){return $false}
    } else {
        if(!$Owner.stdout.IsCompleted -or $Owner.stdout.IsFaulted -or $Owner.stdout.IsCanceled -or
            !$Owner.stderr.IsCompleted -or $Owner.stderr.IsFaulted -or $Owner.stderr.IsCanceled){return $false}
    }
    $Owner.closed=$true
    $Owner.state='original-terminal-reaped-eof'
    return $true
}
function Wait-NrQaOriginalOwner($Owner) {
    # This is the last boundary in the original runtime, not a PID adoption.
    try {
        $Owner.state='original-owner-retained'
        if(!$Owner.kernelAttached) {
            $Owner.handle=$Owner.process.SafeHandle
            $Owner.kernelWait.SafeWaitHandle=[Microsoft.Win32.SafeHandles.SafeWaitHandle]::new($Owner.handle.DangerousGetHandle(),$false)
            $Owner.kernelAttached=$true
        }
        if($Owner.outputStarted -eq $false) {
            $Owner.outputStarted=$null
            if($Owner.nativeOutput){$Owner.process.BeginOutputReadLine()}
            else{$Owner.stdout=$Owner.process.StandardOutput.ReadToEndAsync()}
            $Owner.outputStarted=$true
        }
        if($Owner.errorStarted -eq $false) {
            $Owner.errorStarted=$null
            if($Owner.nativeOutput){$Owner.process.BeginErrorReadLine()}
            else{$Owner.stderr=$Owner.process.StandardError.ReadToEndAsync()}
            $Owner.errorStarted=$true
        }
        if($Owner.outputStarted -ne $true -or $Owner.errorStarted -ne $true){throw 'NR_QA_ORIGINAL_STREAM_STATE_UNKNOWN'}
        $Owner.kernelWait.WaitOne()|Out-Null
        $Owner.process.WaitForExit()
        if($Owner.nativeOutput) {
            $Owner.nativeOutput.OutputClosed.WaitOne()|Out-Null
            $Owner.nativeOutput.ErrorClosed.WaitOne()|Out-Null
        } else {
            $Owner.stdout.GetAwaiter().GetResult()|Out-Null
            $Owner.stderr.GetAwaiter().GetResult()|Out-Null
        }
        $Owner.closed=$true
        $Owner.state='original-terminal-reaped-eof'
    } catch {
        $Owner.secondaryErrors+=$_
        $Owner.state='original-owner-unknown'
        # Created before Start, never signaled or disposed while unresolved.
        # A failed original-handle/reap/EOF observation cannot release the owner.
        $Owner.unknownGate.WaitOne()|Out-Null
    }
}
function Assert-NrInstanceReceipts([string]$Output) {
    $views=@();$dispatch=@();$cleanup=@();$baseline=@()
    foreach($line in ($Output -split [char]10)) {
        foreach($entry in @(@('NR_INSTANCE_VIEW ','view'),@('NR_INSTANCE_DISPATCH ','dispatch'),@('NR_INSTANCE_CLEANUP ','cleanup'),@('NR_INSTANCE_BASELINE ','baseline'))) {
            if(!$line.StartsWith($entry[0])){continue}
            $row=$line.Substring($entry[0].Length)|ConvertFrom-Json
            switch($entry[1]) {'view'{$views+=$row};'dispatch'{$dispatch+=$row};'cleanup'{$cleanup+=$row};'baseline'{$baseline+=$row}}
        }
    }
    if($views.Count -ne 36 -or $dispatch.Count -ne 12 -or $cleanup.Count -ne 2){throw 'NR_QA_INSTANCE_SET'}
    if($baseline.Count -ne 2){throw 'NR_QA_INSTANCE_BASELINE'}
    $scope=@($Output -split '\r?\n'|Where-Object {$_ -eq 'NR_INSTANCE_SCOPE real_filters=2 repeated_OBS_views=true native=true cleanup=complete'})
    if($scope.Count -ne 1 -or $Output.Contains('NR_INSTANCE_RETAINED')){throw 'NR_QA_INSTANCE_CLOSURE'}
    $instances=@{};$groups=@{};$jobs=@{}
    foreach($lane in @(0,1)) {
        $close=@($cleanup|Where-Object {$_.lane -ceq $lane})
        if($close.Count -ne 1 -or !$close[0].instance -or $close[0].actual_evaluations -ne 6){throw 'NR_QA_INSTANCE_CLEANUP'}
        foreach($field in @('closed','worker_exit','retained')) {
            if($close[0].$field -isnot [bool] -or $close[0].$field -ne ($field -ne 'retained')){throw 'NR_QA_INSTANCE_CLOSURE'}
        }
        $instances[$lane]=$close[0].instance
        $b=@($baseline|Where-Object {$_.lane -ceq $lane})
        if($b.Count -ne 1){throw 'NR_QA_INSTANCE_BASELINE'}
        $b=$b[0]
        foreach($field in @('evaluated','composed','submitted','returned','physical_complete','ordinary')) {
            if($b.$field -isnot [bool] -or !$b.$field){throw 'NR_QA_INSTANCE_BASELINE'}
        }
        foreach($field in @('tick','content_epoch','history_epoch','resource_generation','submission_serial','ingress','source_renders')) {
            if($b.$field -le 0){throw 'NR_QA_INSTANCE_BASELINE'}
        }
        if($b.instance -ne $instances[$lane] -or $b.current_hash -isnot [string] -or !$b.current_hash -or
            $b.content_id -ne $(if($lane -eq 0){1}else{9}) -or
            $b.settings_strength -ne $(if($lane -eq 0){0.25}else{0.75})){throw 'NR_QA_INSTANCE_BASELINE'}
        foreach($step in 0..5) {
            $d=@($dispatch|Where-Object {$_.lane -ceq $lane -and $_.step -eq $step})
            $g=@($views|Where-Object {$_.lane -ceq $lane -and $_.step -eq $step})
            if($d.Count -ne 1 -or $g.Count -ne 3 -or (@($g.view|Sort-Object) -join ',') -ne '0,1,2'){throw 'NR_QA_INSTANCE_DISPATCH_SET'}
            $d=$d[0];$first=$g[0];$groups["$lane/$step"]=$g;$jobs["$lane/$step"]=$d
            foreach($field in @('evaluated','composed','submitted','returned','physical_complete')) {
                if($d.$field -isnot [bool] -or !$d.$field){throw 'NR_QA_INSTANCE_DISPATCH'}
            }
            foreach($field in @('tick','content_epoch','history_epoch','resource_generation','submission_serial','ingress')) {
                if($d.$field -le 0){throw 'NR_QA_INSTANCE_KEY'}
            }
            if($d.instance -ne $instances[$lane] -or $d.ingress -ne $b.ingress+$step+1 -or
                $d.submission_serial -ne $b.submission_serial+$step+1){throw 'NR_QA_INSTANCE_KEY'}
            $prior=if($step){$jobs["$lane/$($step-1)"]}else{$b}
            if($d.tick -ne $prior.tick+1){throw 'NR_QA_INSTANCE_DUPLICATE_EVALUATION'}
            if(!$step -and ($d.content_epoch -ne $b.content_epoch -or $d.history_epoch -ne $b.history_epoch -or
                $d.resource_generation -ne $b.resource_generation)){throw 'NR_QA_INSTANCE_KEY'}
            if($step -and $d.content_epoch -ne $jobs["$lane/0"].content_epoch){throw 'NR_QA_INSTANCE_KEY'}
            if($lane -eq 1 -and $step) {
                foreach($field in @('content_epoch','history_epoch','resource_generation')) {
                    if($d.$field -ne $jobs['1/0'].$field){throw 'NR_QA_INSTANCE_B_CHANGED'}
                }
            }
            $currentId=$step+$(if($lane -eq 0){1}else{9})
            $selected=$d.ingress-1
            $kind=if($lane -eq 0 -and $step -eq 3){2}else{3}
            foreach($v in $g) {
                if($v.instance -ne $d.instance -or $v.tick -ne $d.tick -or $v.ingress -ne $d.ingress -or
                    $v.content_epoch -ne $d.content_epoch -or $v.history_epoch -ne $d.history_epoch -or
                    $v.resource_generation -ne $d.resource_generation -or $v.selected -ne $selected -or $v.kind -ne $kind -or
                    $v.current_content_id -ne $currentId -or $v.content_id -ne ($currentId-$(if($step){1}else{0})) -or
                    $v.settings_strength -ne $(if($lane -eq 0){0.25}else{0.75}) -or
                    $v.source_renders -ne $b.source_renders+$step+1 -or $v.retired_generations -lt 0 -or $v.retired_generations -gt 2){throw 'NR_QA_INSTANCE_LINEAGE'}
                foreach($field in @('output_hash','raw_hash','current_hash')) {
                    if($v.$field -isnot [string] -or !$v.$field -or $v.$field -cne $first.$field){throw 'NR_QA_INSTANCE_HASH'}
                }
                if($kind -ne 3) {
                    if($null -ne $v.selected_job -or $v.output_hash -cne $v.raw_hash){throw 'NR_QA_INSTANCE_RAW_KEY'}
                } else {
                    $key=$v.selected_job
                    if(!$key -or (@($key.PSObject.Properties.Name|Sort-Object) -join ',') -ne 'content_epoch,history_epoch,ingress,instance,resource_generation,submission_serial'){throw 'NR_QA_INSTANCE_SELECTED_KEY'}
                    foreach($field in @('instance','content_epoch','ingress','history_epoch','resource_generation','submission_serial')) {
                        if($key.$field -ne $prior.$field){throw 'NR_QA_INSTANCE_SELECTED_KEY'}
                    }
                }
            }
            if($step -and $first.raw_hash -cne $groups["$lane/$($step-1)"][0].current_hash){throw 'NR_QA_INSTANCE_HASH'}
            if(!$step -and $first.raw_hash -cne $b.current_hash){throw 'NR_QA_INSTANCE_HASH'}
        }
        if(@($dispatch|Where-Object {$_.lane -ceq $lane}|Select-Object -ExpandProperty tick -Unique).Count -ne 6 -or
            @($dispatch|Where-Object {$_.lane -ceq $lane}|Select-Object -ExpandProperty submission_serial -Unique).Count -ne 6){throw 'NR_QA_INSTANCE_DUPLICATE_EVALUATION'}
    }
    if($instances[0] -eq $instances[1]){throw 'NR_QA_INSTANCE_IDENTITY'}
    # View calls alternate lanes at each step; lane-by-lane rendering is not this trial.
    foreach($step in 0..5) {
        $ordered=@($views|Where-Object {$_.step -eq $step})
        for($i=0;$i -lt 6;$i+=2) {
            if($ordered[$i].lane -ceq $ordered[$i+1].lane -or $ordered[$i].view -ne $ordered[$i+1].view){throw 'NR_QA_INSTANCE_INTERLEAVE'}
        }
        foreach($lane in @(0,1)) {
            $foreign=$groups["$(1-$lane)/$step"][0]
            $own=$groups["$lane/$step"][0]
            if($own.current_hash -ceq $foreign.current_hash -or $own.output_hash -ceq $foreign.output_hash -or
                $own.raw_hash -ceq $foreign.raw_hash){throw 'NR_QA_INSTANCE_FOREIGN_CONTENT'}
        }
    }
    @{views=$views;dispatch=$dispatch;cleanup=$cleanup;baseline=$baseline}
}
function Assert-NrInstanceCapacityReceipts([string]$Output) {
    $sets=@{VIEW=@();JOB=@();RELEASE=@();RECOVERY=@();CLEANUP=@();SCOPE=@()}
    foreach($line in ($Output -split [char]10)) {
        foreach($name in @('VIEW','JOB','RELEASE','RECOVERY','CLEANUP','SCOPE')) {
            $prefix='NR_INSTANCE_CAPACITY_'+$name+' '
            if($line.StartsWith($prefix)){$sets[$name]+=$line.Substring($prefix.Length)|ConvertFrom-Json}
        }
    }
    if($sets.SCOPE.Count -ne 1 -or $sets.CLEANUP.Count -ne 2 -or $sets.RELEASE.Count -ne 5 -or $sets.RECOVERY.Count -ne 2){throw 'NR_QA_CAPACITY_SET'}
    $scope=$sets.SCOPE[0];$wait=$scope.recovery_wait_frames
    if($wait -lt 0 -or $wait -gt 64 -or $scope.fixed_view_rows -ne 96 -or $scope.copy_jobs_a -ne 10 -or $scope.copy_jobs_b -ne 14+$wait){throw 'NR_QA_CAPACITY_COUNTS'}
    foreach($pair in @(@('native_transport',$true),@('Feature18_capacity',$false),@('source_gate_only',$false))) {
        if($scope.($pair[0]) -isnot [bool] -or $scope.($pair[0]) -ne $pair[1]){throw 'NR_QA_CAPACITY_SCOPE'}
    }
    $fixed=@($sets.VIEW|Where-Object {$_.recovery_wait -ceq $false})
    $waiting=@($sets.VIEW|Where-Object {$_.recovery_wait -ceq $true})
    if($fixed.Count -ne 96 -or $waiting.Count -ne 3*$wait -or $sets.VIEW.Count -ne $fixed.Count+$waiting.Count){throw 'NR_QA_CAPACITY_VIEW_SET'}
    $groups=@{};$instances=@{};$jobs=@{}
    foreach($lane in 0..1) {
        $close=@($sets.CLEANUP|Where-Object {$_.lane -ceq $lane})
        if($close.Count -ne 1 -or $close[0].instance -le 0 -or $close[0].held_consumers -ne 0){throw 'NR_QA_CAPACITY_CLEANUP'}
        $instances[$lane]=$close[0].instance
        foreach($field in @('closed','worker_exit','retained')) {
            if($close[0].$field -isnot [bool] -or $close[0].$field -ne ($field -ne 'retained')){throw 'NR_QA_CAPACITY_CLOSURE'}
        }
        $expectedJobs=if($lane){14+$wait}else{10}
        $expectedIngress=if($lane){14+$wait}else{12}
        if($close[0].copy_jobs -ne $expectedJobs -or $close[0].capture_ingress -ne $expectedIngress -or
            $close[0].recovery_wait_frames -ne $(if($lane){$wait}else{0})){throw 'NR_QA_CAPACITY_COUNTS'}
        $ownJobs=@($sets.JOB|Where-Object {$_.lane -ceq $lane})
        if($ownJobs.Count -ne $expectedJobs){throw 'NR_QA_CAPACITY_JOB_SET'}
        foreach($job in $ownJobs) {
            if($job.cohort -cne 'native-transport-capacity' -or $job.instance -ne $instances[$lane] -or
                !$job.key -or $job.key.instance -ne $job.instance -or $job.tick -le 0){throw 'NR_QA_CAPACITY_JOB_KEY'}
            foreach($field in @('returned','physical_complete','transport_copy','feature18_evaluated')) {
                if($job.$field -isnot [bool] -or $job.$field -ne ($field -ne 'feature18_evaluated')){throw 'NR_QA_CAPACITY_JOB_KIND'}
            }
            if((@($job.key.PSObject.Properties.Name|Sort-Object) -join ',') -ne 'content_epoch,history_epoch,ingress,instance,resource_generation,submission_serial'){throw 'NR_QA_CAPACITY_JOB_KEY'}
            foreach($field in @('instance','content_epoch','ingress','history_epoch','resource_generation','submission_serial')) {
                if($job.key.$field -le 0 -or $job.key.$field -is [double]){throw 'NR_QA_CAPACITY_JOB_KEY'}
            }
            $key="$lane/$($job.tick)"
            if($jobs.ContainsKey($key)){throw 'NR_QA_CAPACITY_DUPLICATE_JOB'}
            $jobs[$key]=$job
        }
        foreach($phase in 0..15) {
            $rows=@($fixed|Where-Object {$_.lane -ceq $lane -and $_.phase -eq $phase})
            if($rows.Count -ne 3 -or (@($rows.view|Sort-Object) -join ',') -ne '0,1,2'){throw 'NR_QA_CAPACITY_VIEW_SET'}
            $first=$rows[0];$groups["$lane/$phase"]=$rows
            $admitted=if($lane){$phase -lt 14}else{$phase -notin @(3,4,10,11,14,15)}
            foreach($row in $rows) {
                if($row.cohort -cne 'native-transport-capacity' -or $row.instance -ne $instances[$lane] -or
                    $row.current_admitted -isnot [bool] -or $row.current_admitted -ne $admitted -or
                    $row.retired_generations -lt 0 -or $row.retired_generations -gt 2 -or
                    $row.held_consumers -lt 0 -or $row.held_consumers -gt 3 -or
                    $row.ingress -lt $row.ingress_before -or $row.slots.Count -ne 3){throw 'NR_QA_CAPACITY_VIEW'}
                foreach($field in @('tick','ingress','instance','content_epoch','history_epoch','resource_generation','output_hash','current_content_id','public_state','public_reason','current_admitted')) {
                    if($row.$field -cne $first.$field){throw 'NR_QA_CAPACITY_REPEATED_VIEW'}
                }
                if($row.output_hash -isnot [string] -or !$row.output_hash){throw 'NR_QA_CAPACITY_PIXELS'}
                if($admitted) {
                    $job=$jobs["$lane/$($row.tick)"]
                    if(!$job -or !$row.current_job){throw 'NR_QA_CAPACITY_ADMISSION'}
                    foreach($field in @('instance','content_epoch','ingress','history_epoch','resource_generation','submission_serial')) {
                        if($row.current_job.$field -ne $job.key.$field){throw 'NR_QA_CAPACITY_ADMISSION'}
                    }
                } elseif($row.current_job){throw 'NR_QA_CAPACITY_FALSE_ADMISSION'}
                foreach($slot in $row.slots) {
                    if($slot.slot -notin 0..2 -or $slot.reusable -isnot [bool] -or $slot.consumer_open -isnot [bool] -or
                        ($slot.key -and $slot.key.instance -ne $row.instance)){throw 'NR_QA_CAPACITY_SLOT'}
                }
                if((@($row.slots.slot|Sort-Object) -join ',') -ne '0,1,2'){throw 'NR_QA_CAPACITY_SLOT'}
                if(!$lane -and $phase -in @(3,4)) {
                    if($row.reason -ne 13 -or $row.admission_reason -ne 13 -or $row.kind -ne 0 -or
                        $row.public_state -cne 'bypassed' -or $row.public_reason -cne 'slotsFull' -or
                        $row.selected_job -or $row.content_id -ne $row.current_content_id -or $row.output_hash -cne $row.raw_hash -or
                        @($row.slots|Where-Object {$_.reusable}).Count -or $row.counters.dispatches -ne 3){throw 'NR_QA_CAPACITY_SLOT_BYPASS'}
                }
                if(!$lane -and $phase -in @(10,11)) {
                    if($row.reason -ne 14 -or $row.kind -ne 0 -or !$row.placeholder -or
                        $row.public_state -cne 'bypassed' -or $row.public_reason -cne 'retirementBudget' -or
                        $row.resource_generation -ne 3 -or $row.retired_generations -ne 2 -or
                        $row.ingress -ne $row.ingress_before -or $row.selected_job -or
                        $row.content_id -ne $row.current_content_id){throw 'NR_QA_CAPACITY_RETIREMENT_BYPASS'}
                }
            }
        }
    }
    if($instances[0] -eq $instances[1]){throw 'NR_QA_CAPACITY_FOREIGN_INSTANCE'}
    foreach($phase in 0..15) {
        $a=$groups["0/$phase"][0];$b=$groups["1/$phase"][0]
        if($a.output_hash -ceq $b.output_hash -or $a.content_id -eq $b.content_id){throw 'NR_QA_CAPACITY_FOREIGN_PIXELS'}
        foreach($field in @('content_epoch','history_epoch','resource_generation')) {
            if($b.$field -ne $groups['1/0'][0].$field){throw 'NR_QA_CAPACITY_B_MUTATED'}
        }
    }
    foreach($row in $waiting) {
        if($row.lane -cne 1 -or $row.instance -ne $instances[1] -or !$row.current_admitted -or
            !$jobs.ContainsKey("1/$($row.tick)")){throw 'NR_QA_CAPACITY_WAIT_LANE'}
    }
    foreach($release in $sets.RELEASE) {
        foreach($pair in @(@('event_observed',$true),@('consumer_open_before',$true),@('consumer_open_after',$false))) {
            if($release.($pair[0]) -isnot [bool] -or $release.($pair[0]) -ne $pair[1]){throw 'NR_QA_CAPACITY_RELEASE'}
        }
        if($release.lane -cne 0 -or $release.key.instance -ne $instances[0] -or
            !$release.event_observed -or !$release.consumer_open_before -or $release.consumer_open_after -or
            $release.required_consumer -le 0 -or $release.actual_consumer -lt $release.required_consumer -or
            $release.required_consumer -is [double] -or $release.actual_consumer -is [double] -or
            $release.actual_consumer -eq [UInt64]::MaxValue){throw 'NR_QA_CAPACITY_RELEASE'}
    }
    if(@($sets.RELEASE|Where-Object {$_.purpose -ceq 'slots'}).Count -ne 3 -or
        @($sets.RELEASE|Where-Object {$_.purpose -ceq 'retirement'}).Count -ne 2){throw 'NR_QA_CAPACITY_RELEASE_SET'}
    foreach($purpose in @('slots','retirement')) {
        $recovery=@($sets.RECOVERY|Where-Object {$_.purpose -ceq $purpose})
        if($recovery.Count -ne 1){throw 'NR_QA_CAPACITY_RECOVERY_SET'}
        $recovery=$recovery[0]
        foreach($field in @('same_instance','subscription_before_release','event_observed')) {
            if($recovery.$field -isnot [bool] -or !$recovery.$field){throw 'NR_QA_CAPACITY_RECOVERY'}
        }
        if($recovery.instance -ne $instances[0] -or !$recovery.same_instance -or !$recovery.subscription_before_release -or
            !$recovery.event_observed -or $recovery.resource_generation -ne $(if($purpose -ceq 'slots'){1}else{4}) -or
            $recovery.retired_generations -ne 0 -or !$recovery.current_job -or !$recovery.selected_job -or
            $recovery.current_job.instance -ne $instances[0] -or $recovery.selected_job.instance -ne $instances[0]){throw 'NR_QA_CAPACITY_RECOVERY'}
    }
    $sets
}
function Assert-NrInstanceCandidate([string]$Manifest,[string]$Sha256,[string]$CandidateRoot,[string]$Build,[string]$Work) {
    $candidate=[IO.Path]::GetFullPath($CandidateRoot).TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar
    $integration=[IO.Path]::GetFullPath((Split-Path $CandidateRoot)).TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar
    if(!$Manifest -or $Sha256 -cnotmatch '^[0-9a-f]{64}$' -or
        ![IO.Path]::GetFullPath($Manifest).StartsWith($integration,[StringComparison]::OrdinalIgnoreCase) -or
        ([IO.Path]::GetFullPath($Build) -ne [IO.Path]::GetFullPath($CandidateRoot) -and
            ![IO.Path]::GetFullPath($Build).StartsWith($candidate,[StringComparison]::OrdinalIgnoreCase))){throw 'NR_QA_INSTANCE_CANDIDATE_PATH'}
    if((Get-FileHash -LiteralPath $Manifest -Algorithm SHA256).Hash.ToLowerInvariant() -cne $Sha256){throw 'NR_QA_INSTANCE_FREEZE_CHANGED'}
    $freeze=Get-Content -LiteralPath $Manifest -Raw|ConvertFrom-Json
    if($freeze.build_origin -cne 'StageB' -or [IO.Path]::GetFullPath($freeze.build) -ne [IO.Path]::GetFullPath($Build)){throw 'NR_QA_INSTANCE_BUILD_ORIGIN'}
    $reusedNative=Join-Path $Work 'out/task-13-diagnostic-r4/native'
    $required=@('obs-nr-filter.dll','nr-qa-host.exe')|ForEach-Object {Join-Path $Build $_}
    $required+=Join-Path $reusedNative 'Debug/nr-driver-inspect.exe'
    $required+=@('nr_codec.dxil','nr_color.dxil','nr_guides.cso','nr_interop_vertex.cso','nr_interop_pixel.cso','nr_obs_analysis.cso')|ForEach-Object {Join-Path $reusedNative $_}
    if(@($freeze.artifacts).Count -ne $required.Count){throw 'NR_QA_INSTANCE_ARTIFACT_SET'}
    foreach($path in $required) {
        $pin=@($freeze.artifacts|Where-Object {[IO.Path]::GetFullPath($_.path) -eq [IO.Path]::GetFullPath($path)})
        if($pin.Count -ne 1 -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $pin[0].sha256){throw 'NR_QA_INSTANCE_ARTIFACT_CHANGED'}
    }
    if(!@($freeze.reused_inputs|Where-Object {[IO.Path]::GetExtension($_.path) -eq '.obj'}).Count -or
        !@($freeze.reused_inputs|Where-Object {[IO.Path]::GetExtension($_.path) -eq '.lib'}).Count){throw 'NR_QA_INSTANCE_REUSED_INPUT_SET'}
    foreach($pin in $freeze.reused_inputs) {
        if((Get-FileHash -LiteralPath $pin.path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $pin.sha256){throw 'NR_QA_INSTANCE_REUSED_INPUT_CHANGED'}
    }
    $main=$freeze.main_sdk_overlay
    if(!$main -or ![IO.Path]::GetFullPath($main.path).StartsWith($integration,[StringComparison]::OrdinalIgnoreCase) -or
        (Get-FileHash -LiteralPath $main.path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $main.sha256){throw 'NR_QA_INSTANCE_MAIN_OVERLAY_CHANGED'}
    # The manifest binds inputs, not merely the candidate pair. Include every production
    # source/build input and host source, so an old compiled producer cannot be admitted.
    $overlay=Join-Path $Work '.omo/evidence/obs-nr-native-filter-20260930/task-13/shared-sdk-lifetime-repair-20261005/overlay'
    $inputs=@(Get-ChildItem -LiteralPath (Join-Path $Work 'src'),(Join-Path $Work 'cmake/nr'),(Join-Path $Work 'tools/host'),(Join-Path $Work 'deps'),$overlay -File -Recurse|
        Where-Object {$_.Extension -in @('.h','.cpp','.c','.hlsl','.cmake','.ps1','.lib')}|ForEach-Object {$_.FullName})
    $inputs+=@('CMakeLists.txt','tools/driver-inspect.cpp','tools/Invoke-NrQa.ps1','tools/tests/bootstrap_runner_tests.ps1','provenance/inputs.spec.json','provenance/sdk-lock.json')|ForEach-Object {Join-Path $Work $_}
    foreach($path in $inputs) {
        $pin=@($freeze.sources|Where-Object {[IO.Path]::GetFullPath($_.path) -eq [IO.Path]::GetFullPath($path)})
        if($pin.Count -ne 1 -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $pin[0].sha256){throw ('NR_QA_INSTANCE_SOURCE_CHANGED: '+$path)}
    }
    $freeze
}
function Start-NrInstanceOriginal($Owner) {
    # The caller publishes this actual object before Start. No PID adoption.
    if($task -eq 'task-15' -and !$global:NrQaPropertyOwners.Contains($Owner)) {
        [void]$global:NrQaPropertyOwners.Add($Owner)
    }
    $Owner.started=$null
    if(!$Owner.process.Start()){$Owner.started=$false;throw 'NR_QA_START_FAILED'}
    $Owner.started=$true
    $Owner.handle=$Owner.process.SafeHandle
    $Owner.kernelWait.SafeWaitHandle=[Microsoft.Win32.SafeHandles.SafeWaitHandle]::new($Owner.handle.DangerousGetHandle(),$false)
    $Owner.kernelAttached=$true
    $Owner.identity=Identity $Owner.process
    $Owner.outputStarted=$null
    if($Owner.nativeOutput){$Owner.process.BeginOutputReadLine()}
    else{$Owner.stdout=$Owner.process.StandardOutput.ReadToEndAsync()}
    $Owner.outputStarted=$true
    $Owner.errorStarted=$null
    if($Owner.nativeOutput){$Owner.process.BeginErrorReadLine()}
    else{$Owner.stderr=$Owner.process.StandardError.ReadToEndAsync()}
    $Owner.errorStarted=$true
}
function Resolve-NrInstanceDriver([string]$Inspector) {
    # The inspector is CPU-only, but a failed start/reap still retains its originals.
    $p=[Diagnostics.Process]::new()
    $o=@{process=$p;handle=$null;started=$false;closed=$false;identity=$null;events=@();retainedEvents=@();
        exitSignal=$null;nativeOutput=$null;stdout=$null;stderr=$null;outputStarted=$false;errorStarted=$false;
        kernelWait=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset);
        kernelAttached=$false;unknownGate=[Threading.ManualResetEvent]::new($false);
        primaryError=$null;secondaryErrors=@();state='original-owner-published'}
    $script:NrQaOriginalInspectorOwner=$o
    $global:NrQaOriginalInspectorOwner=$o
    $p.StartInfo.FileName=$Inspector;$p.StartInfo.WorkingDirectory=Split-Path $Inspector
    $p.StartInfo.UseShellExecute=$false;$p.StartInfo.CreateNoWindow=$true
    $p.StartInfo.RedirectStandardOutput=$true;$p.StartInfo.RedirectStandardError=$true
    $p.EnableRaisingEvents=$true;$o.exitSignal=[NrProcessExit]::new($p)
    try {
        Start-NrInstanceOriginal $o
        if(!$o.kernelWait.WaitOne(15000) -or !$p.WaitForExit(5000) -or
            !$o.stdout.Wait(5000) -or !$o.stderr.Wait(5000) -or !(Test-NrQaOriginalOwnerClosed $o)){throw 'NR_QA_DRIVER_INSPECT_UNRESOLVED'}
        if($p.ExitCode){throw ('NR_QA_DRIVER_BINDING_REFUSED: '+$o.stderr.GetAwaiter().GetResult())}
        $binding=$o.stdout.GetAwaiter().GetResult()|ConvertFrom-Json
        if($binding.schema -ne 1 -or $binding.kind -ne 'active-nvidia-driver-core' -or
            $binding.loaded -isnot [bool] -or $binding.loaded -or $binding.gpu_calls -ne 0 -or
            $binding.signature_verified -isnot [bool] -or !$binding.signature_verified -or
            $binding.required_exports -ne 7 -or $binding.sha256 -cnotmatch '^[0-9a-f]{64}$' -or
            ![IO.Path]::IsPathRooted($binding.path) -or [IO.Path]::GetFileName($binding.path) -ne '_nvngx.dll' -or
            !$binding.device_instance -or !$binding.published_inf -or !$binding.active_package -or
            !$binding.driver_version -or !$binding.inf_section -or ![IO.Path]::IsPathRooted($binding.driver_store_inf)){throw 'NR_QA_DRIVER_INSPECT_SCHEMA'}
        $actual=FileIdentity $binding.path
        if($actual.sha256.ToLowerInvariant() -ne $binding.sha256){throw 'NR_QA_DRIVER_CHANGED'}
        @{binding=$binding;file=$actual;inspector=(FileIdentity $Inspector);identity=$o.identity;
            exit=$p.ExitCode;subscription_before_start=$true;module_loaded=$false;gpu_calls=0}
    }catch{$o.primaryError=$_;$o.state='original-owner-retained';throw}
}
function Stage-NrInstanceHost($Freeze) {
    # Reuse immutable recorded files, not the consumed Task13 stage/admission.
    $donor=Join-Path $work '.omo/evidence/obs-nr-native-filter-20260930/task-13/telemetry-lifecycle-successor-20261010/candidate01/stage-boundary.json'
    if((Get-FileHash -LiteralPath $donor -Algorithm SHA256).Hash.ToLowerInvariant() -cne
        '2ae9db4e0a53ac135fe2502cf9f465ebb9ccafb5dc7e5939499b7394ee985b15'){throw 'NR_QA_INSTANCE_DONOR_CHANGED'}
    $stage=Get-Content -LiteralPath $donor -Raw|ConvertFrom-Json
    if($stage.links.Count -ne 112 -or $stage.physicalCopies -ne 0){throw 'NR_QA_INSTANCE_DONOR_SET'}
    $identitySupport='C:/Users/USER/Documents/Codex/2026-10-04/x20-obs-nr-x20-x20-c/.omo/evidence/core-r6d-exit-recovery-01a106fe/fileid01/FileIdentity.ps1'
    if((Get-FileHash -LiteralPath $identitySupport -Algorithm SHA256).Hash.ToLowerInvariant() -cne
        '29c87318d3487091cc945839956739c38a9bffc1d5e23b0c077411e4eb51905a'){throw 'NR_QA_INSTANCE_IDENTITY_SUPPORT_CHANGED'}
    . $identitySupport
    if(([IO.DriveInfo]::new([IO.Path]::GetPathRoot($root))).DriveFormat -cne 'NTFS'){throw 'NR_QA_INSTANCE_NTFS_REQUIRED'}
    if(!('NrInstanceHardLink' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class NrInstanceHardLink {
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)]
 public static extern bool CreateHardLinkW(string target,string source,IntPtr reserved);
}
'@
    }
    $links=@();$seen=@{}
    foreach($entry in $stage.links) {
        $relative=[string]$entry.relative
        if($relative -notmatch '^(bin|data|obs-plugins)/' -or $relative -match '(?i)(^|/)\.\.(/|$)|nvngx|\.log$|\.cache$|(^|/)profile/|ngx-data' -or
            $relative.Contains('\') -or $seen.ContainsKey($relative)){throw 'NR_QA_INSTANCE_LINK_PATH'}
        $seen[$relative]=$true
        $pin=$entry.source
        $binary=if($relative -ceq 'bin/64bit/nr-qa-host.exe'){'nr-qa-host.exe'}elseif($relative -ceq 'obs-plugins/64bit/obs-nr-filter.dll'){'obs-nr-filter.dll'}else{$null}
        if($binary) {
            $candidatePath=if($Scenario -eq 'property-roundtrip' -and $binary -eq 'obs-nr-filter.dll'){$Freeze.plugin.path}else{Join-Path $build $binary}
            $pins=@($Freeze.artifacts|Where-Object {[IO.Path]::GetFullPath($_.path) -eq [IO.Path]::GetFullPath($candidatePath)})
            if($pins.Count -ne 1){throw 'NR_QA_INSTANCE_LINK_CANDIDATE_SET'}
            $pin=$pins[0]
        }
        if([IO.Path]::GetPathRoot($pin.path) -ine [IO.Path]::GetPathRoot($root)){throw 'NR_QA_INSTANCE_LINK_VOLUME'}
        $before=Get-RootFileIdentity $pin.path
        if($before.sha256.ToLowerInvariant() -cne $pin.sha256.ToLowerInvariant() -or $before.bytes -ne $pin.bytes){throw 'NR_QA_INSTANCE_LINK_SOURCE_CHANGED'}
        if(!$binary -and ($before.file_id -cne $pin.file_id -or $before.volume_serial -cne $pin.volume_serial)){throw 'NR_QA_INSTANCE_DONOR_IDENTITY'}
        $target=Join-Path $root $relative
        if(Test-Path -LiteralPath $target){throw 'NR_QA_INSTANCE_LINK_EXISTS'}
        $parent=Split-Path $target
        [void][IO.Directory]::CreateDirectory($parent)
        for($cursor=$parent;$cursor;$cursor=Split-Path $cursor -Parent) {
            if((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'NR_QA_INSTANCE_LINK_REPARSE'}
            if([IO.Path]::GetFullPath($cursor) -eq [IO.Path]::GetFullPath($root)){break}
        }
        if(![NrInstanceHardLink]::CreateHardLinkW($target,$pin.path,[IntPtr]::Zero)){throw [ComponentModel.Win32Exception]::new([Runtime.InteropServices.Marshal]::GetLastWin32Error())}
        $after=Get-RootFileIdentity $target
        foreach($field in @('file_id','volume_serial','sha256','bytes','attributes')) {
            if($after.$field -cne $before.$field){throw 'NR_QA_INSTANCE_LINK_IDENTITY'}
        }
        $again=Get-RootFileIdentity $pin.path
        foreach($field in @('file_id','volume_serial','sha256','bytes','attributes')) {
            if($again.$field -cne $before.$field){throw 'NR_QA_INSTANCE_LINK_SOURCE_MOVED'}
        }
        $sources.Add($before)
        $links+=@{relative=$relative;source=$before;target=$after;immutableByContract=$true}
    }
    if(!$seen.ContainsKey('bin/64bit/nr-qa-host.exe') -or !$seen.ContainsKey('obs-plugins/64bit/obs-nr-filter.dll')){throw 'NR_QA_INSTANCE_LINK_PAIR'}
    foreach($shader in @('nr_codec.dxil','nr_color.dxil','nr_guides.cso','nr_interop_vertex.cso','nr_interop_pixel.cso','nr_obs_analysis.cso')) {
        if(!$seen.ContainsKey('bin/64bit/'+$shader)){throw 'NR_QA_INSTANCE_LINK_SHADER_SET'}
    }
    [void][IO.Directory]::CreateDirectory((Join-Path $root 'profile/ngx-data'))
    if(@(Get-ChildItem -LiteralPath (Join-Path $root 'profile') -Recurse -File -Force).Count){throw 'NR_QA_INSTANCE_PROFILE_NOT_EMPTY'}
    @{donor=$donor;links=$links;physicalCopies=0;profileEmpty=$true;nativeStarted=$false}
}
function Invoke-NrInstanceTrial([bool]$Foundation=$false) {
    $freeze=Assert-NrInstanceCandidate $Task14CandidateManifest $Task14CandidateSha256 $task14CandidateRoot $build $work
    $sources=[Collections.Generic.List[object]]::new();$binding=@()
    foreach($pin in @($freeze.sources)+@($freeze.artifacts)+@($freeze.reused_inputs)){$sources.Add((FileIdentity $pin.path))}
    $sources.Add((FileIdentity $freeze.main_sdk_overlay.path))
    $driver=Resolve-NrInstanceDriver (Join-Path $task14ReusedNative 'Debug/nr-driver-inspect.exe')
    $sources.Add($driver.file)
    New-Item -ItemType Directory -Force $Evidence|Out-Null
    New-Item -ItemType Directory $root|Out-Null
    # Task14 links the recorded immutable runtime; neighboring staging is unchanged.
    $script:binding=@()
    $staging=Stage-NrInstanceHost $freeze
    $script:binding=@($staging.links|ForEach-Object {$_.target})
    foreach($pair in @(@('nr-qa-host.exe','bin/64bit/nr-qa-host.exe'),@('obs-nr-filter.dll','obs-plugins/64bit/obs-nr-filter.dll'))) {
        if((Get-FileHash (Join-Path $root $pair[1])).Hash -ne (Get-FileHash (Join-Path $build $pair[0])).Hash){throw 'NR_QA_INSTANCE_STAGED_PAIR'}
    }
    $nonce=[Guid]::NewGuid().ToString('N')
    $names=@('ready','action','acted','stop','exit')|ForEach-Object {'Local\nr-qa-'+$nonce+'-'+$_}
    $events=@();$retainedEvents=@();$process=[Diagnostics.Process]::new()
    $owner=@{process=$process;handle=$null;started=$false;closed=$false;identity=$null;
        events=$events;retainedEvents=$retainedEvents;exitSignal=$null;nativeOutput=$null;
        stdout=$null;stderr=$null;outputStarted=$false;errorStarted=$false;
        kernelWait=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset);
        kernelAttached=$false;unknownGate=[Threading.ManualResetEvent]::new($false);
        primaryError=$null;secondaryErrors=@();state='original-owner-published'}
    $script:NrQaOriginalOwner=$owner
    $global:NrQaOriginalOwner=$owner
    $record=@{scenario=$(if($Foundation){'host-foundation'}else{'two-instances'});result='FAIL';failure=$null;retained=$true;native_acceptance=$false;
        build_origin=$freeze.build_origin;build=$build;manifest=$Task14CandidateManifest;manifest_sha256=$Task14CandidateSha256;
        driver_binding=$driver;binding=$script:binding;sources_before=@($sources.ToArray());
        staging=$staging;
        root=$root;raw_log=(Join-Path $Evidence ('attempt-'+$nonce+'-native-host.log'))}
    try {
        foreach($name in $names){$events+=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,$name);$owner.events=$events}
        $retainedEvents=@(New-RetainedHostEvents 'lifecycle' $false $names[3]);$owner.retainedEvents=$retainedEvents
        $process.StartInfo.FileName=Join-Path $root 'bin/64bit/nr-qa-host.exe'
        $process.StartInfo.WorkingDirectory=Join-Path $root 'bin/64bit'
        $process.StartInfo.UseShellExecute=$false;$process.StartInfo.CreateNoWindow=$true
        $process.StartInfo.RedirectStandardOutput=$true;$process.StartInfo.RedirectStandardError=$true
        $process.StartInfo.Arguments='"'+$root+'" '+(($names|ForEach-Object {'"'+$_+'"'}) -join ' ')+$(if($Foundation){''}else{' two-instances'})
        $process.EnableRaisingEvents=$true
        $owner.exitSignal=[NrProcessExit]::new($process)
        $owner.nativeOutput=[NrHostOutput]::new($process,$record.raw_log)
        $record.command=$process.StartInfo.FileName+' '+$process.StartInfo.Arguments
        Assert-NoUnrelatedProcess
        Start-NrInstanceOriginal $owner
        if($owner.identity.image -ne $process.StartInfo.FileName){throw 'NR_QA_CHILD_IDENTITY_REFUSED'}
        if([Threading.WaitHandle]::WaitAny([Threading.WaitHandle[]]@($events[0],$owner.exitSignal.Exited),15000) -ne 0){throw 'NR_QA_INSTANCE_READY_UNRESOLVED'}
        $inventory=@($process.Modules|ForEach-Object {@{path=$_.FileName;sha256=(Get-FileHash $_.FileName).Hash}})
        Assert-CleanInventory $inventory
        Assert-ReadyHostPair $inventory $root $build 'lifecycle' $false
        $record.inventory_ready=$inventory
        $null=Assert-NrInstanceCandidate $Task14CandidateManifest $Task14CandidateSha256 $task14CandidateRoot $build $work
        $events[1].Set()|Out-Null
        if([Threading.WaitHandle]::WaitAny([Threading.WaitHandle[]]@($events[2],$owner.exitSignal.Exited),300000) -ne 0){throw 'NR_QA_INSTANCE_ACTION_UNRESOLVED'}
        $process.Refresh()
        Assert-CleanInventory @($process.Modules|ForEach-Object {@{path=$_.FileName;sha256=(Get-FileHash $_.FileName).Hash}})
        $events[3].Set()|Out-Null
        if(!$events[4].WaitOne(15000) -or !$owner.kernelWait.WaitOne(15000) -or !$process.WaitForExit(15000) -or
            !$owner.nativeOutput.OutputClosed.WaitOne(15000) -or !$owner.nativeOutput.ErrorClosed.WaitOne(15000)){throw 'NR_QA_INSTANCE_CLOSE_UNRESOLVED'}
        if(!(Test-NrQaOriginalOwnerClosed $owner)){throw 'NR_QA_INSTANCE_CLOSE_UNRESOLVED'}
        if($process.ExitCode){throw ('NR_QA_HOST_FAILED: '+$process.ExitCode)}
        if($owner.nativeOutput.SinkFailed){throw ('NR_QA_OUTPUT_SINK_FAILURE: '+$owner.nativeOutput.SinkFailure)}
        if($Foundation) {
            foreach($token in @('NR_QA_READY libobs=32.2.2 module=obs-nr-filter encoder=obs_x264','NR_QA_ACTION content_id=2583','NR_QA_EXIT cleanup=complete result=0')) {
                if(!$owner.nativeOutput.Text.Contains($token)){throw ('NR_QA_EVENT_LITERAL_MISSING: '+$token)}
            }
        } else {
            $record.receipts=Assert-NrInstanceReceipts $owner.nativeOutput.Text
            $record.capacity_receipts=Assert-NrInstanceCapacityReceipts $owner.nativeOutput.Text
        }
        $null=Assert-NrInstanceCandidate $Task14CandidateManifest $Task14CandidateSha256 $task14CandidateRoot $build $work
        foreach($source in $sources) {
            $after=FileIdentity $source.path
            if($after.sha256 -ne $source.sha256 -or $after.file_id -ne $source.file_id -or $after.attributes -ne $source.attributes){throw 'NR_QA_SOURCE_CHANGED'}
        }
        $record.result='PASS';$record.retained=$false
    } catch {
        $owner.primaryError=$_;$owner.state='original-owner-retained'
        $record.failure=$_.Exception.Message
    }
    # No generic finally, automatic stop, kill, dispose, or foreground uncertainty wait.
    # Parent retains this runtime and explicitly closes these original objects.
    $record.identity=$owner.identity;$record.owner_state=$owner.state
    $record.diagnosis_events=@($names[3]+'.host-diagnose',$names[3]+'.host-diagnosed')
    $record.output=if($owner.nativeOutput){$owner.nativeOutput.Text}else{''}
    $record.original_objects_retained=$true
    try{$record|ConvertTo-Json -Depth 20|Set-Content -Encoding UTF8 (Join-Path $Evidence $(if($Foundation){'host-foundation.json'}else{'two-instances.json'}))}
    catch{$owner.secondaryErrors+=$_;$record.result='FAIL';$record.failure='NR_QA_INSTANCE_RECEIPT_WRITE';$record.retained=$true}
    $record
}
if($task -eq 'task-15') {
    # This route never reaches historical generic teardown.
    Invoke-NrPropertyTrial $propertyFreeze
    return
}
if($task -eq 'task-14') {
    # Both routes return before the historical generic finally.
    try{Invoke-NrInstanceTrial ($Scenario -eq 'host-foundation')}
    catch{
        # Preflight/staging or inspector uncertainty also leaves the original
        # inspector (if created) published in this runtime, without a retry.
        @{scenario=$Scenario;result='FAIL';failure=$_.Exception.Message;retained=$true;
            native_acceptance=$false;original_objects_retained=$true;root=$root;
            manifest=$Task14CandidateManifest;manifest_sha256=$Task14CandidateSha256}
    }
    return
}
$originalOwner = $null
$process = $null
$identity = $null
$events = @()
$created = $false
$result = 'FAIL'
$failure = $null
$inventory = @()
$readyInventory = @()
$sourceAfter = @()
$command = $null
$arguments = $null
$exit = $null
$stdout = $null
$stderr = $null
$output = ''
$exitSignal = $null
$nativeOutput = $null
$nativeLog = $null
$views = @()
$patterns = @()
$flowRows = @()
$flowCaps = @()
$flowPasses = @()
$flowInputs = @()
$colorRows = @()
$colorDispatches = @()
$colorContext = @()
$colorControls = @()
$modelRows = @()
$modelContexts = @()
$modelCleanup = @()
$modelModules = @()
$bootstrapFrames=@();$bootstrapLanes=@();$bootstrapDecoded=@();$bootstrapCleanup=@()
$bootstrapSlots=@();$bootstrapSlotCleanup=@();$bootstrapArtifacts=@();$artifactRoot=$null
$lifecycleRows=@();$lifecycleCleanup=@();$lifecycleWitness=@();$retainedEvents=@()
$physicalUnresolved = $false
$driverBinding = $null
$binding = @()
New-Item -ItemType Directory -Force $Evidence | Out-Null
# Teardown registered before the host, links, named events or child exist.
@{state='registered';root=$root;policy='Signal own stop; wait bounded; terminate exact own identity only; remove minimal private host copies; never alter source bytes/ACL/attributes'} |
    ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $Evidence 'cleanup-registration.json')
try {
try {
    foreach ($path in @($spec.protected_paths)) { if (Test-Path -LiteralPath $path) { $sources.Add((FileIdentity $path)) } }
    foreach ($path in @('src/obs_filter.h','src/obs_filter.cpp','src/plugin-main.c','src/nr_contract.h','CMakeLists.txt','tools/Invoke-NrQa.ps1','tools/host/main.cpp','tools/host/HostSupport.ps1','tools/tests/sampler_contract_tests.cpp','tools/tests/host_runner_tests.ps1','provenance/inputs.spec.json','provenance/sdk-lock.json')) {
        if (Test-Path (Join-Path $work $path)) { $binding += FileIdentity (Join-Path $work $path) }
    }
    foreach ($binary in @('obs-nr-filter.dll','nr-qa-host.exe')) { $binding += FileIdentity (Join-Path $build $binary) }
    if ($Scenario -eq 'texture-roundtrip') {
        foreach ($path in @('src/interop.h','src/interop.cpp','src/shaders/nr_interop_blit.hlsl','cmake/nr/Interop.cmake','tools/host/nr_interop_scenario.h','tools/host/nr_interop_scenario.cpp','tools/tests/interop_contract_tests.cpp')) { $binding += FileIdentity (Join-Path $work $path) }
        foreach ($shader in @('nr_interop_vertex.cso','nr_interop_pixel.cso')) { $binding += FileIdentity (Join-Path (Split-Path $build) $shader) }
    }
    if ($Scenario -eq 'color-alpha') {
        foreach ($path in @('src/nr_color.h','src/nr_color.cpp','src/shaders/nr_color.hlsl','cmake/nr/Color.cmake','tools/host/nr_color_scenario.h','tools/host/nr_color_scenario.cpp','tools/tests/color_contract_tests.cpp','tools/tests/color_software_tests.cpp','src/interop.h','src/interop.cpp','cmake/nr/Interop.cmake')) { $binding += FileIdentity (Join-Path $work $path) }
        $binding += FileIdentity (Join-Path (Split-Path $build) 'nr_color.dxil')
    }
    if ($Scenario -in @('native-model','native-bootstrap','ghost-calibration','ghost-baseline','lifecycle')) {
        foreach ($path in @('src/nr_model.h','src/nr_model.cpp','src/nr_runtime.h','src/nr_runtime.cpp',
            'src/nr_core.h','src/nr_core.cpp','src/nr_guides.h','src/nr_guides.cpp','src/nr_color.h','src/nr_color.cpp',
            'src/frame_pipeline.h','src/frame_pipeline.cpp','src/interop.h','src/interop.cpp','src/shaders/nr_codec.hlsl',
            'src/shaders/nr_color.hlsl','src/shaders/nr_guides.hlsl','src/shaders/nr_interop_blit.hlsl',
            'cmake/nr/Model.cmake','cmake/nr/Scheduler.cmake','tools/host/nr_model_scenario.h','tools/host/nr_model_scenario.cpp',
            'tools/tests/model_contract_tests.cpp','tools/tests/model_runner_tests.ps1',
            'tools/driver-inspect.cpp','cmake/nr/Runtime.cmake')) { $binding += FileIdentity (Join-Path $work $path) }
        $driverBinding=Resolve-NrDriver (Join-Path $build 'nr-driver-inspect.exe')
        $binding += $driverBinding.inspector
        $sources.Add($driverBinding.file)
        $driverBinding|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $Evidence 'driver-binding.json') -Encoding UTF8
    }
    if($Scenario -in @('native-bootstrap','ghost-calibration','ghost-baseline','lifecycle')) {
        foreach($path in @('src/obs_native_output.h','src/obs_native_output.cpp','src/shaders/nr_obs_analysis.hlsl',
            'tools/host/nr_slot_scenario.h','tools/host/nr_slot_scenario.cpp','tools/host/nr_bootstrap_scenario.h',
            'tools/host/nr_bootstrap_scenario.cpp','tools/host/nr_bootstrap_images.h','tools/host/nr_bootstrap_images.cpp',
            'tools/host/nr_recording_encode.h','tools/host/nr_recording_encode.cpp','tools/host/nr_recording_decode.h',
            'tools/host/nr_recording_decode.cpp','tools/tests/slot_reservation_tests.cpp','tools/tests/bootstrap_image_tests.cpp',
            'tools/tests/bootstrap_runner_tests.ps1')) { $binding+=FileIdentity (Join-Path $work $path) }
    }
    if($Scenario -eq 'lifecycle' -or $CoreInitControl) {
        foreach($path in @('src/nr_lifecycle.h','src/nr_lifecycle.cpp','cmake/nr/Lifecycle.cmake',
            'tools/tests/lifecycle_contract_tests.cpp','tools/tests/native_output_lifetime_tests.cpp')){$binding+=FileIdentity (Join-Path $work $path)}
    }
    if($Scenario -in @('ghost-calibration','ghost-baseline')) {
        foreach($path in @('tools/host/nr_ghost_scenario.h','tools/host/nr_ghost_scenario.cpp',
            'tools/host/nr_ghost_digest.h','tools/host/nr_ghost_digest.cpp','tools/host/GhostDigestSupport.ps1',
            'tools/host/nr_ghost_chunk.h','tools/host/nr_ghost_chunk.cpp','tools/host/nr_ghost_chunk.hlsl',
            'tools/host/nr_ghost_storage.h','tools/host/nr_ghost_receipt.h','CMakeLists.txt',
            'src/ghost_fixture.h','src/ghost_fixture.cpp','src/ghost_oracle.h','src/ghost_oracle.cpp',
            'cmake/nr/Ghost.cmake','tools/tests/ghost_production_tests.cpp',
            '.omo/evidence/obs-nr-native-filter-20260930/task-26/manifests/cpu-fixtures.json')) { $binding+=FileIdentity (Join-Path $work $path) }
    }
    if($Scenario -eq 'ghost-baseline') {
        foreach($path in Get-GhostBaselineBindingPaths) { $binding+=FileIdentity (Join-Path $work $path) }
        $binding+=FileIdentity $ghostAccepted.calibration_path
        $binding+=FileIdentity $ghostAccepted.controls_path
    }
    New-Item -ItemType Directory $root | Out-Null
    $created = $true
    if ($Scenario -eq 'flow-translation') {
        foreach ($path in @('src/nr_guides.h','src/nr_guides.cpp','src/shaders/nr_guides.hlsl','cmake/nr/Guides.cmake','tools/host/nr_flow_scenario.h','tools/host/nr_flow_scenario.cpp','tools/tests/flow_contract_tests.cpp','src/interop.h','cmake/nr/Interop.cmake','deps/nvofa/nvOpticalFlowCommon.h','deps/nvofa/nvOpticalFlowD3D12.h')) { $binding += FileIdentity (Join-Path $work $path) }
        $binding += FileIdentity (Join-Path (Split-Path $build) 'nr_guides.cso')
    }
    Stage-Host
    $nonce = [Guid]::NewGuid().ToString('N')
    if($Scenario -eq 'native-bootstrap') {
        $artifactRoot=Join-Path $Evidence ('bootstrap-artifacts/attempt-'+$nonce)
        New-Item -ItemType Directory -Path $artifactRoot | Out-Null
    }
    if($Scenario -in @('ghost-calibration','ghost-baseline')) {
        $artifactRoot=Join-Path $Evidence ('ghost-artifacts/attempt-'+$nonce)
        New-Item -ItemType Directory -Path $artifactRoot | Out-Null
    }
    $names = @('ready','action','acted','stop','exit') | ForEach-Object { 'Local\nr-qa-' + $nonce + '-' + $_ }
    foreach ($name in $names) { $events += [Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,$name) }
    $retainedEvents=@(New-RetainedHostEvents $Scenario ([bool]$CoreInitControl) $names[3])
    $process = [Diagnostics.Process]::new()
    $originalOwner=@{process=$process;handle=$null;started=$false;closed=$false;
        events=$events;retainedEvents=$retainedEvents;exitSignal=$null;nativeOutput=$null;
        stdout=$null;stderr=$null;outputStarted=$false;errorStarted=$false;
        kernelWait=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset);
        kernelAttached=$false;unknownGate=[Threading.ManualResetEvent]::new($false);
        primaryError=$null;secondaryErrors=@();state='original-owner-published'}
    $script:NrQaOriginalOwner=$originalOwner
    $process.StartInfo.FileName = Join-Path $root 'bin/64bit/nr-qa-host.exe'
    $process.StartInfo.WorkingDirectory = Join-Path $root 'bin/64bit'
    $process.StartInfo.UseShellExecute = $false
    $process.StartInfo.CreateNoWindow = $true
    $process.StartInfo.RedirectStandardOutput = $true
    $process.StartInfo.RedirectStandardError = $true

    $process.StartInfo.Arguments = '"' + $root + '" ' + (($names | ForEach-Object { '"' + $_ + '"' }) -join ' ')
    if ($Scenario -eq 'sampler') { $process.StartInfo.Arguments += ' sampler' }
    if ($Scenario -eq 'texture-roundtrip') { $process.StartInfo.Arguments += ' texture-roundtrip' }
    if ($Scenario -eq 'flow-translation') { $process.StartInfo.Arguments += ' flow-translation' }
    if ($Scenario -eq 'color-alpha') { $process.StartInfo.Arguments += ' color-alpha' }
    if ($Scenario -eq 'native-model') { $process.StartInfo.Arguments += $(if($CoreInitControl){' core-init-control'}else{' native-model'}) }
    if ($Scenario -eq 'lifecycle') { $process.StartInfo.Arguments += $(if($LifecycleNative){' lifecycle-native'}else{' lifecycle'}) }
    if ($Scenario -eq 'native-bootstrap') { $process.StartInfo.Arguments += ' native-bootstrap "'+$artifactRoot+'"' }
    if ($Scenario -eq 'ghost-calibration') { $process.StartInfo.Arguments += ' ghost-calibration "'+$artifactRoot+'"' }
    if ($Scenario -eq 'ghost-baseline') {
        $process.StartInfo.Arguments += ' ghost-baseline "'+$artifactRoot+'"'
        $process.StartInfo.EnvironmentVariables['NR_GHOST_ACCEPTED_CALIBRATION']=$ghostAccepted.json
    }
    $process.EnableRaisingEvents = $true
    $exitSignal = [NrProcessExit]::new($process)
    $originalOwner.exitSignal=$exitSignal
    if ($Scenario -in @('native-model','native-bootstrap','ghost-calibration','ghost-baseline','lifecycle')) {
        $nativeLog=Join-Path $Evidence ('attempt-'+$nonce+'-native-host.log')
        $nativeOutput=[NrHostOutput]::new($process,$nativeLog)
        $originalOwner.nativeOutput=$nativeOutput
    }
    $command = $process.StartInfo.FileName + ' ' + $process.StartInfo.Arguments
    $arguments = $process.StartInfo.Arguments
    Assert-NoUnrelatedProcess
    if ($Scenario -in @('native-model','native-bootstrap','ghost-calibration','ghost-baseline','lifecycle')) {
        @{state='registered';registered=[DateTime]::UtcNow.ToString('o');command=$process.StartInfo.FileName;
            args=$arguments;root=$root;raw_log=$nativeLog;binding=$binding;runtime=$spec.runtime;
            subscription_before_start=$true;identity=$null} | ConvertTo-Json -Depth 12 |
            Set-Content -Encoding UTF8 (Join-Path $Evidence ('attempt-'+$nonce+'-native-registered.json'))
    }
    $originalOwner.started=$null
    if (!$process.Start()) { $originalOwner.started=$false; throw 'NR_QA_START_FAILED' }
    $originalOwner.started=$true
    $originalOwner.handle=$process.SafeHandle
    $originalOwner.kernelWait.SafeWaitHandle=[Microsoft.Win32.SafeHandles.SafeWaitHandle]::new($originalOwner.handle.DangerousGetHandle(),$false)
    $originalOwner.kernelAttached=$true
    $originalOwner.outputStarted=$null
    if($nativeOutput){$process.BeginOutputReadLine()}else{$stdout=$process.StandardOutput.ReadToEndAsync();$originalOwner.stdout=$stdout}
    $originalOwner.outputStarted=$true
    $originalOwner.errorStarted=$null
    if($nativeOutput){$process.BeginErrorReadLine()}else{$stderr=$process.StandardError.ReadToEndAsync();$originalOwner.stderr=$stderr}
    $originalOwner.errorStarted=$true
    $identity = Identity $process
    if ($Scenario -in @('native-model','native-bootstrap','ghost-calibration','ghost-baseline','lifecycle')) {
        @{state='started';identity=$identity;command=$process.StartInfo.FileName;args=$arguments;
            raw_log=$nativeLog;registered_receipt=('attempt-'+$nonce+'-native-registered.json')} |
            ConvertTo-Json -Depth 8 | Set-Content -Encoding UTF8 (Join-Path $Evidence ('attempt-'+$nonce+'-native-start.json'))
    }
    if ($identity.image -ne $process.StartInfo.FileName) { throw 'NR_QA_CHILD_IDENTITY_REFUSED' }
    $ready = [Threading.WaitHandle]::WaitAny([Threading.WaitHandle[]]@($events[0],$exitSignal.Exited),15000)
    if ($ready -eq 1) { throw ('NR_QA_EARLY_EXIT: ' + $process.ExitCode) }
    if ($ready -ne 0) { throw 'NR_QA_READY_TIMEOUT' }
    $inventory = @($process.Modules | ForEach-Object { @{path=$_.FileName;sha256=(Get-FileHash $_.FileName -Algorithm SHA256).Hash} })
    Assert-CleanInventory $inventory
    Assert-ReadyHostPair $inventory $root $build $Scenario ([bool]$CoreInitControl)
    if($Scenario -in @('native-bootstrap','ghost-calibration','ghost-baseline','lifecycle') -or $CoreInitControl) {
        foreach($module in $(if($Scenario -eq 'lifecycle' -or $CoreInitControl){@()}else{@('obs-nvenc.dll','obs-ffmpeg.dll')})) {
            if(!@($inventory | Where-Object {[IO.Path]::GetFileName($_.path) -eq $module}).Count) { throw ('NR_QA_MODULE_MISSING: '+$module) }
        }
    }
    $readyInventory = $inventory
    if($CoreInitControl){Write-Output ('NR_QA_READY_MODULES '+($readyInventory|ConvertTo-Json -Depth 8 -Compress))}
    $events[1].Set() | Out-Null
    $actionBound = if ($Scenario -in @('ghost-calibration','ghost-baseline')) { 10800000 } elseif ($Scenario -eq 'native-bootstrap' -or ($Scenario -eq 'lifecycle' -and $LifecycleNative)) { 300000 } elseif ($Scenario -in @('flow-translation','color-alpha','native-model','lifecycle')) { 120000 } else { 15000 }
    $acted = [Threading.WaitHandle]::WaitAny([Threading.WaitHandle[]]@($events[2],$exitSignal.Exited),$actionBound)
    if ($acted -eq 1) { throw ('NR_QA_ACTION_FAILED_EXIT: ' + $process.ExitCode) }
    if ($acted -ne 0) { throw 'NR_QA_ACTION_TIMEOUT' }
    # Process.Modules is cached by .NET; the sampler ends D3D11 before its OpenGL refusal lane.
    $process.Refresh()
    $inventory = @($process.Modules | ForEach-Object { @{path=$_.FileName;sha256=(Get-FileHash $_.FileName -Algorithm SHA256).Hash} })
    if ($Scenario -eq 'sampler') { Assert-CleanInventory $inventory 'libobs-opengl.dll' } else { Assert-CleanInventory $inventory }
    $events[3].Set() | Out-Null
    if (!$events[4].WaitOne(15000)) { throw 'NR_QA_EXIT_TIMEOUT' }
    if (!$process.WaitForExit(15000)) { throw 'NR_QA_PROCESS_EXIT_TIMEOUT' }
    $exit = $process.ExitCode
    if ($exit) { throw ('NR_QA_HOST_FAILED: ' + $exit) }
    if ($nativeOutput) {
        if (!$nativeOutput.OutputClosed.WaitOne(15000) -or !$nativeOutput.ErrorClosed.WaitOne(15000)) { throw 'NR_QA_MODEL_OUTPUT_DRAIN' }
        $output=$nativeOutput.Text
    } else { $output = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult() }
    foreach ($token in @('NR_QA_READY libobs=32.2.2 module=obs-nr-filter encoder=obs_x264','NR_QA_ACTION content_id=2583','NR_QA_EXIT cleanup=complete result=0')) {
        if (!$output.Contains($token)) { throw ('NR_QA_EVENT_LITERAL_MISSING: ' + $token) }
    }
    if ($Scenario -eq 'sampler') {
        foreach ($line in ($output -split '\r?\n')) {
            if ($line.StartsWith('NR_SAMPLER_VIEW ')) { $views += $line.Substring(16) | ConvertFrom-Json }
        }
        if ($views.Count -ne 43) { throw ('NR_QA_SAMPLER_VIEW_COUNT: ' + $views.Count) }
        foreach ($row in $views) {
            if ($row.pixel_errors -ne 0 -or !$row.state_restored -or $row.tick -ne ([Math]::Floor($row.clock / $row.interval) + 1)) { throw 'NR_QA_SAMPLER_PIXEL_OR_CLOCK' }
        }
        foreach ($step in 0..3) {
            $group = @($views | Where-Object { $_.step -eq $step })
            if ($group.Count -ne 6 -or @($group.view | Select-Object -Unique).Count -ne 3 -or @($group.tick | Select-Object -Unique).Count -ne 1 -or @($group.ingress | Select-Object -Unique).Count -ne 1) { throw 'NR_QA_SAMPLER_LATCH' }
            foreach ($row in $group) { if ($row.ingress -ne ($step + 1) -or $row.selected -ne $row.ingress -or $row.admission -ne 0 -or $row.source_render_count -ne ($step + 1)) { throw 'NR_QA_SAMPLER_KEY' } }
        }
        foreach ($step in 4..7) {
            $group = @($views | Where-Object { $_.step -eq $step })
            $expected = @(2,4,1,3)[$step - 4]
            $count = @(6,6,1,6)[$step - 4]
            if ($group.Count -ne $count -or @($group.tick | Select-Object -Unique).Count -ne 1 -or @($group.ingress | Select-Object -Unique).Count -ne 1) { throw 'NR_QA_SAMPLER_BYPASS_LATCH' }
            if (@($group | Where-Object { $_.admission -ne $expected -or $_.selected -ne 0 }).Count) { throw 'NR_QA_SAMPLER_BYPASS' }
        }
    }
    if ($Scenario -eq 'texture-roundtrip') {
        $prefix = 'NR_INTEROP_PATTERN '
        foreach ($line in ($output -split [char]10)) {
            if ($line.StartsWith($prefix)) { $patterns += $line.Substring($prefix.Length) | ConvertFrom-Json }
        }
        if ($patterns.Count -ne 4 -or (@($patterns.format | Sort-Object) -join ',') -ne 'BGRA8,RGB10A2,RGBA16F,RGBA8') { throw 'NR_QA_INTEROP_PATTERN_SET' }
        foreach ($row in $patterns) {
            if ($row.raw_errors -ne 0 -or $row.output_errors -ne 0 -or !$row.state_restored -or !$row.cleanup -or $row.progress_clock -le $row.capture_clock -or $row.raw_fnv1a64 -ne $row.expected_fnv1a64 -or $row.output_fnv1a64 -ne $row.expected_fnv1a64 -or $row.producer_fence_completed -ne 4 -or $row.worker_fence_completed -ne 3 -or $row.consumer_fence_completed -ne 6) { throw 'NR_QA_INTEROP_PATTERN_OR_FENCE' }
        }
        if (!$output.Contains('NR_INTEROP_SCOPE transport=physical same_adapter=true model=not_loaded recording=not_tested HDR=transport_only cleanup=complete') -or $output.Contains('NR_INTEROP_RETAINED')) { throw 'NR_QA_INTEROP_CLEANUP_INCOMPLETE' }
    }
    if ($Scenario -eq 'flow-translation') {
        foreach ($line in ($output -split [char]10)) {
            if ($line.StartsWith('NR_FLOW_PATTERN ')) { $flowRows += $line.Substring(16) | ConvertFrom-Json }
            if ($line.StartsWith('NR_FLOW_CAPABILITY ')) { $flowCaps += $line.Substring(19) | ConvertFrom-Json }
            if ($line.StartsWith('NR_FLOW_REESTIMATION ')) { $flowPasses += $line.Substring(21) | ConvertFrom-Json }
            if ($line.StartsWith('NR_FLOW_INPUT_PROOF ')) { $flowInputs += $line.Substring(20) | ConvertFrom-Json }
        }
        Assert-FlowRows $flowRows $flowCaps
        Assert-FlowPasses $flowPasses $flowInputs $flowRows $flowCaps
        foreach ($cap in $flowCaps) {
            $expectedModule=Join-Path $env:SystemRoot 'System32/nvofapi64.dll'
            if ([IO.Path]::GetFullPath($cap.module_path) -ne [IO.Path]::GetFullPath($expectedModule) -or (Get-FileHash -LiteralPath $expectedModule -Algorithm SHA256).Hash.ToLowerInvariant() -ne $cap.module_sha256) { throw 'NR_QA_FLOW_MODULE_IDENTITY' }
        }
        if (!$output.Contains('NR_FLOW_SCOPE physical_ofa=true model=not_loaded Feature18=not_admitted HDR=not_claimed recording=not_tested cleanup=complete') -or $output.Contains('NR_FLOW_RETAINED')) { throw 'NR_QA_FLOW_CLEANUP_INCOMPLETE' }
    }
    if ($Scenario -eq 'color-alpha') {
        foreach ($line in ($output -split [char]10)) {
            if ($line.StartsWith('NR_COLOR_CASE ')) { $colorRows += $line.Substring(14) | ConvertFrom-Json }
            if ($line.StartsWith('NR_COLOR_DISPATCH ')) { $colorDispatches += $line.Substring(18) | ConvertFrom-Json }
            if ($line.StartsWith('NR_COLOR_CONTEXT ')) { $colorContext += $line.Substring(17) | ConvertFrom-Json }
            if ($line.StartsWith('NR_COLOR_CONTROL ')) { $colorControls += $line.Substring(17) | ConvertFrom-Json }
        }
        Assert-ColorRows $colorRows $colorDispatches $colorContext $colorControls
        if (!$output.Contains('NR_COLOR_SCOPE physical_color=true model=not_loaded Feature18=not_admitted HDR=boundary_only recording=not_tested cleanup=complete') -or $output.Contains('NR_COLOR_RETAINED')) { throw 'NR_QA_COLOR_CLEANUP_INCOMPLETE' }
    }
    if($CoreInitControl) {
        Assert-CoreControl @(Read-CoreControlRows $output) @($output -split [char]10|Where-Object {$_.StartsWith('NR_CORE_CONTROL_CAPTURE ')}|ForEach-Object {$_.Substring(24)|ConvertFrom-Json}) @(Read-CoreControlContext $output)
    } elseif ($Scenario -eq 'native-model') {
        foreach ($line in ($output -split [char]10)) {
            if ($line.StartsWith('NR_MODEL_CASE ')) { $modelRows += $line.Substring(14) | ConvertFrom-Json }
            if ($line.StartsWith('NR_MODEL_CONTEXT ')) { $modelContexts += $line.Substring(17) | ConvertFrom-Json }
            if ($line.StartsWith('NR_MODEL_CLEANUP ')) { $modelCleanup += $line.Substring(17) | ConvertFrom-Json }
            if ($line.StartsWith('NR_MODEL_MODULE ')) { $modelModules += $line.Substring(16) | ConvertFrom-Json }
        }
        Assert-ModelRows $modelRows $modelContexts $modelCleanup $modelModules
        foreach ($module in $modelModules) {
            if ([IO.Path]::GetFullPath($module.path) -ne [IO.Path]::GetFullPath($Runtime) -or
                $module.selected_sha256 -ne $spec.runtime.sha256 -or [UInt64]$module.base -eq 0) { throw 'NR_QA_MODEL_MODULE_IDENTITY' }
        }
        if (!$output.Contains('NR_MODEL_SCOPE Feature18=true integrated=true quality=false performance=false recording=false cleanup=complete') -or
            $output.Contains('NR_MODEL_RETAINED')) { throw 'NR_QA_MODEL_CLEANUP_INCOMPLETE' }
    }
    if($Scenario -eq 'native-bootstrap') {
        foreach($line in ($output -split [char]10)) {
            if($line.StartsWith('NR_BOOTSTRAP_FRAME ')){$bootstrapFrames+=$line.Substring(19)|ConvertFrom-Json}
            if($line.StartsWith('NR_BOOTSTRAP_LANE ')){$bootstrapLanes+=$line.Substring(18)|ConvertFrom-Json}
            if($line.StartsWith('NR_BOOTSTRAP_DECODED ')){$bootstrapDecoded+=$line.Substring(21)|ConvertFrom-Json}
            if($line.StartsWith('NR_BOOTSTRAP_CLEANUP ')){$bootstrapCleanup+=$line.Substring(21)|ConvertFrom-Json}
            if($line.StartsWith('NR_BOOTSTRAP_SLOT ')){$bootstrapSlots+=$line.Substring(18)|ConvertFrom-Json}
            if($line.StartsWith('NR_BOOTSTRAP_SLOT_CLEANUP ')){$bootstrapSlotCleanup+=$line.Substring(26)|ConvertFrom-Json}
        }
        Assert-BootstrapRows $bootstrapFrames $bootstrapLanes $bootstrapDecoded $bootstrapCleanup $bootstrapSlots $bootstrapSlotCleanup
        foreach($row in @($bootstrapFrames | Where-Object {$_.genuine_nr})) {
            if([IO.Path]::GetFullPath($row.module_path) -ne [IO.Path]::GetFullPath($Runtime)) { throw 'NR_QA_MODEL_MODULE_IDENTITY' }
        }
        foreach($lane in $bootstrapLanes) {
            $directory=Join-Path $artifactRoot ('lane-'+$lane.lane)
            if([IO.Path]::GetFullPath($lane.artifacts) -ne [IO.Path]::GetFullPath($directory)) { throw 'NR_QA_BOOTSTRAP_ARTIFACT_PATH' }
            foreach($name in @('source.bmp','filter.bmp','program.bmp','decoded.bmp','program.hevc')) {
                $path=Join-Path $directory $name
                if(!(Test-Path -LiteralPath $path) -or (Get-Item -LiteralPath $path).Length -le 0) { throw ('NR_QA_BOOTSTRAP_IMAGE_MISSING: '+$name) }
                $bootstrapArtifacts+=FileIdentity $path
            }
        }
        if(!$output.Contains('NR_BOOTSTRAP_SCOPE real_filter=true Feature18=true SDR=true HDR=true recording=true independently_decoded=true quality=false performance=false cleanup=complete') -or
            $output.Contains('NR_BOOTSTRAP_RETAINED') -or $output.Contains('NR_BOOTSTRAP_FAILURE')) { throw 'NR_QA_BOOTSTRAP_INCOMPLETE' }
    }
    if($Scenario -eq 'ghost-calibration') {
        $ghostValidation=Assert-GhostPublic $output $artifactRoot $Evidence
    }
    if($Scenario -eq 'ghost-baseline') {
        $ghostBaseline=Assert-GhostBaselinePublic $output $artifactRoot $GhostAcceptedCalibrationSha256 $GhostAcceptedControlsSha256
    }
    if($Scenario -eq 'lifecycle') {
        foreach($line in ($output -split [char]10)) {
            if($line.StartsWith('NR_LIFECYCLE_ROW ')){$lifecycleRows+=$line.Substring(17)|ConvertFrom-Json}
            if($line.StartsWith('NR_LIFECYCLE_CLEANUP ')){$lifecycleCleanup+=$line.Substring(21)|ConvertFrom-Json}
            if($line.StartsWith('NR_LIFECYCLE_WITNESS ')){$lifecycleWitness+=$line.Substring(21)|ConvertFrom-Json}
        }
        Assert-LifecycleRows $lifecycleRows $lifecycleCleanup -Native:$LifecycleNative -Witness $lifecycleWitness
        Assert-LifecycleClosure @(Read-LifecycleClosure $output)
        foreach($witness in $lifecycleWitness) {
            if([IO.Path]::GetFullPath($witness.module_path) -ne [IO.Path]::GetFullPath($Runtime)){
                throw 'NR_QA_LIFECYCLE_MODULE_BINDING'
            }
        }
    }
    $result = 'PASS'
} catch {
    $failure = $_.Exception.Message
    if($originalOwner){$originalOwner.primaryError=$_}
} finally {
    if ($process -and $identity -and !$process.HasExited) {
        if ((Identity $process | ConvertTo-Json -Compress) -ne ($identity | ConvertTo-Json -Compress)) { throw 'NR_QA_CLEANUP_IDENTITY_REFUSED' }
        if ($events.Count -eq 5) { $events[3].Set() | Out-Null }
        if (!$process.WaitForExit(15000)) {
            if ($Scenario -in @('texture-roundtrip','flow-translation','color-alpha','native-model','native-bootstrap','ghost-calibration','ghost-baseline','lifecycle')) { $physicalUnresolved=$true; $result='FAIL'; $failure='NR_QA_GPU_OWNERSHIP_RETAINED' }
            else { $process.Kill(); if (!$process.WaitForExit(15000)) { throw 'NR_QA_CLEANUP_BLOCKED' } }
        }
    }
    if($originalOwner -and $originalOwner.started -ne $false -and !(Test-NrQaOriginalOwnerClosed $originalOwner)) {
        $physicalUnresolved=$true;$result='FAIL'
        if(!$failure){$failure='NR_QA_ORIGINAL_OWNERSHIP_RETAINED'}
    }
    if (!$physicalUnresolved) {
        foreach ($event in $events) { $event.Dispose() }
        foreach ($event in $retainedEvents) { $event.Dispose() }
        if ($exitSignal) { $exitSignal.Dispose() }
    }
    if($physicalUnresolved) {
        if($identity -and ($Scenario -eq 'lifecycle' -or $CoreInitControl)) {
            Wait-RetainedHost $process $identity @($events[0],$retainedEvents[0],$retainedEvents[1]) $exitSignal $nativeOutput ($names[3]+'.diagnose') (Join-Path $Evidence ('attempt-'+$nonce+'-supervisor.json'))
        }
        Wait-NrQaOriginalOwner $originalOwner
    }
    foreach ($source in $sources) {
        $after = FileIdentity $source.path
        $sourceAfter += $after
        if ($after.sha256 -ne $source.sha256 -or $after.file_id -ne $source.file_id -or $after.attributes -ne $source.attributes) { $result='FAIL';$failure='NR_QA_SOURCE_CHANGED' }
    }
    if ($created -and !$physicalUnresolved) { Remove-Item -LiteralPath $root -Recurse -Force }
    $cleanup = @{host_absent=!(Test-Path $root);child_exited=(!$process -or $process.HasExited);event_handles_closed=(!$physicalUnresolved);physical_ownership_retained=$physicalUnresolved;linked_bytes_unchanged=($failure -ne 'NR_QA_SOURCE_CHANGED')}
    if ($process -and $process.HasExited -and $stdout) { $output = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult() }
    if ($nativeOutput) { $output=$nativeOutput.Text }
    if ($nativeOutput -and $nativeOutput.SinkFailed) {$result='FAIL';$failure='NR_QA_OUTPUT_SINK_FAILURE: '+$nativeOutput.SinkFailure}
    if ($process -and $process.HasExited) { $exit = $process.ExitCode }
    $receipt = if ($Scenario -eq 'sampler') { 'sampler.json' } elseif ($Scenario -eq 'texture-roundtrip') { 'interop.json' } else { 'host-foundation.json' }
    $scope = if ($Scenario -eq 'texture-roundtrip') { @{class='physical-D3D11-D3D12-transport';NR=$false;FG=$false;recording=$false;HDR=$false;HDR_transport=$true;performance=$false;physical_HDMI_PTS=$false} } else { @{class='native-pass-through-sampler';NR=$false;FG=$false;recording=$false;HDR=$false;performance=$false;physical_HDMI_PTS=$false;unsupported_renderer='actual signed libobs OpenGL bypass lane'} }
    $record = @{scenario=$Scenario;result=$result;failure=$failure;command=$command;args=$arguments;public_command=('powershell.exe -NoProfile -ExecutionPolicy Bypass -File "' + $PSCommandPath + '" -Scenario ' + $Scenario + ' -Evidence "' + $Evidence + '" -HostRoot "' + $HostRoot + '" -Runtime "' + $Runtime + '"');public_args=@('-NoProfile','-ExecutionPolicy','Bypass','-File',$PSCommandPath,'-Scenario',$Scenario,'-Evidence',$Evidence,'-HostRoot',$HostRoot,'-Runtime',$Runtime);exit=$exit;identity=$identity;events=@('ready','action','exit');output=$output;views=$views;patterns=$patterns;binding=$binding;inventory_ready=$readyInventory;inventory=$inventory;sources_before=@($sources.ToArray());sources_after=$sourceAfter;staging='minimal-private-copy';runtime=@{path=$Runtime;sha256=$spec.runtime.sha256;loaded=$false};scope=$scope;assertions=@{result_pass=($result -eq 'PASS');exact_pattern_rows=$patterns.Count};cleanup=$cleanup}
    if($Scenario -eq 'lifecycle' -and $LifecycleNative) {
        $record.public_command+=' -LifecycleNative'
        $record.public_args+='-LifecycleNative'
    }
    if($LifecycleDiagnosticR4) {
        $record.public_command+=' -LifecycleDiagnosticR4';$record.public_args+='-LifecycleDiagnosticR4'
        $record.diagnostic_r4=@{manifest=$diagnosticManifest;sha256=$diagnosticManifestSha256;artifacts_bound=12;entry_only=$true;Task13_acceptance=$false}
    }
    if($originalOwner -and $originalOwner.primaryError){$record.primary_error=$originalOwner.primaryError.Exception.Message}
    if($nativeOutput){$record.output_sink_failure=$nativeOutput.SinkFailure}
    $json = $record | ConvertTo-Json -Depth 12
    if ($Scenario -eq 'flow-translation') {
        $receipt='flow.json'
        $record.flow_rows=$flowRows; $record.capabilities=$flowCaps
        $record.native_passes=$flowPasses; $record.input_proofs=$flowInputs
        $record.scope=@{class='physical-NVOFA-D3D12-guides';NR=$false;Feature18=$false;recording=$false;HDR=$false;quality=$false;performance=$false}
        $record.assertions.exact_flow_rows=$flowRows.Count
        $record.assertions.exact_native_pass_rows=$flowPasses.Count
        $record.assertions.exact_input_proofs=$flowInputs.Count
        $json=$record | ConvertTo-Json -Depth 12
    }
    if ($Scenario -eq 'color-alpha') {
        $receipt='color-alpha.json'
        $record.color_rows=$colorRows; $record.color_dispatches=$colorDispatches
        $record.color_context=$colorContext; $record.color_controls=$colorControls
        $record.scope=@{class='physical-D3D12-color-boundary';NR=$false;Feature18=$false;recording=$false;HDR=$false;HDR_boundary=$true;quality=$false;performance=$false}
        $record.assertions.exact_color_rows=$colorRows.Count
        $record.assertions.exact_dispatch_rows=$colorDispatches.Count
        $json=$record | ConvertTo-Json -Depth 12
    }
    if ($CoreInitControl) {
        $receipt='core-control.json'
        try{$record.core_control=@(Read-CoreControlRows $output)}catch{$record.core_control_parse_failure=$_.Exception.Message;$record.result='FAIL'}
        try{$record.core_control_context=@(Read-CoreControlContext $output)}catch{$record.core_control_context_failure=$_.Exception.Message;$record.result='FAIL'}
        $record.native_raw_log=$nativeLog
        $record.scope=@{class='physical-CORE-init-only-control';NR=$false;Feature18=$false;normal_closed=(!$physicalUnresolved);inference=$false}
        if ($nativeOutput -and !$physicalUnresolved) { $nativeOutput.Dispose() }
        $json=$record|ConvertTo-Json -Depth 20
    } elseif ($Scenario -eq 'native-model') {
        $receipt='native-model.json'
        $record.model_rows=$modelRows; $record.model_contexts=$modelContexts
        $record.model_cleanup=$modelCleanup; $record.model_modules=$modelModules
        $record.runtime.loaded_during_inference=($modelModules.Count -gt 0)
        $record.runtime.loaded_after_cleanup=$false
        $record.native_raw_log=$nativeLog
        if ($nativeOutput -and !$physicalUnresolved) { $nativeOutput.Dispose() }
        if ($nativeLog -and (Test-Path $nativeLog)) { $record.native_raw_log_identity=FileIdentity $nativeLog }
        $record.scope=@{class='physical-joined-Feature18-processor';NR=$true;Feature18=$true;recording=$false;HDR=$false;quality=$false;performance=$false;fixture_fps=1}
        $record.assertions.exact_model_rows=$modelRows.Count
        $json=$record | ConvertTo-Json -Depth 12
    }
    if($Scenario -eq 'native-bootstrap') {
        $receipt='bootstrap.json'
        $record.bootstrap_frames=$bootstrapFrames;$record.bootstrap_lanes=$bootstrapLanes
        $record.bootstrap_decoded=$bootstrapDecoded;$record.bootstrap_cleanup=$bootstrapCleanup
        $record.bootstrap_slots=$bootstrapSlots;$record.bootstrap_slot_cleanup=$bootstrapSlotCleanup
        $record.bootstrap_artifacts=$bootstrapArtifacts;$record.artifact_root=$artifactRoot
        $record.runtime.loaded_during_inference=@($bootstrapFrames | Where-Object {
            $_ -and $_.PSObject.Properties['genuine_nr'] -and $_.genuine_nr
        }).Count -gt 0
        $record.runtime.loaded_after_cleanup=$(if($physicalUnresolved){$null}else{$false});$record.native_raw_log=$nativeLog
        if($nativeOutput -and !$physicalUnresolved){$nativeOutput.Dispose()}
        if($nativeLog -and !$physicalUnresolved -and (Test-Path $nativeLog)){$record.native_raw_log_identity=FileIdentity $nativeLog}
        $record.scope=@{class='physical-native-filter-program-HEVC-independent-decoder';NR=$true;Feature18=$true;
            recording=$true;SDR=$true;HDR=$true;quality=$false;performance=$false;fixture_fps=1;
            OBS_chrome='Owned headless libobs has no GUI chrome; source-scoped filter and unrelated program source are checked'}
        $json=$record|ConvertTo-Json -Depth 14
    }
    if($Scenario -eq 'ghost-calibration') {
        $receipt='ghost-calibration.json'
        $record.artifact_root=$artifactRoot
        $record.native_raw_log=$nativeLog
        $record.runtime.loaded_during_inference=$null
        $record.runtime.loaded_after_cleanup=$null
        if($nativeOutput -and !$physicalUnresolved){$nativeOutput.Dispose()}
        if($nativeLog -and !$physicalUnresolved -and (Test-Path $nativeLog)){$record.native_raw_log_identity=FileIdentity $nativeLog}
        $record.scope=@{class='physical-ghost-calibration';physical_calibration_accepted=($result -eq 'PASS');
            NR=$false;Feature18=$false;recording=$false;HDR=$false;quality=$false;performance=$false}
        $record.historical_validation_gap='NR_QA_GHOST_VALIDATION_PENDING was the preexisting unimplemented validator; historical failure receipts remain immutable.'
        if($result -eq 'PASS'){$record.ghost_validation=$ghostValidation}
        $json=$record|ConvertTo-Json -Depth 14
    }
    if($Scenario -eq 'ghost-baseline') {
        $receipt='ghost-baseline.json'
        $record.artifact_root=$artifactRoot
        $record.native_raw_log=$nativeLog
        $record.runtime.loaded_during_inference=$null
        $record.runtime.loaded_after_cleanup=$null
        if($nativeOutput -and !$physicalUnresolved){$nativeOutput.Dispose()}
        if($nativeLog -and !$physicalUnresolved -and (Test-Path $nativeLog)){$record.native_raw_log_identity=FileIdentity $nativeLog}
        $record.accepted_calibration=@{calibration_sha256=$GhostAcceptedCalibrationSha256;controls_sha256=$GhostAcceptedControlsSha256}
        $record.scope=Get-GhostBaselineScope ($result -eq 'PASS')
        if($result -eq 'PASS'){$record.ghost_baseline=$ghostBaseline;$record.DEFECTS=@($ghostBaseline.DEFECTS)}
        else{$record.ghost_baseline_partial=Read-GhostBaselineSummary $artifactRoot}
        $json=$record|ConvertTo-Json -Depth 14
    }
    if($Scenario -eq 'lifecycle') {
        $receipt='lifecycle-transport.json'
        # Preserve real witnesses even when an early host failure prevents the
        # success-only validator from running. These rows do not turn FAIL green.
        $lifecycleRows=@();$lifecycleCleanup=@();$lifecycleWitness=@();$lifecycleKeys=@();$lifecycleLatches=@()
        foreach($line in ($output -split [char]10)) {
            if($line.StartsWith('NR_LIFECYCLE_ROW ')){$lifecycleRows+=$line.Substring(17)|ConvertFrom-Json}
            if($line.StartsWith('NR_LIFECYCLE_CLEANUP ')){$lifecycleCleanup+=$line.Substring(21)|ConvertFrom-Json}
            if($line.StartsWith('NR_LIFECYCLE_WITNESS ')){$lifecycleWitness+=$line.Substring(21)|ConvertFrom-Json}
            if($line.StartsWith('NR_LIFECYCLE_KEY ')){$lifecycleKeys+=$line.Substring(17)|ConvertFrom-Json}
            if($line.StartsWith('NR_LIFECYCLE_LATCH ')){$lifecycleLatches+=$line.Substring(19)|ConvertFrom-Json}
        }
        $record.lifecycle_rows=$lifecycleRows;$record.lifecycle_cleanup=$lifecycleCleanup
        $record.lifecycle_witness=$lifecycleWitness
        $record.lifecycle_keys=$lifecycleKeys;$record.lifecycle_latches=$lifecycleLatches
        $record.lifecycle_closure=@(Read-LifecycleClosure $output)
        $record.runtime.loaded=@($lifecycleWitness|Where-Object {$_.module_base -gt 0}).Count -gt 0
        $record.validity_semantics=@{source='Task-owned virtual fixture explicit nr_source_lifecycle procedure';
            values='0 Unknown; 1 Valid; 2 Invalid';metadata_present='Procedure supplied a value, even Unknown';
            physical_signal_known='Legacy field: explicit validity value is Valid or Invalid, not HDMI hardware detection';
            physical_HDMI_validity='Unknown in all fixture cases';repeated_pixels_infer_loss=$false}
        $record.native_raw_log=$nativeLog
        $record.scope=@{class='production-lifecycle-transition-transport';NR=$false;Feature18=$false;
            deterministic_transport=$true;physical_acceptance=$false;quality=$false;performance=$false;
            physical_HDMI_validity='Unknown without an explicit source signal'}
        if($LifecycleNative) {
            $receipt='lifecycle-native.json'
            $record.scope=@{class='production-lifecycle-transition-Feature18';NR=($result -eq 'PASS');
                Feature18=($result -eq 'PASS');deterministic_transport=$false;physical_acceptance=$false;
                quality=$false;performance=$false;physical_HDMI_validity='Unknown without an explicit source signal';
                remaining='Device/address reuse, late completion and retirement-budget physical controls must also pass before original Task13 acceptance'}
        }
        $streamsClosed=!$physicalUnresolved -and (!$nativeOutput -or ($nativeOutput.OutputClosed.WaitOne(15000) -and $nativeOutput.ErrorClosed.WaitOne(15000)))
        $record.streams_drained=$streamsClosed
        if($nativeOutput -and !$physicalUnresolved -and $streamsClosed){$nativeOutput.Dispose()}
        if($nativeLog -and !$physicalUnresolved -and $streamsClosed -and (Test-Path $nativeLog)){$record.native_raw_log_identity=FileIdentity $nativeLog}
        if(!$streamsClosed -and !$physicalUnresolved){$result='FAIL';$failure='NR_QA_LIFECYCLE_OUTPUT_DRAIN';$record.result=$result;$record.failure=$failure}
        $json=$record|ConvertTo-Json -Depth 14
    }
    if ($Scenario -in @('texture-roundtrip','flow-translation','color-alpha','native-model','native-bootstrap','ghost-calibration','ghost-baseline','lifecycle')) { $json | Set-Content -Encoding UTF8 (Join-Path $Evidence ('attempt-' + [Guid]::NewGuid().ToString('N') + '-' + $receipt)) }
    $json | Set-Content -Encoding UTF8 (Join-Path $Evidence $receipt)
    if($physicalUnresolved -and $Scenario -eq 'lifecycle') {
        if($nativeOutput.OutputClosed.WaitOne(15000) -and $nativeOutput.ErrorClosed.WaitOne(15000)){$nativeOutput.Dispose()}
        foreach($event in $events){$event.Dispose()}
        foreach($event in $retainedEvents){$event.Dispose()}
        $exitSignal.Dispose();$process.Dispose()
    }
    if ($process -and !$physicalUnresolved) { $process.Dispose() }
}
} catch {
    $result='FAIL'
    if($originalOwner) {
        if(!$originalOwner.primaryError){$originalOwner.primaryError=$_}
        else{$originalOwner.secondaryErrors+=$_}
        $failure=$originalOwner.primaryError.Exception.Message
    } else {$failure=$_.Exception.Message}
} finally {
    if($originalOwner) {
        if(!$originalOwner.closed -and $originalOwner.started -ne $false){Wait-NrQaOriginalOwner $originalOwner}
        if($originalOwner.closed -or $originalOwner.started -eq $false) {
            $originalOwner.kernelWait.Dispose();$originalOwner.unknownGate.Dispose()
        }
    }
}
if ($result -ne 'PASS' -and $originalOwner -and $originalOwner.primaryError) { throw $originalOwner.primaryError }
if ($result -ne 'PASS') { throw $failure }
