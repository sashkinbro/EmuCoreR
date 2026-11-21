param(
    [Parameter(Mandatory = $true)][string]$Serial,
    [string]$CoreLibrary = '',
    [string]$BiosPath = '',
    [int]$Frames = 120,
    [string]$Sdk = "$env:LOCALAPPDATA/Android/Sdk"
)
$ErrorActionPreference = 'Stop'

$cpuParams = @{ Serial = $Serial; Frames = $Frames; Sdk = $Sdk }
if (-not [string]::IsNullOrWhiteSpace($CoreLibrary)) { $cpuParams['CoreLibrary'] = $CoreLibrary }
if (-not [string]::IsNullOrWhiteSpace($BiosPath)) { $cpuParams['BiosPath'] = $BiosPath }

$gpuParams = @{ Serial = $Serial; Sdk = $Sdk }
if (-not [string]::IsNullOrWhiteSpace($CoreLibrary)) { $gpuParams['CoreLibrary'] = $CoreLibrary }

Write-Output '=========================================='
Write-Output ' Running EmuCoreR ECR CPU Accuracy Oracle '
Write-Output '=========================================='
& (Join-Path $PSScriptRoot 'run_cpu_oracle.ps1') @cpuParams

Write-Output ''
Write-Output '=========================================='
Write-Output ' Running EmuCoreR ECR GPU Accuracy Oracle '
Write-Output '=========================================='
& (Join-Path $PSScriptRoot 'run_gpu_oracle.ps1') @gpuParams

Write-Output ''
Write-Output 'ALL CPU AND GPU ACCURACY ORACLES PASSED.'
