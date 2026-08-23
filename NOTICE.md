# Third-party notices

## Moonlight common-c

Vanta's Sunshine receiver links
[moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c)
at commit `874ac9548f1bd6f095ef2b435c42cdde460e7821`.

Its copyright notices are retained in the vendored source. It is licensed
under GPL-3.0-or-later. Its full license text is in
`vendor/moonlight-common-c/LICENSE.txt`.

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
