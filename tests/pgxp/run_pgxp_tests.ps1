param(
    [string]$Serial,
    [ValidateSet('CPH2747', 'RG556')]
    [string]$DeviceModel = 'CPH2747',
    [string]$Sdk = "$env:LOCALAPPDATA/Android/Sdk",
    [string]$CommonLibrary
)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($Serial)) { throw 'Pass the authorized ADB serial explicitly.' }
$adb = Join-Path $Sdk 'platform-tools/adb.exe'
$manufacturer = & $adb -s $Serial shell getprop ro.product.manufacturer
$expectedManufacturer = if ($DeviceModel -eq 'RG556') { 'Anbernic' } else { 'OnePlus' }
if ($LASTEXITCODE -ne 0 -or $manufacturer.Trim() -ine $expectedManufacturer) { throw 'PGXP test device manufacturer does not match.' }
$actualModel = & $adb -s $Serial shell getprop ro.product.model
if ($LASTEXITCODE -ne 0 -or $actualModel.Trim() -ine $DeviceModel) { throw 'PGXP test device model does not match.' }
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$clang = Join-Path $Sdk 'ndk/29.0.14206865/toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe'
if ([string]::IsNullOrWhiteSpace($CommonLibrary)) {
    $common = Get-ChildItem -LiteralPath (Join-Path $repoRoot 'app/.cxx/Debug') -Recurse -Filter libcommon.a |
        Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1 -ExpandProperty FullName
} else { $common = $CommonLibrary }
if (!$common -or !(Test-Path -LiteralPath $common)) { throw 'Build :app:assembleDebug before running PGXP tests.' }
Push-Location $repoRoot
try {
    New-Item -ItemType Directory -Force build/pgxp-tests | Out-Null
    $executable = 'build/pgxp-tests/pgxp-tests'
    & $clang --target=aarch64-none-linux-android26 -std=c++17 -O2 -ffunction-sections -fdata-sections '-Wl,--gc-sections' -static-libstdc++ -Ithird_party/swanstation/src/core -Ithird_party/swanstation/src -Ithird_party/swanstation/dep/libretro-common/include tests/pgxp/pgxp_tests.cpp third_party/swanstation/src/core/cpu/gte.cpp $common -llog -o $executable
    if ($LASTEXITCODE -ne 0) { throw 'PGXP test compilation failed.' }
    & $adb -s $Serial push $executable /data/local/tmp/emucorer-pgxp-tests
    if ($LASTEXITCODE -ne 0) { throw 'PGXP test upload failed.' }
    & $adb -s $Serial shell chmod 755 /data/local/tmp/emucorer-pgxp-tests
    if ($LASTEXITCODE -ne 0) { throw 'Cannot prepare PGXP tests.' }
    & $adb -s $Serial shell /data/local/tmp/emucorer-pgxp-tests
    if ($LASTEXITCODE -ne 0) { throw 'PGXP regression tests failed.' }
}
finally { Pop-Location }
