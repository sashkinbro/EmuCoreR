param(
    [Parameter(Mandatory = $true)][string]$Serial,
    [string]$CoreLibrary,
    [string]$BiosPath = '',
    [int]$Frames = 120,
    [string]$Sdk = "$env:LOCALAPPDATA/Android/Sdk"
)
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$adb = Join-Path $Sdk 'platform-tools/adb.exe'
$toolchain = Join-Path $Sdk 'ndk/29.0.14206865/toolchains/llvm/prebuilt/windows-x86_64'
$clang = Join-Path $toolchain 'bin/clang++.exe'
$runtime = Join-Path $toolchain 'sysroot/usr/lib/aarch64-linux-android/libc++_shared.so'
$remoteRoot = '/data/local/tmp/ecr-cpu-oracle-bin'

if ([string]::IsNullOrWhiteSpace($CoreLibrary)) {
    $CoreLibrary = Get-ChildItem -LiteralPath (Join-Path $repoRoot 'app/.cxx/Debug') -Recurse -Filter libecr_android.so |
        Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (!$CoreLibrary -or !(Test-Path -LiteralPath $CoreLibrary)) {
    throw 'Build :app:assembleDebug first so libecr_android.so exists.'
}

Push-Location $repoRoot
try {
    New-Item -ItemType Directory -Force build/oracles | Out-Null
    $executable = 'build/oracles/cpu_oracle'
    & $clang --target=aarch64-none-linux-android26 -std=c++17 -O2 -Wall -Wextra -static-libstdc++ `
        -Icore/ecr/dep/libecr-common/include core/ecr/oracles/cpu_oracle.cpp -ldl -o $executable
    if ($LASTEXITCODE -ne 0) { throw 'CPU oracle compilation failed.' }

    & $adb -s $Serial shell mkdir -p $remoteRoot
    if ($LASTEXITCODE -ne 0) { throw 'Failed preparing device directory for CPU oracle.' }

    foreach ($upload in @(@($CoreLibrary, 'libecr_android.so'), @($runtime, 'libc++_shared.so'), @($executable, 'cpu_oracle'))) {
        & $adb -s $Serial push $upload[0] "$remoteRoot/$($upload[1])" | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Failed uploading $($upload[0])." }
    }
    & $adb -s $Serial shell chmod 755 "$remoteRoot/cpu_oracle"

    $extraArgs = ''
    if (-not [string]::IsNullOrWhiteSpace($BiosPath)) {
        if ($BiosPath.StartsWith('/')) {
            $extraArgs = "--bios `"$BiosPath`" --frames $Frames"
        } elseif (Test-Path -LiteralPath $BiosPath) {
            & $adb -s $Serial push $BiosPath "$remoteRoot/real_bios.bin" | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "Failed uploading BIOS $BiosPath" }
            $extraArgs = "--bios `"$remoteRoot/real_bios.bin`" --frames $Frames"
        } else {
            throw "BIOS path not found: $BiosPath"
        }
    }

    & $adb -s $Serial shell "LD_LIBRARY_PATH=$remoteRoot timeout 60 $remoteRoot/cpu_oracle $remoteRoot/libecr_android.so $extraArgs"
    if ($LASTEXITCODE -ne 0) { throw "CPU accuracy oracle failed (exit $LASTEXITCODE)." }
} finally {
    Pop-Location
}
