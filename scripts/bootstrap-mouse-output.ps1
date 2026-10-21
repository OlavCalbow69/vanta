[CmdletBinding()]
param([switch]$CompileChecks)
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
$repositoryRoot=Split-Path -Parent $PSScriptRoot
$toolRoot=Join-Path $repositoryRoot 'work\medius-tools'
New-Item -ItemType Directory -Path $toolRoot -Force | Out-Null
if (-not (Test-Path -LiteralPath (Join-Path $repositoryRoot 'vendor\medius\Cargo.lock'))) {
    throw 'Pinned Medius source is missing.'
}
function Get-LockedDownload([string]$Url,[string]$Destination,[string]$Sha256) {
    if (-not (Test-Path -LiteralPath $Destination)) { Invoke-WebRequest -Uri $Url -OutFile $Destination }
    if ((Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash -ne $Sha256) { throw "Dependency checksum mismatch: $Destination" }
}
$env:RUSTUP_HOME=Join-Path $toolRoot 'rustup'
$env:CARGO_HOME=Join-Path $toolRoot 'cargo'
$cargo=Join-Path $env:CARGO_HOME 'bin\cargo.exe'
if (-not (Test-Path -LiteralPath $cargo)) {
    $installer=Join-Path $toolRoot 'rustup-init.exe'
    Get-LockedDownload 'https://static.rust-lang.org/rustup/archive/1.29.1/x86_64-pc-windows-msvc/rustup-init.exe' $installer '6F4BEF66261261FCB43131BE8720BAB817D403A09EDEC7455C371974B90BDB7E'
    & $installer -y --no-modify-path --profile minimal --default-toolchain 1.99.1
    if ($LASTEXITCODE -ne 0) { throw 'Private Rust compiler installation failed' }
}
$env:RUSTUP_TOOLCHAIN=if(Test-Path -LiteralPath (Join-Path $env:RUSTUP_HOME 'toolchains\stable-x86_64-pc-windows-msvc')){'stable'}else{'1.99.1'}
$rustVersion=& (Join-Path $env:CARGO_HOME 'bin\rustc.exe') --version
if ($rustVersion -notmatch '^rustc 1\.99\.1 ') { throw "Expected pinned Rust 1.99.1, got $rustVersion" }
$env:PATH=(Join-Path $env:CARGO_HOME 'bin')+';'+$env:PATH
$env:CARGO_TARGET_DIR=Join-Path $repositoryRoot 'work\medius-target'
$env:RUSTFLAGS='-C target-feature=+crt-static'
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
if (-not $vsRoot) { throw 'MSVC Desktop C++ tools were not found' }
$developerBatch=Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
# Import the compiler environment; no filesystem operation is delegated to cmd.
$developerEnvironment=& $env:ComSpec /d /s /c "`"$developerBatch`" >nul && set"
foreach ($line in $developerEnvironment) {
    $separator=$line.IndexOf('=')
    if ($separator -gt 0) { [Environment]::SetEnvironmentVariable($line.Substring(0,$separator),$line.Substring($separator+1),'Process') }
}
Push-Location -LiteralPath (Join-Path $repositoryRoot 'vendor\medius')
try {
    if ($CompileChecks) {
        & $cargo test --release --locked --all-features --no-run -p medius -p medius-capi
        if ($LASTEXITCODE -ne 0) { throw 'Medius fake-device checks failed to compile' }
    }
    # Leave the ordinary SDK, without opt-in mock features, as the linked library.
    & $cargo build --release --locked -p medius-capi
    if ($LASTEXITCODE -ne 0) { throw 'Medius C API build failed' }
} finally { Pop-Location }
& (Join-Path $PSScriptRoot 'prepare-medius-package.ps1')
Write-Host 'Medius /MT SDK and notices ready. No Vanta or hardware checks were executed.'
