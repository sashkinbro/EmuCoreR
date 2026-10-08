param([Parameter(Mandatory = $true)][string]$Serial)
$ErrorActionPreference = 'Stop'
$sdkPath = "$env:LOCALAPPDATA/Android/Sdk"
$adb = Join-Path $sdkPath 'platform-tools/adb.exe'
$manufacturer = & $adb -s $Serial shell getprop ro.product.manufacturer
if ($LASTEXITCODE -ne 0) { throw 'Cannot identify the GPU FIFO test device.' }
$model = & $adb -s $Serial shell getprop ro.product.model
if ($LASTEXITCODE -ne 0 -or $manufacturer.Trim() -ine 'OnePlus' -or $model.Trim() -ine 'CPH2747') {
    throw 'Use the authorized OnePlus for GPU FIFO tests.'
}
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$clang = Join-Path $sdkPath 'ndk/29.0.14206865/toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe'
$common = Get-ChildItem -LiteralPath (Join-Path $repoRoot 'app/.cxx/Debug') -Filter libcommon.a -Recurse |
    Where-Object { $_.FullName -match '[\\/]arm64-v8a[\\/]swanstation[\\/]src[\\/]common[\\/]' } |
    Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1 -ExpandProperty FullName
if (!$common) { throw 'Build :app:assembleDebug before testing GPU FIFO state.' }
Push-Location $repoRoot
try {
    New-Item -ItemType Directory -Force build/texture-tests | Out-Null
    $executable = 'build/texture-tests/gpu-fifo-tests'
    & $clang --target=aarch64-none-linux-android26 -std=c++17 -O2 -ffunction-sections -fdata-sections '-Wl,--gc-sections' -static-libstdc++ -fsanitize=undefined -fno-sanitize-recover=all -static-libsan -Ithird_party/swanstation/src tests/gpu/gpu_fifo_state_tests.cpp $common -llog -o $executable
    if ($LASTEXITCODE -ne 0) { throw 'GPU FIFO test compilation failed.' }
    & $adb -s $Serial push $executable /data/local/tmp/emucorer-gpu-fifo-tests
    if ($LASTEXITCODE -ne 0) { throw 'GPU FIFO test upload failed.' }
    & $adb -s $Serial shell chmod 755 /data/local/tmp/emucorer-gpu-fifo-tests
    if ($LASTEXITCODE -ne 0) { throw 'GPU FIFO test permissions failed.' }
    & $adb -s $Serial shell /data/local/tmp/emucorer-gpu-fifo-tests
    if ($LASTEXITCODE -ne 0) { throw 'GPU FIFO tests failed.' }
} finally { Pop-Location }
