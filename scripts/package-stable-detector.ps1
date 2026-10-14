[CmdletBinding()]
param(
    [ValidateSet('Debug','Release')][string]$Configuration='Release',
    [switch]$SkipTests
)
$ErrorActionPreference='Stop'
$repositoryRoot=Split-Path -Parent $PSScriptRoot
$buildRoot=Join-Path $repositoryRoot "build\$Configuration"
$packageRoot=Join-Path $repositoryRoot "build\package\VantaStableDetector-$Configuration"
$runtimeFiles=@('vanta.exe','vanta-updater.exe','opencv_world500.dll')
foreach ($name in $runtimeFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $buildRoot $name) -PathType Leaf)) {
        throw "Build $Configuration x64 before packaging: missing $name"
    }
}
if (-not $SkipTests) {
    foreach ($check in @('--detector-self-test','--detector-ui-self-test','--config-self-test','--makcu-self-test',
        '--receiver-capture-self-test','--receiver-self-test')) {
        & (Join-Path $buildRoot 'vanta.exe') $check
        if ($LASTEXITCODE -ne 0) { throw "$Configuration $check failed" }
        Write-Host "$Configuration $check passed"
    }
}
if (Test-Path -LiteralPath $packageRoot) {
    $resolvedTarget=[IO.Path]::GetFullPath($packageRoot)
    $allowedPrefix=[IO.Path]::GetFullPath((Join-Path $repositoryRoot 'build\package')).TrimEnd('\')+'\'
    if (-not $resolvedTarget.StartsWith($allowedPrefix,[StringComparison]::OrdinalIgnoreCase) -or
        ((Get-Item -LiteralPath $packageRoot).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Refusing to replace an unexpected package target: $resolvedTarget"
    }
    Remove-Item -LiteralPath $resolvedTarget -Recurse -Force
}
New-Item -ItemType Directory -Path $packageRoot -Force | Out-Null
foreach ($name in $runtimeFiles) {
    Copy-Item -LiteralPath (Join-Path $buildRoot $name) -Destination $packageRoot
}
& (Join-Path $PSScriptRoot 'copy-vc-runtime.ps1') -Destination $packageRoot
foreach ($name in @('README.md','README-DETECTOR.md','NOTICE.md','LICENSE')) {
    Copy-Item -LiteralPath (Join-Path $repositoryRoot $name) -Destination $packageRoot
}
$licenseRoot=Join-Path $packageRoot 'licenses'
New-Item -ItemType Directory -Path $licenseRoot -Force | Out-Null
$licenses=@{
    'Moonlight.txt'='vendor\moonlight-common-c\LICENSE.txt'
    'ENet.txt'='vendor\moonlight-common-c\enet\LICENSE'
    'nanors.txt'='vendor\moonlight-common-c\nanors\LICENSE'
    'OpenSSL.txt'='work\vcpkg\installed\x64-windows-static\share\openssl\copyright'
    'OpenCV.txt'='vendor\opencv\LICENSE.txt'
    'OpenCV-FFmpeg.txt'='vendor\opencv\LICENSE_FFMPEG.txt'
}
foreach ($entry in $licenses.GetEnumerator()) {
    Copy-Item -LiteralPath (Join-Path $repositoryRoot $entry.Value) -Destination (Join-Path $licenseRoot $entry.Key)
}
@(
    'Stable Vanta detector feature build (local, not an official release)'
    "Configuration: $Configuration x64"
    "Branch: $((& git -C $repositoryRoot branch --show-current).Trim())"
    "Base commit: $((& git -C $repositoryRoot rev-parse HEAD).Trim())"
    'Includes uncommitted feature-branch changes from this workspace.'
    'MAKCU target: 4.094; physical hardware validation is separate.'
    'Runtime: native C++; no system Python or bundled Python required.'
) | Set-Content -LiteralPath (Join-Path $packageRoot 'BUILD-INFO.txt') -Encoding UTF8
$manifest=Join-Path $packageRoot 'MANIFEST.sha256'
$manifestLines=Get-ChildItem -LiteralPath $packageRoot -Recurse -File | Where-Object FullName -ne $manifest | Sort-Object FullName | ForEach-Object {
    $relative=$_.FullName.Substring($packageRoot.Length+1).Replace('\','/')
    "$((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant())  $relative"
}
$manifestLines | Set-Content -LiteralPath $manifest -Encoding Ascii
$archive="$packageRoot.zip"
Compress-Archive -Path (Join-Path $packageRoot '*') -DestinationPath $archive -Force
"$((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant())  $([IO.Path]::GetFileName($archive))" |
    Set-Content -LiteralPath "$archive.sha256" -Encoding Ascii
Write-Host "Created $packageRoot and $archive"
