# Third-party notices

## Moonlight common-c

Vanta's Sunshine receiver links
[moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c)
at commit `aeba988e0a302145eff2e70fa3e11cd98ec01f5d`.

Its copyright notices are retained in the vendored source. It is licensed
under GPL-3.0-or-later. Its full license text is in
`vendor/moonlight-common-c/LICENSE.txt`.

## FFmpeg

The receiver uses FFmpeg 9.0.2 (vcpkg port revision 1), statically built with
libavcodec/libavutil and Windows D3D11VA. No external FFmpeg executable is launched.
The pinned vcpkg revision is `a5f736224e85b32e25a8ffc52562333180396642`.
The package includes FFmpeg's copyright and license notices. FFmpeg's component
licenses and corresponding source are available from https://ffmpeg.org/ .
This build disables default vcpkg features, enables `avcodec`, and uses the
static MSVC runtime. The included FFmpeg build configuration and copyright
file describe its LGPL component licensing; Vanta's overall GPL obligations
also apply to the combined executable. The package includes dependency
revisions and build recipes for reproducing the corresponding source build.

## ENet

Moonlight common-c statically links [ENet](https://github.com/cgutman/enet)
at the revision recorded by its submodule. ENet is copyright (c) 2002-2020
Lee Salzman and is MIT licensed. Its full text is in
`vendor/moonlight-common-c/enet/LICENSE`.

## nanors

Moonlight common-c includes [nanors](https://github.com/sleepybishop/nanors)
at the revision recorded by its submodule. nanors is copyright (c) 2021
Joseph Calderon and is MIT licensed. Its full text is in
`vendor/moonlight-common-c/nanors/LICENSE`.

## OpenSSL

Vanta and Moonlight common-c statically link OpenSSL built by the bootstrap
script through vcpkg. OpenSSL is licensed under Apache-2.0; vcpkg installs its
full text at `work/vcpkg/installed/x64-windows-static/share/openssl/copyright`.
The release workflow includes that text in the binary bundle.

## Microsoft Visual C++ runtime

OpenCV imports Microsoft's release C++ runtime. Binary packages include the
official x64 CRT DLLs from the build machine's licensed Visual Studio Redist
directory as app-local dependencies, not Debug CRT or System32 copies.
These components remain Microsoft software under its redistribution terms;
the Visual Studio redistribution notice is included in the package's licenses
directory. See [Microsoft's deployment documentation](https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files).
App-local runtimes must be updated when repackaging; Microsoft recommends
centrally installed redistributables for independent servicing.


## Optional Medius native output

Medius SDK 3.4.6 (837e4ed99275f2783b2de06940198628be80bccf) is MIT licensed.
The Vanta host SDK patches are described in vendor/medius/VANTA-PATCHES.md.
Firmware is obtained separately from the official dashboard and is not included.
Medius, Rust dependency and Rust standard library license files ship in licenses/.
Exact SDK/library/patch hashes are recorded in DEPENDENCIES-MEDIUS.json.
The current bundle contains no ABCurves code, model, dataset or Python runtime.
