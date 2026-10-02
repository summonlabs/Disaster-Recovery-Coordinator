# Validate that the repository builds from a clean clone at a given ref, without
# any local build directory or untracked file to lean on.
#
# There is no timeout anywhere in this script: a hang is a defect.

param(
    [string]$Ref = 'HEAD',
    [string]$WorkRoot = '',
    [string]$Config = 'Release'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrEmpty($WorkRoot)) {
    $WorkRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("drc-fresh-clone-" + [guid]::NewGuid().ToString("N"))
}
$clone = Join-Path $WorkRoot 'Disaster-Recovery-Coordinator'
New-Item -ItemType Directory -Force -Path $WorkRoot | Out-Null

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue) -and -not (Get-Command g++ -ErrorAction SilentlyContinue)) {
    Write-Warning 'no C++ compiler is on PATH; run this from a developer prompt or with the MSVC environment set up'
}

try {
    git clone --quiet --no-hardlinks $root $clone
    if ($LASTEXITCODE -ne 0) { throw 'clone failed' }
    git -C $clone checkout --quiet $Ref
    if ($LASTEXITCODE -ne 0) { throw "checkout of $Ref failed" }

    $build = Join-Path $clone 'build/fresh'
    $generator = @()
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) { $generator = @('-G', 'Ninja') }
    # Quoted deliberately: PowerShell passes an inline -DNAME=$Var token through
    # verbatim, so CMAKE_BUILD_TYPE would literally become "$Config".
    cmake -S $clone -B $build "-DCMAKE_BUILD_TYPE=$Config" @generator
    if ($LASTEXITCODE -ne 0) { throw 'configure failed in the fresh clone' }
    cmake --build $build
    if ($LASTEXITCODE -ne 0) { throw 'build failed in the fresh clone' }

    $tests = Join-Path $build 'tests/drc_tests.exe'
    if (-not (Test-Path $tests)) { $tests = Join-Path $build 'tests/drc_tests' }
    & $tests
    if ($LASTEXITCODE -ne 0) { throw 'tests failed in the fresh clone' }
    Write-Output "fresh clone validation passed at $Ref"
}
finally {
    if (Test-Path $clone) { Remove-Item -Recurse -Force $clone }
    if ((Test-Path $WorkRoot) -and -not (Get-ChildItem $WorkRoot -Force)) {
        Remove-Item -Recurse -Force $WorkRoot
    }
}
