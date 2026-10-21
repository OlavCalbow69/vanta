[CmdletBinding()]
param(
    [ValidateSet('Debug','Release')][string]$Configuration='Release',
    [ValidatePattern('^[A-Za-z0-9_-]+$')][string]$PackageName='VantaStreaming240Diagnostics',
    [switch]$SkipTests
)
$ErrorActionPreference='Stop'
$repositoryRoot=Split-Path -Parent $PSScriptRoot
$buildRoot=Join-Path $repositoryRoot "build\$Configuration"
$packageRoot=Join-Path $repositoryRoot "build\package\$PackageName-$Configuration"
$runtimeFiles=@('vanta.exe','vanta-updater.exe','opencv_world500.dll','python-config.json')
foreach ($name in $runtimeFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $buildRoot $name) -PathType Leaf)) {
        throw "Build $Configuration x64 before packaging: missing $name"
    }
}
if (-not $SkipTests) {
    foreach ($check in @('--stream-pipeline-self-test','--fps-diagnostics-self-test','--python-self-test','--detector-self-test','--detector-ui-self-test','--config-self-test','--makcu-self-test','--input-lock-self-test','--medius-self-test',
        '--testclick-self-test','--receiver-capture-self-test','--receiver-self-test')) {
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
& (Join-Path $PSScriptRoot 'prepare-medius-package.ps1')
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'work\medius-package\DEPENDENCIES-MEDIUS.json') -Destination $packageRoot
& (Join-Path $PSScriptRoot 'copy-vc-runtime.ps1') -Destination $packageRoot
foreach ($name in @('README.md','README-DETECTOR.md','README-PYTHON.md','README-PYTHON-FUSE.md','README-STREAMING.md','README-INPUT-LOCKS.md','README-MEDIUS.md','README-TESTCLICK.md','NOTICE.md','LICENSE')) {
    Copy-Item -LiteralPath (Join-Path $repositoryRoot $name) -Destination $packageRoot
}
$licenseRoot=Join-Path $packageRoot 'licenses'
New-Item -ItemType Directory -Path $licenseRoot -Force | Out-Null
Copy-Item -Path (Join-Path $repositoryRoot 'work\medius-package\licenses\*') -Destination $licenseRoot -Recurse -Force
$licenses=@{
    'Moonlight.txt'='vendor\moonlight-common-c\LICENSE.txt'
    'ENet.txt'='vendor\moonlight-common-c\enet\LICENSE'
    'nanors.txt'='vendor\moonlight-common-c\nanors\LICENSE'
    'OpenSSL.txt'='work\vcpkg\installed\x64-windows-static\share\openssl\copyright'
    'FFmpeg.txt'='work\vcpkg\installed\x64-windows-static\share\ffmpeg\copyright'
    'OpenCV.txt'='vendor\opencv\LICENSE.txt'
    'OpenCV-FFmpeg.txt'='vendor\opencv\LICENSE_FFMPEG.txt'
}
foreach ($entry in $licenses.GetEnumerator()) {
    Copy-Item -LiteralPath (Join-Path $repositoryRoot $entry.Value) -Destination (Join-Path $licenseRoot $entry.Key)
}
@(
    'Vanta Medius Activation Rearm / TestClick Repeat / Python Fuse Inputs (local, not an official release)'
    "Binary file version: $((Get-Item -LiteralPath (Join-Path $buildRoot 'vanta.exe')).VersionInfo.FileVersion)"
    "Configuration: $Configuration x64"
    "Branch: $((& git -C $repositoryRoot branch --show-current).Trim())"
    "Base commit: $((& git -C $repositoryRoot rev-parse HEAD).Trim())"
    "Packaged UTC: $([DateTime]::UtcNow.ToString('o'))"
    "Moonlight common-c: $((& git -C (Join-Path $repositoryRoot 'vendor\moonlight-common-c') rev-parse HEAD).Trim())"
    "vcpkg: $((& git -C (Join-Path $repositoryRoot 'work\vcpkg') rev-parse HEAD).Trim())"
    'FFmpeg: 9.0.2#1, features core,avcodec; x64-windows-static; /MT'
    "vanta.exe SHA256: $((Get-FileHash -LiteralPath (Join-Path $buildRoot 'vanta.exe') -Algorithm SHA256).Hash.ToLowerInvariant())"
    "Source tree: $(if (@(& git -C $repositoryRoot status --porcelain --untracked-files=no).Count) {'includes uncommitted tracked changes'} else {'clean tracked tree'})"
    'MAKCU identity, baud and physical-button protocol retained; hardware validation is separate.'
    'MAKCU protocol checked against MCP reference (official 4.102); no firmware flashed.'
    'MAKCU COM-port lock removed; a saved port is only preferred when detected, with automatic fallback to an available detected device.'
    'Python activation uses the current MAKCU connection; legacy Python COM-port overrides are ignored.'
    'MAKCU connection logs/JSON include detected ports, the selected port and captured Windows API errors.'
    'Windows serial health polling initializes DCB length and protects the native handle.'
    'Automatic FPS diagnostics: JSON + timings/rates CSV in diagnostics beside vanta.exe, with LocalAppData fallback.'
    'Capture uses cancellable request deadlines and a high-resolution timer when available; no fixed 20 ms preview polling.'
    'Logs/reports distinguish stream-rate/capped/idle CPU demand and record capture wake lateness separately from GPU readback.'
    'Host graceful termination is recorded separately from connection/decoder failure; GPU drain warnings include context/fence status.'
    'Capture: Sunshine only. Output: stock MAKCU or optional Medius 3.4.6. Original movement remains the default.'
    'Runtime: native C++ detection and original movement; no Python interpreter, ABCurves, planner, profiles or calibration runtime.'
    'Python Axis mode: X + Y, Horizontal only, Hybrid. Default is X + Y; Hybrid permits Y for 300 ms by default, then sends X only until release.'
    'Python axis_mode is saved as a JSON string with hybrid_vertical_time_ms; legacy horizontal_only configurations remain supported.'
    'Hybrid duration control and settings serialization fixed; absent legacy COM fields can be safely removed during serialization.'
    'Physical-input masks: correction-scoped X/Y, manual Y in horizontal operation, generated left clicks and shared activation-side-button reservations.'
    'Masks use actual firmware readback; pending motion drains before unlock. Failed queries require a fresh serial session.'
    'Python Purple (narrow): HSV 144,106,172 to 151,255,255. Legacy configurations gain the preset without replacing selections or custom values.'
    'Medius masks use one firmware table query after transitions and before first correction, refreshed every 100 ms during correction.'
    'Due verification completes before further output. Partial masks, drift, stale readbacks and query failures cancel output and invalidate the session.'
    'UI/diagnostics separate requested/applied/confirmed masks, confirmation age, raw scales and correction release reasons. Saved lock opt-outs remain respected.'
    'Python Physical input: Existing behavior (default) or optional Medius Fuse inputs. Matching manual movement passes 100%; opposing motion is blocked per guided axis; zero axes remain manual.'
    'Fuse replaces only Python movement locks. Direction/axis changes drain, clear bearing and require a fresh observation; no catch-up movement or copying/reinjecting human reports.'
    'All four directional slots and 100 ms per-axis bearing configuration use actual firmware readback before output and every 100 ms while active.'
    'Versioned/size-checked recovery state snapshots/restores original bearing settings. Human contribution is not measured as delivered USB movement.'
    'Pure detection/movement algorithms are unchanged. Actual shared-control filtering and physical blocking remain laptop acceptance; see README-PYTHON-FUSE.md.'
    'Hidden same-executable recovery mode, exact Windows device identity and per-device session guard. No helper UI or GPU initialization.'
    'Recovery limits and laptop checklist: README-INPUT-LOCKS.md. Deterministic checks are compiled but not executed here.'
    'Movement baseline: pre-ABCurves build 1.1.0.6; pure Python algorithm and TestMove controller restored without algorithm changes.'
    'Medius SDK 3.4.6 /MT; exact pin, static-library hash and host patch hashes: DEPENDENCIES-MEDIUS.json.'
    'Medius requires both chips 3.4.6 / protocol 9 and confirmed native/clone 1000 Hz. No firmware flashing or rate forcing.'
    'Medius uses ordinary MOVE for each accepted correction, with full signed-16-bit deltas; no four-millisecond host streaming loop.'
    'Firmware render, spread and riding are captured, disabled/verified while owned, then restored during cleanup/recovery.'
    'Generated clicks use a separate finalized firmware clip with 14-34 ms hold; no periodic host refill.'
    'TestClick repeats on fresh valid frames while physical activation remains held; burst settings remain optional.'
    'Medius native button edge information reaches the input coordinator; an unambiguous new press can supply a missing released baseline.'
    'Held/cached snapshots cannot rearm; ambiguous or unmanaged button changes stay conservative. Held-through-emergency protection and stock behavior remain.'
    'Diagnostics include known physical buttons, last native edge and native press release-evidence counters. No detector, ray or movement calculation changes.'
    'Click upload/finalization and both content edges are checked; release completion uses a bounded 100 ms sequence with 20 ms individual query timeouts.'
    'Activation or settings changes during preparation cancel the click without falsely declaring a healthy link failed.'
    'Enable/binding edits publish a new configuration generation immediately, including explicit rearm after emergency release.'
    'TestClick shows backend and exact click status; JSON/logs include requests, starts, confirmations, rejections, cancellations, faults and clip readback.'
    'TestClick repeat, ownership, cancellation and Medius playback checks are opt-in and compiled only. Laptop checks: README-TESTCLICK.md.'
    'Pending movement uses conservative actual HEALTH injection-active readback, which can also include an independent click.'
    'Native sends measure serial acceptance; firmware does not acknowledge per-command USB delivery or expose an XY queue length.'
    'Python output: activation policy and movement ownership publish one generation together; stale workers cannot overwrite newer disable/configuration changes.'
    'Python logs/exports include exact movement gates, target/correction, physical activation and ownership generations. Sunshine pairing PIN has a Copy button.'
    'Setup, changes and laptop checklist: README-MEDIUS.md. Saved ABCurves fields are ignored; stream settings and backend selection are retained.'
    'SDK fake-device checks were compiled with --no-run. Application, GPU and hardware checks were not executed here.'
    'Receiver GPU and live-stream performance acceptance: deferred to the laptop.'
    'This diagnostics package was compiled locally; final binaries were not executed or tested here.'
    'No automatic reduction of resolution, FPS or bitrate on overload.'
) | Set-Content -LiteralPath (Join-Path $packageRoot 'BUILD-INFO.txt') -Encoding UTF8
$recipeRoot=Join-Path $packageRoot 'dependency-build'
New-Item -ItemType Directory -Path $recipeRoot -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'bootstrap-sunshine-receiver.ps1') -Destination $recipeRoot
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'work\vcpkg\ports\ffmpeg') -Destination (Join-Path $recipeRoot 'ffmpeg-port') -Recurse
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'work\vcpkg\installed\x64-windows-static\share\ffmpeg\vcpkg_abi_info.txt') -Destination $recipeRoot
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'bootstrap-mouse-output.ps1') -Destination $recipeRoot
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'prepare-medius-package.ps1') -Destination $recipeRoot
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
