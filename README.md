# vanta

`vanta` is a standalone x64 Windows overlay built with Visual Studio 2026.
It does not inject into, target, or depend on another process.

The overlay uses a transparent DirectComposition surface, D3D11, the supplied
modified Dear ImGui fork, FreeType, embedded Poppins and icon fonts, custom
widgets, console/file logging, and the embedded `assets\favicon.ico` icon.
The same embedded Vanta artwork is uploaded as a D3D11 texture and displayed
beside the `VANTA` title in the menu header.
OpenCV 5.0.0 is vendored under `vendor\opencv`; its runtime DLL is copied
beside `vanta.exe` automatically. MAKCU's vendored C++ sources are compiled into
the executable through their opaque C API; no separate MAKCU DLL is required.

This feature branch adds the [1440p detector and MAKCU 4.094 update](README-DETECTOR.md).
It includes contour filters, mask-top aiming, configurable morphology, and red/yellow presets.
The [Python mode](README-PYTHON.md) replaces the removed timer page.
The [streaming-240 guide](README-STREAMING.md) covers hardware codecs, native GPU
ROI capture, diagnostics, laptop acceptance checks and rollback.

## License

Vanta is GPL-3.0-or-later because its native Sunshine receiver links
Moonlight common-c. See [LICENSE](LICENSE), [NOTICE.md](NOTICE.md), and the
complete GPL text in `vendor\moonlight-common-c\LICENSE.txt`.

## Build

Install Visual Studio Community 2026 with **Desktop development with C++**,
the MSVC `v145` x64 toolset, and Windows SDK `10.0.26100.0`. Open
`vanta.sln` or run the receiver bootstrap once before the first build:

```powershell
.\scripts\bootstrap-sunshine-receiver.ps1
```

The script initializes the Moonlight submodule, builds its static `/MT`
library, and installs pinned static FFmpeg and OpenSSL in the local `work\`
directory. Then run:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' `
  .\vanta.sln /m /p:Configuration=Release /p:Platform=x64
```

The output is:

```text
build\Release\vanta.exe
build\Release\vanta-updater.exe
build\Release\opencv_world500.dll
build\Release\python-config.json
```

## Automated builds and releases

GitHub Actions builds Debug and Release x64 on the Visual Studio 2026 runner
for pull requests and manual workflow runs. Every push to `main` additionally
publishes a permanent stable GitHub Release automatically. CI derives a
monotonically increasing `vMAJOR.MINOR.RUN_NUMBER` version, compiles that exact
version into both executables, and publishes `vanta-v<version>-win64.zip` plus
its SHA-256 checksum. No manual tag or repository secret is required.

The ZIP contains `vanta.exe`, `vanta-updater.exe`, the OpenCV runtime
DLL, this README, and the GPL/third-party license notices required by the
native Sunshine receiver. All builds also remain available as three-day
Actions artifacts.

## Run

Start the always-on-top overlay:

```powershell
.\build\Release\vanta.exe
```

Normal startup first hands control to the styled `vanta-updater.exe` gateway.
It uses the once-daily cached GitHub `releases/latest` result, installs a newer
verified build when available, and then launches Vanta. Diagnostic/self-test
arguments bypass this bootstrap. The gateway is enabled in official CI release
builds; locally compiled development binaries do not replace their own build
folder.

The transparent DirectComposition surface covers the complete virtual desktop
and remains available while switching between applications. The menu is
constrained to the usable area of the current monitor, including while it is
being dragged between monitors. Only the visible menu owns an input region;
the rest of the desktop remains genuinely click-through.

Press `Insert` to show or hide the menu. When hidden, the overlay is fully
click-through and its window is hidden. Drag the lower-right corner to resize
the menu within the current monitor. Press `End` or `Delete` to close Vanta.

Every supported exit route uses the same graceful shutdown path, including
`End`, `Delete`, `WM_CLOSE`, `Ctrl+C`, `Ctrl+Break`, console close, and Windows session
shutdown. Vanta first stops the TestClick, TestMove and Python workers, then capture,
MAKCU, configuration, ImGui and DirectComposition/D3D11.
Shutdown is idempotent
and also handles partial initialization failures.

The console output is mirrored to `vanta.log` beside the executable.

## Sunshine receiver

On the receiving PC, open **Stream Receiver** in Vanta. It automatically
searches the local network for Sunshine's standard Moonlight service
(`_nvstream._tcp.local`); select the desired host from **LAN Sunshine hosts**,
or enter an IP/name and select **Discover entered host**. Vanta shows a new
four-digit pairing PIN. Select **Start Vanta pairing**, then, while Vanta is
waiting, open the Sunshine host's Web UI > **PIN**, enter that exact Vanta PIN,
optionally name the device `Vanta`, and select **Send**. The initial pairing
request remains open for two minutes so there is time to approve it on the
host. Vanta generates its own GameStream client certificate; the private
identity and the host's pinned certificate are saved with Windows DPAPI under
`%LocalAppData%\Vanta\sunshine`.

After pairing, choose a LAN resolution, frame rate, and bitrate, then select
**Connect and start Sunshine Desktop**. **Ask Sunshine to match this display mode**
is enabled by default, matching Moonlight's normal Sunshine behavior so a
physical or virtual host display can use the requested resolution and refresh
rate; turn it off if the host display must remain unchanged. Vanta launches
Sunshine's `Desktop` app
over its certificate-pinned HTTPS GameStream API, uses Moonlight common-c for
the GameStream UDP transport, and uses FFmpeg/D3D11VA for hardware H.264,
HEVC or AV1 decoding. Auto chooses AV1, then HEVC, then H.264 from the common
receiver/host capabilities; forced choices stay fixed. It does not launch an
external Moonlight process. For a clean handoff, stop any separate active
Moonlight session before starting Vanta's stream; Vanta refuses to take over
an already-active Sunshine session.

Sunshine is the only capture source. Background region subscriptions feed
TestClick, TestMove and Python with immutable native BGRA snapshots. The
receiver preview samples GPU surfaces, while the capture pipeline reads back
only the union of active CPU regions. Remote coordinates are never treated as receiver-desktop
coordinates.

## Capture

Vanta opens on the Capture page and waits for the Sunshine stream. Pair and
connect through Stream Receiver. There are no local-window/monitor selectors,
Windows Graphics Capture, or Desktop Duplication backends in this build.

Capture settings retain:

- Full decoded source or a centered square from 128 to 1280 pixels.
- Original, color-filtered or black-and-white preview.
- Purple, yellow and dual-range red preview presets.
- Preview border visibility/color and TestMove FOV visualization.

The default centered ROI is 640x640. Only the GUI preview is resized, to at most
560x315; target detection keeps native decoded pixels and full-source dimensions.
The preview reports source/ROI dimensions, FPS and matching-pixel count.
The purple presets keep the existing exact-color blacklist; red and yellow do
not use it. Preview filtering never changes the raw frames supplied to workers.
Python mode requests its own centered 2*fov_radius region directly from the GPU,
independently of this page's ROI and preview settings.

## Mouse output

Mouse output defaults to stock MAKCU, with an optional Medius 3.4.6 backend.
The stock controller scans supported CH343/CH340 devices,
connects asynchronously, shows the port, VID/PID and numeric firmware version,
and retains high-performance mode, movement, left/right-click and wheel tests.
Auto-connect retries every two seconds and can be disabled. If MAKCU is absent,
output remains unavailable; there is no secondary-device fallback.

Medius is an optional output backend using the original Python and TestMove
calculations. See [README-MEDIUS.md](README-MEDIUS.md) for setup, recovery and
laptop checks. Stock MAKCU remains the default for missing backend settings.

The vendored C++ implementation probes 4 Mbaud before 115,200 and preserves the
V4 compatibility patch for volatile baud switching/reopen, numeric firmware
discovery and interleaved physical-button packets. It never flashes firmware.
TestClick and TestMove activation comes from MAKCU's physical-button monitor
(Left, Right, X1 or X2), not Windows key polling.

Quick clicks retain the existing bounded 14-34 ms hold. Failed release commands
are retried and remain pending for recovery before subsequent commands and
during the device tick. Disconnect, rescan and shutdown release injected state.
A TestClick shot is counted only after its release command succeeds.

## TestClick and TestMove

Both workers wait for monotonically newer capture snapshots and never process
the same frame twice. TestMove provides `Direct`, stateful `WindMouse`, and
`Axis control` movement. WindMouse advances once per fresh frame, exposes
gravity, wind, maximum-step, and slowdown-radius controls, and resets its
velocity when the aim button, target, capture, or mouse device is lost.

Axis control defaults to continuous `X + Y` movement and has independent
horizontal/vertical multipliers plus its own smoothing control. `Horizontal
only` always emits zero vertical movement. `Hybrid` uses a 100–1000 ms window:
both axes move during that window, then vertical movement remains disabled
until the aim button is released and held again or TestMove is disabled and
re-enabled.

The Target section exposes `Target height` from 0% at the top to 100% at the
bottom of each merged target rectangle. Its default is 15%, and it is included
in shareable TestMove profiles.

Optional target behavior includes `Anti Below Objects`, which rejects aim
points below the capture center; randomized short stops with Full Stop or
Slow Move behavior, chance, duration, and slowdown ranges; and a randomized
0–1000 ms delay before a newly selected target starts moving. The new-target
delay keeps tracking an existing target immediately and uses a separate
pending-target identity so the delay persists correctly across fresh frames.
These controls and merge proximity are grouped under the code-icon
`Experimental` section.

TestMove's configurable Kill FOV circle is drawn inside the Sunshine ROI
preview, using decoded-pixel coordinates and the selected color. It is not a
receiver-desktop overlay. The FOV still constrains targeting, and the enhanced
detector preview retains accepted/rejected contours and calibrated aim points.

## Python

The **Python** tab recreates the supplied colorbot.py detection and movement
algorithm in native C++, with Sunshine pixels and physical MAKCU mouse-button
activation. Original and Renderer use native C++; the optional Continuous pipeline
uses the bundled private Python runtime on the receiver. No system Python is needed.
The supplied config is embedded and also shipped as `python-config.json`;
load/save JSON, restore defaults, edit HSV/FOV/smoothing/offsets and inspect
the mask/contour/aim preview on the tab.

Its independent centered crop is 120x120 with the supplied radius 60. Thresholds,
offsets and morphology use decoded pixels and are **not** scaled for 1440p.
It retains double-precision smoothing 0.4030150753768844 and Python's
integer truncation. There is no normal detector, blacklist, tracking, extra speed,
deadzone or automatic shooting in this mode. Physical-input locking is handled
by the shared Mouse Output coordinator; see [README-INPUT-LOCKS.md](README-INPUT-LOCKS.md).
Python and TestMove movement are mutually exclusive; TestClick remains an
independent feature. Python is always disabled on startup and after JSON/profile
loading. Python uses the current MAKCU connection and automatic COM discovery;
legacy Python COM-port overrides are ignored. The saved port is a preference
while detected, rather than a port lock.

See [README-PYTHON.md](README-PYTHON.md) for exact reference behavior,
configuration caveats, software parity tests and hardware limitations.

## Configuration

Machine-local settings are loaded before automatic capture starts and saved
atomically at the end of the UI frame in:

```text
%LocalAppData%\Vanta\local.json
```

This includes Sunshine resolution/FPS/bitrate, ROI and preview tuning, selected
MAKCU port and auto-connect preference, update preferences, Python configuration,
menu geometry/options, opacity, and the style palette.

Local settings use schema 10. Legacy backend/source selections and local
window/monitor identities are ignored on load; other settings are retained.
An older file is backed up once before a schema upgrade is saved.
Shared profiles use schema 9, including detector and optional Python settings.
Old profiles keep their detector tuning. Removed timer fields are ignored.
Neither JSON files nor shared profiles persist an armed Python state.

## Updates

The `Updates` page checks GitHub asynchronously and shows the installed
version, `Up to date` or `Update vX.Y.Z available`, a 0–100% progress bar,
the current operation, and scrollable release notes. It provides manual
`Check for updates`, `Download`, and `Install on exit` actions. Automatic
checks, automatic downloads, and silent automatic installation are enabled by
default and can be changed independently.

Release metadata is checked at most once per 24 hours unless the manual button
is used. Downloads are stored under `%LocalAppData%\Vanta\updates`; the ZIP
must match both the published checksum asset and GitHub's SHA-256 digest.
Installation happens out of process after Vanta's clean shutdown. The updater
waits for Vanta to terminate, extracts into staging, backs up every runtime
file, replaces the bundle, rolls the backup back on any failure, and relaunches
Vanta.

The `Configs` page manages named, shareable TestClick/TestMove profiles under
`%LocalAppData%\Vanta\profiles`. It supports create/overwrite, load, delete,
import, export, and opening the folder. Profiles exclude counters, connection
state, and machine-local settings. TestClick and TestMove enable states are
included, so loading a profile restores whether each test is active.
Malformed or unsupported-schema JSON is retained for diagnosis and reported
in the log and UI.

## Style

The icon-font glyphs are used in the header, sidebar, and panel titles. The
`Style` page exposes every color from the supplied modified ImGui palette,
including accent, backgrounds, strokes, animation, page, element, label,
description, and text colors. Each entry uses the supplied compact color
picker with its alpha bar, so the original opacity values are editable too.

## Automated renderer test

```powershell
.\build\Release\vanta.exe --self-test 120
```

This creates the always-on-top virtual-desktop surface, renders 120 frames,
and exits with code zero when successful. Renderer tests use synthetic Sunshine
frames, disable device auto-connect, ignore local configuration, and do not write
user settings or launch/pair a Sunshine session.

Configuration serialization diagnostics use a temporary directory:

```powershell
.\build\Release\vanta.exe --config-self-test
```

Validate the local receiver without contacting a Sunshine host. This encodes
a small H.264 IDR/P-frame sequence with Windows' inbox encoder, feeds the
Annex-B access units through Vanta's real receiver callback path, and verifies
that Vanta publishes a D3D11 GPU presentation texture:

```powershell
.\build\Release\vanta.exe --receiver-self-test
```

Validate the handoff from a decoded Sunshine-style frame into Vanta's normal
capture consumers without contacting a host:

```powershell
.\build\Release\vanta.exe --receiver-capture-self-test
```

Verify that Vanta can discover a Sunshine host without pairing or starting a
stream:

```powershell
.\build\Release\vanta.exe --receiver-discover 192.168.1.10
```

To test the same automatic LAN discovery used by the receiver page:

```powershell
.\build\Release\vanta.exe --receiver-discover-lan
```

After pairing Vanta through the **Stream Receiver** page, this explicit live
smoke test launches the paired host's Desktop with display-mode matching off,
waits for a verified hardware frame to reach Vanta's decoder, and stops the session
again. It does not pair a client or change the host display mode:

```powershell
.\build\Release\vanta.exe --receiver-stream-smoke 192.168.1.10
```

The updater's version comparison, daily-cache boundary, SHA-256 success/failure,
replacement, and forced rollback paths can be checked without networking or
touching the installed bundle:

```powershell
.\build\Release\vanta-updater.exe --self-test
```

Sunshine-only GUI diagnostics (Capture, TestClick, TestMove, Python):

```powershell
.\build\Release\vanta.exe --self-test 60 --page-test 1
.\build\Release\vanta.exe --self-test 60 --page-test 4
.\build\Release\vanta.exe --self-test 60 --page-test 5
.\build\Release\vanta.exe --self-test 60 --page-test 7
```
