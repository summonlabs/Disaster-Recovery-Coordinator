# Build the package, install it into a scratch prefix, build an independent
# downstream consumer against that prefix (never against the build tree), and
# run the consumer. Any failure stops the script with a non-zero exit code.
#
# There is no timeout anywhere in this script: a hang is a defect.

param(
    [string]$BuildDir = 'build/release',
    [string]$Prefix = '_install',
    [string]$Config = 'Release',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    if (-not $SkipBuild) {
        cmake --build $BuildDir
        if ($LASTEXITCODE -ne 0) { throw 'build failed' }
    }
    cmake --install $BuildDir --prefix $Prefix --config $Config
    if ($LASTEXITCODE -ne 0) { throw 'install failed' }

    $packageConfig = Join-Path $Prefix 'lib/cmake/DisasterRecoveryCoordinator/DisasterRecoveryCoordinatorConfig.cmake'
    if (-not (Test-Path $packageConfig)) {
        throw "the installed package has no CMake package config at $packageConfig"
    }
    $header = Join-Path $Prefix 'include/drc/engine.hpp'
    if (-not (Test-Path $header)) {
        throw "the installed package has no public headers at $header"
    }

    $consumerBuild = Join-Path $Prefix 'consumer-build'
    if (Test-Path $consumerBuild) { Remove-Item -Recurse -Force $consumerBuild }
    # Quoted deliberately: PowerShell passes an inline -DNAME=$Var token through
    # verbatim, so CMAKE_BUILD_TYPE would literally become "$Config".
    cmake -S tests/downstream -B $consumerBuild -G Ninja "-DCMAKE_BUILD_TYPE=$Config" "-DCMAKE_PREFIX_PATH=$root/$Prefix"
    if ($LASTEXITCODE -ne 0) { throw 'downstream consumer configure failed' }
    cmake --build $consumerBuild
    if ($LASTEXITCODE -ne 0) { throw 'downstream consumer build failed' }

    $consumer = Join-Path $consumerBuild 'downstream_consumer.exe'
    if (-not (Test-Path $consumer)) { $consumer = Join-Path $consumerBuild 'downstream_consumer' }
    & $consumer
    if ($LASTEXITCODE -ne 0) { throw 'downstream consumer failed' }
    Write-Output "install and downstream consumption verified against $Prefix"
}
finally {
    Pop-Location
}
