param(
    [Parameter(Mandatory = $true)]
    [string]$Serial,
    [string]$Sdk = "$env:LOCALAPPDATA/Android/Sdk",
    [string]$CoreLibrary
)

$ErrorActionPreference = 'Stop'
$adb = Join-Path $Sdk 'platform-tools/adb.exe'
$manufacturer = & $adb -s $Serial shell getprop ro.product.manufacturer
if ($LASTEXITCODE -ne 0 -or $manufacturer.Trim() -ine 'OnePlus') { throw 'Use the authorized OnePlus for state tests.' }
$model = & $adb -s $Serial shell getprop ro.product.model
if ($LASTEXITCODE -ne 0 -or $model.Trim() -ine 'CPH2747') { throw 'The state test device model does not match.' }
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if ([string]::IsNullOrWhiteSpace($CoreLibrary)) {
    $CoreLibrary = Get-ChildItem -LiteralPath (Join-Path $repoRoot 'app/.cxx/Debug') -Recurse -Filter swanstation_libretro_android.so |
        Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (!$CoreLibrary -or !(Test-Path -LiteralPath $CoreLibrary)) { throw 'Build :app:assembleDebug before testing state loads.' }
$runtime = Join-Path $Sdk 'ndk/29.0.14206865/toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so'
$clang = Join-Path $Sdk 'ndk/29.0.14206865/toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe'
$remoteRoot = '/data/local/tmp/emucorer-pgxp-libretro'
Push-Location $repoRoot
try {
    New-Item -ItemType Directory -Force build/pgxp-tests | Out-Null
    $executable = 'build/pgxp-tests/libretro-state-tests'
    & $clang --target=aarch64-none-linux-android26 -std=c++17 -O2 -static-libstdc++ -Ithird_party/swanstation/dep/libretro-common/include tests/pgxp/libretro_state_tests.cpp -ldl -o $executable
    if ($LASTEXITCODE -ne 0) { throw 'State test compilation failed.' }
    & $adb -s $Serial shell mkdir -p $remoteRoot
    if ($LASTEXITCODE -ne 0) { throw 'Cannot prepare the state test directory.' }
    foreach ($upload in @(@($CoreLibrary, 'core.so'), @($runtime, 'libc++_shared.so'), @($executable, 'state-tests'))) {
        & $adb -s $Serial push $upload[0] "$remoteRoot/$($upload[1])"
        if ($LASTEXITCODE -ne 0) { throw 'State test upload failed.' }
    }
    & $adb -s $Serial shell chmod 755 "$remoteRoot/state-tests"
    if ($LASTEXITCODE -ne 0) { throw 'Cannot prepare the state test executable.' }
    & $adb -s $Serial shell "LD_LIBRARY_PATH=$remoteRoot $remoteRoot/state-tests $remoteRoot/core.so"
    if ($LASTEXITCODE -ne 0) { throw 'Frontend state regression tests failed.' }
}
finally { Pop-Location }
