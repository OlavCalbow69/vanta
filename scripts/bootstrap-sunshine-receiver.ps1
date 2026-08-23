[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$workRoot = Join-Path $repositoryRoot 'work'
$vcpkgRoot = Join-Path $workRoot 'vcpkg'
$vcpkgExe = Join-Path $vcpkgRoot 'vcpkg.exe'
$vcpkgTripletRoot = Join-Path $vcpkgRoot 'installed\x64-windows-static'
$moonlightSource = Join-Path $repositoryRoot 'vendor\moonlight-common-c'
$moonlightBuild = Join-Path $workRoot 'moonlight-common-c-mt-build'

function Invoke-Checked {
    param([string]$FilePath, [string[]]$Arguments)

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed ($LASTEXITCODE): $FilePath $($Arguments -join ' ')"
    }
}

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    throw 'Git is required to initialize moonlight-common-c and vcpkg.'
}

New-Item -ItemType Directory -Force -Path $workRoot | Out-Null
Invoke-Checked 'git' @('-C', $repositoryRoot, 'submodule', 'update', '--init', '--recursive')

if (-not (Test-Path (Join-Path $moonlightSource 'CMakeLists.txt'))) {
    throw "moonlight-common-c was not initialized at $moonlightSource"
}

if (-not (Test-Path $vcpkgExe)) {
    if (-not (Test-Path $vcpkgRoot)) {
        Invoke-Checked 'git' @('clone', 'https://github.com/microsoft/vcpkg.git', $vcpkgRoot)
    }
    Invoke-Checked (Join-Path $vcpkgRoot 'bootstrap-vcpkg.bat') @('-disableMetrics')
}

Invoke-Checked $vcpkgExe @('install', 'openssl:x64-windows-static')

$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if ($null -eq $cmake) {
    $bundledCmake = 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (-not (Test-Path $bundledCmake)) {
        throw 'CMake was not found. Install CMake or Visual Studio Desktop development with C++.'
    }
    $cmakePath = $bundledCmake
}
else {
    $cmakePath = $cmake.Source
}

Invoke-Checked $cmakePath @(
    '-S', $moonlightSource,
    '-B', $moonlightBuild,
    '-G', 'Visual Studio 18 2026',
    '-A', 'x64',
    '-DBUILD_SHARED_LIBS=OFF',
    '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded',
    "-DOPENSSL_ROOT_DIR=$vcpkgTripletRoot")
Invoke-Checked $cmakePath @('--build', $moonlightBuild, '--config', 'Release', '--parallel')

Write-Host 'Sunshine receiver dependencies are ready. Build vanta.sln in Release|x64.'
