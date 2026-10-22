param(
    [Parameter(Mandatory = $true)][string]$Serial,
    [Parameter(Mandatory = $true)][string]$CoreLibrary,
    [string]$Sdk = "$env:LOCALAPPDATA/Android/Sdk"
)
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$adb = Join-Path $Sdk 'platform-tools/adb.exe'
$toolchain = Join-Path $Sdk 'ndk/29.0.14206865/toolchains/llvm/prebuilt/windows-x86_64'
$clang = Join-Path $toolchain 'bin/clang++.exe'
$runtime = Join-Path $toolchain 'sysroot/usr/lib/aarch64-linux-android/libc++_shared.so'
$remoteRoot = '/data/local/tmp/emucorer-delay-slot'
Push-Location $repoRoot
try {
    New-Item -ItemType Directory -Force build/pgxp-tests | Out-Null
    $executable = 'build/pgxp-tests/recompiler-delay-slot-tests'
    & $clang --target=aarch64-none-linux-android26 -std=c++17 -O2 -static-libstdc++ -Ithird_party/swanstation/dep/libretro-common/include tests/pgxp/recompiler_delay_slot_tests.cpp -ldl -o $executable
    if ($LASTEXITCODE -ne 0) { throw 'Delay-slot test compilation failed.' }
    & $adb -s $Serial shell mkdir -p $remoteRoot
    if ($LASTEXITCODE -ne 0) { throw 'Cannot prepare the test directory.' }
    foreach ($upload in @(@($CoreLibrary, 'core.so'), @($runtime, 'libc++_shared.so'), @($executable, 'tests'))) {
        & $adb -s $Serial push $upload[0] "$remoteRoot/$($upload[1])"
        if ($LASTEXITCODE -ne 0) { throw 'Delay-slot test upload failed.' }
    }
    & $adb -s $Serial shell chmod 755 "$remoteRoot/tests"
    if ($LASTEXITCODE -ne 0) { throw 'Cannot make the test executable.' }
    foreach ($mode in @('Interpreter', 'Recompiler')) {
        foreach ($scenario in @('unreadable', 'readable')) {
            & $adb -s $Serial shell "LD_LIBRARY_PATH=$remoteRoot timeout 20 $remoteRoot/tests $remoteRoot/core.so $mode $scenario"
            if ($LASTEXITCODE -ne 0) { throw "Delay-slot regression failed for $mode/$scenario (exit $LASTEXITCODE)." }
        }
    }
} finally { Pop-Location }
