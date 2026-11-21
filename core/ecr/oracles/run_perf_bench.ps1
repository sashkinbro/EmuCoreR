param(
    [Parameter(Mandatory = $true)][string]$Serial,
    [string]$CoreLibrary,
    [string]$BiosPath = '/sdcard/Download/PS1Bios/scph101.bin',
    [string[]]$Games = @(
        'crash2=/data/local/tmp/ecr-bench-games/crash2/Crash Bandicoot 2 - Cortex Strikes Back (USA).cue',
        'dbgt=/data/local/tmp/ecr-bench-games/dbgt/Dragon Ball GT - Final Bout (USA).cue',
        'tekken3=/data/local/tmp/ecr-bench-games/tekken3/Tekken 3 (USA).cue'
    ),
    [int]$GameFrames = 360,
    [string]$DumpDir = '',
    [switch]$SkipInterp,
    [string]$Sdk = "$env:LOCALAPPDATA/Android/Sdk"
)
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$adb = Join-Path $Sdk 'platform-tools/adb.exe'
$toolchain = Join-Path $Sdk 'ndk/29.0.14206865/toolchains/llvm/prebuilt/windows-x86_64'
$clang = Join-Path $toolchain 'bin/clang++.exe'
$runtime = Join-Path $toolchain 'sysroot/usr/lib/aarch64-linux-android/libc++_shared.so'
$remoteRoot = '/data/local/tmp/ecr-perf-bench-bin'
$remoteDump = '/data/local/tmp/ecr-perf-bench-dumps'

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
    $executable = 'build/oracles/perf_bench'
    & $clang --target=aarch64-none-linux-android26 -std=c++17 -O2 -Wall -Wextra -static-libstdc++ `
        -Icore/ecr/dep/libecr-common/include core/ecr/oracles/perf_bench.cpp -ldl -lEGL -lGLESv3 -lvulkan -o $executable
    if ($LASTEXITCODE -ne 0) { throw 'perf_bench compilation failed.' }

    & $adb -s $Serial shell mkdir -p $remoteRoot | Out-Null
    foreach ($upload in @(@($CoreLibrary, 'libecr_android.so'), @($runtime, 'libc++_shared.so'), @($executable, 'perf_bench'))) {
        & $adb -s $Serial push $upload[0] "$remoteRoot/$($upload[1])" | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Failed uploading $($upload[0])." }
    }
    & $adb -s $Serial shell chmod 755 "$remoteRoot/perf_bench"

    $shQuote = { param([string]$s) "'" + ($s -replace "'", "'`"`'`"`'") + "'" }
    $args = "--bios " + (& $shQuote $BiosPath) + " --game-frames $GameFrames"
    if ($SkipInterp) { $args += ' --skip-interp' }
    foreach ($g in $Games) {
        if (-not [string]::IsNullOrWhiteSpace($g)) {
            $args += ' --game ' + (& $shQuote $g)
        }
    }
    if (-not [string]::IsNullOrWhiteSpace($DumpDir)) {
        New-Item -ItemType Directory -Force -Path $DumpDir | Out-Null
        & $adb -s $Serial shell "rm -rf $remoteDump && mkdir -p $remoteDump" | Out-Null
        $args += " --dump-dir $remoteDump"
    }

    & $adb -s $Serial shell "LD_LIBRARY_PATH=$remoteRoot timeout 600 $remoteRoot/perf_bench $remoteRoot/libecr_android.so $args"
    $runExit = $LASTEXITCODE

    if (-not [string]::IsNullOrWhiteSpace($DumpDir)) {
        & $adb -s $Serial pull "$remoteDump/." $DumpDir | Out-Null
        Write-Host "Pulled perf_bench screenshots to $DumpDir"
    }
    if ($runExit -ne 0) { throw "perf_bench failed (exit $runExit)." }
} finally {
    Pop-Location
}
