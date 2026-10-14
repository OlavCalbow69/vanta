[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Destination,
    [string]$VisualStudioRoot
)
$ErrorActionPreference='Stop'
# OpenCV imports the release CRT even when Vanta itself links /MT. Copy only
# official app-local redistributables, never the DLLs in System32 or Debug CRT.
if (-not $VisualStudioRoot) {
    $vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw 'Pass -VisualStudioRoot for the licensed Visual Studio installation containing the x64 redistributables.'
    }
    $VisualStudioRoot=(& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
    if (-not $VisualStudioRoot) { throw 'Visual Studio C++ tools were not found.' }
}
$redistRoot=Join-Path $VisualStudioRoot 'VC\Redist\MSVC'
$versions=Get-ChildItem -LiteralPath $redistRoot -Directory |
    Where-Object Name -Match '^\d+\.\d+\.\d+$' | Sort-Object { [version]$_.Name } -Descending
$crtDirectory=$null
foreach ($version in $versions) {
    $architectureRoot=Join-Path $version.FullName 'x64'
    $crtDirectory=Get-ChildItem -LiteralPath $architectureRoot -Directory -Filter 'Microsoft.VC*.CRT' -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($crtDirectory) { break }
}
if (-not $crtDirectory) { throw 'The official x64 Visual C++ release CRT redistributables were not found.' }
foreach ($required in @('msvcp140.dll','concrt140.dll','vcruntime140.dll','vcruntime140_1.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $crtDirectory.FullName $required) -PathType Leaf)) {
        throw "Incomplete x64 CRT redistributables: missing $required"
    }
}
$redistNotice=Join-Path $VisualStudioRoot 'Licenses\1033\Redist.txt'
if (-not (Test-Path -LiteralPath $redistNotice -PathType Leaf)) {
    throw 'The Visual Studio redistribution notice was not found.'
}
New-Item -ItemType Directory -Path $Destination -Force | Out-Null
Get-ChildItem -LiteralPath $crtDirectory.FullName -File -Filter '*.dll' |
    Copy-Item -Destination $Destination
$licenseDestination=Join-Path $Destination 'licenses'
New-Item -ItemType Directory -Path $licenseDestination -Force | Out-Null
Copy-Item -LiteralPath $redistNotice -Destination (Join-Path $licenseDestination 'Microsoft-VC-runtime-redistribution.txt')
Write-Host "Included official x64 Visual C++ runtime from $($crtDirectory.FullName)"
