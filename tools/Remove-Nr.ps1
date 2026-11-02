# SPDX-License-Identifier: GPL-2.0-or-later
param([Parameter(Mandatory)][string]$DestRoot)
. (Join-Path $PSScriptRoot 'NrInstallerSupport.ps1')
Invoke-NrInstaller Remove $DestRoot|ConvertTo-Json -Depth 14
