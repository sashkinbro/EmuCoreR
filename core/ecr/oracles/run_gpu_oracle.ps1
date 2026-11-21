param(
    [Parameter(Mandatory = $true)][string]$Serial,
    [string]$CoreLibrary,
    [string]$Sdk = "$env:LOCALAPPDATA/Android/Sdk"
)
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$adb = Join-Path $Sdk 'platform-tools/adb.exe'
$toolchain = Join-Path $Sdk 'ndk/29.0.14206865/toolchains/llvm/prebuilt/windows-x86_64'
$clang = Join-Path $toolchain 'bin/clang++.exe'
$runtime = Join-Path $toolchain 'sysroot/usr/lib/aarch64-linux-android/libc++_shared.so'
$remoteRoot = '/data/local/tmp/ecr-gpu-oracle-bin'

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
    $executable = 'build/oracles/gpu_oracle'
    & $clang --target=aarch64-none-linux-android26 -std=c++17 -O2 -Wall -Wextra -static-libstdc++ `
        -Icore/ecr/dep/libecr-common/include core/ecr/oracles/gpu_oracle.cpp -ldl -lEGL -lGLESv3 -o $executable
    if ($LASTEXITCODE -ne 0) { throw 'GPU oracle compilation failed.' }

    & $adb -s $Serial shell mkdir -p $remoteRoot
    if ($LASTEXITCODE -ne 0) { throw 'Failed preparing device directory for GPU oracle.' }

    foreach ($upload in @(@($CoreLibrary, 'libecr_android.so'), @($runtime, 'libc++_shared.so'), @($executable, 'gpu_oracle'))) {
        & $adb -s $Serial push $upload[0] "$remoteRoot/$($upload[1])" | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Failed uploading $($upload[0])." }
    }
    & $adb -s $Serial shell chmod 755 "$remoteRoot/gpu_oracle"

    & $adb -s $Serial shell "LD_LIBRARY_PATH=$remoteRoot timeout 60 $remoteRoot/gpu_oracle $remoteRoot/libecr_android.so"
    if ($LASTEXITCODE -ne 0) { throw "GPU accuracy oracle failed (exit $LASTEXITCODE)." }
} finally {
    Pop-Location
}
