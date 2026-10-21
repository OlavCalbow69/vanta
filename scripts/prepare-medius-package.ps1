[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
$repositoryRoot=Split-Path -Parent $PSScriptRoot
$toolRoot=Join-Path $repositoryRoot 'work\medius-tools'
$destination=Join-Path $repositoryRoot 'work\medius-package'
$licenses=Join-Path $destination 'licenses'
New-Item -ItemType Directory -Path $licenses -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'vendor\medius\LICENSE') -Destination (Join-Path $licenses 'Medius-MIT.txt')
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'vendor\medius\VANTA-PATCHES.md') -Destination (Join-Path $licenses 'Medius-Vanta-host-patch.md')
$registry=Join-Path $toolRoot 'cargo\registry\src'
$noticeCount=0
foreach ($registryRoot in Get-ChildItem -LiteralPath $registry -Directory) {
    foreach ($crate in Get-ChildItem -LiteralPath $registryRoot.FullName -Directory) {
        foreach ($notice in Get-ChildItem -LiteralPath $crate.FullName -File | Where-Object Name -Match '^(LICENSE|COPYING|NOTICE|COPYRIGHT)') {
            $crateNotices=Join-Path $licenses ('Rust-dependencies\'+$crate.Name)
            New-Item -ItemType Directory -Path $crateNotices -Force | Out-Null
            Copy-Item -LiteralPath $notice.FullName -Destination $crateNotices
            ++$noticeCount
        }
    }
}
$stdNotices=0
foreach ($toolchain in Get-ChildItem -LiteralPath (Join-Path $toolRoot 'rustup\toolchains') -Directory) {
    $docs=Join-Path $toolchain.FullName 'share\doc\rust'
    $standard=Join-Path $docs 'licenses'
    if(Test-Path -LiteralPath $standard) {
        New-Item -ItemType Directory -Path (Join-Path $licenses 'Rust-standard-library') -Force | Out-Null
        Copy-Item -Path (Join-Path $standard '*') -Destination (Join-Path $licenses 'Rust-standard-library') -Recurse -Force
        ++$stdNotices
    }
    foreach ($notice in Get-ChildItem -LiteralPath $docs -File -Filter 'COPYRIGHT*') {
        Copy-Item -LiteralPath $notice.FullName -Destination (Join-Path $licenses ('Rust-'+$notice.Name)) -Force
    }
}
if(-not $noticeCount -or -not $stdNotices){throw 'Rust license files are missing; run bootstrap-mouse-output.ps1 first.'}
function Hash([string]$Path){(Get-FileHash -LiteralPath (Join-Path $repositoryRoot $Path) -Algorithm SHA256).Hash.ToLowerInvariant()}
$manifest=[ordered]@{
    medius=[ordered]@{
        version='3.4.6';commit='837e4ed99275f2783b2de06940198628be80bccf'
        static_library_sha256=(Hash 'work\medius-target\release\medius_capi.lib')
        cargo_lock_sha256=(Hash 'vendor\medius\Cargo.lock')
        host_patch_sha256=(Hash 'vendor\medius\VANTA-PATCHES.md')
        c_api_header_sha256=(Hash 'vendor\medius\medius-capi\include\medius.h')
        recovery_policy='manual; no SDK automatic reconnect or held-input replay'
        query_timeout_ms=20
    }
    rust_version='1.99.1';target='x86_64-pc-windows-msvc';crt='static /MT'
    output_path='native MOVE; separate finalized finite click clip'
    python_physical_input_modes=@('existing','fuse')
    fuse_policy='per-axis bearing 100 ms; fixed +/- and with 100%; against 0%; no human reinjection'
    recovery_record_version=2
    firmware_included=$false;receiver_python_required=$false
}
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $destination 'DEPENDENCIES-MEDIUS.json') -Encoding UTF8
Write-Host 'Medius dependency manifest and license files prepared.'
