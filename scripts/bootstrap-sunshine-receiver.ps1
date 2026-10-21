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
$moonlightRevision = 'aeba988e0a302145eff2e70fa3e11cd98ec01f5d'
$vcpkgRevision = 'a5f736224e85b32e25a8ffc52562333180396642'

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

function Ensure-SourceMetadata {
    param([string]$Path, [string]$Url, [string]$Name, [string]$SourceRevision)
    $resolvedSource=[IO.Path]::GetFullPath($Path)
    $workspacePrefix=[IO.Path]::GetFullPath($repositoryRoot).TrimEnd('\')+'\'
    if (-not $resolvedSource.StartsWith($workspacePrefix,[StringComparison]::OrdinalIgnoreCase)) {
        throw "Dependency source is outside the workspace: $resolvedSource"
    }
    if (-not (Test-Path -LiteralPath $Path) -or @(Get-ChildItem -LiteralPath $Path -Force -ErrorAction SilentlyContinue).Count -eq 0) {
        Invoke-Checked 'git' @('clone', $Url, $Path)
        return
    }
    & git -C $Path rev-parse --show-toplevel 2>$null | Out-Null
    if ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath (Join-Path $Path '.git'))) { return }
    # A copied worktree can retain a gitfile pointing at a missing worktree.
    # Rebuild only metadata; retain every existing source file and old gitfile.
    $metadata = Join-Path $workRoot "git\$Name"
    if (Test-Path -LiteralPath $metadata) { throw "Repair metadata already exists: $metadata" }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $metadata) | Out-Null
    Invoke-Checked 'git' @('clone', '--bare', $Url, $metadata)
    if ($SourceRevision) { Invoke-Checked 'git' @('--git-dir', $metadata, 'update-ref', 'HEAD', $SourceRevision) }
    Invoke-Checked 'git' @('--git-dir', $metadata, 'config', 'core.bare', 'false')
    Invoke-Checked 'git' @('--git-dir', $metadata, 'config', 'core.worktree', $Path)
    $marker=Join-Path $Path '.git'
    if (Test-Path -LiteralPath $marker) {
        Move-Item -LiteralPath $marker -Destination "$marker.stale-$(Get-Date -Format yyyyMMdd-HHmmss)" -Force
    }
    "gitdir: $($metadata.Replace('\','/'))" | Set-Content -LiteralPath $marker -Encoding Ascii -Force
    Invoke-Checked 'git' @('-C', $Path, 'read-tree', 'HEAD')
}
function Set-PinnedRevision {
    param([string]$Path, [string]$Revision)
    $current=(& git -C $Path rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0) { throw "Cannot inspect repository: $Path" }
    $changes=& git -C $Path status --porcelain --untracked-files=no --ignore-submodules=all
    if ($changes) { throw "Retaining modified dependency sources at $Path; reconcile them before updating to $Revision" }
    if ($current -eq $Revision) { return }
    Invoke-Checked 'git' @('-C', $Path, 'fetch', 'origin', $Revision)
    Invoke-Checked 'git' @('-C', $Path, 'checkout', '--detach', $Revision)
}
New-Item -ItemType Directory -Force -Path $workRoot | Out-Null
$originalCommon=(& git -C $repositoryRoot rev-parse 'HEAD:vendor/moonlight-common-c').Trim()
Ensure-SourceMetadata $moonlightSource 'https://github.com/moonlight-stream/moonlight-common-c.git' 'moonlight-common-c' $originalCommon
Set-PinnedRevision $moonlightSource $moonlightRevision
foreach ($child in @(
    @{ Name='enet'; Url='https://github.com/cgutman/enet.git' },
    @{ Name='nanors'; Url='https://github.com/sleepybishop/nanors.git' })) {
    $revision=(& git -C $moonlightSource rev-parse "HEAD:$($child.Name)").Trim()
    $childPath=Join-Path $moonlightSource $child.Name
    Ensure-SourceMetadata $childPath $child.Url $child.Name $revision
    Set-PinnedRevision $childPath $revision
}
Invoke-Checked 'git' @('-C', $moonlightSource, 'submodule', 'init')

if (-not (Test-Path (Join-Path $moonlightSource 'CMakeLists.txt'))) {
    throw "moonlight-common-c was not initialized at $moonlightSource"
}

if (-not (Test-Path $vcpkgRoot)) {
    Invoke-Checked 'git' @('clone', 'https://github.com/microsoft/vcpkg.git', $vcpkgRoot)
}
Set-PinnedRevision $vcpkgRoot $vcpkgRevision
if (-not (Test-Path $vcpkgExe)) {
    Invoke-Checked (Join-Path $vcpkgRoot 'bootstrap-vcpkg.bat') @('-disableMetrics')
}

Invoke-Checked $vcpkgExe @('install', 'openssl:x64-windows-static', 'ffmpeg[core,avcodec]:x64-windows-static', '--clean-after-build')

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
