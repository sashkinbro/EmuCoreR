param([Parameter(Mandatory = $true)][string]$Serial)
$ErrorActionPreference = 'Stop'
$sdkPath = "$env:LOCALAPPDATA/Android/Sdk"
$adb = Join-Path $sdkPath 'platform-tools/adb.exe'
$manufacturer = & $adb -s $Serial shell getprop ro.product.manufacturer
if ($LASTEXITCODE -ne 0) { throw 'Cannot identify the logging test device.' }
$model = & $adb -s $Serial shell getprop ro.product.model
if ($LASTEXITCODE -ne 0 -or $manufacturer.Trim() -ine 'OnePlus' -or $model.Trim() -ine 'CPH2747') {
    throw 'Use the authorized OnePlus for release logging tests.'
}
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$clang = Join-Path $sdkPath 'ndk/29.0.14206865/toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe'
Push-Location $repoRoot
try {
    New-Item -ItemType Directory -Force build/texture-tests | Out-Null
    $executable = 'build/texture-tests/release-logging-tests'
    & $clang --target=aarch64-none-linux-android26 -std=c++17 -O3 -DNDEBUG -static-libstdc++ -Ithird_party/swanstation/src tests/gpu/release_logging_tests.cpp third_party/swanstation/src/common/log.cpp -o $executable
    if ($LASTEXITCODE -ne 0) { throw 'Release logging test compilation failed.' }
    & $adb -s $Serial push $executable /data/local/tmp/emucorer-release-logging-tests
    if ($LASTEXITCODE -ne 0) { throw 'Release logging test upload failed.' }
    & $adb -s $Serial shell chmod 755 /data/local/tmp/emucorer-release-logging-tests
    if ($LASTEXITCODE -ne 0) { throw 'Release logging test permissions failed.' }
    & $adb -s $Serial shell /data/local/tmp/emucorer-release-logging-tests
    if ($LASTEXITCODE -ne 0) { throw 'Release logging tests failed.' }
} finally { Pop-Location }
