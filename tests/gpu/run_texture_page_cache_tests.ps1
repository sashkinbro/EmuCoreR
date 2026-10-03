param(
    [Parameter(Mandatory = $true)][string]$Clang,
    [Parameter(Mandatory = $true)][string]$CommonLibrary,
    [Parameter(Mandatory = $true)][string]$Adb,
    [Parameter(Mandatory = $true)][int]$TransportId
)

$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
Push-Location $repoRoot
try {
    New-Item -ItemType Directory -Force build/texture-cache-tests | Out-Null
    $executable = 'build/texture-cache-tests/texture-page-cache-tests'
    & $Clang --target=aarch64-none-linux-android26 -std=c++17 -O2 -ffunction-sections -fdata-sections '-Wl,--gc-sections' -static-libstdc++ -Ithird_party/swanstation/src/core -Ithird_party/swanstation/src tests/gpu/texture_page_cache_tests.cpp third_party/swanstation/src/core/gpu/gpu_hw_texture_cache.cpp $CommonLibrary -o $executable
    if ($LASTEXITCODE -ne 0) { throw 'Test compilation failed.' }
    & $Adb -t $TransportId push $executable /data/local/tmp/texture-page-cache-tests
    if ($LASTEXITCODE -ne 0) { throw 'Test upload failed.' }
    & $Adb -t $TransportId shell chmod 755 /data/local/tmp/texture-page-cache-tests
    if ($LASTEXITCODE -ne 0) { throw 'Cannot prepare test executable.' }
    & $Adb -t $TransportId shell /data/local/tmp/texture-page-cache-tests
    if ($LASTEXITCODE -ne 0) { throw 'Texture page cache regression tests failed.' }
}
finally {
    Pop-Location
}
